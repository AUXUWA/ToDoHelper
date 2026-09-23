/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "weather_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "storage_manager.h"
#include "sys_utils.h"

static const char *TAG = "weather";

ESP_EVENT_DEFINE_BASE(WEATHER_EVENT);

#ifndef CONFIG_TODOHELPER_WEATHER_INTERVAL_S
#define CONFIG_TODOHELPER_WEATHER_INTERVAL_S WEATHER_DEFAULT_INTERVAL_S
#endif
#ifndef CONFIG_TODOHELPER_WEATHER_HTTP_TIMEOUT_MS
#define CONFIG_TODOHELPER_WEATHER_HTTP_TIMEOUT_MS 12000
#endif

#define WEATHER_CACHE_FILE "weather.json"
#define WEATHER_TASK_STACK 6144
#define WEATHER_TASK_PRIO  4
#define WEATHER_REFRESH_WAIT_MS 20000

/**
 * Default IP geolocation endpoint (FR3 priority 2).
 *
 * ipapi.co answers HTTPS without an API key and reports city/latitude/longitude.
 * Overridable through the NVS key `geo_url`; the parser also accepts the
 * `lat`/`lon` spelling and a nested `location` object, so switching providers is
 * usually just a URL change.
 */
#define WEATHER_GEO_ENDPOINT_DEFAULT "https://ipapi.co/json/"

/** Worker request opcodes. */
#define WEATHER_REQ_REFRESH 0
#define WEATHER_REQ_DETECT  1

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

static bool s_initialized;
static weather_data_t s_data;
static char s_city[64];
static char s_provider[24] = WEATHER_PROVIDER_DEFAULT;
static char s_geo_url[192] = WEATHER_GEO_ENDPOINT_DEFAULT;
static uint32_t s_interval_s = CONFIG_TODOHELPER_WEATHER_INTERVAL_S;

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

static TaskHandle_t s_task;
static QueueHandle_t s_requests;
static SemaphoreHandle_t s_done;

static esp_timer_handle_t s_timer;

/* ------------------------------------------------------------------------ */
/* Provider registry                                                         */
/* ------------------------------------------------------------------------ */

static const weather_provider_t *const s_providers[] = {
    &weather_provider_open_meteo,
};

const char *const *weather_provider_names(size_t *count)
{
    static const char *names[sizeof(s_providers) / sizeof(s_providers[0])];
    static bool filled;

    if (!filled) {
        for (size_t i = 0; i < sizeof(s_providers) / sizeof(s_providers[0]); i++) {
            names[i] = s_providers[i]->name;
        }
        filled = true;
    }
    if (count != NULL) {
        *count = sizeof(s_providers) / sizeof(s_providers[0]);
    }
    return names;
}

const weather_provider_t *weather_provider_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_providers) / sizeof(s_providers[0]); i++) {
        if (strcmp(s_providers[i]->name, name) == 0) {
            return s_providers[i];
        }
    }
    return NULL;
}

const weather_provider_t *weather_provider_current(void)
{
    const weather_provider_t *provider = weather_provider_find(s_provider);
    return (provider != NULL) ? provider : &weather_provider_open_meteo;
}

const char *weather_get_provider(void)
{
    return weather_provider_current()->name;
}

esp_err_t weather_set_provider(const char *name)
{
    if (name == NULL || weather_provider_find(name) == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    strlcpy(s_provider, name, sizeof(s_provider));
    esp_err_t err = sys_config_set_str(SYS_KEY_WEATHER_PROV, s_provider);
    SYS_LOGI(TAG, "provider set to '%s'", s_provider);
    (void)weather_refresh_async();
    return err;
}

/* ------------------------------------------------------------------------ */
/* Shared helpers (weather_internal.h)                                       */
/* ------------------------------------------------------------------------ */

esp_err_t weather_url_encode(const char *in, char *out, size_t out_len)
{
    if (in == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        const unsigned char c = (unsigned char)in[i];
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            if (o + 1 >= out_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[o++] = (char)c;
        } else {
            /* UTF-8 city names must be percent-encoded byte by byte. */
            if (o + 3 >= out_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[o++] = '%';
            out[o++] = hex[(c >> 4) & 0x0F];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
    return ESP_OK;
}

esp_err_t weather_read_api_key(char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return sys_config_get_str(SYS_KEY_WEATHER_KEY, out, len);
}

/** Accumulates the response body across HTTP_EVENT_ON_DATA callbacks. */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool overflow;
} http_body_t;

#define WEATHER_MAX_BODY (16 * 1024)

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_body_t *ctx = (http_body_t *)evt->user_data;
    if (ctx == NULL || evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    if (ctx->overflow) {
        return ESP_OK;
    }
    if (ctx->len + (size_t)evt->data_len >= WEATHER_MAX_BODY) {
        ctx->overflow = true;
        return ESP_OK;
    }

    if (ctx->buf == NULL) {
        ctx->cap = 2048;
        ctx->buf = malloc(ctx->cap);
        if (ctx->buf == NULL) {
            ctx->overflow = true;
            return ESP_OK;
        }
    }
    if (ctx->len + (size_t)evt->data_len + 1 > ctx->cap) {
        size_t new_cap = ctx->cap * 2;
        while (new_cap < ctx->len + (size_t)evt->data_len + 1) {
            new_cap *= 2;
        }
        char *grown = realloc(ctx->buf, new_cap);
        if (grown == NULL) {
            ctx->overflow = true;
            return ESP_OK;
        }
        ctx->buf = grown;
        ctx->cap = new_cap;
    }

    memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
    ctx->len += (size_t)evt->data_len;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

esp_err_t weather_http_get_json(const char *url, char **out)
{
    if (url == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    http_body_t ctx = { 0 };
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = CONFIG_TODOHELPER_WEATHER_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        /* Certificate bundle keeps HTTPS working without pinning a leaf cert. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .disable_auto_redirect = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        free(ctx.buf);
        return err;
    }
    if (ctx.overflow) {
        free(ctx.buf);
        return ESP_ERR_INVALID_SIZE;
    }
    if (status != 200) {
        SYS_LOGW(TAG, "HTTP %d for %s", status, url);
        free(ctx.buf);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (ctx.buf == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out = ctx.buf;
    return ESP_OK;
}

void weather_wmo_to_condition(int code, char *text, size_t text_len,
                              char *icon, size_t icon_len)
{
    const char *condition = "未知";
    const char *name = "w_overcast";

    switch (code) {
    case 0:
        condition = "晴";
        name = "w_sunny";
        break;
    case 1:
        condition = "少云";
        name = "w_cloudy";
        break;
    case 2:
        condition = "多云";
        name = "w_cloudy";
        break;
    case 3:
        condition = "阴";
        name = "w_overcast";
        break;
    case 45:
    case 48:
        condition = "雾";
        name = "w_fog";
        break;
    case 51:
    case 53:
    case 55:
        condition = "毛毛雨";
        name = "w_rain";
        break;
    case 56:
    case 57:
        condition = "冻雨";
        name = "w_rain";
        break;
    case 61:
        condition = "小雨";
        name = "w_rain";
        break;
    case 63:
        condition = "中雨";
        name = "w_rain";
        break;
    case 65:
        condition = "大雨";
        name = "w_rain";
        break;
    case 66:
    case 67:
        condition = "冻雨";
        name = "w_rain";
        break;
    case 71:
        condition = "小雪";
        name = "w_snow";
        break;
    case 73:
        condition = "中雪";
        name = "w_snow";
        break;
    case 75:
        condition = "大雪";
        name = "w_snow";
        break;
    case 77:
        condition = "米雪";
        name = "w_snow";
        break;
    case 80:
        condition = "阵雨";
        name = "w_rain";
        break;
    case 81:
        condition = "强阵雨";
        name = "w_rain";
        break;
    case 82:
        condition = "暴雨";
        name = "w_rain";
        break;
    case 85:
    case 86:
        condition = "阵雪";
        name = "w_snow";
        break;
    case 95:
        condition = "雷阵雨";
        name = "w_thunder";
        break;
    case 96:
    case 99:
        condition = "雷阵雨伴冰雹";
        name = "w_thunder";
        break;
    default:
        break;
    }

    if (text != NULL) {
        strlcpy(text, condition, text_len);
    }
    if (icon != NULL) {
        strlcpy(icon, name, icon_len);
    }
}

/* ------------------------------------------------------------------------ */
/* Cache                                                                     */
/* ------------------------------------------------------------------------ */

esp_err_t weather_save_cache(void)
{
    if (!storage_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const weather_data_t snapshot = s_data;
    const char *city = s_city;
    xSemaphoreGive(s_lock);

    /* Never store the API key here (requirement 8.8). */
    cJSON_AddStringToObject(root, "city", city);
    cJSON_AddNumberToObject(root, "lat", snapshot.lat);
    cJSON_AddNumberToObject(root, "lon", snapshot.lon);
    cJSON_AddNumberToObject(root, "temp", (double)snapshot.temp);
    cJSON_AddNumberToObject(root, "feels_like", (double)snapshot.feels_like);
    cJSON_AddNumberToObject(root, "humidity", snapshot.humidity);
    cJSON_AddStringToObject(root, "condition", snapshot.condition);
    cJSON_AddStringToObject(root, "icon", snapshot.icon);
    cJSON_AddNumberToObject(root, "weather_code", snapshot.weather_code);
    cJSON_AddNumberToObject(root, "update_ts", (double)snapshot.update_ts);
    cJSON_AddStringToObject(root, "provider", weather_provider_current()->name);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = storage_write_cache(WEATHER_CACHE_FILE, json, strlen(json));
    cJSON_free(json);
    return err;
}

esp_err_t weather_load_cache(void)
{
    if (!storage_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    char *json = NULL;
    size_t len = 0;
    esp_err_t err = storage_read_cache(WEATHER_CACHE_FILE, &json, &len);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    weather_data_t loaded = { 0 };
    const cJSON *item = NULL;

    if ((item = cJSON_GetObjectItemCaseSensitive(root, "city")) != NULL && cJSON_IsString(item)) {
        strlcpy(loaded.city, item->valuestring, sizeof(loaded.city));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "condition")) != NULL && cJSON_IsString(item)) {
        strlcpy(loaded.condition, item->valuestring, sizeof(loaded.condition));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "icon")) != NULL && cJSON_IsString(item)) {
        strlcpy(loaded.icon, item->valuestring, sizeof(loaded.icon));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "lat")) != NULL && cJSON_IsNumber(item)) {
        loaded.lat = item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "lon")) != NULL && cJSON_IsNumber(item)) {
        loaded.lon = item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "temp")) != NULL && cJSON_IsNumber(item)) {
        loaded.temp = (float)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "feels_like")) != NULL && cJSON_IsNumber(item)) {
        loaded.feels_like = (float)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "humidity")) != NULL && cJSON_IsNumber(item)) {
        loaded.humidity = (int)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "weather_code")) != NULL && cJSON_IsNumber(item)) {
        loaded.weather_code = (int)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "update_ts")) != NULL && cJSON_IsNumber(item)) {
        loaded.update_ts = (int64_t)item->valuedouble;
    }
    cJSON_Delete(root);

    loaded.valid = (loaded.city[0] != '\0') || (loaded.condition[0] != '\0');
    if (!loaded.valid) {
        return ESP_ERR_NOT_FOUND;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data = loaded;
    if (loaded.city[0] != '\0' && s_city[0] == '\0') {
        strlcpy(s_city, loaded.city, sizeof(s_city));
    }
    xSemaphoreGive(s_lock);

    SYS_LOGI(TAG, "cache loaded: %s %.1f C '%s'", loaded.city, (double)loaded.temp,
             loaded.condition);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Location                                                                  */
/* ------------------------------------------------------------------------ */

const char *weather_get_geo_endpoint(void)
{
    return s_geo_url;
}

esp_err_t weather_set_geo_endpoint(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        strlcpy(s_geo_url, WEATHER_GEO_ENDPOINT_DEFAULT, sizeof(s_geo_url));
        return sys_config_erase(SYS_KEY_GEO_URL);
    }
    if (strlen(url) >= sizeof(s_geo_url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(s_geo_url, url, sizeof(s_geo_url));
    return sys_config_set_str(SYS_KEY_GEO_URL, s_geo_url);
}

esp_err_t weather_set_location(double lat, double lon, const char *label)
{
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data.lat = lat;
    s_data.lon = lon;
    if (label != NULL && label[0] != '\0') {
        strlcpy(s_city, label, sizeof(s_city));
    }
    xSemaphoreGive(s_lock);

    if (label != NULL && label[0] != '\0') {
        (void)sys_config_set_str(SYS_KEY_CITY, s_city);
    }
    char coord[24];
    snprintf(coord, sizeof(coord), "%.4f", lat);
    (void)sys_config_set_str(SYS_KEY_CITY_LAT, coord);
    snprintf(coord, sizeof(coord), "%.4f", lon);
    (void)sys_config_set_str(SYS_KEY_CITY_LON, coord);

    SYS_LOGI(TAG, "location set to %.4f, %.4f ('%s')", lat, lon, s_city);
    return ESP_OK;
}

/** Read a number under any of the spellings the supported providers use. */
static bool json_number_any(const cJSON *root, const char *const *keys, size_t key_count,
                            double *out)
{
    for (size_t i = 0; i < key_count; i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
        if (cJSON_IsNumber(item)) {
            *out = item->valuedouble;
            return true;
        }
    }
    return false;
}

esp_err_t weather_detect_location(void)
{
    sys_status_t st;
    if (sys_status_get(&st) == ESP_OK && !st.online && !st.wifi_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    char *body = NULL;
    esp_err_t err = weather_http_get_json(s_geo_url, &body);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "IP geolocation request failed: %s", esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Some providers nest the coordinates under "location". */
    const cJSON *scope = root;
    const cJSON *nested = cJSON_GetObjectItemCaseSensitive(root, "location");
    if (cJSON_IsObject(nested)) {
        scope = nested;
    }

    static const char *const lat_keys[] = { "latitude", "lat" };
    static const char *const lon_keys[] = { "longitude", "lon", "lng" };
    double lat = 0;
    double lon = 0;

    if (!json_number_any(scope, lat_keys, 2, &lat) ||
        !json_number_any(scope, lon_keys, 3, &lon)) {
        /* A provider that reports a failure usually says so in "status"/"error". */
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
        SYS_LOGW(TAG, "geolocation response unusable (%s / %s)",
                 cJSON_IsString(status) ? status->valuestring : "?",
                 cJSON_IsString(reason) ? reason->valuestring : "-");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char label[64] = { 0 };
    const cJSON *city = cJSON_GetObjectItemCaseSensitive(scope, "city");
    const cJSON *region = cJSON_GetObjectItemCaseSensitive(scope, "region");
    if (cJSON_IsString(city) && city->valuestring[0] != '\0') {
        strlcpy(label, city->valuestring, sizeof(label));
    } else if (cJSON_IsString(region)) {
        strlcpy(label, region->valuestring, sizeof(label));
    }
    cJSON_Delete(root);

    err = weather_set_location(lat, lon, label[0] != '\0' ? label : NULL);
    if (err != ESP_OK) {
        return err;
    }
    SYS_LOGI(TAG, "located by IP: %s (%.4f, %.4f)", label, lat, lon);
    (void)sys_notify("weather_located", NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Refresh                                                                   */
/* ------------------------------------------------------------------------ */

static esp_err_t weather_do_refresh(void)
{
    sys_status_t st;
    if (sys_status_get(&st) == ESP_OK && !st.online && !st.wifi_connected) {
        SYS_LOGD(TAG, "offline, keeping cached reading");
        return ESP_ERR_INVALID_STATE;
    }

    const weather_provider_t *provider = weather_provider_current();
    if (provider->needs_api_key) {
        char key[80];
        if (weather_read_api_key(key, sizeof(key)) != ESP_OK || key[0] == '\0') {
            SYS_LOGW(TAG, "provider '%s' needs an API key", provider->name);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    char city[64];
    double lat = 0;
    double lon = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(city, s_city, sizeof(city));
    lat = s_data.lat;
    lon = s_data.lon;
    const bool have_coords = (s_data.lat != 0.0 || s_data.lon != 0.0) &&
                             (strcmp(s_data.city, s_city) == 0);
    xSemaphoreGive(s_lock);

    if (city[0] == '\0') {
        SYS_LOGD(TAG, "no city configured yet");
        return ESP_ERR_NOT_FOUND;
    }

    char label[64] = { 0 };
    if (!have_coords) {
        esp_err_t err = provider->geocode(city, &lat, &lon, label, sizeof(label));
        if (err != ESP_OK) {
            return err;
        }
    }

    weather_data_t fresh = { 0 };
    esp_err_t err = provider->fetch(lat, lon, &fresh);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(fresh.city, label[0] != '\0' ? label : city, sizeof(fresh.city));
    s_data = fresh;
    if (fresh.city[0] != '\0') {
        strlcpy(s_city, fresh.city, sizeof(s_city));
    }
    xSemaphoreGive(s_lock);

    /* Remember the resolved place so a later boot can show the right label, and
     * keep the coordinates in NVS for diagnostics/inspection. */
    (void)sys_config_set_str(SYS_KEY_CITY, s_city);
    char coord[24];
    snprintf(coord, sizeof(coord), "%.4f", lat);
    (void)sys_config_set_str(SYS_KEY_CITY_LAT, coord);
    snprintf(coord, sizeof(coord), "%.4f", lon);
    (void)sys_config_set_str(SYS_KEY_CITY_LON, coord);

    (void)weather_save_cache();

    (void)sys_event_post_to(WEATHER_EVENT, WEATHER_EVENT_UPDATED, NULL, 0);
    (void)sys_notify("weather_changed", NULL);
    return ESP_OK;
}

static void weather_task(void *arg)
{
    (void)arg;
    uint8_t opcode;

    while (xQueueReceive(s_requests, &opcode, portMAX_DELAY) == pdTRUE) {
        if (opcode == WEATHER_REQ_DETECT) {
            /* Priority 2 of the FR3 location list: only used when nothing was
             * chosen manually, or when the caller explicitly asked for it. */
            const esp_err_t geo_err = weather_detect_location();
            if (geo_err != ESP_OK) {
                SYS_LOGD(TAG, "IP geolocation unavailable (%s)", esp_err_to_name(geo_err));
            }
        }

        const esp_err_t err = weather_do_refresh();
        if (err != ESP_OK) {
            SYS_LOGW(TAG, "refresh failed: %s", esp_err_to_name(err));
            (void)sys_event_post_to(WEATHER_EVENT, WEATHER_EVENT_REFRESH_FAILED,
                                    &err, sizeof(err));
        }
        xSemaphoreGive(s_done);
    }
    vTaskDelete(NULL);
}

/** Queue an IP geolocation attempt followed by a refresh. */
static esp_err_t weather_detect_async(void)
{
    if (!s_initialized || s_requests == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t opcode = WEATHER_REQ_DETECT;
    if (xQueueSend(s_requests, &opcode, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;   /* a request is already queued */
    }
    return ESP_OK;
}

esp_err_t weather_refresh_async(void)
{
    if (!s_initialized || s_requests == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t opcode = WEATHER_REQ_REFRESH;
    if (xQueueSend(s_requests, &opcode, 0) != pdTRUE) {
        /* A refresh is already queued: coalescing is the desired behaviour. */
        return ESP_OK;
    }
    return ESP_OK;
}

esp_err_t weather_refresh(void)
{
    if (!s_initialized || s_requests == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)xSemaphoreTake(s_done, 0);   /* drop a stale completion */
    const uint8_t opcode = WEATHER_REQ_REFRESH;
    if (xQueueSend(s_requests, &opcode, 0) != pdTRUE) {
        /* Someone else is refreshing: wait for that result instead of piling on. */
    }

    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(WEATHER_REFRESH_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void refresh_timer_cb(void *arg)
{
    (void)arg;
    (void)weather_refresh_async();
}

/* ------------------------------------------------------------------------ */
/* Configuration / queries                                                   */
/* ------------------------------------------------------------------------ */

esp_err_t weather_get(weather_data_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_data;
    xSemaphoreGive(s_lock);

    return out->valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

const char *weather_get_city(void)
{
    return s_city;
}

esp_err_t weather_set_city(const char *city)
{
    if (city == NULL || city[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_city, city, sizeof(s_city));
    /* Force a geocode on the next refresh. */
    s_data.lat = 0;
    s_data.lon = 0;
    xSemaphoreGive(s_lock);

    esp_err_t err = sys_config_set_str(SYS_KEY_CITY, city);
    SYS_LOGI(TAG, "city set to '%s'", city);

    (void)weather_refresh_async();
    return err;
}

uint32_t weather_get_interval_s(void)
{
    return s_interval_s;
}

esp_err_t weather_set_interval_s(uint32_t seconds)
{
    if (seconds < 300) {
        seconds = 300;
    }
    if (seconds > 86400) {
        seconds = 86400;
    }

    s_interval_s = seconds;
    esp_err_t err = sys_config_set_i32(SYS_KEY_WEATHER_INT, (int32_t)seconds);

    if (s_timer != NULL) {
        (void)esp_timer_stop(s_timer);
        (void)esp_timer_start_periodic(s_timer, (uint64_t)seconds * 1000000ULL);
    }
    SYS_LOGI(TAG, "refresh interval set to %u s", (unsigned)seconds);
    return err;
}

esp_err_t weather_set_api_key(const char *key)
{
    if (key == NULL || key[0] == '\0') {
        (void)sys_config_erase(SYS_KEY_WEATHER_KEY);
        return ESP_OK;
    }
    /* Stored only; never logged and never returned by any API. */
    return sys_config_set_str(SYS_KEY_WEATHER_KEY, key);
}

bool weather_has_api_key(void)
{
    char key[80];
    return weather_read_api_key(key, sizeof(key)) == ESP_OK && key[0] != '\0';
}

/* ------------------------------------------------------------------------ */
/* Init                                                                      */
/* ------------------------------------------------------------------------ */

esp_err_t weather_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_done = xSemaphoreCreateBinary();
    s_requests = xQueueCreate(1, sizeof(uint8_t));
    if (s_done == NULL || s_requests == NULL) {
        return ESP_ERR_NO_MEM;
    }

    (void)sys_config_get_str_or(SYS_KEY_CITY, s_city, sizeof(s_city), "");
    (void)sys_config_get_str_or(SYS_KEY_WEATHER_PROV, s_provider, sizeof(s_provider),
                               WEATHER_PROVIDER_DEFAULT);
    (void)sys_config_get_str_or(SYS_KEY_GEO_URL, s_geo_url, sizeof(s_geo_url),
                               WEATHER_GEO_ENDPOINT_DEFAULT);
    s_interval_s = (uint32_t)sys_config_get_i32_or(SYS_KEY_WEATHER_INT,
                                                  CONFIG_TODOHELPER_WEATHER_INTERVAL_S);

    s_initialized = true;

    /* Cache first: the page must render immediately, even with no network. */
    if (weather_load_cache() != ESP_OK) {
        SYS_LOGI(TAG, "no cached weather yet");
    }

    if (xTaskCreate(weather_task, "th_weather", WEATHER_TASK_STACK, NULL,
                    WEATHER_TASK_PRIO, &s_task) != pdPASS) {
        SYS_LOGE(TAG, "refresh task creation failed");
        s_initialized = false;
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = refresh_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "th_weather",
    };
    if (esp_timer_create(&timer_args, &s_timer) == ESP_OK) {
        (void)esp_timer_start_periodic(s_timer, (uint64_t)s_interval_s * 1000000ULL);
    }

    esp_err_t http_err = weather_service_register_http();
    if (http_err != ESP_OK) {
        SYS_LOGW(TAG, "HTTP routes unavailable: %s", esp_err_to_name(http_err));
    }

    SYS_LOGI(TAG, "ready (provider '%s', city '%s', every %u s, geo endpoint '%s')",
             weather_provider_current()->name, s_city[0] ? s_city : "<unset>",
             (unsigned)s_interval_s, s_geo_url);

    if (s_city[0] != '\0') {
        /* A city is already configured: keep it and just refresh. */
        (void)weather_refresh_async();
    } else {
        /* FR3 location priority 2: with nothing configured, try the public IP so
         * the page is useful on the very first boot without any setup. */
        SYS_LOGI(TAG, "no city configured, trying IP geolocation");
        (void)weather_detect_async();
    }
    return ESP_OK;
}
