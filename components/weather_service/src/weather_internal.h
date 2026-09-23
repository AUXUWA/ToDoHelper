/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file weather_internal.h
 * @brief Provider interface and shared helpers. Not a public header.
 */

#pragma once

#include <stddef.h>

#include "esp_err.h"

#include "weather_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Short name used in NVS and the HTTP API. */
    const char *name;
    /** True when the provider cannot work without weather_key in NVS. */
    bool needs_api_key;
    /**
     * Resolve a city name to coordinates.
     *
     * @param label Optional display name returned by the provider (may be NULL).
     */
    esp_err_t (*geocode)(const char *city, double *lat, double *lon,
                         char *label, size_t label_len);
    /** Fetch the current reading for a coordinate. */
    esp_err_t (*fetch)(double lat, double lon, weather_data_t *out);
} weather_provider_t;

/** Provider selected through the NVS key `weather_prov`. */
const weather_provider_t *weather_provider_current(void);

/** Look a provider up by name; NULL when unknown. */
const weather_provider_t *weather_provider_find(const char *name);

/** Names of all compiled-in providers, for the settings page / API. */
const char *const *weather_provider_names(size_t *count);

/** Open-Meteo (no API key required). */
extern const weather_provider_t weather_provider_open_meteo;

/**
 * @brief GET a URL and return the body as a NUL-terminated heap string.
 *
 * The response is capped at 16 KB and requires HTTP 200.
 */
esp_err_t weather_http_get_json(const char *url, char **out);

/** @brief Percent-encode @p in for use in a query string. */
esp_err_t weather_url_encode(const char *in, char *out, size_t out_len);

/** @brief Stable identifier for an API key lookup, used by provider checks. */
esp_err_t weather_read_api_key(char *out, size_t len);

/**
 * @brief Map a WMO weather code to a Chinese description and icon base name.
 *
 * `icon` receives a name matching the packed assets, e.g. "w_sunny".
 */
void weather_wmo_to_condition(int code, char *text, size_t text_len,
                              char *icon, size_t icon_len);

#ifdef __cplusplus
}
#endif
