/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file word_service_http.c
 * @brief Requirement 8.4: the /api/v1/words/... endpoints.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "http_router.h"
#include "sys_utils.h"
#include "word_service.h"

static const char *TAG = "word_http";

static cJSON *word_to_json(const word_item_t *word)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(obj, "id", (double)word->id);
    cJSON_AddStringToObject(obj, "spelling", word->spelling);
    cJSON_AddStringToObject(obj, "phonetic", word->phonetic);
    cJSON_AddStringToObject(obj, "meaning", word->meaning);
    cJSON_AddStringToObject(obj, "example", word->example);
    cJSON_AddNumberToObject(obj, "level", word->level);
    cJSON_AddNumberToObject(obj, "next_review_ts", (double)word->next_review_ts);
    return obj;
}

static cJSON *stats_to_json(const word_stats_t *stats)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(obj, "total", (double)stats->total);
    cJSON_AddNumberToObject(obj, "new", (double)stats->new_words);
    cJSON_AddNumberToObject(obj, "due", (double)stats->due);
    cJSON_AddNumberToObject(obj, "learned", (double)stats->learned);
    cJSON_AddNumberToObject(obj, "reviewed_total", (double)stats->reviewed_total);
    cJSON_AddNumberToObject(obj, "known_total", (double)stats->known_total);
    cJSON_AddNumberToObject(obj, "fuzzy_total", (double)stats->fuzzy_total);
    cJSON_AddNumberToObject(obj, "unknown_total", (double)stats->unknown_total);
    cJSON_AddStringToObject(obj, "book", word_get_book());
    cJSON_AddNumberToObject(obj, "daily_count", (double)word_get_daily_count());
    return obj;
}

/**
 * Answer a handler that needs the wordbook.
 *
 * The book is loaded in the background (v1.7.0), so a request can arrive
 * before it is there. That is not an internal error but a temporary state:
 * 503 says "not yet", 500 would say "broken". A failed load keeps the 500,
 * because then the request really cannot be served.
 */
static esp_err_t word_reply_book_error(httpd_req_t *req, esp_err_t err,
                                        const char *context)
{
    if (err == ESP_ERR_NOT_FINISHED) {
        return http_reply_unavailable(req, "the wordbook is still loading");
    }
    return http_reply_esp_error(req, err, context);
}

static esp_err_t today_get_handler(httpd_req_t *req)
{
    const size_t limit = word_get_daily_count();
    /* PSRAM: up to 100 cards x ~650 B is bulk data, and the internal heap is
     * the scarce resource on this board (roadmap B3). */
    word_item_t *words = heap_caps_calloc_prefer(limit, sizeof(word_item_t), 2,
                                                  MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (words == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "today's words");
    }

    size_t count = 0;
    esp_err_t err = word_get_today(words, limit, &count);
    if (err != ESP_OK) {
        heap_caps_free(words);
        return word_reply_book_error(req, err, "today's words");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    if (root == NULL || array == NULL) {
        heap_caps_free(words);
        cJSON_Delete(root);
        cJSON_Delete(array);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "today's words");
    }
    cJSON_AddItemToObject(root, "items", array);
    for (size_t i = 0; i < count && i < limit; i++) {
        cJSON *obj = word_to_json(&words[i]);
        if (obj != NULL) {
            cJSON_AddItemToArray(array, obj);
        }
    }
    heap_caps_free(words);

    cJSON_AddNumberToObject(root, "count", (double)count);
    cJSON_AddStringToObject(root, "book", word_get_book());
    cJSON_AddNumberToObject(root, "server_ts", (double)sys_time_ts());

    word_stats_t stats;
    if (word_stats(&stats) == ESP_OK) {
        cJSON *stats_obj = stats_to_json(&stats);
        if (stats_obj != NULL) {
            cJSON_AddItemToObject(root, "stats", stats_obj);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t stats_get_handler(httpd_req_t *req)
{
    word_stats_t stats;
    esp_err_t err = word_stats(&stats);
    if (err != ESP_OK) {
        return word_reply_book_error(req, err, "word stats");
    }

    cJSON *obj = stats_to_json(&stats);
    if (obj == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "word stats");
    }
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t next_get_handler(httpd_req_t *req)
{
    word_item_t word;
    esp_err_t err = word_next(&word);
    if (err == ESP_ERR_NOT_FOUND) {
        return http_reply_err(req, 404, "no words loaded");
    }
    if (err != ESP_OK) {
        return word_reply_book_error(req, err, "next word");
    }

    cJSON *obj = word_to_json(&word);
    if (obj == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "next word");
    }

    size_t position = 0;
    size_t total = 0;
    (void)word_queue_position(&position, &total);
    cJSON_AddNumberToObject(obj, "position", (double)position);
    cJSON_AddNumberToObject(obj, "queue_total", (double)total);

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t review_post_handler(httpd_req_t *req)
{
    uint32_t id = 0;
    if (!http_path_segment_u32(req, 3, &id)) {
        return http_reply_bad_request(req, "invalid word id");
    }

    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing body");
    }

    cJSON *root = sys_json_parse(body, 32);
    free(body);
    if (root == NULL) {
        SYS_LOGW(TAG, "review body is not valid JSON");
        return http_reply_bad_request(req, "invalid json");
    }

    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    word_review_result_t grade;
    if (!cJSON_IsString(result) || !word_review_from_name(result->valuestring, &grade)) {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "field 'result' must be known|fuzzy|unknown");
    }
    cJSON_Delete(root);

    esp_err_t err = word_review(id, grade);
    if (err == ESP_ERR_NOT_FOUND) {
        return http_reply_not_found(req);
    }
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "review word");
    }

    word_item_t word;
    if (word_get(id, &word) != ESP_OK) {
        return http_reply_ok(req, NULL);
    }

    cJSON *obj = word_to_json(&word);
    if (obj == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "review word");
    }
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing body");
    }

    cJSON *root = sys_json_parse(body, 32);
    free(body);
    if (root == NULL) {
        SYS_LOGW(TAG, "config body is not valid JSON");
        return http_reply_bad_request(req, "invalid json");
    }

    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily_count");
    const cJSON *book = cJSON_GetObjectItemCaseSensitive(root, "book");
    const cJSON *reset = cJSON_GetObjectItemCaseSensitive(root, "reset");

    if (cJSON_IsNumber(daily)) {
        (void)word_set_daily_count((uint32_t)daily->valuedouble);
    }
    const bool reset_requested = cJSON_IsTrue(reset);
    char book_copy[32] = { 0 };
    const bool book_given = cJSON_IsString(book);
    if (book_given) {
        strlcpy(book_copy, book->valuestring, sizeof(book_copy));
    }
    cJSON_Delete(root);

    if (book_given) {
        esp_err_t err = word_set_book(book_copy);
        if (err != ESP_OK) {
            return http_reply_bad_request(req, "book not found or unreadable");
        }
    }
    if (reset_requested) {
        (void)word_reset_progress();
    }

    word_stats_t stats = { 0 };
    /* The book may still be loading (v1.7.0): then there are no counters yet,
     * and the answer says so instead of reporting uninitialised memory. */
    if (word_stats(&stats) != ESP_OK) {
        SYS_LOGD(TAG, "the wordbook is not ready yet; the stats stay empty");
    }
    cJSON *obj = stats_to_json(&stats);
    if (obj == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "word config");
    }
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    const esp_err_t err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

/**
 * Async plumbing for handlers that touch the card.
 *
 * ## Why (v1.4.0 fix, learned the hard way)
 *
 * `esp_http_server` runs URI handlers **inline on its single httpd task**
 * (AGENT.md §9.0.1, last bullet). `/words/history` reads the whole study log and
 * `/words/export` writes a wordbook - both are FAT operations on a card that can
 * take hundreds of milliseconds and grows with the file. Doing them in the
 * handler wedges the httpd task: on the real board this produced
 * `task_wdt: IDLE0 (CPU 0) did not reset the watchdog` repeating forever,
 * `CPU 1: httpd` stuck, and the device answering **ping but refusing every TCP
 * connection** - the same failure v1.3.3 fixed for the MaiMemo routes.
 *
 * So both hand their socket to a worker and return immediately. The worker must
 * call `httpd_req_async_handler_complete()`, or the session slot leaks and httpd
 * eventually stops accepting connections entirely (IDF is explicit about this).
 */
typedef void (*word_async_work_t)(httpd_req_t *req, void *arg);

typedef struct {
    httpd_req_t *req;
    word_async_work_t work;
    void *arg;
} word_async_job_t;

static void word_async_trampoline(void *param)
{
    word_async_job_t *job = (word_async_job_t *)param;
    job->work(job->req, job->arg);
    /* Mandatory: without it the session slot is never released. */
    (void)httpd_req_async_handler_complete(job->req);
    free(job);
    vTaskDelete(NULL);
}

/** Start the worker task for a job that is already fully built. */
static esp_err_t word_spawn_async(httpd_req_t *req, httpd_req_t *async,
                                   word_async_job_t *job, uint32_t stack,
                                   const char *name)
{
    /* Stack must be internal RAM and is scarce here (AGENT.md §9.0.1): 4 KB is
     * enough for a FAT read plus the JSON build, which are heap-backed. */
    if (xTaskCreate(word_async_trampoline, name, stack, job, 3, NULL) != pdPASS) {
        free(job);
        (void)httpd_req_async_handler_complete(async);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "async handler");
    }
    return ESP_OK;
}

static esp_err_t word_run_async(httpd_req_t *req, word_async_work_t work, void *arg,
                                uint32_t stack, const char *name)
{
    httpd_req_t *async = NULL;
    if (httpd_req_async_handler_begin(req, &async) != ESP_OK) {
        return http_reply_esp_error(req, ESP_FAIL, "async handler");
    }

    word_async_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        (void)httpd_req_async_handler_complete(async);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "async handler");
    }
    job->req = async;
    job->work = work;
    job->arg = arg;

    return word_spawn_async(req, async, job, stack, name);
}

/**
 * Same as word_run_async(), but the worker's argument is **copied by value**
 * into the job.
 *
 * ## Why this exists (roadmap B4)
 *
 * The obvious way to hand arguments to a worker is to pass the address of a
 * local variable. That is a use-after-return: the handler's stack frame dies
 * when the handler returns, the httpd task reuses that memory for the next
 * request, and the worker then reads whatever happens to be there. The symptom
 * is not a crash but a *random* answer - the history endpoint returned an
 * empty list, a few entries, or an out-of-memory error depending on timing.
 *
 * So the payload is copied into the job, which the worker owns from the moment
 * it is created. The rule for this component: **an async worker's parameters
 * are always passed by value**, never as the address of a stack variable.
 */
static esp_err_t word_run_async_copy(httpd_req_t *req, word_async_work_t work,
                                     const void *payload, size_t payload_size,
                                     uint32_t stack, const char *name)
{
    httpd_req_t *async = NULL;
    if (httpd_req_async_handler_begin(req, &async) != ESP_OK) {
        return http_reply_esp_error(req, ESP_FAIL, "async handler");
    }

    /* The payload follows the header in the same allocation, so there is
     * exactly one thing to free. sizeof(word_async_job_t) keeps the payload
     * pointer-aligned (three pointers). */
    word_async_job_t *job = calloc(1, sizeof(*job) + payload_size);
    if (job == NULL) {
        (void)httpd_req_async_handler_complete(async);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "async handler");
    }
    job->req = async;
    job->work = work;
    if (payload != NULL && payload_size > 0) {
        job->arg = (void *)((uint8_t *)job + sizeof(*job));
        memcpy(job->arg, payload, payload_size);
    }

    return word_spawn_async(req, async, job, stack, name);
}

/** Arguments for the history read, parsed on the httpd task. */
typedef struct {
    bool today_only;
    size_t limit;
} history_args_t;

/** Worker: read the log and answer. */
static void history_get_worker(httpd_req_t *req, void *arg)
{
    /* The argument is a copy that lives in the job, not a pointer into the
     * handler's (already gone) stack frame. */
    const history_args_t *args = (const history_args_t *)arg;
    const bool today_only = args->today_only;
    const size_t limit = args->limit;

    /* PSRAM: a history page is bulk data and internal RAM is the scarce
     * resource on this board (AGENT.md 9.0.5). */
    word_review_log_entry_t *entries = heap_caps_calloc_prefer(
        limit, sizeof(word_review_log_entry_t), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (entries == NULL) {
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "study history");
        return;
    }

    size_t count = 0;
    const esp_err_t err = word_history_read(entries, limit, &count, today_only);

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    if (root == NULL || array == NULL) {
        heap_caps_free(entries);
        cJSON_Delete(root);
        cJSON_Delete(array);
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "study history");
        return;
    }

    if (err == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            cJSON *obj = cJSON_CreateObject();
            if (obj == NULL) {
                continue;
            }
            cJSON_AddNumberToObject(obj, "id", (double)entries[i].id);
            cJSON_AddStringToObject(obj, "spelling", entries[i].spelling);
            cJSON_AddStringToObject(obj, "result",
                                    word_review_name((word_review_result_t)entries[i].result));
            cJSON_AddNumberToObject(obj, "at", (double)entries[i].at);
            cJSON_AddBoolToObject(obj, "cloud", entries[i].cloud);
            cJSON_AddItemToArray(array, obj);
        }
    }
    heap_caps_free(entries);

    cJSON_AddItemToObject(root, "items", array);
    cJSON_AddNumberToObject(root, "count", (double)count);
    cJSON_AddBoolToObject(root, "today_only", today_only);
    cJSON_AddNumberToObject(root, "server_ts", (double)sys_time_ts());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "study history");
        return;
    }

    /* An empty history is not an error: the console shows "no records yet". */
    (void)http_reply_ok(req, json);
    cJSON_free(json);
}

/**
 * GET /api/v1/words/history[?today=1&limit=N]
 *
 * The device-side study record. MaiMemo's Open API cannot accept a review grade
 * (see maimemo_client.h), so this is where "what the user studied on the board"
 * is actually observable - both for the web console and for anything that wants
 * to act on it.
 *
 * The query is parsed here (the whole request is valid now) and the file read is
 * handed to a worker: see word_run_async() for why.
 */
static esp_err_t history_get_handler(httpd_req_t *req)
{
    history_args_t args = { .today_only = false, .limit = 50 };

    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char value[16];
        if (httpd_query_key_value(query, "today", value, sizeof(value)) == ESP_OK) {
            args.today_only = (value[0] == '1' || value[0] == 't');
        }
        if (httpd_query_key_value(query, "limit", value, sizeof(value)) == ESP_OK) {
            const long parsed = strtol(value, NULL, 10);
            if (parsed > 0 && parsed <= 200) {
                args.limit = (size_t)parsed;
            }
        }
    }

    /* The arguments are copied into the job: `args` lives on this handler's
     * stack, which is gone by the time the worker runs (see word_run_async_copy). */
    return word_run_async_copy(req, history_get_worker, &args, sizeof(args),
                               5120, "th_word_hist");
}

/** Worker: delete the study log, then answer. */
static void history_clear_worker(httpd_req_t *req, void *arg)
{
    (void)arg;

    const esp_err_t err = word_history_clear();
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        /* "There was no log" is not a failure: clearing twice must still
         * answer the same way the old synchronous handler did. */
        (void)http_reply_esp_error(req, err, "clear study history");
        return;
    }
    (void)http_reply_ok(req, "{\"cleared\":true}");
}

/**
 * POST /api/v1/words/history  body {"clear":true}
 *
 * Kept as a POST so the destructive action is never a bare GET that a browser
 * prefetch could trigger.
 */
static esp_err_t history_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing body");
    }
    cJSON *root = sys_json_parse(body, 32);
    free(body);
    if (root == NULL) {
        SYS_LOGW(TAG, "history body is not valid JSON");
        return http_reply_bad_request(req, "invalid json");
    }

    const cJSON *clear = cJSON_GetObjectItemCaseSensitive(root, "clear");
    const bool do_clear = cJSON_IsTrue(clear);
    cJSON_Delete(root);

    if (!do_clear) {
        return http_reply_bad_request(req, "nothing to do (expected \"clear\":true)");
    }
    /* The delete is a FAT operation on the SD card, so it goes to a worker
     * like every other card access here (see word_run_async). */
    return word_run_async(req, history_clear_worker, NULL, 5120, "th_word_hclr");
}

/** Worker: write the wordbook, then answer. */
static void export_post_worker(httpd_req_t *req, void *arg)
{
    (void)arg;

    const esp_err_t err = word_cloud_export(true);
    if (err != ESP_OK) {
        (void)http_reply_esp_error(req, err, "export memo wordbook");
        return;
    }
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "export memo wordbook");
        return;
    }
    cJSON_AddBoolToObject(obj, "written", true);
    cJSON_AddStringToObject(obj, "path", WORD_MAIMEMO_BOOK_FILE);
    cJSON_AddNumberToObject(obj, "count", (double)word_cloud_count());
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (json == NULL) {
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "export memo wordbook");
        return;
    }
    (void)http_reply_ok(req, json);
    cJSON_free(json);
}

/**
 * POST /api/v1/words/export
 *
 * Rewrite `words/maimemo.json` from the current cloud queue, so the day's list
 * can be captured on demand (offline study, inspection, backup).
 *
 * The write goes to a worker: it is a whole-file FAT write and would otherwise
 * block the httpd task (see word_run_async).
 */
static esp_err_t export_post_handler(httpd_req_t *req)
{
    if (word_cloud_count() == 0) {
        return http_reply_bad_request(req, "no cloud queue loaded; sync first");
    }
    return word_run_async(req, export_post_worker, NULL, 5120, "th_word_exp");
}

esp_err_t word_service_register_http(void)
{
    static const struct {
        const char *path;
        httpd_method_t method;
        http_route_handler_t handler;
    } routes[] = {
        { "/api/v1/words/today", HTTP_GET, today_get_handler },
        { "/api/v1/words/stats", HTTP_GET, stats_get_handler },
        { "/api/v1/words/next", HTTP_GET, next_get_handler },
        { "/api/v1/words/*/review", HTTP_POST, review_post_handler },
        { "/api/v1/words/config", HTTP_POST, config_post_handler },
        { "/api/v1/words/history", HTTP_GET, history_get_handler },
        { "/api/v1/words/history", HTTP_POST, history_post_handler },
        { "/api/v1/words/export", HTTP_POST, export_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = http_router_register(routes[i].path, routes[i].method, routes[i].handler);
        if (err != ESP_OK) {
            SYS_LOGE(TAG, "register %s failed: %s", routes[i].path, esp_err_to_name(err));
            return err;
        }
    }
    SYS_LOGD(TAG, "%u word routes registered", (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
