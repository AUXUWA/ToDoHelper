/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file maimemo_http.c
 * @brief The /api/v1/maimemo/... routes.
 *
 * These are what the web console's 墨墨 panel talks to. The token itself is
 * **write-only** here: `POST /api/v1/maimemo/config` accepts it and
 * `GET /api/v1/maimemo/status` deliberately reports only whether one is set and
 * a masked hint, never the value (requirement 8.8).
 */

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "http_router.h"
#include "maimemo_client.h"
#include "sys_utils.h"

static const char *TAG = "maimemo_http";

/* ------------------------------------------------------------------------ */
/* Async handler plumbing                                                    */
/* ------------------------------------------------------------------------ */

/**
 * Run the blocking part of a handler on its own task.
 *
 * ## The rule this enforces
 *
 * `esp_http_server` executes handlers **inline on its single httpd task**, so a
 * handler that blocks on TLS stops serving *every* other request - the device
 * answers ping while refusing all TCP connections (AGENT.md 9.0.1, last bullet).
 * Every maimemo route does blocking TLS (`mm_post`/`mm_get`), so every one of
 * them has to hand its socket off instead of working in the handler.
 *
 * `httpd_req_async_handler_complete()` is mandatory afterwards: skipping it
 * leaks the session slot and httpd eventually stops accepting connections
 * altogether (the IDF header states this explicitly).
 *
 * @param req    The request as received by the handler.
 * @param work   Runs on the worker task; writes the response through
 *               http_req->aux (the async duplicate) and must not return early
 *               without completing the request.
 * @param arg    Free-form argument owned by @p work (freed by it).
 * @param stack  Worker stack in bytes. TLS needs several KB.
 */
typedef void (*mm_async_work_t)(httpd_req_t *req, void *arg);

/** Carries the async request plus the caller's argument to the worker task. */
typedef struct {
    mm_async_work_t work;
    void *arg;
    httpd_req_t *req;   /**< async duplicate; owned by the worker */
} mm_async_job_t;

static void mm_async_trampoline(void *param)
{
    mm_async_job_t *job = (mm_async_job_t *)param;
    job->work(job->req, job->arg);
    /* Mandatory, and deliberately here rather than in each worker: a worker that
     * forgets it permanently consumes a session slot (see the header note). */
    (void)httpd_req_async_handler_complete(job->req);
    free(job);
    vTaskDelete(NULL);
}

static esp_err_t mm_run_async(httpd_req_t *req, mm_async_work_t work, void *arg,
                              uint32_t stack, const char *task_name)
{
    httpd_req_t *async = NULL;
    if (httpd_req_async_handler_begin(req, &async) != ESP_OK) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "async begin");
    }

    mm_async_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        (void)httpd_req_async_handler_complete(async);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "async job");
    }
    job->work = work;
    job->arg = arg;
    job->req = async;

    if (xTaskCreate(mm_async_trampoline, task_name, stack, job, 3, NULL) != pdPASS) {
        free(job);
        (void)httpd_req_async_handler_complete(async);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "async task");
    }
    return ESP_OK;
}

/**
 * Show at most the first 4 characters of the token.
 *
 * Enough for the user to tell two tokens apart in the console, useless to
 * anyone who reads it - which is the point of masking rather than omitting.
 */
static void mask_token(char *out, size_t len)
{
    out[0] = '\0';

    char token[192];
    if (sys_config_get_str(SYS_KEY_MAIMEMO_TOKEN, token, sizeof(token)) != ESP_OK) {
        return;
    }
    const size_t n = strlen(token);
    if (n == 0) {
        return;
    }
    if (n <= 4) {
        strlcpy(out, "****", len);
        return;
    }
    snprintf(out, len, "%.4s****", token);
}

static cJSON *status_to_json(void)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }

    const bool has_token = maimemo_has_token();
    cJSON_AddBoolToObject(obj, "configured", has_token);
    cJSON_AddBoolToObject(obj, "push_enabled", maimemo_push_enabled());

    char masked[16];
    mask_token(masked, sizeof(masked));
    cJSON_AddStringToObject(obj, "token_hint", masked);

    /* Progress comes from the last sync, so this route never blocks on TLS and
     * is safe to poll from the console. */
    maimemo_progress_t progress;
    if (maimemo_get_cached_progress(&progress) == ESP_OK) {
        cJSON_AddBoolToObject(obj, "synced", true);
        cJSON *p = cJSON_AddObjectToObject(obj, "progress");
        if (p != NULL) {
            cJSON_AddNumberToObject(p, "finished", progress.finished);
            cJSON_AddNumberToObject(p, "total", progress.total);
            cJSON_AddNumberToObject(p, "remaining",
                                    progress.total > progress.finished
                                        ? progress.total - progress.finished : 0);
            cJSON_AddNumberToObject(p, "study_time_ms", progress.study_time_ms);
            cJSON_AddNumberToObject(p, "study_time_min", progress.study_time_ms / 60000);
            cJSON_AddNumberToObject(p, "plan_count", progress.due_count);
        }
    } else {
        cJSON_AddBoolToObject(obj, "synced", false);
    }

    return obj;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    cJSON *obj = status_to_json();
    if (obj == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "maimemo status");
    }
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (json == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "maimemo status");
    }
    const esp_err_t err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

/**
 * Send the status object, optionally annotated with a `verify` outcome.
 *
 * Shared by the synchronous "just store it" path and the async verify worker, so
 * both answer with exactly the same shape (the console renders one function).
 */
static esp_err_t config_reply(httpd_req_t *req, const char *verify_note)
{
    cJSON *obj = status_to_json();
    if (obj == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "maimemo config");
    }
    if (verify_note != NULL) {
        cJSON_AddStringToObject(obj, "verify", verify_note);
    }
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (json == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "maimemo config");
    }
    const esp_err_t err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

/** Worker for the `verify` path: one live call, then the status reply. */
static void config_verify_worker(httpd_req_t *req, void *arg)
{
    (void)arg;
    /* 显式的用户动作：无视退避闸门，真的发一次请求。 */
    maimemo_allow_request_now();
    const esp_err_t err = maimemo_verify_token();
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "token verification failed: %s", esp_err_to_name(err));
    }
    (void)config_reply(req, (err == ESP_OK) ? "ok" : esp_err_to_name(err));
}

/**
 * Accept `{"token":"...","push":bool,"verify":bool,"clear":bool}`.
 *
 * `verify` runs a live call, which blocks - so it is opt-in from the console
 * rather than implied by saving.
 */
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

    const cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "token");
    const cJSON *push = cJSON_GetObjectItemCaseSensitive(root, "push");
    const cJSON *verify = cJSON_GetObjectItemCaseSensitive(root, "verify");
    const bool clear = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "clear"));
    const bool verify_requested = cJSON_IsTrue(verify);

    const bool token_given = cJSON_IsString(token) && token->valuestring[0] != '\0';
    char token_copy[192] = { 0 };
    if (token_given) {
        strlcpy(token_copy, token->valuestring, sizeof(token_copy));
    }
    const bool push_given = cJSON_IsBool(push);
    const bool push_value = cJSON_IsTrue(push);
    cJSON_Delete(root);

    if (clear) {
        (void)maimemo_set_token(NULL);
    } else if (token_given) {
        const esp_err_t err = maimemo_set_token(token_copy);
        if (err != ESP_OK) {
            return http_reply_esp_error(req, err, "store token");
        }
    }
    if (push_given) {
        (void)maimemo_set_push_enabled(push_value);
    }

    /*
     * Verification is deliberately separate from storing: a study-scoped token
     * is perfectly usable for plan A even though it cannot read 助记/例句, so a
     * failed verify must not be reported as "the token is bad".
     *
     * It is also **slow** (one live call, 5-15 s). Doing it here would hold the
     * httpd task - and therefore every other HTTP request on the device - for the
     * whole round trip, so it is handed to a worker like the other TLS routes.
     */
    if (verify_requested && maimemo_has_token()) {
        return mm_run_async(req, config_verify_worker, NULL, 6144, "th_mm_verify");
    }
    return config_reply(req, NULL);
}

static esp_err_t sync_post_handler(httpd_req_t *req)
{
    if (!maimemo_has_token()) {
        return http_reply_bad_request(req, "no token configured");
    }

    const esp_err_t err = maimemo_sync_start();
    if (err == ESP_ERR_INVALID_STATE) {
        return http_reply_bad_request(req, "a sync is already running");
    }
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "start sync");
    }

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "sync");
    }
    cJSON_AddBoolToObject(obj, "started", true);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    const esp_err_t reply = http_reply_ok(req, json);
    cJSON_free(json);
    return reply;
}

/** Worker: fetch today's words (blocking TLS), then answer. */
static void today_worker(httpd_req_t *req, void *arg)
{
    (void)arg;

    /* 显式的用户动作：无视退避闸门，真的发一次请求。 */
    maimemo_allow_request_now();

    /*
     * 64 个词条数组有十几 KB，**必须显式走 PSRAM**（roadmap B3）：
     * 裸 calloc 会按 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 的规则先试内部堆，
     * 十几 KB 的块虽然最终会落到 PSRAM，但先占内部堆、再释放，留下的洞
     * 正是开机末期「最大连续块」掉到 12 KB 的原因之一。
     * 释放用 heap_caps_free()：IDF 里 free(p) 与 heap_caps_free(p) 等价。
     */
    maimemo_item_t *items = heap_caps_calloc_prefer(MAIMEMO_MAX_TODAY,
                                                    sizeof(maimemo_item_t), 2,
                                                    MALLOC_CAP_SPIRAM,
                                                    MALLOC_CAP_DEFAULT);
    if (items == NULL) {
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "today items");
        return;
    }

    size_t count = 0;
    const esp_err_t err = maimemo_fetch_today(items, MAIMEMO_MAX_TODAY, &count);
    if (err != ESP_OK) {
        heap_caps_free(items);
        (void)http_reply_esp_error(req, err, "fetch today items");
        return;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON *array = (obj != NULL) ? cJSON_AddArrayToObject(obj, "items") : NULL;
    if (obj == NULL || array == NULL) {
        heap_caps_free(items);
        cJSON_Delete(obj);
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "today items");
        return;
    }

    for (size_t i = 0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            continue;
        }
        cJSON_AddStringToObject(entry, "spelling", items[i].spelling);
        cJSON_AddBoolToObject(entry, "is_new", items[i].is_new);
        cJSON_AddBoolToObject(entry, "is_finished", items[i].is_finished);
        cJSON_AddStringToObject(entry, "first_response", items[i].first_response);
        cJSON_AddItemToArray(array, entry);
    }
    heap_caps_free(items);

    cJSON_AddNumberToObject(obj, "count", (double)count);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (json == NULL) {
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "today items");
        return;
    }

    (void)http_reply_ok(req, json);
    cJSON_free(json);
}

/** Live fetch of today's words — the console's "拉取今日词表" button. */
static esp_err_t today_get_handler(httpd_req_t *req)
{
    if (!maimemo_has_token()) {
        return http_reply_bad_request(req, "no token configured");
    }
    /* 6 KB for TLS; the 64-item array itself is PSRAM and lives in the worker. */
    return mm_run_async(req, today_worker, NULL, 6144, "th_mm_http_today");
}

/** Worker: enrichment lookup (blocking TLS on a cache miss), then answer. */
static void enrich_worker(httpd_req_t *req, void *arg)
{
    /* Owned by the worker: the handler's query buffer is gone by now. Freed on
     * every path below, hence the single-exit shape. */
    char *spelling = (char *)arg;

    /* 显式的用户动作：无视退避闸门，真的发一次请求。 */
    maimemo_allow_request_now();

    maimemo_enrich_t enrich;
    const esp_err_t err = maimemo_enrich(spelling, &enrich);
    /* ESP_ERR_NOT_SUPPORTED (token scope) and ESP_ERR_NOT_FOUND (word unknown to
     * MaiMemo, or it has no mnemonic) are both normal outcomes that still carry a
     * well-formed body, so they fall through to the reply below. */
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED && err != ESP_ERR_NOT_FOUND) {
        (void)http_reply_esp_error(req, err, "enrich word");
        free(spelling);
        return;
    }

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "enrich");
        free(spelling);
        return;
    }
    cJSON_AddStringToObject(obj, "spelling", spelling);
    cJSON_AddStringToObject(obj, "mnemonic", enrich.mnemonic);
    cJSON_AddStringToObject(obj, "example", enrich.example);
    /* 释义 (v1.4.0): read from /interpretations, which has its own permission
     * scope. It is therefore possible - and expected - for this to carry a value
     * while "unavailable" is true (that flag only covers 助记/例句). */
    cJSON_AddStringToObject(obj, "meaning", enrich.meaning);
    cJSON_AddBoolToObject(obj, "from_cache", enrich.from_cache);
    /* "unavailable" is distinct from "not found": the token simply may not read
     * this endpoint, and the console says so rather than "no mnemonic exists". */
    cJSON_AddBoolToObject(obj, "unavailable", err == ESP_ERR_NOT_SUPPORTED);

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (json == NULL) {
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "enrich");
    } else {
        (void)http_reply_ok(req, json);
        cJSON_free(json);
    }
    free(spelling);
}

static esp_err_t enrich_get_handler(httpd_req_t *req)
{
    char spelling[MAIMEMO_SPELLING_MAX];
    if (!http_query_get(req, "spelling", spelling, sizeof(spelling)) ||
        spelling[0] == '\0') {
        return http_reply_bad_request(req, "spelling is required");
    }

    /* No token is a configuration state, not a server fault: answer 400 with a
     * clear message so the caller knows to configure one, rather than a 500 that
     * reads as "the device is broken". Matches today_get_handler(). */
    if (!maimemo_has_token()) {
        return http_reply_bad_request(req, "no token configured");
    }

    /* The worker outlives this handler, so it needs its own copy of the query
     * value (which lives on this stack). A cache hit still goes async - the
     * handler must not branch on that without doing the lookup itself. */
    char *owned = strdup(spelling);
    if (owned == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "enrich");
    }

    const esp_err_t err = mm_run_async(req, enrich_worker, owned, 6144, "th_mm_http_enrich");
    if (err != ESP_OK) {
        free(owned);
    }
    return err;
}

/**
 * Work item for the asynchronous push.
 *
 * The socket is handed to a worker task with
 * httpd_req_async_handler_begin(), so the httpd task returns immediately.
 */
typedef struct {
    char **spellings;      /**< NULL-terminated, each strdup'ed */
    size_t count;
    bool advance;
} push_job_t;

static void push_job_free(push_job_t *job)
{
    if (job == NULL) {
        return;
    }
    for (size_t i = 0; i < job->count; i++) {
        free(job->spellings[i]);
    }
    /* 指针数组是 PSRAM 块：IDF 里 free() 与 heap_caps_free() 等价，
     * 这里写 heap_caps_free() 只是让「谁分配的」一眼可见。 */
    heap_caps_free(job->spellings);
    free(job);
}

/**
 * Push the words, then answer, on a task of our own.
 *
 * ## Why this is not done in the handler
 *
 * `maimemo_push_words()` is blocking TLS, and it calls `mm_resolve_voc_id()` once
 * **per word** - so a 10-word push is 11 sequential HTTPS round trips.
 * `esp_http_server` runs handlers inline on its single httpd task (AGENT.md
 * 9.0.1), so doing that in the handler froze the entire web server for the whole
 * duration: the device still answered ping, but **every** TCP connection was
 * refused, including `GET /` and `/api/v1/device/info`. Observed on the real
 * board - the API went dead and only a reboot brought it back.
 *
 * `httpd_req_async_handler_complete()` is mandatory: without it the session slot
 * is never released and httpd eventually stops accepting connections at all
 * (IDF says so explicitly in the header).
 */
static void push_worker(httpd_req_t *req, void *arg)
{
    push_job_t *job = (push_job_t *)arg;
    int accepted = 0;

    /* 显式的用户动作：无视退避闸门，真的发一次请求。 */
    maimemo_allow_request_now();

    const esp_err_t err = maimemo_push_words((const char **)job->spellings,
                                             job->count, job->advance, &accepted);

    char json[128];
    if (err == ESP_OK) {
        snprintf(json, sizeof(json), "{\"accepted\":%d,\"count\":%u}", accepted,
                 (unsigned)job->count);
        (void)http_reply_ok(req, json);
    } else {
        /* Report the reason the way the synchronous version did, so the console's
         * error handling keeps working. */
        snprintf(json, sizeof(json),
                 "{\"code\":500,\"msg\":\"internal error\","
                 "\"data\":{\"detail\":\"push words\",\"esp_err\":\"%s\"}}",
                 esp_err_to_name(err));
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        (void)httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        (void)httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    }

    push_job_free(job);
}

/** `{"spellings":["a","b"],"advance":bool}` — plan C, asynchronous. */
static esp_err_t push_post_handler(httpd_req_t *req)
{
    if (!maimemo_has_token()) {
        return http_reply_bad_request(req, "no token configured");
    }

    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing body");
    }
    cJSON *root = sys_json_parse(body, 32);
    free(body);
    if (root == NULL) {
        SYS_LOGW(TAG, "push body is not valid JSON");
        return http_reply_bad_request(req, "invalid json");
    }

    const cJSON *spellings = cJSON_GetObjectItemCaseSensitive(root, "spellings");
    const bool advance = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "advance"));
    if (!cJSON_IsArray(spellings)) {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "spellings must be an array");
    }

    const int count = cJSON_GetArraySize(spellings);
    if (count <= 0) {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "spellings is empty");
    }
    if (count > 1000) {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "at most 1000 words per call");
    }

    /*
     * Copy the spellings out of the cJSON tree: the tree dies with this handler,
     * but the worker outlives it. (The old synchronous version could borrow the
     * pointers because everything happened inside this function.)
     */
    push_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        cJSON_Delete(root);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "push");
    }
    /* 词条指针数组：1000 个指针 = 8 KB，**显式走 PSRAM**（roadmap B3）。
     * 裸 calloc 会先占内部堆再回落，留下的洞会切碎开机末期的连续块。 */
    job->spellings = heap_caps_calloc_prefer((size_t)count + 1, sizeof(char *), 2,
                                             MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (job->spellings == NULL) {
        cJSON_Delete(root);
        push_job_free(job);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "push");
    }
    job->advance = advance;

    for (int i = 0; i < count; i++) {
        const cJSON *item = cJSON_GetArrayItem(spellings, i);
        if (!cJSON_IsString(item) || item->valuestring[0] == '\0') {
            continue;
        }
        char *copy = strdup(item->valuestring);
        if (copy == NULL) {
            continue;   /* skip this one rather than fail the whole batch */
        }
        job->spellings[job->count++] = copy;
    }
    cJSON_Delete(root);

    if (job->count == 0) {
        push_job_free(job);
        return http_reply_bad_request(req, "no usable spellings");
    }

    /*
     * Hand the socket to a worker and return immediately.
     *
     * This route is the worst offender for the blocking-handler problem: it does
     * one `GET /vocabulary` per word **plus** the `add_words` call, all on the
     * httpd task, so a 10-word push used to freeze the whole web server for the
     * duration of 11 TLS round trips. The reply still arrives on this socket, so
     * a client watching the HTTP response sees the real result.
     *
     * 6 KB: TLS handshake + a 24 KB PSRAM response buffer per resolve.
     */
    const esp_err_t err = mm_run_async(req, push_worker, job, 6144, "th_mm_push");
    if (err != ESP_OK) {
        push_job_free(job);   /* ownership stays here when the task never starts */
    }
    return err;
}

/* ------------------------------------------------------------------------ */
/* Capability probe and studied-word uplink (v1.4.0)                         */
/* ------------------------------------------------------------------------ */

/**
 * Probe worker: exercises every endpoint and reports the outcome per endpoint.
 *
 * Runs off the httpd task for the same reason as every other handler here: it
 * performs up to six sequential HTTPS round trips.
 */
static void probe_worker(httpd_req_t *req, void *arg)
{
    (void)arg;

    /* 显式的用户动作：无视退避闸门，真的发一次请求。 */
    maimemo_allow_request_now();

    maimemo_probe_result_t results[MAIMEMO_PROBE_MAX];
    size_t count = 0;
    const esp_err_t err = maimemo_probe(results, MAIMEMO_PROBE_MAX, &count);
    if (err != ESP_OK) {
        char json[192];
        snprintf(json, sizeof(json),
                 "{\"code\":400,\"msg\":\"probe failed\","
                 "\"data\":{\"esp_err\":\"%s\"}}", esp_err_to_name(err));
        (void)http_reply_json(req, 400, "probe failed", json);
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (root == NULL || items == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "probe");
        return;
    }

    int usable = 0;
    for (size_t i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        if (obj == NULL) {
            continue;
        }
        cJSON_AddStringToObject(obj, "endpoint", results[i].path);
        cJSON_AddBoolToObject(obj, "ok", results[i].err == ESP_OK);
        /*
         * The esp_err_t name is the useful part: ESP_ERR_NOT_SUPPORTED means the
         * token's scope excludes the endpoint (a normal, expected answer), while
         * ESP_ERR_INVALID_STATE means the token itself was rejected. Collapsing
         * those two is what made the earlier "token does not work" reports
         * misleading.
         */
        cJSON_AddStringToObject(obj, "status", esp_err_to_name(results[i].err));
        if (results[i].detail >= 0) {
            cJSON_AddNumberToObject(obj, "detail", (double)results[i].detail);
        }
        cJSON_AddItemToArray(items, obj);
        if (results[i].err == ESP_OK) {
            usable++;
        }
    }

    cJSON_AddItemToObject(root, "endpoints", items);
    cJSON_AddNumberToObject(root, "usable", (double)usable);
    cJSON_AddNumberToObject(root, "total", (double)count);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "probe");
        return;
    }
    (void)http_reply_ok(req, json);
    cJSON_free(json);
}

/** POST /api/v1/maimemo/probe - "what can this token actually do?" */
static esp_err_t probe_post_handler(httpd_req_t *req)
{
    if (!maimemo_has_token()) {
        return http_reply_bad_request(req, "no token configured");
    }
    return mm_run_async(req, probe_worker, NULL, 6144, "th_mm_probe");
}

/** Body for the advance endpoint: spellings the device has studied. */
typedef struct {
    char **spellings;
    size_t count;
} advance_job_t;

static void advance_job_free(advance_job_t *job)
{
    if (job == NULL) {
        return;
    }
    for (size_t i = 0; i < job->count; i++) {
        free(job->spellings[i]);
    }
    heap_caps_free(job->spellings);
    free(job);
}

static void advance_worker(httpd_req_t *req, void *arg)
{
    advance_job_t *job = (advance_job_t *)arg;
    int advanced = 0;

    /* 显式的用户动作：无视退避闸门，真的发一次请求。 */
    maimemo_allow_request_now();

    const esp_err_t err = maimemo_advance_spellings((const char **)job->spellings,
                                                    job->count, &advanced);

    char json[192];
    if (err == ESP_OK) {
        snprintf(json, sizeof(json), "{\"advanced\":%d,\"count\":%u}", advanced,
                 (unsigned)job->count);
        (void)http_reply_ok(req, json);
    } else {
        snprintf(json, sizeof(json),
                 "{\"code\":500,\"msg\":\"internal error\","
                 "\"data\":{\"detail\":\"advance words\",\"esp_err\":\"%s\"}}",
                 esp_err_to_name(err));
        (void)http_reply_json(req, 500, "advance words", json);
    }
    /* The job and the spellings it owns belong to the worker: the handler has
     * long returned, so nobody else can free them. */
    advance_job_free(job);
}

/**
 * POST /api/v1/maimemo/advance  body {"spellings":[...]}
 *
 * The device-side "upload the study state" path. MaiMemo has no endpoint that
 * accepts a review grade, so this puts the studied words back into the account's
 * review plan instead - see maimemo_client.h. Requires the same opt-in as the
 * add-words push, because it also mutates the account.
 */
static esp_err_t advance_post_handler(httpd_req_t *req)
{
    if (!maimemo_has_token()) {
        return http_reply_bad_request(req, "no token configured");
    }
    if (!maimemo_advance_enabled()) {
        return http_reply_bad_request(req,
                                      "disabled; enable pushing in the maimemo panel first");
    }

    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing body");
    }
    cJSON *root = sys_json_parse(body, 32);
    free(body);
    if (root == NULL) {
        SYS_LOGW(TAG, "advance body is not valid JSON");
        return http_reply_bad_request(req, "invalid json");
    }

    const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "spellings");
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "spellings must be an array");
    }

    const size_t count = (size_t)cJSON_GetArraySize(array);
    if (count == 0 || count > 64) {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "spellings must hold 1..64 entries");
    }

    advance_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        cJSON_Delete(root);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "advance words");
    }
    /* 词条指针数组（最多 64 个）：显式走 PSRAM，与推送那条路保持一致
     * （roadmap B3）。 */
    job->spellings = heap_caps_calloc_prefer(count, sizeof(char *), 2,
                                               MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (job->spellings == NULL) {
        cJSON_Delete(root);
        advance_job_free(job);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "advance words");
    }

    /*
     * strdup each spelling: the cJSON tree is freed before the worker runs, and
     * this is exactly the dangling-pointer bug the push handler had to fix.
     */
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, array) {
        if (!cJSON_IsString(entry) || entry->valuestring[0] == '\0') {
            continue;
        }
        job->spellings[job->count] = strdup(entry->valuestring);
        if (job->spellings[job->count] != NULL) {
            job->count++;
        }
    }
    cJSON_Delete(root);

    if (job->count == 0) {
        advance_job_free(job);
        return http_reply_bad_request(req, "no usable spellings");
    }

    const esp_err_t err = mm_run_async(req, advance_worker, job, 6144, "th_mm_adv");
    if (err != ESP_OK) {
        advance_job_free(job);
    }
    return err;
}

esp_err_t maimemo_register_http(void)
{
    static const struct {
        const char *path;
        httpd_method_t method;
        http_route_handler_t handler;
    } routes[] = {
        { "/api/v1/maimemo/status", HTTP_GET, status_get_handler },
        { "/api/v1/maimemo/config", HTTP_POST, config_post_handler },
        { "/api/v1/maimemo/sync", HTTP_POST, sync_post_handler },
        { "/api/v1/maimemo/today", HTTP_GET, today_get_handler },
        { "/api/v1/maimemo/enrich", HTTP_GET, enrich_get_handler },
        { "/api/v1/maimemo/push", HTTP_POST, push_post_handler },
        { "/api/v1/maimemo/probe", HTTP_POST, probe_post_handler },
        { "/api/v1/maimemo/advance", HTTP_POST, advance_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        const esp_err_t err =
            http_router_register(routes[i].path, routes[i].method, routes[i].handler);
        if (err != ESP_OK) {
            SYS_LOGE(TAG, "register %s failed: %s", routes[i].path, esp_err_to_name(err));
            return err;
        }
    }
    SYS_LOGD(TAG, "%u maimemo routes registered",
             (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
