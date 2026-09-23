/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file calendar_service_http.c
 * @brief Requirement 8.4: the /api/v1/calendar/... endpoints.
 */

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "calendar_service.h"
#include "http_router.h"

static const char *TAG = "calendar_http";

static esp_err_t now_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "calendar now");
    }

    const bool synced = calendar_time_synced();
    cJSON_AddBoolToObject(root, "synced", synced);
    cJSON_AddNumberToObject(root, "ts", (double)sys_time_ts());
    cJSON_AddStringToObject(root, "tz", calendar_get_timezone());
    cJSON_AddStringToObject(root, "ntp_server", calendar_get_ntp_server());

    /* "unsynced" tells the client to show the placeholder instead of 1970. */
    struct tm now;
    if (calendar_get_now(&now) == ESP_OK) {
        char text[64];
        snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d",
                 now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                 now.tm_hour, now.tm_min, now.tm_sec);
        cJSON_AddStringToObject(root, "datetime", text);
        cJSON_AddNumberToObject(root, "year", now.tm_year + 1900);
        cJSON_AddNumberToObject(root, "month", now.tm_mon + 1);
        cJSON_AddNumberToObject(root, "day", now.tm_mday);
        cJSON_AddNumberToObject(root, "weekday", now.tm_wday);
    } else {
        cJSON_AddStringToObject(root, "datetime", "时间未同步");
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    const esp_err_t err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t month_get_handler(httpd_req_t *req)
{
    int64_t year = 0;
    int64_t month = 0;
    if (!http_query_get_i64(req, "year", &year) || !http_query_get_i64(req, "month", &month)) {
        return http_reply_bad_request(req, "query parameters 'year' and 'month' are required");
    }
    if (month < 1 || month > 12 || year < 1970 || year > 2200) {
        return http_reply_bad_request(req, "year/month out of range");
    }

    struct tm days[31];
    size_t count = 0;
    esp_err_t err = calendar_get_month((int)year, (int)month, days, &count);
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "calendar month");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    if (root == NULL || array == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(array);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "calendar month");
    }

    cJSON_AddNumberToObject(root, "year", (double)year);
    cJSON_AddNumberToObject(root, "month", (double)month);
    cJSON_AddNumberToObject(root, "days", (double)count);
    cJSON_AddNumberToObject(root, "first_weekday", count > 0 ? days[0].tm_wday : 0);
    cJSON_AddItemToObject(root, "weekdays", array);
    for (size_t i = 0; i < count; i++) {
        cJSON_AddItemToArray(array, cJSON_CreateNumber(days[i].tm_wday));
    }

    /* Mark today so the client can highlight it without its own clock. */
    struct tm now;
    if (calendar_get_now(&now) == ESP_OK &&
        (now.tm_year + 1900) == (int)year && (now.tm_mon + 1) == (int)month) {
        cJSON_AddNumberToObject(root, "today", now.tm_mday);
    } else {
        cJSON_AddNullToObject(root, "today");
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t sync_post_handler(httpd_req_t *req)
{
    esp_err_t err = calendar_sync_ntp(0);
    if (err != ESP_OK) {
        return http_reply_err(req, 503, "NTP sync failed (is the network up?)");
    }

    char data[64];
    snprintf(data, sizeof(data), "{\"synced\":true,\"ts\":%lld}", (long long)sys_time_ts());
    return http_reply_ok(req, data);
}

static esp_err_t time_post_handler(httpd_req_t *req)
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

    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "ts");
    if (!cJSON_IsNumber(ts)) {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "field 'ts' (unix seconds) is required");
    }
    const int64_t value = (int64_t)ts->valuedouble;
    cJSON_Delete(root);

    esp_err_t err = calendar_set_time(value);
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "set time");
    }
    return http_reply_ok(req, NULL);
}

static esp_err_t timezone_post_handler(httpd_req_t *req)
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

    const cJSON *tz = cJSON_GetObjectItemCaseSensitive(root, "tz");
    if (!cJSON_IsString(tz)) {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "field 'tz' is required");
    }
    char value[48];
    strlcpy(value, tz->valuestring, sizeof(value));
    cJSON_Delete(root);

    esp_err_t err = calendar_set_timezone(value);
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "set timezone");
    }
    return http_reply_ok(req, NULL);
}

esp_err_t calendar_service_register_http(void)
{
    static const struct {
        const char *path;
        httpd_method_t method;
        http_route_handler_t handler;
    } routes[] = {
        { "/api/v1/calendar/now", HTTP_GET, now_get_handler },
        { "/api/v1/calendar/month", HTTP_GET, month_get_handler },
        { "/api/v1/calendar/sync", HTTP_POST, sync_post_handler },
        { "/api/v1/calendar/time", HTTP_POST, time_post_handler },
        { "/api/v1/calendar/tz", HTTP_POST, timezone_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = http_router_register(routes[i].path, routes[i].method, routes[i].handler);
        if (err != ESP_OK) {
            SYS_LOGE(TAG, "register %s failed: %s", routes[i].path, esp_err_to_name(err));
            return err;
        }
    }
    SYS_LOGD(TAG, "%u calendar routes registered", (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
