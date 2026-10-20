/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file weather_service_http.c
 * @brief Requirement 8.4: /api/v1/weather*
 */

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "http_router.h"
#include "weather_internal.h"
#include "weather_service.h"

static const char *TAG = "weather_http";

/** Serialise a reading. `city` is left out when unset so the UI can prompt. */
static char *weather_to_json(const weather_data_t *data)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddBoolToObject(root, "valid", data->valid);
    cJSON_AddStringToObject(root, "city", data->city);
    cJSON_AddNumberToObject(root, "lat", data->lat);
    cJSON_AddNumberToObject(root, "lon", data->lon);
    cJSON_AddNumberToObject(root, "temp", (double)data->temp);
    cJSON_AddNumberToObject(root, "feels_like", (double)data->feels_like);
    cJSON_AddNumberToObject(root, "humidity", data->humidity);
    cJSON_AddStringToObject(root, "condition", data->condition);
    cJSON_AddStringToObject(root, "icon", data->icon);
    cJSON_AddNumberToObject(root, "weather_code", data->weather_code);
    cJSON_AddNumberToObject(root, "update_ts", (double)data->update_ts);
    cJSON_AddNumberToObject(root, "server_ts", (double)sys_time_ts());

    /* Staleness is what lets the UI say "showing the last cached reading"
     * instead of silently presenting old data as current (FR3). */
    const int64_t now = sys_time_ts();
    bool stale = false;
    if (data->update_ts > 0 && now > 0) {
        stale = (now - data->update_ts) > WEATHER_STALE_AFTER_S;
    }
    cJSON_AddBoolToObject(root, "stale", stale);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t weather_get_handler(httpd_req_t *req)
{
    weather_data_t data;
    if (weather_get(&data) != ESP_OK) {
        /* 200 with valid:false is friendlier than an error: the page can then
         * ask the user to pick a city. */
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "valid", false);
        cJSON_AddStringToObject(root, "city", weather_get_city());
        cJSON_AddStringToObject(root, "provider", weather_get_provider());
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        const esp_err_t err = http_reply_ok(req, json);
        cJSON_free(json);
        return err;
    }

    char *json = weather_to_json(&data);
    if (json == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "weather");
    }

    const esp_err_t err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

/**
 * POST /api/v1/weather/refresh - trigger a fetch and answer when it lands.
 *
 * weather_refresh() waits up to WEATHER_REFRESH_WAIT_MS (20 s) for the worker.
 * esp_http_server runs every handler in the single httpd task, so doing that
 * inline serializes all HTTP for 20 s (AGENT.md 9.0.1). The socket is detached
 * with httpd_req_async_handler_begin() and a short-lived task owns the reply.
 */
#define WEATHER_HTTP_TASK_STACK 6144
#define WEATHER_HTTP_TASK_PRIO  4

static void weather_refresh_respond_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;

    /* Blocking is fine here: this task exists only to wait for the fetch. */
    const esp_err_t err = weather_refresh();
    switch (err) {
    case ESP_OK:
        break;
    case ESP_ERR_NOT_FOUND:
        (void)http_reply_bad_request(req, "city not set: POST /api/v1/weather/city first");
        goto done;
    case ESP_ERR_INVALID_STATE:
        (void)http_reply_unavailable(req, "offline, showing the cached reading");
        goto done;
    case ESP_ERR_TIMEOUT:
        (void)http_reply_unavailable(req, "weather API did not answer in time");
        goto done;
    default:
        (void)http_reply_unavailable(req, esp_err_to_name(err));
        goto done;
    }

    weather_data_t data;
    if (weather_get(&data) != ESP_OK) {
        (void)http_reply_unavailable(req, "no reading available");
        goto done;
    }

    char *json = weather_to_json(&data);
    if (json == NULL) {
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "weather");
        goto done;
    }
    (void)http_reply_ok(req, json);
    cJSON_free(json);

done:
    /* Frees the session slot; skipping this eventually stops accept() entirely
     * (the IDF header is explicit about it). */
    (void)httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t refresh_post_handler(httpd_req_t *req)
{
    httpd_req_t *async_req = NULL;
    /* After this call `req` may no longer be used. */
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        return http_reply_unavailable(req, "async handler unavailable");
    }

    if (xTaskCreate(weather_refresh_respond_task, "th_wx_http", WEATHER_HTTP_TASK_STACK,
                    async_req, WEATHER_HTTP_TASK_PRIO, NULL) != pdPASS) {
        (void)httpd_resp_set_status(async_req, "503 Service Unavailable");
        (void)httpd_resp_send(async_req, "no memory for refresh task", HTTPD_RESP_USE_STRLEN);
        (void)httpd_req_async_handler_complete(async_req);
        return ESP_OK;
    }

    return ESP_OK;
}

static esp_err_t city_post_handler(httpd_req_t *req)
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

    const cJSON *city = cJSON_GetObjectItemCaseSensitive(root, "city");
    if (!cJSON_IsString(city) || city->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "field 'city' is required");
    }
    char value[64];
    strlcpy(value, city->valuestring, sizeof(value));
    cJSON_Delete(root);

    esp_err_t err = weather_set_city(value);
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "set city");
    }

    /* Refresh right away so the effect is visible; a failure is not fatal
     * because the city is already stored and the timer will retry.
     *
     * Async on purpose: weather_refresh() waits up to 20 s for the worker and
     * this handler runs in the single httpd task (AGENT.md 9.0.1). The result is
     * discarded here anyway, so waiting for it was pure latency. */
    (void)weather_refresh_async();

    weather_data_t data;
    if (weather_get(&data) == ESP_OK) {
        char *json = weather_to_json(&data);
        if (json != NULL) {
            err = http_reply_ok(req, json);
            cJSON_free(json);
            return err;
        }
    }

    char data_json[96];
    snprintf(data_json, sizeof(data_json), "{\"city\":\"%s\",\"valid\":false}", value);
    return http_reply_ok(req, data_json);
}

/**
 * POST /api/v1/weather/locate - 按公网 IP 定位城市。
 *
 * 定位要跑一次完整的 HTTPS 请求（超时 12 s）。esp_http_server 只有唯一的
 * httpd 任务，handler 在里面串行执行 —— 同步做的话，这 12 s 内整台设备的
 * HTTP 都建不上连接（AGENT.md §9.0.1）。所以照 sync_server 的 SSE 范式：
 * handler 立刻用 httpd_req_async_handler_begin() 把会话交给一个自有任务，
 * 任务算完结果再把响应发回去。
 *
 * 这是「请求-响应」语义，不是流式：不能边写边关。
 */
#define WEATHER_LOCATE_TASK_STACK 8192
#define WEATHER_LOCATE_TASK_PRIO  4

static void weather_locate_respond_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;

    /* 阻塞在这里没关系：这个任务存在的意义就是等这次定位。
     * 栈按跑得下 mbedTLS 握手给（esp_http_client 没有自己的任务）。 */
    const esp_err_t err = weather_detect_location();
    switch (err) {
    case ESP_OK:
        break;
    case ESP_ERR_INVALID_STATE:
        /* Genuinely cannot serve it: no network, or the clock is not synced yet so
         * the TLS handshake to the provider would fail certificate validation. */
        (void)http_reply_unavailable(req, "offline: IP geolocation needs a network");
        goto done;
    case ESP_ERR_INVALID_RESPONSE:
        /* 502, not 503. This service is healthy - the geolocation *provider* gave
         * an unusable answer. 503 "service unavailable" was misleading: the caller
         * would conclude ToDoHelper is down when in fact the upstream failed, and
         * the device very likely still has a perfectly good city (there is a
         * built-in default), so nothing the caller depends on has changed. */
        (void)http_reply_err(req, 502, "geolocation provider returned an unusable response");
        goto done;
    default:
        /* Upstream unreachable, timed out, DNS failure, ... same reasoning. */
        (void)http_reply_err(req, 502, "geolocation provider unreachable");
        goto done;
    }

    /* Detection only sets the coordinates. Kick the fetch off asynchronously
     * rather than waiting 20 s for it inside the httpd task; the reading arrives
     * via WEATHER_EVENT_UPDATED (or the next GET). */
    (void)weather_refresh_async();

    weather_data_t data;
    if (weather_get(&data) != ESP_OK) {
        char partial[96];
        snprintf(partial, sizeof(partial), "{\"city\":\"%s\",\"valid\":false}",
                 weather_get_city());
        (void)http_reply_ok(req, partial);
        goto done;
    }

    char *json = weather_to_json(&data);
    if (json == NULL) {
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "weather");
        goto done;
    }
    (void)http_reply_ok(req, json);
    cJSON_free(json);

done:
    /* 漏掉这一句就漏掉 httpd 的会话槽，最终服务器会停止接受连接
     * （esp_http_server 的头文件专门警告过这一点）。 */
    (void)httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t locate_post_handler(httpd_req_t *req)
{
    httpd_req_t *async_req = NULL;
    /* 之后 req 不能再用。 */
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        return http_reply_unavailable(req, "async handler unavailable");
    }

    if (xTaskCreate(weather_locate_respond_task, "th_wx_loc", WEATHER_LOCATE_TASK_STACK,
                    async_req, WEATHER_LOCATE_TASK_PRIO, NULL) != pdPASS) {
        (void)httpd_resp_set_status(async_req, "503 Service Unavailable");
        (void)httpd_resp_send(async_req, "no memory for locate task", HTTPD_RESP_USE_STRLEN);
        (void)httpd_req_async_handler_complete(async_req);
        return ESP_OK;
    }

    /* 立刻返回：定位由自有任务去做，httpd 任务不阻塞。 */
    return ESP_OK;
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

    const cJSON *provider = cJSON_GetObjectItemCaseSensitive(root, "provider");
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(root, "interval_s");
    const cJSON *geo = cJSON_GetObjectItemCaseSensitive(root, "geo_url");

    esp_err_t err = ESP_OK;
    if (cJSON_IsString(provider)) {
        err = weather_set_provider(provider->valuestring);
        if (err != ESP_OK) {
            cJSON_Delete(root);
            return http_reply_bad_request(req, "unknown provider");
        }
    }
    if (cJSON_IsString(key)) {
        (void)weather_set_api_key(key->valuestring);
    }
    if (cJSON_IsNumber(interval)) {
        (void)weather_set_interval_s((uint32_t)interval->valuedouble);
    }
    if (cJSON_IsString(geo)) {
        (void)weather_set_geo_endpoint(geo->valuestring);
    }
    cJSON_Delete(root);

    /* The API key is deliberately not echoed back. */
    char data[320];
    snprintf(data, sizeof(data),
             "{\"provider\":\"%s\",\"has_api_key\":%s,\"interval_s\":%u,\"geo_url\":\"%s\"}",
             weather_get_provider(), weather_has_api_key() ? "true" : "false",
             (unsigned)weather_get_interval_s(), weather_get_geo_endpoint());
    return http_reply_ok(req, data);
}

esp_err_t weather_service_register_http(void)
{
    static const struct {
        const char *path;
        httpd_method_t method;
        http_route_handler_t handler;
    } routes[] = {
        { "/api/v1/weather", HTTP_GET, weather_get_handler },
        { "/api/v1/weather/refresh", HTTP_POST, refresh_post_handler },
        { "/api/v1/weather/city", HTTP_POST, city_post_handler },
        { "/api/v1/weather/locate", HTTP_POST, locate_post_handler },
        { "/api/v1/weather/config", HTTP_POST, config_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = http_router_register(routes[i].path, routes[i].method, routes[i].handler);
        if (err != ESP_OK) {
            SYS_LOGE(TAG, "register %s failed: %s", routes[i].path, esp_err_to_name(err));
            return err;
        }
    }
    SYS_LOGD(TAG, "%u weather routes registered", (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
