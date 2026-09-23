/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file weather_service.h
 * @brief FR3: weather for a manually selected city.
 *
 * Everything here comes from a network API - the board has **no** temperature
 * sensor and **no** GPS (requirements FR3 / 4), so:
 *   - the city is set manually (the only P0 path);
 *   - offline means "show the last cached reading", never a blank page.
 *
 * The provider is pluggable. The default is **Open-Meteo**, which needs no API
 * key, so the feature works out of the box; `weather_key` in NVS is reserved
 * for providers that do (HeFeng / OpenWeather), see docs/API.md.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#include "sys_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(WEATHER_EVENT);

typedef enum {
    WEATHER_EVENT_UPDATED = 0,   /**< no payload: read weather_get() */
    WEATHER_EVENT_REFRESH_FAILED /**< payload: esp_err_t */
} weather_event_id_t;

/** Field names follow docs/TODOHELPER_REQUIREMENTS.md 7.4. */
typedef struct {
    char city[64];
    double lat;
    double lon;
    float temp;
    float feels_like;
    int humidity;
    char condition[64];
    char icon[24];          /**< resource base name of the mmap'd icon, e.g. "w_sunny" */
    int weather_code;       /**< raw provider code, for debugging */
    int64_t update_ts;      /**< Unix seconds of the reading */
    bool valid;             /**< a reading (fresh or cached) is available */
} weather_data_t;

/** Default refresh period (requirement FR3: 15 minutes). */
#define WEATHER_DEFAULT_INTERVAL_S 900
/** Readings older than this are flagged as stale in the HTTP API. */
#define WEATHER_STALE_AFTER_S (2 * WEATHER_DEFAULT_INTERVAL_S)

/** Name of the default, key-less provider. */
#define WEATHER_PROVIDER_DEFAULT "open-meteo"

/**
 * @brief Load the cache and start the refresh worker + periodic timer.
 *
 * Never fails because of the network: an unreachable API only means
 * WEATHER_EVENT_REFRESH_FAILED and the cached reading stays visible.
 */
esp_err_t weather_service_init(void);

/**
 * @brief Trigger a refresh and wait for it (up to ~15 s).
 *
 * @return ESP_ERR_INVALID_STATE when offline, ESP_ERR_NOT_FOUND when no city
 *         has been chosen yet, ESP_ERR_TIMEOUT when the request took too long.
 */
esp_err_t weather_refresh(void);

/** @brief Trigger a refresh without waiting; the result arrives as an event. */
esp_err_t weather_refresh_async(void);

/** @brief Last known reading (fresh or cached). */
esp_err_t weather_get(weather_data_t *out);

/** @brief Persist and adopt @p city, then refresh in the background. */
esp_err_t weather_set_city(const char *city);

/** @brief Currently configured city, "" when unset. */
const char *weather_get_city(void);

/** @brief Load the reading stored in cache/weather.json. */
esp_err_t weather_load_cache(void);

/** @brief Store the current reading in cache/weather.json. */
esp_err_t weather_save_cache(void);

/** @brief Selected provider name. */
const char *weather_get_provider(void);

/** @brief Select a provider by name and persist it. */
esp_err_t weather_set_provider(const char *name);

/** @brief Refresh period in seconds. */
uint32_t weather_get_interval_s(void);

/** @brief Change the refresh period (seconds), clamped to a sane range. */
esp_err_t weather_set_interval_s(uint32_t seconds);

/** @brief Store the provider API key (never returned by any API). */
esp_err_t weather_set_api_key(const char *key);

/** @brief True when an API key is configured (the key itself is never exposed). */
bool weather_has_api_key(void);

/** @brief Publish /api/v1/weather* routes (requirement 8.4). */
esp_err_t weather_service_register_http(void);

#ifdef __cplusplus
}
#endif
