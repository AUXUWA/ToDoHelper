/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file maimemo_client.c
 * @brief MaiMemo OpenAPI client (plans A/B/C). See maimemo_client.h.
 *
 * ## Why this is hand-rolled rather than a generic HTTP helper
 *
 * The API is small and uniform: every call is a POST with a JSON body and a
 * bearer token, and every response is `{"success":bool,"data":{...},
 * "errors":[...]}`. One `mm_post()` plus a per-endpoint parser is less code than
 * a generic layer, and keeps the response-size limits explicit - which matters,
 * because this board has ~310 KB of internal RAM and a word book that once
 * drained it (AGENT.md 9.0.5).
 *
 * ## Error model
 *
 * The API returns HTTP 200 even for permission errors, with `success:false` and
 * a `common_permission_denied` code. So the status code alone is not enough:
 * every call inspects the envelope. `ESP_ERR_NOT_SUPPORTED` is used deliberately
 * for permission errors so callers can tell "this token may not do that, stop
 * asking" apart from "the network hiccuped, try again later".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "maimemo_client.h"
#include "storage_manager.h"
#include "sys_keys.h"
#include "sys_utils.h"

static const char *TAG = "maimemo";

#define MM_BASE           "https://open.maimemo.com/open/api/v1"
#define MM_TIMEOUT_MS     15000
/** Response cap. `/study/get_today_items` for 64 words is ~8 KB; 24 KB is ample
 *  and bounded, which is what matters on this board. */
#define MM_MAX_RESPONSE   (24 * 1024)
/** Cap on the enrichment cache read from disk (see enrich_cache_load()). */
#define MM_MAX_CACHE_FILE (256 * 1024)
#define MM_TOKEN_MAX      192

ESP_EVENT_DEFINE_BASE(MAIMEMO_EVENT);

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static TaskHandle_t s_sync_task;
static bool s_initialized;

/** Last successful progress, kept so the UI can paint without the network. */
static maimemo_progress_t s_progress;
static bool s_have_progress;

/** Enrichment cache, held in PSRAM: (spelling -> mnemonic/example). */
typedef struct {
    char spelling[MAIMEMO_SPELLING_MAX];
    char mnemonic[256];
    char example[256];
    /** true when the word was asked for and the API declined (permission).
     *  Cached so a study-scoped token does not re-ask on every card. */
    bool unavailable;
} mm_cache_entry_t;

static mm_cache_entry_t *s_cache;
static size_t s_cache_count;
static bool s_cache_loaded;

/* ------------------------------------------------------------------------ */
/* Token / config                                                            */
/* ------------------------------------------------------------------------ */

bool maimemo_has_token(void)
{
    char token[MM_TOKEN_MAX];
    if (sys_config_get_str(SYS_KEY_MAIMEMO_TOKEN, token, sizeof(token)) != ESP_OK) {
        return false;
    }
    return token[0] != '\0';
}

static bool mm_token(char *out, size_t len)
{
    if (sys_config_get_str(SYS_KEY_MAIMEMO_TOKEN, out, len) != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return out[0] != '\0';
}

esp_err_t maimemo_set_token(const char *token)
{
    if (token == NULL || token[0] == '\0') {
        (void)sys_config_erase(SYS_KEY_MAIMEMO_TOKEN);
        SYS_LOGI(TAG, "token cleared");
        return ESP_OK;
    }
    /* Stored only: never logged, never returned by any API (requirement 8.8).
     * The length is logged because that is the one thing that helps diagnose a
     * truncated paste without revealing the secret. */
    const esp_err_t err = sys_config_set_str(SYS_KEY_MAIMEMO_TOKEN, token);
    if (err == ESP_OK) {
        SYS_LOGI(TAG, "token stored (%u chars)", (unsigned)strlen(token));
    }
    return err;
}

bool maimemo_push_enabled(void)
{
    char value[8];
    if (sys_config_get_str(SYS_KEY_MAIMEMO_PUSH, value, sizeof(value)) != ESP_OK) {
        return false;   /* plan C mutates cloud data: opt-in only */
    }
    return value[0] == '1';
}

esp_err_t maimemo_set_push_enabled(bool enabled)
{
    return sys_config_set_str(SYS_KEY_MAIMEMO_PUSH, enabled ? "1" : "0");
}

esp_err_t maimemo_get_cached_progress(maimemo_progress_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    const bool have = s_have_progress;
    if (have) {
        *out = s_progress;
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    return have ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------------ */
/* HTTP plumbing                                                             */
/* ------------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t len;
    bool overflow;
} mm_body_t;

static esp_err_t mm_http_event(esp_http_client_event_t *evt)
{
    mm_body_t *body = (mm_body_t *)evt->user_data;
    if (body == NULL || evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) {
        return ESP_OK;
    }
    /* Bodies are large here (a 64-word today list), so allocate lazily and
     * prefer PSRAM: this is bulk data, not a hot buffer (AGENT.md 9.0.5). */
    if (body->buf == NULL) {
        body->buf = heap_caps_malloc_prefer(MM_MAX_RESPONSE, 2,
                                            MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
        if (body->buf == NULL) {
            body->overflow = true;
            return ESP_OK;
        }
        body->buf[0] = '\0';
    }
    if (body->overflow || body->len + (size_t)evt->data_len + 1 >= MM_MAX_RESPONSE) {
        body->overflow = true;
        return ESP_OK;
    }
    memcpy(body->buf + body->len, evt->data, (size_t)evt->data_len);
    body->len += (size_t)evt->data_len;
    body->buf[body->len] = '\0';
    return ESP_OK;
}

/** Envelope codes we care about, parsed from `errors[].code`. */
typedef enum {
    MM_ERR_NONE = 0,
    MM_ERR_PERMISSION,   /**< common_permission_denied: token scope too narrow */
    MM_ERR_AUTH,         /**< token missing/expired */
    MM_ERR_OTHER,
} mm_err_class_t;

/**
 * Inspect `{"success":..,"errors":[{"code":..}]}`.
 *
 * HTTP 200 with success:false is the normal shape of a MaiMemo permission or
 * validation error, so this is not an edge case - it is the main error path.
 */
static mm_err_class_t mm_classify(const cJSON *root, esp_err_t *out_err)
{
    const cJSON *success = cJSON_GetObjectItemCaseSensitive(root, "success");
    if (cJSON_IsBool(success) && cJSON_IsTrue(success)) {
        *out_err = ESP_OK;
        return MM_ERR_NONE;
    }

    const cJSON *errors = cJSON_GetObjectItemCaseSensitive(root, "errors");
    const cJSON *first = cJSON_IsArray(errors) ? cJSON_GetArrayItem(errors, 0) : NULL;
    const cJSON *code = first ? cJSON_GetObjectItemCaseSensitive(first, "code") : NULL;
    const char *code_str = cJSON_IsString(code) ? code->valuestring : "";

    if (strstr(code_str, "permission_denied") != NULL) {
        /* The token authenticated but its scope does not include this endpoint.
         * Log at info, not error: with a study-scoped token this is expected and
         * the feature degrades by design. */
        SYS_LOGI(TAG, "endpoint not in token scope (%s)", code_str);
        *out_err = ESP_ERR_NOT_SUPPORTED;
        return MM_ERR_PERMISSION;
    }
    if (strstr(code_str, "unauthorized") != NULL || strstr(code_str, "token") != NULL) {
        SYS_LOGW(TAG, "token rejected (%s)", code_str);
        *out_err = ESP_ERR_INVALID_STATE;
        return MM_ERR_AUTH;
    }

    const cJSON *msg = first ? cJSON_GetObjectItemCaseSensitive(first, "msg") : NULL;
    SYS_LOGW(TAG, "api error: %s (%s)", cJSON_IsString(msg) ? msg->valuestring : "?",
             code_str);
    *out_err = ESP_ERR_INVALID_RESPONSE;
    return MM_ERR_OTHER;
}

/**
 * POST @p path with @p body (may be NULL) and parse the reply into a cJSON tree.
 *
 * On success the caller owns *out_root. The returned esp_err_t already carries
 * the permission/auth distinction described in mm_classify().
 */
static esp_err_t mm_post(const char *path, const char *body, cJSON **out_root)
{
    *out_root = NULL;

    char token[MM_TOKEN_MAX];
    if (!mm_token(token, sizeof(token))) {
        return ESP_ERR_NOT_FOUND;   /* no token: caller degrades quietly */
    }

    char url[192];
    snprintf(url, sizeof(url), "%s/%s", MM_BASE, path);

    char auth[MM_TOKEN_MAX + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", token);

    mm_body_t resp = { 0 };
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = MM_TIMEOUT_MS,
        .event_handler = mm_http_event,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    const char *payload = (body != NULL) ? body : "{}";
    esp_http_client_set_post_field(client, payload, (int)strlen(payload));

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        SYS_LOGW(TAG, "POST %s failed: %s", path, esp_err_to_name(err));
        free(resp.buf);
        return err;
    }
    if (status != 200) {
        /* Same reasoning as mm_get(): a 401/403 still carries the error envelope
         * that names the actual problem, so inspect it before giving up. */
        if (resp.buf == NULL || resp.overflow) {
            SYS_LOGW(TAG, "POST %s -> HTTP %d (no parsable body)", path, status);
            free(resp.buf);
            return (status == 401 || status == 403) ? ESP_ERR_INVALID_STATE
                                                    : ESP_ERR_INVALID_RESPONSE;
        }
        SYS_LOGD(TAG, "POST %s -> HTTP %d, inspecting the error envelope", path, status);
    } else if (resp.buf == NULL || resp.overflow) {
        SYS_LOGW(TAG, "POST %s: response missing or over %d bytes", path, MM_MAX_RESPONSE);
        free(resp.buf);
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);
    if (root == NULL) {
        SYS_LOGW(TAG, "POST %s: response is not JSON", path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t api_err = ESP_OK;
    (void)mm_classify(root, &api_err);
    if (api_err != ESP_OK) {
        cJSON_Delete(root);
        return api_err;
    }

    *out_root = root;
    return ESP_OK;
}

/**
 * GET @p path with @p query and parse the reply.
 *
 * @p path must **not** start with '/': MM_BASE has no trailing slash and the
 * format string below supplies the separator, so a leading slash yields a double
 * slash (".../v1//vocabulary") that the API answers with HTTP 404. That is
 * exactly how the spelling lookup failed on the device.
 *
 * Needed because the note/phrase **read** endpoints are GETs: `POST /notes`
 * exists too, but it *creates* a note (body `{"note":{voc_id,note_type,note}}`),
 * so using POST to read returns the wrong thing entirely. Verified against the
 * official CLI (`memo-api-cli`), which lists notes as
 * `GET /api/v1/notes?voc_id=...`.
 */
static esp_err_t mm_get(const char *path, const char *query, cJSON **out_root)
{
    *out_root = NULL;

    char token[MM_TOKEN_MAX];
    if (!mm_token(token, sizeof(token))) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Tolerate a caller that passes the leading slash anyway - the double slash
     * failure is silent (a 404 that reads like "word not found"), so making this
     * robust is cheaper than trusting every future call site to remember. */
    while (*path == '/') {
        path++;
    }

    char url[320];
    snprintf(url, sizeof(url), "%s/%s%s%s", MM_BASE, path,
             (query != NULL && query[0] != '\0') ? "?" : "",
             (query != NULL) ? query : "");

    char auth[MM_TOKEN_MAX + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", token);

    mm_body_t resp = { 0 };
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = MM_TIMEOUT_MS,
        .event_handler = mm_http_event,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Authorization", auth);

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        SYS_LOGW(TAG, "GET %s failed: %s", path, esp_err_to_name(err));
        free(resp.buf);
        return err;
    }
    if (status != 200) {
        /*
         * **Do not decide from the status code alone.**
         *
         * The API answers 403 (and 401) with a fully-formed error envelope, e.g.
         * `{"errors":[{"code":"common_permission_denied"}]}` for a token whose
         * scope excludes the endpoint. Returning early on 401/403 discarded that
         * body and collapsed every auth/scope problem into one opaque
         * ESP_ERR_INVALID_STATE - which is how "the token may not read 助记"
         * first looked like a generic failure on the device.
         *
         * So: parse the envelope whenever there is one and let mm_classify() name
         * the real reason; fall back to the status code only when the body is
         * missing or unparseable.
         */
        if (resp.buf == NULL || resp.overflow) {
            SYS_LOGW(TAG, "GET %s -> HTTP %d (no parsable body)", path, status);
            free(resp.buf);
            return (status == 401 || status == 403) ? ESP_ERR_INVALID_STATE
                                                    : ESP_ERR_INVALID_RESPONSE;
        }
        SYS_LOGD(TAG, "GET %s -> HTTP %d, inspecting the error envelope", path, status);
    } else if (resp.buf == NULL || resp.overflow) {
        free(resp.buf);
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t api_err = ESP_OK;
    (void)mm_classify(root, &api_err);
    if (api_err != ESP_OK) {
        cJSON_Delete(root);
        return api_err;
    }

    *out_root = root;
    return ESP_OK;
}

/** `data` object of a parsed envelope (borrowed, do not delete). */
static const cJSON *mm_data(const cJSON *root)
{
    return cJSON_GetObjectItemCaseSensitive(root, "data");
}

static void mm_copy(char *dst, size_t len, const cJSON *obj, const char *field)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, field);
    if (cJSON_IsString(item)) {
        strlcpy(dst, item->valuestring, len);
    } else {
        dst[0] = '\0';
    }
}

/* ------------------------------------------------------------------------ */
/* Plan A: study sync                                                        */
/* ------------------------------------------------------------------------ */

esp_err_t maimemo_fetch_progress(maimemo_progress_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    cJSON *root = NULL;
    esp_err_t err = mm_post("study/get_study_progress", "{}", &root);
    if (err != ESP_OK) {
        return err;
    }

    const cJSON *data = mm_data(root);
    const cJSON *progress = data ? cJSON_GetObjectItemCaseSensitive(data, "progress") : NULL;
    if (cJSON_IsObject(progress)) {
        const cJSON *finished = cJSON_GetObjectItemCaseSensitive(progress, "finished");
        const cJSON *total = cJSON_GetObjectItemCaseSensitive(progress, "total");
        const cJSON *study_time = cJSON_GetObjectItemCaseSensitive(progress, "study_time");
        out->finished = cJSON_IsNumber(finished) ? (int)finished->valuedouble : 0;
        out->total = cJSON_IsNumber(total) ? (int)total->valuedouble : 0;
        out->study_time_ms = cJSON_IsNumber(study_time) ? (int)study_time->valuedouble : 0;
    }
    cJSON_Delete(root);

    /* The number of words in the plan needs a second call; it is cheap and it is
     * the "词库" figure the UI shows. Failure here must not lose the progress we
     * already have, so the error is deliberately swallowed. */
    cJSON *rec_root = NULL;
    if (mm_post("study/query_study_records", "{\"as_count\":true}", &rec_root) == ESP_OK) {
        const cJSON *data = mm_data(rec_root);
        const cJSON *count = data ? cJSON_GetObjectItemCaseSensitive(data, "count") : NULL;
        if (cJSON_IsNumber(count)) {
            out->due_count = (int)count->valuedouble;
        }
        cJSON_Delete(rec_root);
    }

    return ESP_OK;
}

esp_err_t maimemo_fetch_today(maimemo_item_t *out, size_t max, size_t *count)
{
    if (out == NULL || count == NULL || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;

    cJSON *root = NULL;
    const esp_err_t err = mm_post("study/get_today_items", "{}", &root);
    if (err != ESP_OK) {
        return err;
    }

    const cJSON *data = mm_data(root);
    const cJSON *items = data ? cJSON_GetObjectItemCaseSensitive(data, "today_items") : NULL;
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t index = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, items) {
        if (index >= max) {
            break;
        }
        if (!cJSON_IsObject(entry)) {
            continue;
        }
        maimemo_item_t *dst = &out[index];
        memset(dst, 0, sizeof(*dst));
        mm_copy(dst->voc_id, sizeof(dst->voc_id), entry, "voc_id");
        mm_copy(dst->spelling, sizeof(dst->spelling), entry, "voc_spelling");
        mm_copy(dst->first_response, sizeof(dst->first_response), entry, "first_response");
        const cJSON *is_new = cJSON_GetObjectItemCaseSensitive(entry, "is_new");
        const cJSON *is_finished = cJSON_GetObjectItemCaseSensitive(entry, "is_finished");
        dst->is_new = cJSON_IsBool(is_new) && cJSON_IsTrue(is_new);
        dst->is_finished = cJSON_IsBool(is_finished) && cJSON_IsTrue(is_finished);
        if (dst->spelling[0] != '\0') {
            index++;
        }
    }

    cJSON_Delete(root);
    *count = index;
    SYS_LOGI(TAG, "today: %u words from the study plan", (unsigned)index);
    return ESP_OK;
}

/** Worker body of the asynchronous sync. */
static void mm_sync_worker(void *arg)
{
    (void)arg;

    (void)sys_event_post_to(MAIMEMO_EVENT, MAIMEMO_EVENT_SYNC_STARTED, NULL, 0);

    maimemo_progress_t progress = { 0 };
    esp_err_t err = maimemo_fetch_progress(&progress);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_progress = progress;
        s_have_progress = true;
        xSemaphoreGive(s_lock);

        SYS_LOGI(TAG, "study progress: %d/%d, %d ms, plan %d",
                 progress.finished, progress.total, progress.study_time_ms,
                 progress.due_count);
        (void)sys_event_post_to(MAIMEMO_EVENT, MAIMEMO_EVENT_SYNC_DONE,
                                &progress, sizeof(progress));
    } else {
        (void)sys_event_post_to(MAIMEMO_EVENT, MAIMEMO_EVENT_SYNC_FAILED,
                                &err, sizeof(err));
    }

    s_sync_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t maimemo_sync_start(void)
{
    if (!maimemo_has_token()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_sync_task != NULL) {
        return ESP_ERR_INVALID_STATE;   /* one in flight is enough */
    }

    /* 6 KB stack: TLS plus a 24 KB heap body (AGENT.md 9.0.2 on stack sizing). */
    if (xTaskCreate(mm_sync_worker, "th_mm_sync", 6144, NULL, 3, &s_sync_task) != pdPASS) {
        s_sync_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t maimemo_verify_token(void)
{
    maimemo_progress_t progress = { 0 };
    const esp_err_t err = maimemo_fetch_progress(&progress);
    if (err == ESP_OK) {
        SYS_LOGI(TAG, "token OK: %d/%d finished today", progress.finished, progress.total);
    }
    return err;
}

/* ------------------------------------------------------------------------ */
/* Plan B: enrichment cache on the SD card                                   */
/* ------------------------------------------------------------------------ */

/**
 * Cache file, relative to the application directory: `words/enrich.json`.
 *
 * One file, loaded once into PSRAM, rewritten whole. A per-word file layout
 * would be friendlier to partial writes but costs a directory entry per word and
 * makes "is it cached?" a stat() call per card - the whole file is < 256 KB and
 * there are at most 2000 words, so holding it is cheaper than probing for it.
 *
 * The path is **relative** and goes through storage_read_text()/write_file() so
 * the cache follows whichever backend is mounted (SD card, or the internal
 * fallback when no card is present) instead of assuming a root.
 */
#define MM_CACHE_REL "words/enrich.json"

static void mm_cache_load(void)
{
    if (s_cache_loaded) {
        return;
    }
    s_cache_loaded = true;

    if (!storage_available()) {
        return;   /* no storage at all: enrichment simply is not cached */
    }

    char *text = NULL;
    size_t len = 0;
    if (storage_read_text(MM_CACHE_REL, &text, &len) != ESP_OK || text == NULL) {
        /* First boot: no cache yet is the normal case, not an error. */
        return;
    }
    if (len == 0 || len > MM_MAX_CACHE_FILE) {
        SYS_LOGW(TAG, "enrich cache has implausible size %u; ignoring", (unsigned)len);
        free(text);
        return;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (root == NULL) {
        SYS_LOGW(TAG, "enrich cache is not valid JSON; starting empty");
        return;
    }

    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    const size_t count = cJSON_IsArray(entries) ? (size_t)cJSON_GetArraySize(entries) : 0;
    if (count > 0) {
        s_cache = heap_caps_calloc_prefer(count, sizeof(mm_cache_entry_t), 2,
                                         MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
        if (s_cache != NULL) {
            size_t index = 0;
            const cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, entries) {
                if (index >= count || !cJSON_IsObject(entry)) {
                    break;
                }
                mm_cache_entry_t *dst = &s_cache[index];
                mm_copy(dst->spelling, sizeof(dst->spelling), entry, "spelling");
                mm_copy(dst->mnemonic, sizeof(dst->mnemonic), entry, "mnemonic");
                mm_copy(dst->example, sizeof(dst->example), entry, "example");
                const cJSON *unavailable =
                    cJSON_GetObjectItemCaseSensitive(entry, "unavailable");
                dst->unavailable = cJSON_IsBool(unavailable) && cJSON_IsTrue(unavailable);
                if (dst->spelling[0] != '\0') {
                    index++;
                }
            }
            s_cache_count = index;
        }
    }
    cJSON_Delete(root);
    SYS_LOGI(TAG, "enrich cache: %u entries", (unsigned)s_cache_count);
}

/** Persist the whole cache. Called from the worker path only, never the UI. */
static void mm_cache_save(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    if (entries == NULL) {
        cJSON_Delete(root);
        return;
    }
    for (size_t i = 0; i < s_cache_count; i++) {
        const mm_cache_entry_t *src = &s_cache[i];
        cJSON *obj = cJSON_CreateObject();
        if (obj == NULL) {
            continue;
        }
        cJSON_AddStringToObject(obj, "spelling", src->spelling);
        if (src->mnemonic[0] != '\0') {
            cJSON_AddStringToObject(obj, "mnemonic", src->mnemonic);
        }
        if (src->example[0] != '\0') {
            cJSON_AddStringToObject(obj, "example", src->example);
        }
        if (src->unavailable) {
            cJSON_AddBoolToObject(obj, "unavailable", true);
        }
        cJSON_AddItemToArray(entries, obj);
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL || !storage_available()) {
        free(text);
        return;
    }

    const esp_err_t err = storage_write_file(MM_CACHE_REL, text, strlen(text));
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "cannot write %s: %s", MM_CACHE_REL, esp_err_to_name(err));
    }
    free(text);
}

static mm_cache_entry_t *mm_cache_find(const char *spelling)
{
    for (size_t i = 0; i < s_cache_count; i++) {
        if (strcasecmp(s_cache[i].spelling, spelling) == 0) {
            return &s_cache[i];
        }
    }
    return NULL;
}

static mm_cache_entry_t *mm_cache_add(const char *spelling)
{
    /* One realloc-style grow per miss: the array is PSRAM and small, and misses
     * are far rarer than hits once warmed. */
    mm_cache_entry_t *grown = heap_caps_realloc_prefer(
        s_cache, (s_cache_count + 1) * sizeof(mm_cache_entry_t), 2,
        MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (grown == NULL) {
        return NULL;
    }
    s_cache = grown;
    mm_cache_entry_t *slot = &s_cache[s_cache_count++];
    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->spelling, spelling, sizeof(slot->spelling));
    return slot;
}

/** Resolve a spelling to a voc_id. Returns false when unknown to MaiMemo. */
/** Resolve a spelling to a voc_id via `GET /vocabulary?spelling=`.
 *
 * Returns false when the word is unknown to MaiMemo, the token is missing, or
 * the request fails - the caller cannot act differently on those cases, so they
 * are deliberately collapsed. */
static bool mm_resolve_voc_id(const char *spelling, char *out, size_t len)
{
    out[0] = '\0';

    /* Spellings are single ASCII words, so no percent-encoding is needed. */
    char query[128];
    snprintf(query, sizeof(query), "spelling=%s", spelling);

    cJSON *root = NULL;
    if (mm_get("vocabulary", query, &root) != ESP_OK) {
        return false;
    }

    bool found = false;
    const cJSON *data = mm_data(root);
    const cJSON *voc = data ? cJSON_GetObjectItemCaseSensitive(data, "voc") : NULL;
    if (cJSON_IsObject(voc)) {
        mm_copy(out, len, voc, "id");
        found = out[0] != '\0';
    }
    cJSON_Delete(root);
    return found;
}

/**
 * Read one field of the enrichment (`/notes` or `/phrases`).
 *
 * Both are **GET with `?voc_id=`**, not POST - see mm_get() for why that
 * distinction is load-bearing. The reply shape is
 * `{"data":{"notes":[{"note":"..."}]}}` (resp. `phrases`/`phrase`), confirmed
 * against the official CLI's response handling (`res.notes.map(...)`).
 *
 * Returns:
 *   ESP_OK                -> @p out filled in
 *   ESP_ERR_NOT_FOUND     -> endpoint answered, nothing for this word
 *   ESP_ERR_NOT_SUPPORTED -> token scope excludes the endpoint (cached as such)
 */
static esp_err_t mm_fetch_field(const char *path, const char *voc_id,
                                const char *array_key, const char *text_key,
                                char *out, size_t len)
{
    out[0] = '\0';

    char query[128];
    snprintf(query, sizeof(query), "voc_id=%s", voc_id);

    cJSON *root = NULL;
    const esp_err_t err = mm_get(path, query, &root);
    if (err != ESP_OK) {
        return err;
    }

    const cJSON *data = mm_data(root);
    const cJSON *list = data ? cJSON_GetObjectItemCaseSensitive(data, array_key) : NULL;
    esp_err_t result = ESP_ERR_NOT_FOUND;

    if (cJSON_IsArray(list)) {
        const cJSON *first = cJSON_GetArrayItem(list, 0);
        if (cJSON_IsObject(first)) {
            mm_copy(out, len, first, text_key);
            if (out[0] != '\0') {
                result = ESP_OK;
            }
        }
    }
    cJSON_Delete(root);
    return result;
}

esp_err_t maimemo_enrich(const char *spelling, maimemo_enrich_t *out)
{
    if (spelling == NULL || spelling[0] == '\0' || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    mm_cache_load();

    mm_cache_entry_t *cached = mm_cache_find(spelling);
    if (cached != NULL) {
        if (cached->unavailable) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NOT_SUPPORTED;   /* already known unobtainable */
        }
        if (cached->mnemonic[0] != '\0' || cached->example[0] != '\0') {
            strlcpy(out->mnemonic, cached->mnemonic, sizeof(out->mnemonic));
            strlcpy(out->example, cached->example, sizeof(out->example));
            out->from_cache = true;
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);

    if (!maimemo_has_token()) {
        return ESP_ERR_NOT_FOUND;
    }

    char voc_id[MAIMEMO_VOC_ID_MAX];
    if (!mm_resolve_voc_id(spelling, voc_id, sizeof(voc_id))) {
        SYS_LOGD(TAG, "'%s' is not in the MaiMemo vocabulary", spelling);
        return ESP_ERR_NOT_FOUND;
    }

    /* Both endpoints are permission-gated for a study-scoped token; probing one
     * is enough to decide, so the second call is skipped when the first says
     * "not in scope". That halves the wasted requests on the common path. */
    esp_err_t notes_err = mm_fetch_field("notes", voc_id, "notes", "note",
                                         out->mnemonic, sizeof(out->mnemonic));
    esp_err_t phrases_err = ESP_ERR_NOT_SUPPORTED;
    if (notes_err != ESP_ERR_NOT_SUPPORTED) {
        phrases_err = mm_fetch_field("phrases", voc_id, "phrases", "phrase",
                                     out->example, sizeof(out->example));
    }

    const bool out_of_scope = (notes_err == ESP_ERR_NOT_SUPPORTED &&
                               phrases_err == ESP_ERR_NOT_SUPPORTED);
    SYS_LOGD(TAG, "'%s': 助记=%s 例句=%s%s", spelling, esp_err_to_name(notes_err),
             esp_err_to_name(phrases_err), out_of_scope ? " (token scope limits these)" : "");

    /*
     * Re-look the entry up instead of reusing the `cached` pointer taken above.
     *
     * The lock was released for the whole network fetch, and another task may
     * have called mm_cache_add() in the meantime - that reallocs s_cache, so the
     * old pointer would dangle and writing through it would corrupt the heap.
     * A fresh lookup inside the lock is the only safe way to reach the slot.
     */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mm_cache_entry_t *slot = mm_cache_find(spelling);
    if (slot == NULL) {
        slot = mm_cache_add(spelling);
    }
    if (slot != NULL) {
        strlcpy(slot->mnemonic, out->mnemonic, sizeof(slot->mnemonic));
        strlcpy(slot->example, out->example, sizeof(slot->example));
        slot->unavailable = out_of_scope;
        mm_cache_save();
    }
    xSemaphoreGive(s_lock);

    if (out_of_scope) {
        /* Cached as unavailable, so this is reported once per word rather than
         * once per card shown. */
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (out->mnemonic[0] == '\0' && out->example[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    SYS_LOGI(TAG, "enriched '%s' (助记 %u B, 例句 %u B)", spelling,
             (unsigned)strlen(out->mnemonic), (unsigned)strlen(out->example));
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Plan C: reverse push                                                      */
/* ------------------------------------------------------------------------ */

esp_err_t maimemo_push_words(const char **spellings, size_t count, bool advance,
                             int *accepted)
{
    if (accepted != NULL) {
        *accepted = 0;
    }
    if (spellings == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Resolve each spelling to a voc_id first: /study/add_words takes ids, not
     * spellings. Words MaiMemo does not know are skipped rather than failing the
     * whole batch. */
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *words = cJSON_AddArrayToObject(root, "words");
    if (words == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    /* `advance` is a **required** property of /study/add_words, not an optional
     * one: omitting it fails with
     *   common_invalid_param: {base} must have required property 'advance'
     * even when the words array is otherwise valid. Verified against the live
     * API. It must therefore always be sent, false included. */
    cJSON_AddBoolToObject(root, "advance", advance);

    size_t resolved = 0;
    for (size_t i = 0; i < count; i++) {
        if (spellings[i] == NULL || spellings[i][0] == '\0') {
            continue;
        }
        char voc_id[MAIMEMO_VOC_ID_MAX];
        if (!mm_resolve_voc_id(spellings[i], voc_id, sizeof(voc_id))) {
            SYS_LOGD(TAG, "push: '%s' unknown to MaiMemo, skipped", spellings[i]);
            continue;
        }
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            continue;
        }
        cJSON_AddStringToObject(entry, "id", voc_id);
        cJSON_AddItemToArray(words, entry);
        resolved++;
    }

    if (resolved == 0) {
        cJSON_Delete(root);
        SYS_LOGW(TAG, "push: none of the %u words resolved", (unsigned)count);
        return ESP_ERR_NOT_FOUND;
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *reply = NULL;
    const esp_err_t err = mm_post("study/add_words", body, &reply);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    /* Report the server's own count: it may reject individual ids (a word that is
     * already in the plan, for instance), so the number we sent is not the number
     * that landed. */
    int added = (int)resolved;
    const cJSON *data = mm_data(reply);
    const cJSON *added_count = data ? cJSON_GetObjectItemCaseSensitive(data, "added_count") : NULL;
    if (cJSON_IsNumber(added_count)) {
        added = (int)added_count->valuedouble;
    }
    cJSON_Delete(reply);

    if (accepted != NULL) {
        *accepted = added;
    }
    SYS_LOGI(TAG, "pushed to the study plan: %d accepted of %u resolved (advance=%d)",
             added, (unsigned)resolved, (int)advance);
    return ESP_OK;
}

esp_err_t maimemo_push_unknown(const char *spelling)
{
    if (spelling == NULL || spelling[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    /* Mutating the user's cloud plan is opt-in: without it this is a no-op, not
     * a failure the UI needs to report. */
    if (!maimemo_push_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!maimemo_has_token()) {
        return ESP_ERR_NOT_FOUND;
    }

    const char *list[1] = { spelling };
    int accepted = 0;
    const esp_err_t err = maimemo_push_words(list, 1, false, &accepted);
    if (err == ESP_OK) {
        (void)sys_event_post_to(MAIMEMO_EVENT, MAIMEMO_EVENT_PUSH_DONE,
                                &accepted, sizeof(accepted));
    }
    return err;
}

/* ------------------------------------------------------------------------ */
/* Init                                                                      */
/* ------------------------------------------------------------------------ */

esp_err_t maimemo_client_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;

    esp_err_t http_err = maimemo_register_http();
    if (http_err != ESP_OK) {
        SYS_LOGW(TAG, "HTTP routes unavailable: %s", esp_err_to_name(http_err));
    }

    SYS_LOGI(TAG, "ready (token=%s, push=%s)", maimemo_has_token() ? "set" : "unset",
             maimemo_push_enabled() ? "on" : "off");
    return ESP_OK;
}
