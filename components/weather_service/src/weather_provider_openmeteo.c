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

#include "sys_utils.h"

static const char *TAG = "weather_om";

/** Open-Meteo needs no API key at all, which keeps FR3 usable out of the box. */
#define OPEN_METEO_GEOCODE "https://geocoding-api.open-meteo.com/v1/search"
#define OPEN_METEO_FORECAST "https://api.open-meteo.com/v1/forecast"

/* ------------------------------------------------------------------------ */
/* Geocoding                                                                 */
/* ------------------------------------------------------------------------ */

static esp_err_t open_meteo_geocode(const char *city, double *lat, double *lon,
                                    char *label, size_t label_len)
{
    if (city == NULL || lat == NULL || lon == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char encoded[192];
    esp_err_t err = weather_url_encode(city, encoded, sizeof(encoded));
    if (err != ESP_OK) {
        return err;
    }

    char url[320];
    snprintf(url, sizeof(url), "%s?name=%s&count=1&language=zh&format=json",
             OPEN_METEO_GEOCODE, encoded);

    char *body = NULL;
    err = weather_http_get_json(url, &body);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "geocoding request failed: %s", esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        cJSON_Delete(root);
        SYS_LOGW(TAG, "city '%s' not found", city);
        return ESP_ERR_NOT_FOUND;
    }

    const cJSON *first = cJSON_GetArrayItem(results, 0);
    const cJSON *lat_item = cJSON_GetObjectItemCaseSensitive(first, "latitude");
    const cJSON *lon_item = cJSON_GetObjectItemCaseSensitive(first, "longitude");
    if (!cJSON_IsNumber(lat_item) || !cJSON_IsNumber(lon_item)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *lat = lat_item->valuedouble;
    *lon = lon_item->valuedouble;

    if (label != NULL && label_len > 0) {
        /* Prefer "name, admin1" when the provider supplies a region. */
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(first, "name");
        const cJSON *admin = cJSON_GetObjectItemCaseSensitive(first, "admin1");
        const char *name_str = cJSON_IsString(name) ? name->valuestring : city;
        if (cJSON_IsString(admin) && admin->valuestring[0] != '\0') {
            snprintf(label, label_len, "%s, %s", name_str, admin->valuestring);
        } else {
            strlcpy(label, name_str, label_len);
        }
    }

    cJSON_Delete(root);
    SYS_LOGI(TAG, "'%s' -> %.4f, %.4f", city, *lat, *lon);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Forecast                                                                  */
/* ------------------------------------------------------------------------ */

static esp_err_t open_meteo_fetch(double lat, double lon, weather_data_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[384];
    snprintf(url, sizeof(url),
             "%s?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code"
             "&timezone=auto",
             OPEN_METEO_FORECAST, lat, lon);

    char *body = NULL;
    esp_err_t err = weather_http_get_json(url, &body);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (!cJSON_IsObject(current)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *temp = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
    const cJSON *feels = cJSON_GetObjectItemCaseSensitive(current, "apparent_temperature");
    const cJSON *humidity = cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(current, "weather_code");

    if (!cJSON_IsNumber(temp)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    out->lat = lat;
    out->lon = lon;
    out->temp = (float)temp->valuedouble;
    out->feels_like = cJSON_IsNumber(feels) ? (float)feels->valuedouble : out->temp;
    out->humidity = cJSON_IsNumber(humidity) ? (int)humidity->valuedouble : 0;
    out->weather_code = cJSON_IsNumber(code) ? (int)code->valuedouble : -1;
    out->update_ts = sys_time_ts();
    out->valid = true;

    weather_wmo_to_condition(out->weather_code, out->condition, sizeof(out->condition),
                             out->icon, sizeof(out->icon));

    cJSON_Delete(root);

    SYS_LOGI(TAG, "%.1f C, feels %.1f C, %d%%, '%s' (code %d)",
             (double)out->temp, (double)out->feels_like, out->humidity,
             out->condition, out->weather_code);
    return ESP_OK;
}

const weather_provider_t weather_provider_open_meteo = {
    .name = "open-meteo",
    .needs_api_key = false,
    .geocode = open_meteo_geocode,
    .fetch = open_meteo_fetch,
};
