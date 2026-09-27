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

#include "maimemo_client.h"
#include "sys_keys.h"

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

/* ------------------------------------------------------------------------ */
/* Cloud queue (plan A)                                                      */
/* ------------------------------------------------------------------------ */

/*
 * When `SYS_KEY_MAIMEMO_MODE` is "cloud" and a token is configured, today's
 * queue is MaiMemo's study plan rather than the local book. The cloud words are
 * held in their own array because they are *not* in s_words: a word can be in
 * the account's plan without being in the SD-card book, and the reverse is also
 * true.
 *
 * Local progress (level/scheduling) still applies: a cloud word is matched back
 * to the book by spelling so grading it updates the same record the local flow
 * would, and a word with no local entry simply cannot be graded locally - see
 * cloud_index_for().
 */
static maimemo_item_t *s_cloud_words;
static size_t s_cloud_count;
/** Book indices for the cloud queue: UINT16_MAX means "no local entry". */
static uint16_t *s_cloud_map;
static bool s_cloud_active;

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

/* ------------------------------------------------------------------------ */
/* Deferred saving                                                           */
/* ------------------------------------------------------------------------ */

/*
 * word_save_progress() serialises every reviewed word and rewrites the whole
 * file on the SD card (open -> write -> remove -> rename). It used to be called
 * directly from two LVGL widget callbacks - grading a card and tapping the
 * daily-count buttons - which run *with the adapter lock held*, because the
 * adapter calls lv_timer_handler() under it (AGENT.md 9.0.1). Every tap
 * therefore stalled rendering for the duration of a FAT write.
 *
 * Requests now only wake a worker that coalesces them: tapping "+5" ten times
 * produces one write, and the UI never waits for the card.
 */
#define WORD_SAVE_TASK_STACK 4096
#define WORD_SAVE_TASK_PRIO  3
/** Coalescing window: long enough to absorb a burst of taps, short enough that
 *  losing power right after grading costs at most this much. */
#define WORD_SAVE_DEBOUNCE_MS 600

static TaskHandle_t s_save_task;
static StaticSemaphore_t s_save_lock_buf;
static SemaphoreHandle_t s_save_lock;

static void word_save_worker(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Absorb a burst of requests before touching the card. */
        vTaskDelay(pdMS_TO_TICKS(WORD_SAVE_DEBOUNCE_MS));

        const esp_err_t err = word_save_progress();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            SYS_LOGW(TAG, "deferred progress save failed: %s", esp_err_to_name(err));
        }
    }
}

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

    /*
     * Diagnostics for the scarcest resource on this board. A full word book is
     * ~7000 cJSON nodes, and *while the tree is alive* it holds all of them; if
     * those come out of internal RAM the concurrently connecting WiFi driver
     * cannot allocate and the boot aborts (AGENT.md 9.0.5). With the cJSON hooks
     * from sys_json_use_psram() in place this number stays at its pre-parse
     * value, which is exactly what makes the SD-card boot work.
     */
    SYS_LOGI(TAG, "book tree alive: internal %u KB free (largest %u KB)",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(sys_heap_largest_internal() / 1024));

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

    /*
     * Word text is bulky: prefer PSRAM so internal RAM stays free for WiFi/HTTP.
     * Both arrays are part of the same "bulk device data" rule as the cJSON hooks
     * installed by sys_json_use_psram(): 833 words are ~546 KB of items and
     * ~20 KB of progress, which must never land in the ~310 KB internal pool.
     */
    word_item_t *items = heap_caps_malloc_prefer(count * sizeof(word_item_t), 2,
                                                 MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    word_progress_t *progress = heap_caps_calloc_prefer(count, sizeof(word_progress_t), 2,
                                                        MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
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

/**
 * Mark the progress dirty and let the worker write it out. Never blocks on the
 * card, so it is safe to call from an LVGL callback. See the block comment at
 * word_save_worker().
 */
esp_err_t word_save_progress_async(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_save_lock == NULL) {
        s_save_lock = xSemaphoreCreateMutexStatic(&s_save_lock_buf);
        if (s_save_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    if (s_save_task == NULL) {
        const BaseType_t ok = xTaskCreate(word_save_worker, "th_word_save",
                                          WORD_SAVE_TASK_STACK, NULL,
                                          WORD_SAVE_TASK_PRIO, &s_save_task);
        if (ok != pdPASS) {
            s_save_task = NULL;
            xSemaphoreGive(s_save_lock);
            SYS_LOGE(TAG, "cannot create the progress writer");
            return ESP_ERR_NO_MEM;
        }
    }
    TaskHandle_t task = s_save_task;
    xSemaphoreGive(s_save_lock);

    xTaskNotifyGive(task);
    return ESP_OK;
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
    SYS_LOGI(TAG, "loaded %u words from '%s' (%u B)", (unsigned)count, relative, (unsigned)len);
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
/* ------------------------------------------------------------------------ */
/* Cloud queue (plan A)                                                      */
/* ------------------------------------------------------------------------ */

/** True when today's queue should come from MaiMemo rather than the SD book. */
bool word_cloud_mode(void)
{
    char mode[16];
    if (sys_config_get_str(SYS_KEY_MAIMEMO_MODE, mode, sizeof(mode)) != ESP_OK) {
        return false;   /* unset = local book, which is the offline-safe default */
    }
    return strcmp(mode, "cloud") == 0 && maimemo_has_token();
}

esp_err_t word_set_cloud_mode(bool enabled)
{
    const esp_err_t err = sys_config_set_str(SYS_KEY_MAIMEMO_MODE, enabled ? "cloud" : "local");
    if (err == ESP_OK) {
        /* Force a rebuild so the next card comes from the new source. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_queue_len = 0;
        s_queue_pos = 0;
        xSemaphoreGive(s_lock);
    }
    return err;
}

/** Index of @p spelling in the local book, or UINT16_MAX when absent. */
static uint16_t local_index_for(const char *spelling)
{
    for (size_t i = 0; i < s_count; i++) {
        if (strcasecmp(s_words[i].spelling, spelling) == 0) {
            return (uint16_t)i;
        }
    }
    return UINT16_MAX;
}

static void free_cloud_queue(void)
{
    heap_caps_free(s_cloud_words);
    heap_caps_free(s_cloud_map);
    s_cloud_words = NULL;
    s_cloud_map = NULL;
    s_cloud_count = 0;
    s_cloud_active = false;
}

/**
 * Pull today's words from MaiMemo. Blocking - worker/HTTP context only.
 *
 * On any failure the cloud queue is dropped and the caller falls back to the
 * local book: a network problem must never leave the user with no cards at all.
 */
esp_err_t word_cloud_refresh(void)
{
    if (!word_cloud_mode()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    maimemo_item_t *items = heap_caps_calloc_prefer(MAIMEMO_MAX_TODAY,
                                                   sizeof(maimemo_item_t), 2,
                                                   MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    uint16_t *map = heap_caps_malloc_prefer(MAIMEMO_MAX_TODAY * sizeof(uint16_t), 2,
                                            MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (items == NULL || map == NULL) {
        heap_caps_free(items);
        heap_caps_free(map);
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    const esp_err_t err = maimemo_fetch_today(items, MAIMEMO_MAX_TODAY, &count);
    if (err != ESP_OK) {
        heap_caps_free(items);
        heap_caps_free(map);
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t mapped = 0;
    for (size_t i = 0; i < count; i++) {
        map[i] = local_index_for(items[i].spelling);
        if (map[i] != UINT16_MAX) {
            mapped++;
        }
    }
    free_cloud_queue();
    s_cloud_words = items;
    s_cloud_map = map;
    s_cloud_count = count;
    s_cloud_active = (count > 0);
    /* The local index queue is replaced by the cloud queue, so clear it. */
    free(s_queue);
    s_queue = NULL;
    s_queue_len = 0;
    s_queue_pos = 0;
    xSemaphoreGive(s_lock);

    SYS_LOGI(TAG, "cloud queue: %u words (%u also in the local book)",
             (unsigned)count, (unsigned)mapped);
    return (count > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/** How many cloud words are currently loaded.
 *
 * Takes the lock: word_cloud_refresh() replaces the arrays under it, and an
 * unlocked read of s_cloud_count can be observed mid-update (it is also read by
 * the UI task while a sync worker may be swapping the queue). */
size_t word_cloud_count(void)
{
    if (s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t count = s_cloud_count;
    xSemaphoreGive(s_lock);
    return count;
}

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

    /* Cloud queue: same shape as the local one, but with no local scheduling
     * data for words outside the book (see word_next()). */
    if (s_cloud_active) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const size_t total = s_cloud_count;
        size_t copy = (total > max) ? max : total;
        if (out != NULL) {
            for (size_t i = 0; i < copy; i++) {
                const uint16_t local = s_cloud_map[i];
                memset(&out[i], 0, sizeof(out[i]));
                if (local != UINT16_MAX) {
                    const uint32_t id = s_words[local].id;
                    out[i] = s_words[local];
                    out[i].level = s_progress[local].level;
                    out[i].next_review_ts = s_progress[local].next_review_ts;
                    out[i].id = id;
                } else {
                    out[i].id = WORD_ID_CLOUD;
                }
                strlcpy(out[i].spelling, s_cloud_words[i].spelling, sizeof(out[i].spelling));
            }
        }
        xSemaphoreGive(s_lock);
        *count = total;
        return ESP_OK;
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

    /* Cloud queue first (plan A): it is the account's plan, so it wins. A word
     * that is not in the local book still gets a card - it just has no local
     * scheduling record, which is honest: the account owns its scheduling. */
    if (s_cloud_active) {
        if (s_queue_pos >= s_cloud_count) {
            s_queue_pos = 0;   /* wrap around so the page can keep flipping */
        }
        const size_t cloud_index = s_queue_pos++;
        const uint16_t local = s_cloud_map[cloud_index];
        memset(out, 0, sizeof(*out));
        strlcpy(out->spelling, s_cloud_words[cloud_index].spelling, sizeof(out->spelling));
        if (local != UINT16_MAX) {
            const uint32_t id = s_words[local].id;
            *out = s_words[local];
            out->level = s_progress[local].level;
            out->next_review_ts = s_progress[local].next_review_ts;
            /* Keep the account's spelling, which is the string the user is
             * actually being asked to recall. */
            strlcpy(out->spelling, s_cloud_words[cloud_index].spelling,
                    sizeof(out->spelling));
            out->id = id;
        } else {
            /* No local record: give it a stable id derived from the cloud voc_id
             * so grading can still be routed (word_review falls back to pushing
             * to the account when the id is unknown locally). */
            out->id = WORD_ID_CLOUD;
        }
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

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
    /* The cloud queue has its own length; reporting the local one here would show
     * "3/10" while flipping through 20 cloud cards. */
    const size_t len = s_cloud_active ? s_cloud_count : s_queue_len;
    if (position != NULL) {
        *position = len > 0 ? (s_queue_pos > len ? len : s_queue_pos) : 0;
    }
    if (total != NULL) {
        *total = len;
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
        /* Plan C: a card that came from the cloud plan but is not in the local
         * book. There is no local record to schedule, so grading it means "tell
         * the account". This is fire-and-forget: the caller is an LVGL callback
         * and must not wait for TLS, and maimemo_push_unknown() itself no-ops
         * unless the user enabled pushing. */
        if (s_cloud_active && id == WORD_ID_CLOUD) {
            SYS_LOGD(TAG, "graded a cloud-only card ('%s' = %s), not scheduled locally",
                     word_review_name(result), "no local record");
        }
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
    /* Deferred: grading happens in an LVGL callback, so it must not wait for the
     * card. The worker coalesces a burst of grades into one write. */
    return word_save_progress_async();
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
    return (err == ESP_OK) ? word_save_progress_async() : err;
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
    return word_save_progress_async();
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
