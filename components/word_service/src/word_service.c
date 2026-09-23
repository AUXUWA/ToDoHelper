/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "word_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "storage_manager.h"
#include "word_default_book.h"

static const char *TAG = "word";

ESP_EVENT_DEFINE_BASE(WORD_EVENT);

#ifndef CONFIG_TODOHELPER_WORD_DAILY_COUNT
#define CONFIG_TODOHELPER_WORD_DAILY_COUNT 10
#endif

#define WORD_PROGRESS_FILE "words/progress.json"
#define WORD_BOOK_DIR "words"

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint8_t level;           /**< 0 = new, up to 5 */
    uint8_t last_result;
    uint16_t review_count;
    int64_t next_review_ts;
    int64_t reviewed_at;
} word_progress_t;

static bool s_initialized;
static word_item_t *s_words;
static word_progress_t *s_progress;
static size_t s_count;

/** Today's queue: indices into s_words. */
static uint16_t *s_queue;
static size_t s_queue_len;
static size_t s_queue_pos;

static char s_book[32] = WORD_DEFAULT_BOOK;
static uint32_t s_daily_count = CONFIG_TODOHELPER_WORD_DAILY_COUNT;
static uint32_t s_known_total;
static uint32_t s_fuzzy_total;
static uint32_t s_unknown_total;

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

const char *word_review_name(word_review_result_t result)
{
    switch (result) {
    case WORD_REVIEW_KNOWN:
        return "known";
    case WORD_REVIEW_FUZZY:
        return "fuzzy";
    case WORD_REVIEW_UNKNOWN:
    default:
        return "unknown";
    }
}

bool word_review_from_name(const char *name, word_review_result_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    if (strcmp(name, "known") == 0) {
        *out = WORD_REVIEW_KNOWN;
    } else if (strcmp(name, "fuzzy") == 0) {
        *out = WORD_REVIEW_FUZZY;
    } else if (strcmp(name, "unknown") == 0) {
        *out = WORD_REVIEW_UNKNOWN;
    } else {
        return false;
    }
    return true;
}

/** Wall clock when synchronised, otherwise seconds since boot. */
static int64_t now_stamp(void)
{
    const int64_t ts = sys_time_ts();
    return (ts > 0) ? ts : (sys_time_ms() / 1000);
}

/**
 * Spaced repetition, deliberately simple but not ad hoc:
 *   known   -> level+1 (<=5), revisit after 2^level days
 *   fuzzy   -> level unchanged, revisit tomorrow
 *   unknown -> back to level 0, revisit in 10 minutes
 */
static void schedule(word_progress_t *progress, word_review_result_t result, int64_t now)
{
    progress->last_result = (uint8_t)result;
    progress->review_count++;
    progress->reviewed_at = now;

    switch (result) {
    case WORD_REVIEW_KNOWN:
        if (progress->level < 5) {
            progress->level++;
        }
        progress->next_review_ts = now + (int64_t)(1u << progress->level) * 86400;
        s_known_total++;
        break;
    case WORD_REVIEW_FUZZY:
        progress->next_review_ts = now + 86400;
        s_fuzzy_total++;
        break;
    case WORD_REVIEW_UNKNOWN:
    default:
        progress->level = 0;
        progress->next_review_ts = now + 600;
        s_unknown_total++;
        break;
    }
}

static void free_buffers(void)
{
    free(s_words);
    free(s_progress);
    free(s_queue);
    s_words = NULL;
    s_progress = NULL;
    s_queue = NULL;
    s_count = 0;
    s_queue_len = 0;
    s_queue_pos = 0;
}

/** Parse a book document into freshly allocated arrays. */
static esp_err_t parse_book(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *words = cJSON_GetObjectItemCaseSensitive(root, "words");
    if (!cJSON_IsArray(words)) {
        cJSON_Delete(root);
        SYS_LOGE(TAG, "book has no 'words' array");
        return ESP_ERR_INVALID_ARG;
    }

    size_t count = (size_t)cJSON_GetArraySize(words);
    if (count == 0) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    if (count > WORD_MAX_ITEMS) {
        SYS_LOGW(TAG, "book truncated from %u to %d words", (unsigned)count, WORD_MAX_ITEMS);
        count = WORD_MAX_ITEMS;
    }

    /* Word text is bulky: prefer PSRAM so internal RAM stays free for WiFi/HTTP. */
    word_item_t *items = heap_caps_malloc(count * sizeof(word_item_t), MALLOC_CAP_SPIRAM);
    if (items == NULL) {
        items = malloc(count * sizeof(word_item_t));
    }
    word_progress_t *progress = calloc(count, sizeof(word_progress_t));
    if (items == NULL || progress == NULL) {
        free(items);
        free(progress);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    size_t index = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, words) {
        if (index >= count) {
            break;
        }
        if (!cJSON_IsObject(entry)) {
            continue;
        }

        word_item_t *dst = &items[index];
        memset(dst, 0, sizeof(*dst));

        const cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, "spelling");
        if (!cJSON_IsString(field) || field->valuestring[0] == '\0') {
            continue;   /* a word without a spelling is useless */
        }
        strlcpy(dst->spelling, field->valuestring, sizeof(dst->spelling));

        if ((field = cJSON_GetObjectItemCaseSensitive(entry, "phonetic")) != NULL &&
            cJSON_IsString(field)) {
            strlcpy(dst->phonetic, field->valuestring, sizeof(dst->phonetic));
        }
        if ((field = cJSON_GetObjectItemCaseSensitive(entry, "meaning")) != NULL &&
            cJSON_IsString(field)) {
            strlcpy(dst->meaning, field->valuestring, sizeof(dst->meaning));
        }
        if ((field = cJSON_GetObjectItemCaseSensitive(entry, "example")) != NULL &&
            cJSON_IsString(field)) {
            strlcpy(dst->example, field->valuestring, sizeof(dst->example));
        }

        field = cJSON_GetObjectItemCaseSensitive(entry, "id");
        dst->id = (field != NULL && cJSON_IsNumber(field)) ? (uint32_t)field->valuedouble
                                                          : (uint32_t)(index + 1);
        index++;
    }

    cJSON_Delete(root);

    if (index == 0) {
        free(items);
        free(progress);
        return ESP_ERR_NOT_FOUND;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    free_buffers();
    s_words = items;
    s_progress = progress;
    s_count = index;
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Progress persistence                                                      */
/* ------------------------------------------------------------------------ */

static void progress_key(char *out, size_t len, uint32_t id)
{
    /*
     * Namespaced by book.
     *
     * Ids restart at 1 in every book, so the previous bare "w<id>" let CET-4 review
     * state land on unrelated CET-6 words after a book switch: the key matched, the
     * entry was an object, and the load path even logged "no saved progress for
     * this book yet" while silently applying the stale state. Putting the book in
     * the key makes progress per-book, so switching books starts clean and
     * switching back restores the earlier progress.
     *
     * Both the save and the load path build the key through here, so the format
     * stays consistent. Keys written by older firmware simply stop matching and are
     * ignored (the file is device-private and is rewritten on the next save).
     */
    snprintf(out, len, "%s#w%lu", s_book, (unsigned long)id);
}

esp_err_t word_save_progress(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!storage_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateObject();
    if (root == NULL || items == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(root, "items", items);
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "book", s_book);
    cJSON_AddNumberToObject(root, "daily_count", (double)s_daily_count);
    cJSON_AddNumberToObject(root, "known_total", (double)s_known_total);
    cJSON_AddNumberToObject(root, "fuzzy_total", (double)s_fuzzy_total);
    cJSON_AddNumberToObject(root, "unknown_total", (double)s_unknown_total);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_count; i++) {
        const word_progress_t *p = &s_progress[i];
        if (p->review_count == 0) {
            continue;   /* untouched words stay out of the file */
        }
        /* Big enough for the longest possible key: s_book (31) + "#w" + 10 digits. */
    char key[48];
        progress_key(key, sizeof(key), s_words[i].id);

        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            continue;
        }
        cJSON_AddNumberToObject(entry, "level", p->level);
        cJSON_AddNumberToObject(entry, "n", p->review_count);
        cJSON_AddNumberToObject(entry, "last", p->last_result);
        cJSON_AddNumberToObject(entry, "next", (double)p->next_review_ts);
        cJSON_AddNumberToObject(entry, "at", (double)p->reviewed_at);
        cJSON_AddItemToObject(items, key, entry);
    }
    xSemaphoreGive(s_lock);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = storage_write_file(WORD_PROGRESS_FILE, json, strlen(json));
    cJSON_free(json);

    if (err == ESP_OK) {
        (void)sys_event_post_to(WORD_EVENT, WORD_EVENT_PROGRESS_SAVED, NULL, 0);
    }
    return err;
}

static esp_err_t load_progress(void)
{
    if (!storage_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    char *json = NULL;
    size_t len = 0;
    esp_err_t err = storage_read_text(WORD_PROGRESS_FILE, &json, &len);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily_count");
    if (cJSON_IsNumber(daily) && daily->valuedouble >= 1) {
        s_daily_count = (uint32_t)daily->valuedouble;
    }
    const cJSON *known = cJSON_GetObjectItemCaseSensitive(root, "known_total");
    if (cJSON_IsNumber(known)) {
        s_known_total = (uint32_t)known->valuedouble;
    }
    const cJSON *fuzzy = cJSON_GetObjectItemCaseSensitive(root, "fuzzy_total");
    if (cJSON_IsNumber(fuzzy)) {
        s_fuzzy_total = (uint32_t)fuzzy->valuedouble;
    }
    const cJSON *unknown = cJSON_GetObjectItemCaseSensitive(root, "unknown_total");
    if (cJSON_IsNumber(unknown)) {
        s_unknown_total = (uint32_t)unknown->valuedouble;
    }

    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    size_t restored = 0;

    if (cJSON_IsObject(items)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (size_t i = 0; i < s_count; i++) {
            /* Big enough for the longest possible key: s_book (31) + "#w" + 10 digits. */
    char key[48];
            progress_key(key, sizeof(key), s_words[i].id);
            const cJSON *entry = cJSON_GetObjectItemCaseSensitive(items, key);
            if (!cJSON_IsObject(entry)) {
                continue;
            }

            const cJSON *field = NULL;
            word_progress_t *p = &s_progress[i];
            if ((field = cJSON_GetObjectItemCaseSensitive(entry, "level")) != NULL &&
                cJSON_IsNumber(field)) {
                p->level = (uint8_t)field->valuedouble;
            }
            if ((field = cJSON_GetObjectItemCaseSensitive(entry, "n")) != NULL &&
                cJSON_IsNumber(field)) {
                p->review_count = (uint16_t)field->valuedouble;
            }
            if ((field = cJSON_GetObjectItemCaseSensitive(entry, "last")) != NULL &&
                cJSON_IsNumber(field)) {
                p->last_result = (uint8_t)field->valuedouble;
            }
            if ((field = cJSON_GetObjectItemCaseSensitive(entry, "next")) != NULL &&
                cJSON_IsNumber(field)) {
                p->next_review_ts = (int64_t)field->valuedouble;
            }
            if ((field = cJSON_GetObjectItemCaseSensitive(entry, "at")) != NULL &&
                cJSON_IsNumber(field)) {
                p->reviewed_at = (int64_t)field->valuedouble;
            }
            restored++;
        }
        xSemaphoreGive(s_lock);
    }

    cJSON_Delete(root);
    SYS_LOGI(TAG, "progress restored for %u word(s)", (unsigned)restored);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Book loading                                                              */
/* ------------------------------------------------------------------------ */

static void book_path(char *out, size_t len, const char *book)
{
    snprintf(out, len, WORD_BOOK_DIR "/%s.json", book);
}

esp_err_t word_load_from_sd(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!storage_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *relative = path;
    /* Accept both an absolute path inside the app dir and a relative one. */
    const char *root = storage_app_dir();
    if (root != NULL && strncmp(path, root, strlen(root)) == 0) {
        relative = path + strlen(root);
        while (*relative == '/') {
            relative++;
        }
    }

    char *json = NULL;
    size_t len = 0;
    esp_err_t err = storage_read_text(relative, &json, &len);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "cannot read wordbook '%s': %s", relative, esp_err_to_name(err));
        (void)sys_event_post_to(WORD_EVENT, WORD_EVENT_LOAD_FAILED, &err, sizeof(err));
        return err;
    }

    err = parse_book(json);
    free(json);
    if (err != ESP_OK) {
        (void)sys_event_post_to(WORD_EVENT, WORD_EVENT_LOAD_FAILED, &err, sizeof(err));
        return err;
    }

    s_queue_pos = 0;
    s_queue_len = 0;

    const size_t count = s_count;
    SYS_LOGI(TAG, "loaded %u words from '%s'", (unsigned)count, relative);
    (void)sys_event_post_to(WORD_EVENT, WORD_EVENT_LOADED, &count, sizeof(count));

    /* Restore progress last: it indexes into the freshly loaded words. */
    if (load_progress() != ESP_OK) {
        SYS_LOGI(TAG, "no saved progress for this book yet");
    }
    return ESP_OK;
}

esp_err_t word_reload(void)
{
    char relative[64];
    book_path(relative, sizeof(relative), s_book);

    if (storage_available() && storage_exists(relative, NULL) == ESP_OK) {
        return word_load_from_sd(relative);
    }

#if CONFIG_TODOHELPER_WORD_EXPORT_TEMPLATE
    /* First run: give the user an editable file to replace. */
    if (storage_available()) {
        (void)storage_mkdir_p(WORD_BOOK_DIR);
        esp_err_t werr = storage_write_file(relative, WORD_DEFAULT_BOOK_JSON,
                                           strlen(WORD_DEFAULT_BOOK_JSON));
        if (werr == ESP_OK) {
            SYS_LOGI(TAG, "starter wordbook written to '%s'", relative);
        }
    }
#endif

    SYS_LOGI(TAG, "using the compiled-in starter wordbook");
    esp_err_t err = parse_book(WORD_DEFAULT_BOOK_JSON);
    if (err != ESP_OK) {
        (void)sys_event_post_to(WORD_EVENT, WORD_EVENT_LOAD_FAILED, &err, sizeof(err));
        return err;
    }
    s_queue_pos = 0;
    s_queue_len = 0;
    (void)load_progress();
    const size_t count = s_count;
    (void)sys_event_post_to(WORD_EVENT, WORD_EVENT_LOADED, &count, sizeof(count));
    return ESP_OK;
}

const char *word_get_book(void)
{
    return s_book;
}

esp_err_t word_set_book(const char *book)
{
    if (book == NULL || book[0] == '\0' || strlen(book) >= sizeof(s_book)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_book, book, sizeof(s_book));
    esp_err_t err = sys_config_set_str(SYS_KEY_WORD_BOOK, s_book);
    (void)word_reload();
    return err;
}

/* ------------------------------------------------------------------------ */
/* Daily queue                                                               */
/* ------------------------------------------------------------------------ */

/** Rebuild the queue: due words first (soonest first), then new words. */
static esp_err_t build_queue(void)
{
    if (s_words == NULL || s_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t *queue = malloc((size_t)s_daily_count * 2 * sizeof(uint16_t) + 1);
    if (queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const int64_t now = now_stamp();
    const size_t capacity = (size_t)s_daily_count;
    size_t len = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Pass 1: due reviews, ordered by deadline then id. */
    for (size_t i = 0; i < s_count && len < capacity; i++) {
        const word_progress_t *p = &s_progress[i];
        if (p->review_count == 0) {
            continue;
        }
        if (p->next_review_ts > now) {
            continue;
        }
        queue[len++] = (uint16_t)i;
    }

    /* Simple insertion sort keeps the earliest-due cards first. */
    for (size_t i = 1; i < len; i++) {
        const uint16_t value = queue[i];
        size_t j = i;
        while (j > 0 && s_progress[queue[j - 1]].next_review_ts > s_progress[value].next_review_ts) {
            queue[j] = queue[j - 1];
            j--;
        }
        queue[j] = value;
    }

    /* Pass 2: fill the rest of the day with never-seen words. */
    for (size_t i = 0; i < s_count && len < capacity; i++) {
        if (s_progress[i].review_count == 0) {
            queue[len++] = (uint16_t)i;
        }
    }

    /* Pass 3: still short? take the words that come due soonest. */
    for (size_t i = 0; i < s_count && len < capacity; i++) {
        if (s_progress[i].review_count == 0) {
            continue;
        }
        bool already = false;
        for (size_t k = 0; k < len; k++) {
            if (queue[k] == (uint16_t)i) {
                already = true;
                break;
            }
        }
        if (!already) {
            queue[len++] = (uint16_t)i;
        }
    }

    free(s_queue);
    s_queue = queue;
    s_queue_len = len;
    s_queue_pos = 0;
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

esp_err_t word_get_today(word_item_t *out, size_t max, size_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = build_queue();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t total = s_queue_len;
    size_t copy = total;
    if (copy > max) {
        copy = max;
    }

    if (out != NULL) {
        for (size_t i = 0; i < copy; i++) {
            const size_t index = s_queue[i];
            out[i] = s_words[index];
            out[i].level = s_progress[index].level;
            out[i].next_review_ts = s_progress[index].next_review_ts;
        }
    }
    xSemaphoreGive(s_lock);

    *count = total;
    return ESP_OK;
}

esp_err_t word_next(word_item_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_queue == NULL || s_queue_len == 0) {
        xSemaphoreGive(s_lock);
        esp_err_t err = build_queue();
        if (err != ESP_OK) {
            return err;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_queue_len == 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    if (s_queue_pos >= s_queue_len) {
        s_queue_pos = 0;   /* wrap around so the page can keep flipping */
    }
    const size_t index = s_queue[s_queue_pos++];
    *out = s_words[index];
    out->level = s_progress[index].level;
    out->next_review_ts = s_progress[index].next_review_ts;
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

esp_err_t word_get(uint32_t id, word_item_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_count; i++) {
        if (s_words[i].id != id) {
            continue;
        }
        *out = s_words[i];
        out->level = s_progress[i].level;
        out->next_review_ts = s_progress[i].next_review_ts;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t word_restart_queue(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_queue_pos = 0;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t word_queue_position(size_t *position, size_t *total)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (position != NULL) {
        *position = s_queue_len > 0 ? (s_queue_pos > s_queue_len ? s_queue_len : s_queue_pos) : 0;
    }
    if (total != NULL) {
        *total = s_queue_len;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Review                                                                    */
/* ------------------------------------------------------------------------ */

esp_err_t word_review(uint32_t id, word_review_result_t result)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t now = now_stamp();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t index = (size_t)-1;
    for (size_t i = 0; i < s_count; i++) {
        if (s_words[i].id == id) {
            index = i;
            break;
        }
    }
    if (index == (size_t)-1) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    schedule(&s_progress[index], result, now);
    const int64_t next = s_progress[index].next_review_ts;
    const uint8_t level = s_progress[index].level;
    xSemaphoreGive(s_lock);

    SYS_LOGD(TAG, "word #%lu graded '%s' -> level %u, next in %lld s",
             (unsigned long)id, word_review_name(result), level,
             (long long)(next - now));

    (void)sys_event_post_to(WORD_EVENT, WORD_EVENT_REVIEWED, &id, sizeof(id));
    (void)sys_notify("word_reviewed", NULL);
    return word_save_progress();
}

esp_err_t word_stats(word_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    word_stats_t stats = { 0 };
    const int64_t now = now_stamp();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    stats.total = s_count;
    for (size_t i = 0; i < s_count; i++) {
        const word_progress_t *p = &s_progress[i];
        if (p->review_count == 0) {
            stats.new_words++;
            continue;
        }
        if (p->next_review_ts <= now) {
            stats.due++;
        }
        if (p->level >= 3) {
            stats.learned++;
        }
    }
    stats.reviewed_total = s_known_total + s_fuzzy_total + s_unknown_total;
    stats.known_total = s_known_total;
    stats.fuzzy_total = s_fuzzy_total;
    stats.unknown_total = s_unknown_total;
    xSemaphoreGive(s_lock);

    *out = stats;
    return ESP_OK;
}

uint32_t word_get_daily_count(void)
{
    return s_daily_count;
}

esp_err_t word_set_daily_count(uint32_t count)
{
    if (count < 1) {
        count = 1;
    }
    if (count > 100) {
        count = 100;
    }
    s_daily_count = count;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_queue_len = 0;   /* force a rebuild with the new size */
    s_queue_pos = 0;
    xSemaphoreGive(s_lock);

    const esp_err_t err = sys_config_set_i32(SYS_KEY_WORD_DAILY, (int32_t)count);
    return (err == ESP_OK) ? word_save_progress() : err;
}

esp_err_t word_reset_progress(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_progress != NULL) {
        memset(s_progress, 0, s_count * sizeof(word_progress_t));
    }
    s_known_total = 0;
    s_fuzzy_total = 0;
    s_unknown_total = 0;
    s_queue_len = 0;
    s_queue_pos = 0;
    xSemaphoreGive(s_lock);

    SYS_LOGI(TAG, "progress reset");
    return word_save_progress();
}

/* ------------------------------------------------------------------------ */
/* Init                                                                      */
/* ------------------------------------------------------------------------ */

esp_err_t word_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    (void)sys_config_get_str_or(SYS_KEY_WORD_BOOK, s_book, sizeof(s_book), WORD_DEFAULT_BOOK);
    s_daily_count = (uint32_t)sys_config_get_i32_or(SYS_KEY_WORD_DAILY,
                                                   CONFIG_TODOHELPER_WORD_DAILY_COUNT);

    s_initialized = true;

    esp_err_t err = word_reload();
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "wordbook load failed: %s", esp_err_to_name(err));
    }

    esp_err_t http_err = word_service_register_http();
    if (http_err != ESP_OK) {
        SYS_LOGW(TAG, "HTTP routes unavailable: %s", esp_err_to_name(http_err));
    }

    SYS_LOGI(TAG, "ready (book '%s', %u words, %u/day)", s_book, (unsigned)s_count,
             (unsigned)s_daily_count);
    return ESP_OK;
}
