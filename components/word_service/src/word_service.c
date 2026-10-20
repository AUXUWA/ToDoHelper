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
#include "freertos/task.h"

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

/*
 * ## The card currently on screen (v1.5.0 fix for the wrong spelling in the log)
 *
 * Every cloud-only card carries the **same** id, WORD_ID_CLOUD, so the id
 * cannot say which word the user just graded. `word_review()` used to answer
 * that question by scanning the queue for "the first entry whose id is
 * WORD_ID_CLOUD" - which is always the *first* cloud word of the day, never
 * the card that was on screen. Every line of `review_log.jsonl` therefore
 * carried the wrong spelling (roadmap B5).
 *
 * So the spelling of the card handed out by word_next() is remembered here and
 * handed to the history writer explicitly. The copy is taken while the lock is
 * held, because a sync replaces the whole cloud queue.
 */
static char s_current_spelling[WORD_SPELLING_MAX];
static uint32_t s_current_id;

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

/* ------------------------------------------------------------------------ */
/* Book loading state (v1.7.0 A2)                                            */
/* ------------------------------------------------------------------------ */

/*
 * ## Why the book is loaded in the background (v1.7.0, roadmap A2)
 *
 * `word_service_init()` used to read the 120 KB `words/cet6.json` and parse its
 * 833 words **synchronously on the main task**: 343 ms measured on the board
 * (build/after_ui.log, 6333 -> 6412 ms). At that moment the only thing on screen
 * is the splash log and nobody can ask for a word yet, so those 343 ms were
 * pure boot latency - and the roadmap's target is a boot under 5.0 s.
 *
 * The load now happens in a worker task and the public interfaces say
 * "not ready yet" until it is done, instead of handing out an empty list.
 *
 * The three states must be distinguishable, because the UI treats them
 * differently:
 *
 *   WORD_BOOK_LOADING  still loading - the page shows 「词库加载中…」;
 *   WORD_BOOK_READY    the book is there;
 *   WORD_BOOK_FAILED   the load failed - the page has to show the error.
 *
 * `word_book_state()` answers without listening to events; the events
 * (WORD_EVENT_BOOK_LOADING / _LOADED / _LOAD_FAILED) are the push variant.
 */
static word_book_state_t s_book_state;
static esp_err_t s_book_error;
/** Cached cloud-mode switch: an event handler must not read NVS. */
static bool s_cloud_mode_selected;

/* Defined with the book-loading section below; the save path uses them too. */
static esp_err_t book_guard(void);
static bool book_settled(void);

/* ------------------------------------------------------------------------ */
/* Cloud queue bookkeeping (v1.7.0 C1)                                       */
/* ------------------------------------------------------------------------ */

/*
 * A request counter rather than a flag: the queue can be asked for from three
 * places (a finished sync, the station got an IP, the IP was already there at
 * boot). One worker must serve those requests with a single round, but a
 * request that arrives *while* it runs must not be lost.
 *
 * The variables are only touched through the __atomic builtins, which provide
 * the ordering - no volatile needed.
 */
static uint32_t s_cloud_requests;
/** Whether a worker is already running (never two TLS tasks at once). */
static bool s_cloud_worker_running;

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

        esp_err_t err = word_save_progress();
        if (err == ESP_ERR_NOT_FINISHED) {
            /*
             * The wordbook is still being parsed in the background (v1.7.0), and
             * writing now would overwrite the progress on the card with an
             * empty list. Wait for the loader - it is a fraction of a second -
             * and try again.
             */
            for (int i = 0; i < 10 && err == ESP_ERR_NOT_FINISHED; i++) {
                vTaskDelay(pdMS_TO_TICKS(200));
                err = word_save_progress();
            }
        }
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
    heap_caps_free(s_queue);   /* PSRAM (see build_queue) */
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
    /*
     * sys_json_parse(), not cJSON_Parse(): the book is a file the user can
     * edit on the card, and cJSON recurses once per nesting level (~96 bytes
     * of stack per level, up to 1000 levels). A malformed book would therefore
     * blow the stack of whoever parses it - on the boot path that means the
     * device reboots in a loop (roadmap B3). The depth limit turns that into a
     * clean "cannot read the book".
     */
    cJSON *root = sys_json_parse(json, 32);
    if (root == NULL) {
        SYS_LOGW(TAG, "the wordbook is not valid JSON (or nests too deeply)");
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
    if (!book_settled()) {
        /*
         * The book is still being parsed in the background (v1.7.0). Writing
         * now would serialise an **empty** word list and overwrite the
         * progress that is on the card - and the loader restores the progress
         * right after the parse, so the old file is the truth. The honest
         * answer is "not now"; the save worker waits and retries.
         */
        return ESP_ERR_NOT_FINISHED;
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

    cJSON *root = sys_json_parse(json, 32);
    free(json);
    if (root == NULL) {
        SYS_LOGW(TAG, "the saved progress is not valid JSON; starting fresh");
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

/**
 * Record the new book state.
 *
 * The events are posted by the loading code itself (WORD_EVENT_LOADED /
 * _LOAD_FAILED are already part of the published contract), so this only
 * remembers the state; the "loading started" event is posted by the caller
 * that starts the load.
 */
static void book_set_state(word_book_state_t state, esp_err_t err)
{
    __atomic_store_n(&s_book_error, err, __ATOMIC_RELEASE);
    __atomic_store_n(&s_book_state, state, __ATOMIC_RELEASE);
}

/**
 * Is the book usable?
 *
 * @return ESP_OK when it is ready, ESP_ERR_NOT_FINISHED while the background
 *         loader is still running, ESP_ERR_INVALID_STATE when the load failed.
 *
 * "still loading" and "failed" are different user-facing situations, so they
 * must not collapse into one error code: the first one is a normal state the UI
 * shows as 「词库加载中…」, the second one is a real failure.
 */
static esp_err_t book_guard(void)
{
    switch (__atomic_load_n(&s_book_state, __ATOMIC_ACQUIRE)) {
    case WORD_BOOK_READY:
        return ESP_OK;
    case WORD_BOOK_LOADING:
        return ESP_ERR_NOT_FINISHED;
    case WORD_BOOK_FAILED:
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

/** Has the background loader finished (either way)? */
static bool book_settled(void)
{
    return __atomic_load_n(&s_book_state, __ATOMIC_ACQUIRE) != WORD_BOOK_LOADING;
}

/*
 * The loader's stack.
 *
 * 4 KB is the smallest stack that still leaves room for the SD-card read and
 * the cJSON parse of the book (the parse itself is bounded by the depth guard
 * in sys_json_parse(), so the recursion is shallow - the 7000 cJSON nodes live
 * in the heap, not on the stack).
 *
 * The task prints its stack high water mark when it is done, so "is 4 KB
 * enough?" is answered by the log of the next boot instead of by a guess.
 * See the README for the exact threshold.
 */
#define WORD_BOOK_TASK_STACK 4096

/*
 * The loader runs at the **same priority as the main task** (1), and that is
 * the whole point of moving the load off the boot path:
 *
 *   - a *higher* priority would preempt the main task for the whole parse, so
 *     the boot would still wait the same 343 ms - only the bill would change;
 *   - the same priority means the two share the core: the main task finishes
 *     its remaining init while the loader grinds in the background, and the
 *     loader even gets the main task's blocking waits (NVS, SD, network) for
 *     free.
 *
 * The UI is not affected: the LVGL task runs at priority 6 (see
 * esp_lv_adapter.h), far above this one.
 */
#define WORD_BOOK_TASK_PRIO  1

/**
 * Load the book in the background.
 *
 * Runs on its own task so the boot path does not pay the ~343 ms of the SD-card
 * read plus the JSON parse. The result is announced through the events.
 */
static void word_book_worker(void *arg)
{
    (void)arg;

    const esp_err_t err = word_reload();
    if (err != ESP_OK) {
        /* The failure was already logged and posted as WORD_EVENT_LOAD_FAILED. */
        SYS_LOGE(TAG, "the wordbook could not be loaded: %s", esp_err_to_name(err));
    } else {
        SYS_LOGI(TAG, "the wordbook is ready");
    }

    /*
     * Stack high water mark: how many bytes of the 4 KB were *never* touched.
     * The number is what decides whether the stack can be shrunk (or has to
     * grow) - see the README.
     */
    const UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    SYS_LOGI(TAG, "book loader: %u stack item(s) (~%u B) never used",
             (unsigned)hwm, (unsigned)(hwm * sizeof(StackType_t)));

    vTaskDelete(NULL);
}

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
        book_set_state(WORD_BOOK_FAILED, err);
        (void)sys_event_post_to(WORD_EVENT, WORD_EVENT_LOAD_FAILED, &err, sizeof(err));
        return err;
    }

    err = parse_book(json);
    free(json);
    if (err != ESP_OK) {
        book_set_state(WORD_BOOK_FAILED, err);
        (void)sys_event_post_to(WORD_EVENT, WORD_EVENT_LOAD_FAILED, &err, sizeof(err));
        return err;
    }

    s_queue_pos = 0;
    s_queue_len = 0;

    const size_t count = s_count;
    SYS_LOGI(TAG, "loaded %u words from '%s' (%u B)", (unsigned)count, relative, (unsigned)len);
    book_set_state(WORD_BOOK_READY, ESP_OK);
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
        book_set_state(WORD_BOOK_FAILED, err);
        (void)sys_event_post_to(WORD_EVENT, WORD_EVENT_LOAD_FAILED, &err, sizeof(err));
        return err;
    }
    s_queue_pos = 0;
    s_queue_len = 0;
    (void)load_progress();
    const size_t count = s_count;
    book_set_state(WORD_BOOK_READY, ESP_OK);
    (void)sys_event_post_to(WORD_EVENT, WORD_EVENT_LOADED, &count, sizeof(count));
    return ESP_OK;
}

const char *word_get_book(void)
{
    return s_book;
}

word_book_state_t word_book_state(void)
{
    return __atomic_load_n(&s_book_state, __ATOMIC_ACQUIRE);
}

esp_err_t word_book_error(void)
{
    return __atomic_load_n(&s_book_error, __ATOMIC_ACQUIRE);
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

/**
 * The mode switch alone, without the token check.
 *
 * `word_cloud_mode()` reads NVS twice (the mode and the token), which is fine
 * for a UI call but not for an event handler: the handlers only need to know
 * whether the user wants the cloud book at all, and the token is checked in the
 * worker (where a blocking NVS read is fine).
 */
static bool cloud_mode_selected(void)
{
    char mode[16];
    if (sys_config_get_str(SYS_KEY_MAIMEMO_MODE, mode, sizeof(mode)) != ESP_OK) {
        return false;
    }
    return strcmp(mode, "cloud") == 0;
}

esp_err_t word_set_cloud_mode(bool enabled)
{
    const esp_err_t err = sys_config_set_str(SYS_KEY_MAIMEMO_MODE, enabled ? "cloud" : "local");
    if (err == ESP_OK) {
        /* Keep the cache in step with the stored setting. */
        s_cloud_mode_selected = enabled;
        if (enabled) {
            /*
             * The user just asked for the cloud book: that is an explicit
             * action, and it must not be swallowed by a retry backoff window
             * that was opened by an earlier transport failure.
             */
            maimemo_allow_request_now();
        }
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

/*
 * ## Why the cloud queue waits for the IP (v1.7.0, roadmap C1)
 *
 * The boot log of the real board (build/after_ui.log) reads:
 *
 *     I (6546) TH:mm: sync started
 *     E (6018) esp-tls: couldn't get hostname for ...: getaddrinfo() returns 202
 *     I (7032) TH:wifi: got ip 192.168.137.95
 *
 * The cloud queue was built **before** the DHCP lease existed, so the DNS lookup
 * could not even start: one useless DNS query plus one useless TLS attempt on
 * every boot.
 *
 * Two things changed:
 *
 *   1. the builder waits for an address before it touches the network, and
 *   2. the trigger is "the address arrived" instead of "the boot happened".
 *
 * The address itself is read from `sys_status`, the only cross-component state
 * channel this project allows (see sys_status.h): wifi_manager publishes the
 * address there *before* it posts its own events, so it is already there when
 * the event arrives.
 */
/*
 * 6 KB, **not** 4 KB.
 *
 * v1.7.0 tried 4 KB to save internal RAM (a FreeRTOS stack can only come from
 * internal RAM) and the board proved it wrong:
 *
 *     ***ERROR*** A stack overflow in task th_word_cq has been detected.
 *
 * The worker does a full TLS handshake with open.maimemo.com, and esp-tls
 * needs more than 4 KB of task stack for that. The old 6 KB value is the one
 * that has actually run on the board, so it goes back.
 *
 * The worker prints its stack high water mark when it is done, so "is 6 KB
 * enough?" is answered by the log of the next boot instead of by a guess -
 * see the README for the exact threshold.
 */
#define WORD_CLOUD_TASK_STACK 6144
#define WORD_CLOUD_TASK_PRIO  3
/** How long the builder waits for an IP (and for the local book) at most. */
#define WORD_CLOUD_WAIT_MS 30000
/** Poll period while waiting. */
#define WORD_CLOUD_POLL_MS 1000

/**
 * Does the station have an IPv4 address?
 *
 * `sys_status` is the sanctioned channel for it: wifi_manager publishes the
 * address there before it posts its events, so the value is already current
 * when the event handler runs.
 */
static bool have_ip(void)
{
    sys_status_t status;
    if (sys_status_get(&status) != ESP_OK) {
        return false;
    }
    return status.wifi_ip[0] != '\0' && strcmp(status.wifi_ip, "0.0.0.0") != 0;
}

/**
 * Wait until the queue can actually be built.
 *
 * Two prerequisites: an address (otherwise the DNS lookup cannot even start)
 * and a settled local book (otherwise the cloud words lose the content the
 * local book provides - measured 12 of 20 words on a live account).
 *
 * @param timeout_ms How long to wait at most.
 * @return true when both are ready, false on timeout.
 */
static bool cloud_prerequisites_ready(uint32_t timeout_ms)
{
    const int64_t deadline = sys_time_ms() + (int64_t)timeout_ms;
    for (;;) {
        if (have_ip() && book_settled()) {
            return true;
        }
        if (sys_time_ms() >= deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(WORD_CLOUD_POLL_MS));
    }
}

/*
 * wifi_manager's event base.
 *
 * word_service deliberately does **not** depend on the wifi_manager component:
 * this round forbids new component dependencies, and an L3 -> L3 edge is what
 * the project avoids ("services publish their state to sys_status and everyone
 * else reads it", sys_status.h). So the single symbol that is needed is
 * declared here: `ESP_EVENT_DEFINE_BASE(WIFI_MGR_EVENT)` in wifi_manager.c
 * defines it as a global `const char *` whose value is "WIFI_MGR_EVENT".
 *
 * A wifi_manager that is not linked in is a **link error**, never a silent
 * misbehaviour - which is the failure mode we want.
 */
extern esp_event_base_t const WIFI_MGR_EVENT;

/** Whether the station had an address at the last wifi_manager event. */
static bool s_had_ip;

/* Forward declarations: the event handlers below start the worker. */
static void cloud_start_worker(const char *why);
static void cloud_request_build(const char *why);

/**
 * A wifi_manager event arrived.
 *
 * The subscription covers **every** event of that base rather than one
 * hard-coded id: the id would have to be copied out of wifi_manager.h into
 * this file (a silent breakage if the enum ever changes), while the thing we
 * actually care about - "is there an address now?" - is published by
 * wifi_manager in `sys_status` *before* it posts its events.
 *
 * So the trigger is the **transition**: the address appeared. A scan (which
 * also posts a WIFI_MGR event) therefore never starts a cloud request, and a
 * disconnect clears the flag again.
 */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)base;
    (void)event_id;
    (void)data;

    const bool now = have_ip();
    const bool had = s_had_ip;
    s_had_ip = now;

    if (!now || had) {
        return;   /* no address, or nothing changed since the last event */
    }
    if (!s_cloud_mode_selected) {
        return;   /* the user did not ask for the cloud book */
    }

    /*
     * The boot sync failed with "no address yet" (a transport failure), and the
     * maimemo client therefore has a retry backoff window open - inside it the
     * automatic paths do not touch the network at all. The address is here now,
     * so that window is obsolete: open the gate before asking for the queue,
     * otherwise the build would be refused for the rest of the window and the
     * device would keep serving the local book.
     */
    maimemo_allow_request_now();

    if (word_cloud_count() > 0) {
        return;   /* the queue is already there: do not spend a round trip */
    }
    cloud_request_build("the station got an IP");
}

/**
 * Ask for the cloud queue to be (re)built.
 *
 * The request is **counted**, not flagged: a request that arrives while a
 * worker is running must not be lost, it has to produce a second round.
 */
static void cloud_request_build(const char *why)
{
    __atomic_add_fetch(&s_cloud_requests, 1, __ATOMIC_ACQ_REL);
    SYS_LOGD(TAG, "cloud queue requested (%s)", why);
    cloud_start_worker(why);
}

/**
 * Worker that builds the cloud queue off the event task.
 *
 * The trigger events are delivered on the shared event task, whose stack is
 * small (AGENT.md 9.0.1), and word_cloud_refresh() does blocking TLS plus
 * SD-card writes - so it must not run there.
 *
 * The task deletes itself when it is done: it is a one-shot, and a leftover
 * 4 KB task stack is a hole in the internal heap (the thing roadmap B1 is
 * about).
 */
static void cloud_build_worker(void *arg)
{
    (void)arg;

    for (;;) {
        /* Consume the requests that piled up so far. */
        __atomic_store_n(&s_cloud_requests, 0, __ATOMIC_RELEASE);

        /*
         * Wait for the two things the queue needs: an address (otherwise the
         * DNS lookup cannot even start) and the local book (otherwise the
         * cloud words lose the content the local book provides).
         */
        if (!cloud_prerequisites_ready(WORD_CLOUD_WAIT_MS)) {
            /*
             * Nothing arrived within the timeout: build the queue anyway. The
             * old behaviour was "build it at boot, whatever the network does",
             * and a cloud queue that cannot be fetched must not leave the user
             * without cards - word_cloud_refresh() falls back to the local book
             * on every failure.
             */
            SYS_LOGW(TAG, "no IP (or no wordbook) after %d ms; building the cloud queue anyway",
                     (int)WORD_CLOUD_WAIT_MS);
        }

        if (!word_cloud_mode()) {
            /* The mode was switched off, or the token was removed. */
            break;
        }

        const esp_err_t err = word_cloud_refresh();
        if (err != ESP_OK) {
            SYS_LOGW(TAG, "cloud queue refresh failed: %s", esp_err_to_name(err));
        }

        /*
         * Stack high water mark: the number that says whether 4 KB is enough
         * for the TLS handshake (roadmap B1). See the README for the threshold.
         */
        const UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        SYS_LOGI(TAG, "cloud queue worker: %u stack item(s) (~%u B) never used",
                 (unsigned)hwm, (unsigned)(hwm * sizeof(StackType_t)));

        if (__atomic_load_n(&s_cloud_requests, __ATOMIC_ACQUIRE) == 0) {
            break;   /* nothing new came in while we were building */
        }
    }

    __atomic_store_n(&s_cloud_worker_running, false, __ATOMIC_RELEASE);
    if (__atomic_load_n(&s_cloud_requests, __ATOMIC_ACQUIRE) > 0) {
        /* A request arrived between the last check and the flag reset. */
        cloud_start_worker("a late request");
    }
    vTaskDelete(NULL);
}

static void cloud_start_worker(const char *why)
{
    if (__atomic_exchange_n(&s_cloud_worker_running, true, __ATOMIC_ACQ_REL)) {
        return;   /* one is already running; it will pick the request up */
    }

    /*
     * 4 KB (was 6 KB, roadmap B1): a FreeRTOS task stack can only come from
     * internal RAM, so every kilobyte here is a permanent hole in the scarcest
     * resource on this board. The task does a TLS handshake, so the number has
     * to be verified on hardware - the worker prints its stack high water
     * mark, which is the threshold (see the README).
     */
    if (xTaskCreate(cloud_build_worker, "th_word_cq", WORD_CLOUD_TASK_STACK, NULL,
                    WORD_CLOUD_TASK_PRIO, NULL) != pdPASS) {
        __atomic_store_n(&s_cloud_worker_running, false, __ATOMIC_RELEASE);
        /* Not fatal: the local book still serves cards, and the next sync (or a
         * page visit) retries. Logged because it means the queue silently did not
         * get built, which is exactly the symptom this path exists to fix. */
        SYS_LOGW(TAG, "cannot start the cloud queue worker (%s); using the local book", why);
    }
}

/**
 * A MaiMemo sync finished: rebuild the queue.
 *
 * A successful sync means the network worked, so the address is there and the
 * worker starts building right away.
 */
static void on_maimemo_sync_done(void *arg, esp_event_base_t base,
                                 int32_t event_id, void *data)
{
    (void)arg;
    (void)base;
    (void)event_id;
    (void)data;

    /* Refresh the cached flag: the mode may have been switched since boot. */
    s_cloud_mode_selected = cloud_mode_selected();

    if (!word_cloud_mode()) {
        return;
    }

    cloud_request_build("a MaiMemo sync finished");
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
    heap_caps_free(s_queue);   /* PSRAM (see build_queue) */
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

    /*
     * The queue is a word-index array, i.e. bulk device data: ask for PSRAM
     * explicitly (roadmap B3). With CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 a
     * plain malloc() of this size would land in the internal heap, and the
     * internal heap is the scarce resource on this board.
     */
    uint16_t *queue = heap_caps_calloc_prefer((size_t)s_daily_count * 2 + 1,
                                               sizeof(uint16_t), 2,
                                               MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
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

    heap_caps_free(s_queue);   /* PSRAM (see build_queue) */
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
     * The book may still be loading in the background (roadmap A2). Then the
     * answer is "not ready yet" - **not** an empty list, which the page would
     * render as "today there are no words" and the console as an empty array.
     *
     * The cloud queue is exempt: it does not come from the local book, so it
     * can serve cards even while the book is still being parsed.
     */
    if (!s_cloud_active) {
        const esp_err_t guard = book_guard();
        if (guard != ESP_OK) {
            return guard;
        }
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

    /* Not loaded yet: say so instead of handing out a stale/empty card. */
    if (!s_cloud_active) {
        const esp_err_t guard = book_guard();
        if (guard != ESP_OK) {
            return guard;
        }
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
        /* Remember which card is on screen: the cloud ids are all the same,
         * so word_review() cannot recover the spelling from the id. */
        strlcpy(s_current_spelling, out->spelling, sizeof(s_current_spelling));
        s_current_id = out->id;
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
    /* Remember which card is on screen (see s_current_spelling). */
    strlcpy(s_current_spelling, out->spelling, sizeof(s_current_spelling));
    s_current_id = out->id;
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
    /* The id belongs to the book, so the book has to be there. */
    const esp_err_t guard = book_guard();
    if (guard != ESP_OK) {
        return guard;
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
    /* The cloud queue has its own length, so it can answer before the local
     * book is loaded. */
    if (!s_cloud_active) {
        const esp_err_t guard = book_guard();
        if (guard != ESP_OK) {
            return guard;
        }
    }

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
     * The spelling is the one of the card that is **on screen** (s_current_*),
     * copied out while the lock is held (s_cloud_words is replaced wholesale by
     * a sync, so a pointer taken here could dangle). It used to be taken from
     * "the first cloud word in the queue", which is never the card the user
     * graded - all cloud ids are WORD_ID_CLOUD, so the log recorded the wrong
     * word every single time (roadmap B5).
     */
    if (index == (size_t)-1) {
        char cloud_spelling[WORD_SPELLING_MAX] = { 0 };
        const bool was_cloud_only = (s_cloud_active && id == WORD_ID_CLOUD);
        if (was_cloud_only && s_current_id == id) {
            strlcpy(cloud_spelling, s_current_spelling, sizeof(cloud_spelling));
        }
        xSemaphoreGive(s_lock);

        if (was_cloud_only) {
            if (cloud_spelling[0] == '\0') {
                /* No card on screen to take the spelling from: writing a
                 * guessed one would poison the log, so nothing is written. */
                SYS_LOGW(TAG, "graded a cloud-only card whose spelling is unknown; "
                              "no history line written");
            } else {
                SYS_LOGD(TAG, "graded a cloud-only card '%s' (%s): history only, "
                              "no local record",
                         cloud_spelling, word_review_name(result));
                (void)word_history_append(WORD_ID_CLOUD, cloud_spelling, result, true);
            }
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
    /* The counters are the book's counters: without the book there is nothing
     * to count (and an all-zero answer would read as "no words at all"). */
    const esp_err_t guard = book_guard();
    if (guard != ESP_OK) {
        return guard;
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
    if (!book_settled()) {
        /*
         * The book is still loading: there is no progress to reset yet, and the
         * loader would restore the old progress from the card right after the
         * parse - so the reset would look like it worked and then undo itself.
         */
        SYS_LOGW(TAG, "the wordbook is still loading; the reset was refused");
        return ESP_ERR_NOT_FINISHED;
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

/*
 * Serialises the flushers.
 *
 * Both the save worker and the HTTP reader drain the queue, and the flush
 * touches the file in several steps (check the tail, terminate a torn line,
 * append). Those steps must not interleave with another flusher, or a torn
 * line can still be glued to a line written in between.
 *
 * It is deliberately **not** the queue lock: that one is taken by the LVGL
 * callback on every grade, and it must never be held across a FAT write.
 */
static StaticSemaphore_t s_flush_lock_buf;
static SemaphoreHandle_t s_flush_lock;

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

/**
 * Put a line back at the head of the queue after a failed write.
 *
 * The flush pops the head before writing (so two flushers cannot write the
 * same line twice), which meant a failed write silently dropped the record.
 * Restoring it at the head keeps the chronological order.
 */
static void history_requeue(const char *line)
{
    xSemaphoreTake(s_history_lock, portMAX_DELAY);
    if (s_history_len < WORD_HISTORY_QUEUE) {
        s_history_head = (s_history_head + WORD_HISTORY_QUEUE - 1) % WORD_HISTORY_QUEUE;
        strlcpy(s_history_queue[s_history_head], line, WORD_HISTORY_LINE_MAX);
        s_history_len++;
    } else {
        /* 16 other lines are already waiting: they are newer, so they win. */
        SYS_LOGW(TAG, "study history queue is full; the failed line is dropped");
    }
    xSemaphoreGive(s_history_lock);
}

/*
 * ## Is the last line of the log complete?
 *
 * A power loss in the middle of an append leaves a half line at the end of the
 * file (no trailing newline). The next append is then glued onto it, and the
 * reader - which splits on '\n' - sees one broken line and skips **both**
 * records. So the tail is checked before appending, and a newline is written
 * first when the previous line is incomplete.
 *
 * The answer is remembered: every line this module appends ends with a
 * newline, so the tail stays complete for the rest of the session. It only
 * becomes unknown again when an append itself fails halfway.
 */
static bool s_history_tail_known;
static bool s_history_tail_complete;

static bool history_tail_is_complete(void)
{
    if (s_history_tail_known) {
        return s_history_tail_complete;
    }

    size_t size = 0;
    if (storage_exists(WORD_HISTORY_FILE, &size) != ESP_OK || size == 0) {
        s_history_tail_known = true;
        s_history_tail_complete = true;
        return true;
    }

    /* The storage abstraction has no seek, so the last byte can only be seen by
     * reading the file. Beyond the reader's own limit we do not bother: the
     * cheap fix (writing a newline first) is correct either way. */
    bool complete = false;
    if (size <= (size_t)WORD_HISTORY_MAX_READ) {
        char *text = NULL;
        size_t len = 0;
        if (storage_read_text(WORD_HISTORY_FILE, &text, &len) == ESP_OK && text != NULL) {
            complete = (len > 0 && text[len - 1] == '\n');
        }
        free(text);
    }

    s_history_tail_known = true;
    s_history_tail_complete = complete;
    return complete;
}

/** Drain queued history lines onto the card. Worker context. */
static void history_flush_locked(void)
{
    /*
     * Make sure the first line of this batch starts on a fresh line. Doing it
     * once per batch is enough: every line we append ends with a newline.
     */
    if (!history_tail_is_complete()) {
        const esp_err_t err = storage_append_file(WORD_HISTORY_FILE, "\n", 1);
        if (err != ESP_OK) {
            SYS_LOGW(TAG, "cannot terminate the torn history line: %s",
                     esp_err_to_name(err));
            return;
        }
        s_history_tail_known = true;
        s_history_tail_complete = true;
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
            /* The line was already taken out of the queue: put it back, or the
             * record is lost for good. */
            history_requeue(line);
            /* The file may now end in the middle of a line: re-check next time. */
            s_history_tail_known = false;
            return;   /* keep the rest queued for the next wake */
        }
        s_history_tail_known = true;
        s_history_tail_complete = true;

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

/** Drain queued history lines onto the card. Worker context. */
void word_history_flush_pending(void)
{
    if (s_history_lock == NULL || !storage_available()) {
        return;
    }

    /* One flusher at a time: the tail check and the appends that follow it
     * must not interleave with another flusher (see s_flush_lock). */
    if (s_flush_lock != NULL) {
        xSemaphoreTake(s_flush_lock, portMAX_DELAY);
    }
    history_flush_locked();
    if (s_flush_lock != NULL) {
        xSemaphoreGive(s_flush_lock);
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

        /* A torn tail simply fails to parse and is skipped. The depth limit
         * keeps a hand-edited line from blowing the stack (roadmap B3). */
        cJSON *entry = sys_json_parse(cursor, 32);
        if (entry != NULL) {
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

    /* Wait for a running flush to finish: deleting the file while a line is
     * being appended would put that line back into a file that no longer
     * exists. */
    if (s_flush_lock != NULL) {
        xSemaphoreTake(s_flush_lock, portMAX_DELAY);
    }

    /* Drop anything still queued, or the save worker would write it back into
     * the file this call is about to delete. */
    if (s_history_lock != NULL) {
        xSemaphoreTake(s_history_lock, portMAX_DELAY);
        s_history_head = 0;
        s_history_len = 0;
        xSemaphoreGive(s_history_lock);
    }
    /* The file is gone, so there is no torn tail to terminate any more. */
    s_history_tail_known = true;
    s_history_tail_complete = true;

    const esp_err_t err = storage_delete_file(WORD_HISTORY_FILE);
    if (s_flush_lock != NULL) {
        xSemaphoreGive(s_flush_lock);
    }
    return err;
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

    /* Serialises the flushers (save worker vs. HTTP reader). */
    s_flush_lock = xSemaphoreCreateMutexStatic(&s_flush_lock_buf);
    if (s_flush_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    (void)sys_config_get_str_or(SYS_KEY_WORD_BOOK, s_book, sizeof(s_book), WORD_DEFAULT_BOOK);
    s_daily_count = (uint32_t)sys_config_get_i32_or(SYS_KEY_WORD_DAILY,
                                                   CONFIG_TODOHELPER_WORD_DAILY_COUNT);

    s_initialized = true;

    /*
     * The book is loaded in the background (roadmap A2).
     *
     * Reading the 120 KB `words/cet6.json` and parsing its 833 words used to
     * happen right here, on the main task: 343 ms measured on the board, at a
     * moment when the only thing on screen is the splash log and nobody can
     * ask for a word yet. The load now runs in its own task and the public
     * interfaces report "not ready yet" until it is done.
     */
    book_set_state(WORD_BOOK_LOADING, ESP_OK);
    (void)sys_event_post_to(WORD_EVENT, WORD_EVENT_BOOK_LOADING, NULL, 0);

    if (xTaskCreate(word_book_worker, "th_word_book", WORD_BOOK_TASK_STACK, NULL,
                    WORD_BOOK_TASK_PRIO, NULL) != pdPASS) {
        /* No memory for the loader: load it inline. Slow (the old behaviour),
         * but the feature is complete - the alternative would be a word page
         * that never gets a book. */
        SYS_LOGW(TAG, "no memory for the book loader; loading it inline");
        (void)word_reload();
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

    /*
     * The other trigger for the cloud queue: the station got an IP (roadmap
     * C1). Before that the DNS lookup cannot even start, so building the queue
     * then is a wasted round trip.
     *
     * The subscription covers every event of the base; the handler reacts to
     * the moment the address appears (see on_wifi_event).
     */
    const esp_err_t wifi_sub = sys_event_subscribe(WIFI_MGR_EVENT, ESP_EVENT_ANY_ID,
                                                   on_wifi_event, NULL, NULL);
    if (wifi_sub != ESP_OK) {
        SYS_LOGW(TAG, "cannot subscribe to the WiFi events: %s",
                 esp_err_to_name(wifi_sub));
    }

    /* The mode cache (the event handlers must not read NVS) and the state of
     * the network at this very moment. */
    s_cloud_mode_selected = cloud_mode_selected();
    s_had_ip = have_ip();
    if (s_cloud_mode_selected && s_had_ip && word_cloud_count() == 0) {
        /* The address was already there when this module started (a warm boot,
         * or a reconnect that finished before us): no event will come, so ask
         * for the queue now. */
        cloud_request_build("the station already has an IP");
    }

    SYS_LOGI(TAG, "ready (book '%s', %u/day, loading in the background)", s_book,
             (unsigned)s_daily_count);
    return ESP_OK;
}
