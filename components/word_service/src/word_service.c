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
 * queue is MaiMemo's study plan rather than the local book.
 *
 * The cloud words are held in their own array because they are *not* in s_words:
 * a word can be in the account's plan without being in the SD-card book, and the
 * reverse is also true.
 *
 * ## v1.4.0: the queue carries content, not just spellings
 *
 * MaiMemo's `/study/get_today_items` returns only `voc_id` + `voc_spelling`, so
 * a cloud word used to be matched back to the book by spelling and, when absent,
 * shown as a bare spelling with no meaning - which is what the user reported
 * ("单词背诵里面没有墨墨单词的单词 只有本地词库的"). The queue now holds a full
 * `word_item_t` per entry, filled in two stages:
 *
 *   - **at build time, without any network**: a word found in the local book
 *     (CET-6) gets its curated phonetic/meaning/example, and a cloud-only word
 *     gets whatever a previous view already cached (an in-PSRAM read);
 *   - **when the card is shown**: a cloud-only word with no cached text is
 *     enriched then, by page_word's per-card worker, and the result is cached on
 *     the SD card so later views are free.
 *
 * Fetching everything up front was the first attempt and it **broke the board**
 * (see cloud_fill_content); the lazy split is what makes the feature safe here.
 *
 * `s_cloud_map` still records the book index when there is one, so grading keeps
 * updating the same local record the local flow would.
 */
static word_item_t *s_cloud_words;
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

/* Defined with the study-history section below; the save worker drains it. */
void word_history_flush_pending(void);

static void word_save_worker(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Absorb a burst of requests before touching the card. */
        vTaskDelay(pdMS_TO_TICKS(WORD_SAVE_DEBOUNCE_MS));

        /* Study-history lines first: they are appended, so unlike progress.json
         * there is nothing to coalesce - and draining them here means the UI
         * path never touches the card itself. */
        word_history_flush_pending();

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
 * Start of the current local day, for the "today only" history filter.
 *
 * `sys_time_ts()` is already UTC+offset because calendar_service applies the
 * configured timezone when it anchors the clock (see sys_config_get_str(
 * SYS_KEY_TZ)), so the day boundary is a plain integer division. Only valid when
 * the clock is synced; before that the history filter falls back to "everything
 * since boot", which is the same compromise the todo list makes.
 */
static int64_t day_start_stamp(void)
{
    const int64_t ts = sys_time_ts();
    if (ts <= 0) {
        return 0;
    }
    return (ts / 86400) * 86400;
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
            /*
             * Could not start the writer.
             *
             * This used to be a hard ESP_ERR_NO_MEM, and because a task stack
             * must come from **internal** RAM (AGENT.md 9.0.1) while this board
             * runs with as little as ~13 KB of contiguous internal heap after
             * boot (measured v1.3.1), grading a card intermittently failed with
             * "记录失败：ESP_ERR_NO_MEM" - the grade itself was applied, only the
             * persistence was refused, which reads as "the button is broken".
             *
             * So: fall back to writing inline. That is the slow path the async
             * design exists to avoid, but a rare slow write beats losing the
             * grade, and the write is what the user's tap was meant to cause.
             * The WARN stays because a failure here means the board is under
             * real internal-RAM pressure, which is worth seeing in the log.
             */
            SYS_LOGW(TAG, "no memory for the progress writer; saving inline");
            return word_save_progress();
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

/* Defined after word_cloud_refresh(), which calls it. */
esp_err_t word_cloud_export(bool force);

/*
 * ## Why word_service reacts to the sync event itself (v1.4.0)
 *
 * Until 1.4.0 the only caller of word_cloud_refresh() was page_word.c, on
 * MAIMEMO_EVENT_SYNC_DONE. That made building the queue depend on the user
 * *having the word page open*: syncing from the web console (or on boot, before
 * anyone navigates) fetched the plan and updated the progress numbers, but never
 * populated a single card - so the device kept serving the local book, and the
 * user's report ("单词背诵里面没有墨墨单词") reproduced even with a working token
 * and a successful sync. Verified on the real board: `sync` returned
 * `{"started":true}`, `study progress: 0/55` appeared in the log, and
 * `GET /words/today` still answered with local CET-6 words.
 *
 * Subscribing here means the queue is rebuilt wherever the sync came from: boot,
 * the console's 立即同步 button, or the UI. The dependency direction is
 * unchanged (word_service already calls maimemo_*), so no new debt is added.
 */
static void on_maimemo_sync_done(void *arg, esp_event_base_t base,
                                 int32_t event_id, void *data);

/**
 * Worker that builds the cloud queue off the event task.
 *
 * The sync event is delivered on the shared sys_evt task, whose stack is small
 * (AGENT.md 9.0.1), and word_cloud_refresh() does blocking TLS plus SD-card
 * writes - so it must not run there. One worker per completed sync, which is
 * naturally rate-limited by the sync itself (maimemo_sync_start() refuses to
 * start a second one while one is in flight).
 */
static void cloud_build_worker(void *arg)
{
    (void)arg;
    const esp_err_t err = word_cloud_refresh();
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "cloud queue refresh failed: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

static void on_maimemo_sync_done(void *arg, esp_event_base_t base,
                                 int32_t event_id, void *data)
{
    (void)arg;
    (void)base;
    (void)event_id;
    (void)data;

    if (!word_cloud_mode()) {
        return;
    }

    /* 6 KB: TLS handshake, the enrichment fetches this may trigger, and the
     * JSON work for words/maimemo.json. Internal RAM only (AGENT.md 9.0.1). */
    if (xTaskCreate(cloud_build_worker, "th_word_cq", 6144, NULL, 3, NULL) != pdPASS) {
        /* Not fatal: the local book still serves cards, and the next sync (or a
         * page visit) retries. Logged because it means the queue silently did not
         * get built, which is exactly the symptom this path exists to fix. */
        SYS_LOGW(TAG, "cannot start the cloud queue worker; using the local book");
    }
}

/**
 * Fill in the display content for one cloud word.
 *
 * Order matters and is the heart of the v1.4.0 change:
 *
 *   1. **Local book by spelling** - if the word is in the CET-6 book we already
 *      have a curated phonetic/meaning/example, and using it costs no network
 *      call and no cloud quota. This covers the words the account and the book
 *      share.
 *   2. **MaiMemo** - only for the leftovers ("云端独占词"). `maimemo_enrich()`
 *      reads 释义 from `/interpretations` and 助记/例句 from `/notes` and
 *      `/phrases`, caching everything on the SD card, so this is paid once per
 *      word rather than once per card view.
 *
 * A word with neither still gets a card - it just shows the spelling alone,
 * which is now the exception instead of the rule.
 */
/*
 * ## Why content is filled lazily (v1.4.0, after a real regression)
 *
 * The obvious implementation is to enrich every cloud word while building the
 * queue. That was the first version of this code and it **took the board down**:
 * with 64 words, each needing `GET /vocabulary` plus two or three more TLS
 * round trips, the pass ran for minutes, and because every `maimemo_enrich()`
 * ends with a *whole-file* rewrite of `enrich.json`, the repeated allocations
 * fragmented internal RAM until `internal largest` read **160 bytes**. The web
 * server stopped answering; only a reboot recovered it.
 *
 * So the queue build now does **no network I/O at all** for content:
 *
 *   - a word in the local book (the common case - measured 12 of 20 on a live
 *     account, and 10 of 10 in the queue the device actually serves) is complete
 *     the moment it is built;
 *   - a cloud-only word starts as spelling-only, and its meaning/example are
 *     filled in **when the user actually flips to that card** (page_word's
 *     existing per-card enrich worker), then cached on the SD card so the second
 *     view is offline.
 *
 * That keeps the cost proportional to what the user looks at instead of what the
 * account contains, which is the same reasoning behind the per-card fetch that
 * was already there.
 */
static void cloud_fill_content(word_item_t *dst, const maimemo_item_t *src,
                               uint16_t local_index)
{
    memset(dst, 0, sizeof(*dst));
    strlcpy(dst->spelling, src->spelling, sizeof(dst->spelling));
    dst->id = WORD_ID_CLOUD;

    if (local_index == UINT16_MAX) {
        /* Cloud-only: the card will fetch its own content when shown. */
        return;
    }

    const word_item_t *local = &s_words[local_index];
    strlcpy(dst->phonetic, local->phonetic, sizeof(dst->phonetic));
    strlcpy(dst->meaning, local->meaning, sizeof(dst->meaning));
    strlcpy(dst->example, local->example, sizeof(dst->example));
    dst->level = s_progress[local_index].level;
    dst->next_review_ts = s_progress[local_index].next_review_ts;
    /* Keep the local id so grading updates the existing record. */
    dst->id = local->id;
}

/**
 * Look up already-cached content for a cloud-only card. **No network.**
 *
 * This is the read half of the lazy scheme: it serves whatever a previous view
 * (or a previous session) already cached, so a card that has been seen once
 * shows its meaning immediately and offline.
 */
static void cloud_apply_cached_content(word_item_t *card)
{
    maimemo_enrich_t enrich;
    if (maimemo_enrich_cached(card->spelling, &enrich) != ESP_OK) {
        return;
    }
    if (card->phonetic[0] == '\0') {
        /* The API has no phonetic for cloud words; leave it empty rather than
         * inventing one - the card simply shows the spelling, which is what a
         * flashcard front is for. */
        (void)0;
    }
    strlcpy(card->meaning, enrich.meaning, sizeof(card->meaning));
    strlcpy(card->example, enrich.example, sizeof(card->example));
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
    /* The resolved cards: full content, so the queue is display-ready. */
    word_item_t *cards = heap_caps_calloc_prefer(MAIMEMO_MAX_TODAY, sizeof(word_item_t), 2,
                                                 MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (items == NULL || map == NULL || cards == NULL) {
        heap_caps_free(items);
        heap_caps_free(map);
        heap_caps_free(cards);
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    const esp_err_t err = maimemo_fetch_today(items, MAIMEMO_MAX_TODAY, &count);
    if (err != ESP_OK) {
        heap_caps_free(items);
        heap_caps_free(map);
        heap_caps_free(cards);
        return err;
    }

    size_t mapped = 0;
    size_t with_content = 0;
    for (size_t i = 0; i < count; i++) {
        /* local_index_for() reads s_words/s_count, which only change on a reload
         * (not concurrently with a sync), so this needs no lock. */
        map[i] = local_index_for(items[i].spelling);
        if (map[i] != UINT16_MAX) {
            mapped++;
        }
        const uint16_t local = map[i];
        cloud_fill_content(&cards[i], &items[i], local);
        /*
         * Cloud-only cards: attach anything a previous view already cached. This
         * is a pure PSRAM read (maimemo_enrich_cached never does I/O), so it is
         * cheap enough to do for the whole queue - unlike the network fetch it
         * replaces. It is what makes a word seen yesterday show its meaning
         * today without re-asking.
         */
        if (local == UINT16_MAX) {
            cloud_apply_cached_content(&cards[i]);
        }
        if (cards[i].meaning[0] != '\0') {
            with_content++;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    free_cloud_queue();
    s_cloud_words = cards;
    s_cloud_map = map;
    s_cloud_count = count;
    s_cloud_active = (count > 0);
    /* The local index queue is replaced by the cloud queue, so clear it. */
    free(s_queue);
    s_queue = NULL;
    s_queue_len = 0;
    s_queue_pos = 0;
    xSemaphoreGive(s_lock);

    heap_caps_free(items);   /* the cards now own the content */

    SYS_LOGI(TAG, "cloud queue: %u words (%u from the local book, %u with a meaning)",
             (unsigned)count, (unsigned)mapped, (unsigned)with_content);

    /* Keep a copy on the card so the queue survives a reboot and works offline,
     * and so the console can see what is being studied. Best effort: a storage
     * failure must not fail the sync. */
    if (count > 0) {
        const esp_err_t werr = word_cloud_export(false);
        if (werr != ESP_OK) {
            SYS_LOGD(TAG, "memo wordbook not written: %s", esp_err_to_name(werr));
        }
    }
    return (count > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t word_cloud_export(bool force)
{
    (void)force;
    if (!storage_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "name", "墨墨今日词表（设备同步）");
    cJSON_AddNumberToObject(root, "version", 1);

    cJSON *words = cJSON_AddArrayToObject(root, "words");
    if (words == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t count = s_cloud_count;
    for (size_t i = 0; i < count; i++) {
        const word_item_t *card = &s_cloud_words[i];
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            continue;
        }
        /* id is per-card: a cloud-only word has no local id, so the queue index
         * is used, which is stable for the day's list. */
        cJSON_AddNumberToObject(entry, "id", (i + 1));
        cJSON_AddStringToObject(entry, "spelling", card->spelling);
        if (card->phonetic[0] != '\0') {
            cJSON_AddStringToObject(entry, "phonetic", card->phonetic);
        }
        if (card->meaning[0] != '\0') {
            cJSON_AddStringToObject(entry, "meaning", card->meaning);
        }
        if (card->example[0] != '\0') {
            cJSON_AddStringToObject(entry, "example", card->example);
        }
        cJSON_AddItemToArray(words, entry);
    }
    xSemaphoreGive(s_lock);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    (void)storage_mkdir_p(WORD_BOOK_DIR);
    const esp_err_t err = storage_write_file(WORD_MAIMEMO_BOOK_FILE, text, strlen(text));
    const size_t len = strlen(text);
    free(text);

    if (err == ESP_OK) {
        SYS_LOGI(TAG, "memo wordbook written: %u cards, %u B", (unsigned)count,
                 (unsigned)len);
    }
    return err;
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

size_t word_count(void)
{
    return s_count;
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

    /*
     * Cloud queue: the entries already carry full content (see cloud_fill_content),
     * so this is a straight copy - the old code rebuilt each card from the local
     * book here, which is exactly why a word outside the book came out blank.
     */
    if (s_cloud_active) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const size_t total = s_cloud_count;
        size_t copy = (total > max) ? max : total;
        if (out != NULL) {
            for (size_t i = 0; i < copy; i++) {
                out[i] = s_cloud_words[i];
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

    /* Cloud queue first (plan A): it is the account's plan, so it wins. The
     * entry is already a fully populated card - including the cloud-only words,
     * which is the v1.4.0 fix. */
    if (s_cloud_active) {
        if (s_queue_pos >= s_cloud_count) {
            s_queue_pos = 0;   /* wrap around so the page can keep flipping */
        }
        const size_t cloud_index = s_queue_pos++;
        *out = s_cloud_words[cloud_index];
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

    /*
     * A card that came from the cloud plan but is not in the local book. There
     * is no local scheduling record, so grading it means "tell the account" -
     * which, per maimemo_client.h, the Open API cannot actually accept as a
     * grade. What we *can* do honestly is record it in the study history, so the
     * console shows the user studied it here; the caller's uplink path is what
     * puts it back into the cloud plan. The old code only logged, which is why a
     * graded cloud card left no trace anywhere.
     *
     * The spelling is copied out while the lock is held (s_cloud_words is
     * replaced wholesale by a sync, so a pointer taken here could dangle).
     */
    if (index == (size_t)-1) {
        char cloud_spelling[WORD_SPELLING_MAX] = { 0 };
        const bool was_cloud_only = (s_cloud_active && id == WORD_ID_CLOUD);
        if (was_cloud_only) {
            for (size_t i = 0; i < s_cloud_count; i++) {
                if (s_cloud_words[i].id == WORD_ID_CLOUD) {
                    strlcpy(cloud_spelling, s_cloud_words[i].spelling,
                            sizeof(cloud_spelling));
                    break;
                }
            }
        }
        xSemaphoreGive(s_lock);

        if (was_cloud_only) {
            SYS_LOGD(TAG, "graded a cloud-only card '%s' (%s): history only, no local record",
                     cloud_spelling, word_review_name(result));
            (void)word_history_append(WORD_ID_CLOUD, cloud_spelling, result, true);
            (void)sys_event_post_to(WORD_EVENT, WORD_EVENT_REVIEWED, &id, sizeof(id));
        }
        return ESP_ERR_NOT_FOUND;
    }

    schedule(&s_progress[index], result, now);
    const int64_t next = s_progress[index].next_review_ts;
    const uint8_t level = s_progress[index].level;
    char spelling[WORD_SPELLING_MAX];
    strlcpy(spelling, s_words[index].spelling, sizeof(spelling));
    /* A word matched back to the book may still have arrived through the cloud
     * queue; the history marks that so the console can tell the two apart. */
    const bool from_cloud = s_cloud_active;
    xSemaphoreGive(s_lock);

    SYS_LOGD(TAG, "word #%lu graded '%s' -> level %u, next in %lld s",
             (unsigned long)id, word_review_name(result), level,
             (long long)(next - now));

    /* Study history for the console: progress.json only keeps the latest state
     * per word, so it cannot answer "what did I study today". */
    (void)word_history_append(id, spelling, result, from_cloud);

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
/* Study history (v1.4.0)                                                    */
/* ------------------------------------------------------------------------ */

/*
 * `words/review_log.jsonl`: one JSON object per graded card, appended.
 *
 * ## Why JSONL and not a JSON array
 *
 * The file grows forever and is only ever appended to, so rewriting it (as a
 * JSON array would require) costs O(n) flash writes per grade - on a card where
 * a single FAT write is already the slowest thing the word page does. Appending
 * one line is O(1), and a truncated final line (power lost mid-write) is
 * simply skipped by the reader instead of corrupting the whole file.
 *
 * It is also the artefact behind "上传设备端学习情况": MaiMemo's Open API has no
 * endpoint that accepts a review grade, so this log is the record. The web
 * console reads it through GET /api/v1/words/history.
 */
#define WORD_HISTORY_FILE "words/review_log.jsonl"
/** Size past which the log is reported as worth clearing (see the flush path). */
#define WORD_HISTORY_MAX_READ (128 * 1024)

static bool s_history_full;

/*
 * ## Why appends are queued instead of written inline
 *
 * `word_history_append()` is reached from `word_review()`, i.e. from an **LVGL
 * widget callback**, and those run with the adapter lock held (AGENT.md
 * §9.0.1.1). `storage_append_file()` is a synchronous fopen/fwrite/fclose - a
 * FAT write - so doing it inline would stall rendering for the duration of a
 * card write on every single grade. That is the same mistake `word_review` made
 * before `word_save_progress_async()` existed, and the fix is the same shape:
 * build the line (cheap, in RAM) and let the existing debounced save worker
 * drain the queue.
 *
 * The queue is a small fixed ring: a study session cannot allocate without
 * bound, and if it ever overflows the oldest line is dropped (the newest grades
 * are the ones worth keeping).
 */
#define WORD_HISTORY_QUEUE 16
#define WORD_HISTORY_LINE_MAX 192

static char s_history_queue[WORD_HISTORY_QUEUE][WORD_HISTORY_LINE_MAX];
static size_t s_history_head;   /**< next slot to write */
static size_t s_history_len;    /**< entries currently queued */
static StaticSemaphore_t s_history_lock_buf;
static SemaphoreHandle_t s_history_lock;

/** Format one JSONL line into @p out. Returns the length, or 0 on failure. */
static size_t history_format_line(char *out, size_t len, uint32_t id,
                                  const char *spelling, word_review_result_t result,
                                  bool cloud)
{
    /* Built by hand rather than with cJSON: this runs in an LVGL callback, and
     * cJSON would allocate several nodes per card for a five-field line. The
     * spelling is the only string, and it is a single ASCII word (see the
     * book format), so no escaping is needed - but it is length-clamped anyway
     * so a corrupt book cannot overflow the queue slot. */
    char safe[WORD_SPELLING_MAX];
    strlcpy(safe, spelling, sizeof(safe));
    for (char *p = safe; *p != '\0'; p++) {
        /* Defensive: keep the line parseable even if a book contains a quote or
         * a backslash. Innocuous for real spellings. */
        if (*p == '"' || *p == '\\') {
            *p = '_';
        }
    }

    const int written = snprintf(out, len, "{\"id\":%lu,\"w\":\"%s\",\"r\":\"%s\",\"t\":%lld%s}\n",
                                 (unsigned long)id, safe, word_review_name(result),
                                 (long long)now_stamp(), cloud ? ",\"cloud\":true" : "");
    if (written <= 0 || (size_t)written >= len) {
        return 0;
    }
    return (size_t)written;
}

esp_err_t word_history_append(uint32_t id, const char *spelling,
                              word_review_result_t result, bool cloud)
{
    if (spelling == NULL || spelling[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_history_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char line[WORD_HISTORY_LINE_MAX];
    const size_t len = history_format_line(line, sizeof(line), id, spelling, result, cloud);
    if (len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_history_lock, portMAX_DELAY);
    if (s_history_len == WORD_HISTORY_QUEUE) {
        /* Full: drop the oldest so the newest grades survive. A session that
         * reaches this has been grading faster than the card can be written for
         * many seconds, which is already pathological. */
        s_history_head = (s_history_head + 1) % WORD_HISTORY_QUEUE;
        s_history_len--;
    }
    const size_t slot = (s_history_head + s_history_len) % WORD_HISTORY_QUEUE;
    memcpy(s_history_queue[slot], line, len);
    s_history_queue[slot][len] = '\0';
    s_history_len++;
    xSemaphoreGive(s_history_lock);

    /* Wake the (already existing) writer. Never creates a task here: this is an
     * LVGL callback and the board only has ~13 KB of contiguous internal heap
     * (AGENT.md §9.0.7 ③). */
    return word_save_progress_async();
}

/** Drain queued history lines onto the card. Worker context. */
void word_history_flush_pending(void)
{
    if (s_history_lock == NULL || !storage_available()) {
        return;
    }

    for (;;) {
        char line[WORD_HISTORY_LINE_MAX];
        size_t len = 0;

        xSemaphoreTake(s_history_lock, portMAX_DELAY);
        if (s_history_len > 0) {
            strlcpy(line, s_history_queue[s_history_head], sizeof(line));
            s_history_head = (s_history_head + 1) % WORD_HISTORY_QUEUE;
            s_history_len--;
            len = strlen(line);
        }
        xSemaphoreGive(s_history_lock);

        if (len == 0) {
            return;   /* queue drained */
        }

        const esp_err_t err = storage_append_file(WORD_HISTORY_FILE, line, len);
        if (err != ESP_OK) {
            SYS_LOGW(TAG, "cannot append to the study history: %s", esp_err_to_name(err));
            return;   /* keep the rest queued for the next wake */
        }

        /*
         * Size check, memory-only and at most once per session.
         *
         * Compaction would mean reading and rewriting the whole file, which must
         * not happen on a grading path; the cap is therefore recorded and the
         * file left alone, with the console able to clear it explicitly
         * (POST /api/v1/words/history). Silent unbounded growth would be worse
         * than a stale log, so it is at least logged once.
         */
        if (!s_history_full) {
            size_t size = 0;
            if (storage_exists(WORD_HISTORY_FILE, &size) == ESP_OK &&
                size > (size_t)WORD_HISTORY_MAX_READ) {
                s_history_full = true;
                SYS_LOGW(TAG, "study history is %u B; clear it from the web console "
                              "to reclaim space", (unsigned)size);
            }
        }
    }
}


esp_err_t word_history_read(word_review_log_entry_t *out, size_t max,
                            size_t *count, bool today_only)
{
    if (out == NULL || count == NULL || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    if (!storage_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Write out anything still queued before reading.
     *
     * Appends are deferred to the save worker (see word_history_append), so a
     * card graded seconds ago may still be in RAM. A reader that skipped this
     * would show a stale history and look broken precisely when the user is
     * checking that grading works. This runs on the HTTP worker, not on LVGL.
     */
    word_history_flush_pending();

    char *text = NULL;
    size_t len = 0;
    if (storage_read_text(WORD_HISTORY_FILE, &text, &len) != ESP_OK || text == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (len == 0) {
        free(text);
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * Only the tail is wanted (newest entries), and the file is read whole
     * because it is bounded by WORD_HISTORY_MAX_READ in practice. Walking it
     * twice would double the parsing for no gain.
     */
    const int64_t day_start = today_only ? day_start_stamp() : 0;
    size_t written = 0;

    char *cursor = text;
    while (cursor != NULL && *cursor != '\0' && written < max) {
        char *newline = strchr(cursor, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }

        cJSON *entry = cJSON_Parse(cursor);
        if (entry != NULL) {   /* a torn tail simply fails to parse and is skipped */
            word_review_log_entry_t parsed = { 0 };
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(entry, "id");
            const cJSON *w = cJSON_GetObjectItemCaseSensitive(entry, "w");
            const cJSON *r = cJSON_GetObjectItemCaseSensitive(entry, "r");
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(entry, "t");
            const cJSON *cloud = cJSON_GetObjectItemCaseSensitive(entry, "cloud");

            if (cJSON_IsNumber(id)) {
                parsed.id = (uint32_t)id->valuedouble;
            }
            if (cJSON_IsString(w)) {
                strlcpy(parsed.spelling, w->valuestring, sizeof(parsed.spelling));
            }
            if (cJSON_IsString(r)) {
                word_review_result_t grade = WORD_REVIEW_UNKNOWN;
                if (word_review_from_name(r->valuestring, &grade)) {
                    parsed.result = (uint8_t)grade;
                }
            }
            if (cJSON_IsNumber(t)) {
                parsed.at = (int64_t)t->valuedouble;
            }
            parsed.cloud = cJSON_IsBool(cloud) && cJSON_IsTrue(cloud);

            const bool keep = (parsed.spelling[0] != '\0') &&
                              (!today_only || parsed.at >= day_start);
            if (keep) {
                out[written++] = parsed;
            }
            cJSON_Delete(entry);
        }

        if (newline == NULL) {
            break;
        }
        cursor = newline + 1;
    }

    free(text);
    *count = written;
    return (written > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t word_history_clear(void)
{
    s_history_full = false;

    /* Drop anything still queued, or the save worker would write it back into
     * the file this call is about to delete. */
    if (s_history_lock != NULL) {
        xSemaphoreTake(s_history_lock, portMAX_DELAY);
        s_history_head = 0;
        s_history_len = 0;
        xSemaphoreGive(s_history_lock);
    }
    return storage_delete_file(WORD_HISTORY_FILE);
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

    /* The study-history queue needs its own lock: word_history_append() is
     * called from LVGL callbacks while the save worker drains it. */
    s_history_lock = xSemaphoreCreateMutexStatic(&s_history_lock_buf);
    if (s_history_lock == NULL) {
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

    /*
     * Build the cloud queue on every completed MaiMemo sync, whoever started it
     * (boot, the web console, or the UI). Registered once and never removed:
     * word_service lives for the lifetime of the firmware. Failure is not fatal -
     * the local book still works, which is the documented offline fallback.
     *
     * **sys_event_subscribe(), not esp_event_handler_register().** The project
     * posts through a *custom* event loop (`sys_event_post_to` ->
     * `esp_event_post_to(s_loop, ...)`), so registering on the ESP-IDF default
     * loop silently never fires. That cost a full debug round on the board: the
     * sync completed (`study progress: 0/55` in the log) and the handler simply
     * never ran, with no error anywhere - this helper is the only correct way in.
     */
    const esp_err_t sub_err = sys_event_subscribe(
        MAIMEMO_EVENT, MAIMEMO_EVENT_SYNC_DONE, on_maimemo_sync_done, NULL, NULL);
    if (sub_err != ESP_OK) {
        SYS_LOGW(TAG, "cannot subscribe to MaiMemo sync events: %s",
                 esp_err_to_name(sub_err));
    }

    SYS_LOGI(TAG, "ready (book '%s', %u words, %u/day)", s_book, (unsigned)s_count,
             (unsigned)s_daily_count);
    return ESP_OK;
}
