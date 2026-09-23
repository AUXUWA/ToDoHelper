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

static esp_err_t refresh_post_handler(httpd_req_t *req)
{
    esp_err_t err = weather_refresh();
    switch (err) {
    case ESP_OK:
        break;
    case ESP_ERR_NOT_FOUND:
        return http_reply_bad_request(req, "city not set: POST /api/v1/weather/city first");
    case ESP_ERR_INVALID_STATE:
        return http_reply_unavailable(req, "offline, showing the cached reading");
    case ESP_ERR_TIMEOUT:
        return http_reply_unavailable(req, "weather API did not answer in time");
    default:
        return http_reply_unavailable(req, esp_err_to_name(err));
    }

    weather_data_t data;
    if (weather_get(&data) != ESP_OK) {
        return http_reply_unavailable(req, "no reading available");
    }

    char *json = weather_to_json(&data);
    if (json == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "weather");
    }
    err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
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

    /* Refresh right away so the caller sees the effect; a failure is not fatal
     * because the city is already stored and the timer will retry. */
    (void)weather_refresh();

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

static esp_err_t locate_post_handler(httpd_req_t *req)
{
    esp_err_t err = weather_detect_location();
    switch (err) {
    case ESP_OK:
        break;
    case ESP_ERR_INVALID_STATE:
        return http_reply_unavailable(req, "offline: IP geolocation needs a network");
    case ESP_ERR_INVALID_RESPONSE:
        return http_reply_unavailable(req, "geolocation provider returned an unusable response");
    default:
        return http_reply_unavailable(req, esp_err_to_name(err));
    }

    /* Detection only sets the coordinates; fetch the reading before answering so
     * the caller sees the result of the whole operation. */
    (void)weather_refresh();

    weather_data_t data;
    if (weather_get(&data) != ESP_OK) {
        char partial[96];
        snprintf(partial, sizeof(partial), "{\"city\":\"%s\",\"valid\":false}",
                 weather_get_city());
        return http_reply_ok(req, partial);
    }

    char *json = weather_to_json(&data);
    if (json == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "weather");
    }
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
