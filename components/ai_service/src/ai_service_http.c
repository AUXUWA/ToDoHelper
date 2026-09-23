/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_service_http.c
 * @brief Requirement 8.5: the /api/v1/ai/... endpoints.
 *
 * `/api/v1/ai/config` and `DELETE /api/v1/ai/history` are in the always-auth
 * list of sync_server because they can change credentials or destroy data.
 */

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ai_service.h"
#include "http_router.h"

static const char *TAG = "ai_http";

/* ------------------------------------------------------------------------ */
/* Streaming helper                                                          */
/* ------------------------------------------------------------------------ */

typedef struct {
    httpd_req_t *req;
    bool failed;
} sse_sink_t;

/**
 * Forward one delta as an SSE record.
 *
 * The record is JSON-encoded so a delta containing newlines cannot break the
 * framing.
 */
static esp_err_t stream_chunk_cb(const char *text, void *arg)
{
    sse_sink_t *sink = (sse_sink_t *)arg;
    if (sink == NULL || sink->failed) {
        return ESP_FAIL;
    }

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(obj, "delta", text);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const size_t needed = strlen(json) + 32;
    char *record = malloc(needed);
    if (record == NULL) {
        cJSON_free(json);
        return ESP_ERR_NO_MEM;
    }
    snprintf(record, needed, "event: chunk\ndata: %s\n\n", json);
    cJSON_free(json);

    const esp_err_t err = httpd_resp_send_chunk(sink->req, record, HTTPD_RESP_USE_STRLEN);
    free(record);
    if (err != ESP_OK) {
        sink->failed = true;
        return err;
    }
    return ESP_OK;
}

static esp_err_t chat_stream_post_handler(httpd_req_t *req)
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

    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(text) || text->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "field 'text' is required");
    }
    char prompt[AI_MESSAGE_MAX];
    strlcpy(prompt, text->valuestring, sizeof(prompt));
    cJSON_Delete(root);

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    sse_sink_t sink = { .req = req };
    esp_err_t err = ai_service_chat_stream(prompt, stream_chunk_cb, &sink);

    if (err != ESP_OK) {
        /* The stream is already open, so report inside the event stream rather
         * than switching to a JSON error the client is not parsing. */
        char data[96];
        snprintf(data, sizeof(data), "{\"code\":%d,\"msg\":\"%s\",\"data\":null}",
                 (err == ESP_ERR_NOT_FOUND) ? 400 : 503, esp_err_to_name(err));
        char record[128];
        snprintf(record, sizeof(record), "event: error\ndata: %s\n\n", data);
        (void)httpd_resp_send_chunk(req, record, HTTPD_RESP_USE_STRLEN);
    } else {
        const char *reply = ai_service_get_last_reply();
        if (reply != NULL) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "reply", reply);
            char *json = cJSON_PrintUnformatted(obj);
            cJSON_Delete(obj);
            if (json != NULL) {
                const size_t needed = strlen(json) + 32;
                char *record = malloc(needed);
                if (record != NULL) {
                    snprintf(record, needed, "event: done\ndata: %s\n\n", json);
                    (void)httpd_resp_send_chunk(req, record, HTTPD_RESP_USE_STRLEN);
                    free(record);
                }
                cJSON_free(json);
            }
        }
    }

    (void)httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Plain endpoints                                                           */
/* ------------------------------------------------------------------------ */

static esp_err_t chat_post_handler(httpd_req_t *req)
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

    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(text) || text->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "field 'text' is required");
    }
    char prompt[AI_MESSAGE_MAX];
    strlcpy(prompt, text->valuestring, sizeof(prompt));
    cJSON_Delete(root);

    char reply[AI_MESSAGE_MAX];
    esp_err_t err = ai_service_chat(prompt, reply, sizeof(reply));
    switch (err) {
    case ESP_OK:
        break;
    case ESP_ERR_NOT_FOUND:
        return http_reply_bad_request(req, "no API key configured (POST /api/v1/ai/config)");
    case ESP_ERR_INVALID_STATE:
        return http_reply_unavailable(req, "offline: AI is unavailable without a network");
    case ESP_ERR_TIMEOUT:
        return http_reply_unavailable(req, "request aborted or timed out");
    default:
        return http_reply_unavailable(req, esp_err_to_name(err));
    }

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "ai chat");
    }
    cJSON_AddStringToObject(obj, "reply", reply);
    cJSON_AddStringToObject(obj, "state", ai_service_state_name(ai_service_get_state()));
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t history_delete_handler(httpd_req_t *req)
{
    esp_err_t err = ai_service_clear_history();
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "clear history");
    }
    return http_reply_ok(req, "{\"cleared\":true}");
}

static esp_err_t history_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    if (root == NULL || array == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(array);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "ai history");
    }
    cJSON_AddItemToObject(root, "messages", array);

    const size_t count = ai_service_history_count();
    for (size_t i = 0; i < count; i++) {
        ai_message_t msg;
        if (ai_service_history_get(i, &msg) != ESP_OK) {
            continue;
        }
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            continue;
        }
        cJSON_AddStringToObject(entry, "role", msg.role);
        cJSON_AddStringToObject(entry, "content", msg.content);
        cJSON_AddItemToArray(array, entry);
    }
    cJSON_AddNumberToObject(root, "count", (double)count);
    cJSON_AddStringToObject(root, "model", ai_service_get_model());
    cJSON_AddStringToObject(root, "state", ai_service_state_name(ai_service_get_state()));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    const esp_err_t err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

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

    const cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    const cJSON *prompt = cJSON_GetObjectItemCaseSensitive(root, "system_prompt");
    const cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
    const cJSON *rounds = cJSON_GetObjectItemCaseSensitive(root, "history_rounds");
    const cJSON *save = cJSON_GetObjectItemCaseSensitive(root, "save_history");

    if (cJSON_IsString(key)) {
        /* Accepting an empty string is how a key is removed. */
        (void)ai_service_set_api_key(key->valuestring);
    }
    if (cJSON_IsString(prompt)) {
        (void)ai_service_set_system_prompt(prompt->valuestring);
    }
    if (cJSON_IsString(model)) {
        esp_err_t err = ai_service_set_model(model->valuestring);
        if (err != ESP_OK) {
            cJSON_Delete(root);
            return http_reply_bad_request(req, "model must be deepseek-chat or deepseek-reasoner");
        }
    }
    if (cJSON_IsNumber(rounds)) {
        (void)ai_service_set_history_rounds((uint32_t)rounds->valuedouble);
    }
    if (cJSON_IsBool(save)) {
        (void)sys_config_set_bool(SYS_KEY_AI_SAVE_HIST, cJSON_IsTrue(save));
    }
    cJSON_Delete(root);

    /* The API key is deliberately not echoed back. */
    char data[160];
    snprintf(data, sizeof(data),
             "{\"model\":\"%s\",\"has_api_key\":%s,\"history_rounds\":%u,\"state\":\"%s\"}",
             ai_service_get_model(), ai_service_has_api_key() ? "true" : "false",
             (unsigned)ai_service_get_history_rounds(),
             ai_service_state_name(ai_service_get_state()));
    return http_reply_ok(req, data);
}

static esp_err_t abort_post_handler(httpd_req_t *req)
{
    (void)ai_service_abort();
    return http_reply_ok(req, "{\"aborted\":true}");
}

esp_err_t ai_service_register_http(void)
{
    static const struct {
        const char *path;
        httpd_method_t method;
        http_route_handler_t handler;
    } routes[] = {
        { "/api/v1/ai/chat", HTTP_POST, chat_post_handler },
        { "/api/v1/ai/chat/stream", HTTP_POST, chat_stream_post_handler },
        { "/api/v1/ai/history", HTTP_GET, history_get_handler },
        { "/api/v1/ai/history", HTTP_DELETE, history_delete_handler },
        { "/api/v1/ai/config", HTTP_POST, config_post_handler },
        { "/api/v1/ai/abort", HTTP_POST, abort_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = http_router_register(routes[i].path, routes[i].method, routes[i].handler);
        if (err != ESP_OK) {
            SYS_LOGE(TAG, "register %s failed: %s", routes[i].path, esp_err_to_name(err));
            return err;
        }
    }
    SYS_LOGD(TAG, "%u AI routes registered", (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
