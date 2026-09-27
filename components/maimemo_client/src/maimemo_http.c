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
#include "esp_log.h"

#include "http_router.h"
#include "maimemo_client.h"
#include "sys_utils.h"

static const char *TAG = "maimemo_http";

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

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
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

    /* Verification is deliberately separate from storing: a study-scoped token
     * is perfectly usable for plan A even though it cannot read 助记/例句, so a
     * failed verify must not be reported as "the token is bad". */
    const char *verify_note = NULL;
    if (verify_requested && maimemo_has_token()) {
        const esp_err_t err = maimemo_verify_token();
        verify_note = (err == ESP_OK) ? "ok" : esp_err_to_name(err);
        if (err != ESP_OK) {
            SYS_LOGW(TAG, "token verification failed: %s", esp_err_to_name(err));
        }
    }

    cJSON *obj = status_to_json();
    if (obj == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "maimemo config");
    }
    if (verify_note != NULL) {
        cJSON_AddStringToObject(obj, "verify", verify_note);
    }
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    const esp_err_t err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
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

/** Live fetch of today's words — the console's "拉取今日词表" button. */
static esp_err_t today_get_handler(httpd_req_t *req)
{
    if (!maimemo_has_token()) {
        return http_reply_bad_request(req, "no token configured");
    }

    maimemo_item_t *items = calloc(MAIMEMO_MAX_TODAY, sizeof(maimemo_item_t));
    if (items == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "today items");
    }

    size_t count = 0;
    const esp_err_t err = maimemo_fetch_today(items, MAIMEMO_MAX_TODAY, &count);
    if (err != ESP_OK) {
        free(items);
        return http_reply_esp_error(req, err, "fetch today items");
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON *array = (obj != NULL) ? cJSON_AddArrayToObject(obj, "items") : NULL;
    if (obj == NULL || array == NULL) {
        free(items);
        cJSON_Delete(obj);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "today items");
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
    free(items);

    cJSON_AddNumberToObject(obj, "count", (double)count);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    const esp_err_t reply = http_reply_ok(req, json);
    cJSON_free(json);
    return reply;
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

    maimemo_enrich_t enrich;
    const esp_err_t err = maimemo_enrich(spelling, &enrich);
    /* ESP_ERR_NOT_SUPPORTED (token scope) and ESP_ERR_NOT_FOUND (word unknown to
     * MaiMemo, or it has no mnemonic) are both normal outcomes that still carry a
     * well-formed body, so they fall through to the reply below. */
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED && err != ESP_ERR_NOT_FOUND) {
        return http_reply_esp_error(req, err, "enrich word");
    }

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "enrich");
    }
    cJSON_AddStringToObject(obj, "spelling", spelling);
    cJSON_AddStringToObject(obj, "mnemonic", enrich.mnemonic);
    cJSON_AddStringToObject(obj, "example", enrich.example);
    cJSON_AddBoolToObject(obj, "from_cache", enrich.from_cache);
    /* "unavailable" is distinct from "not found": the token simply may not read
     * this endpoint, and the console says so rather than "no mnemonic exists". */
    cJSON_AddBoolToObject(obj, "unavailable", err == ESP_ERR_NOT_SUPPORTED);

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    const esp_err_t reply = http_reply_ok(req, json);
    cJSON_free(json);
    return reply;
}

/** `{"spellings":["a","b"],"advance":bool}` — plan C. */
static esp_err_t push_post_handler(httpd_req_t *req)
{
    if (!maimemo_has_token()) {
        return http_reply_bad_request(req, "no token configured");
    }

    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
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

    const char **list = calloc((size_t)count, sizeof(char *));
    if (list == NULL) {
        cJSON_Delete(root);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "push");
    }
    int used = 0;
    for (int i = 0; i < count; i++) {
        const cJSON *item = cJSON_GetArrayItem(spellings, i);
        if (cJSON_IsString(item) && item->valuestring[0] != '\0') {
            list[used++] = item->valuestring;
        }
    }

    int accepted = 0;
    const esp_err_t err = maimemo_push_words(list, (size_t)used, advance, &accepted);
    free(list);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "push words");
    }

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "push");
    }
    cJSON_AddNumberToObject(obj, "accepted", accepted);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    const esp_err_t reply = http_reply_ok(req, json);
    cJSON_free(json);
    return reply;
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
