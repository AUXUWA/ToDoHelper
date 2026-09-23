/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file calendar_service.h
 * @brief FR4: wall clock, timezone and month calendar.
 *
 * **This board has no RTC** (docs/TODOHELPER_REQUIREMENTS.md 4). Until SNTP has
 * completed, sys_time_ts() returns 0 and every time-dependent UI must show
 * "时间未同步" instead of 1970. Power loss always loses the time.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "esp_event.h"

#include "sys_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(CALENDAR_EVENT);

typedef enum {
    CALENDAR_EVENT_TIME_SYNCED = 0, /**< payload: int64_t unix seconds */
    CALENDAR_EVENT_SYNC_FAILED,     /**< no payload */
    CALENDAR_EVENT_TIMEZONE_CHANGED /**< payload: const char *tz */
} calendar_event_id_t;

/** Timezone used when the NVS key is absent: China Standard Time. */
#define CALENDAR_DEFAULT_TZ "CST-8"
/** NTP server used when the NVS key is absent. */
#define CALENDAR_DEFAULT_NTP "ntp.aliyun.com"

/**
 * @brief Apply the stored timezone and start SNTP (once the network is up).
 *
 * Without a network the component stays idle and reports "not synced"; it
 * subscribes to SYS_EVENT_CONNECTIVITY and starts SNTP when the link comes up.
 */
esp_err_t calendar_service_init(void);

/**
 * @brief Wait for a time synchronisation.
 *
 * @param timeout_ms 0 uses the default (10 s).
 * @return ESP_OK when the clock is synchronised, ESP_ERR_TIMEOUT otherwise.
 */
esp_err_t calendar_sync_ntp(uint32_t timeout_ms);

/** @brief True once a real wall-clock time is available. */
bool calendar_time_synced(void);

/**
 * @brief Current local time.
 *
 * @return ESP_ERR_INVALID_STATE while the clock has never been synchronised.
 */
esp_err_t calendar_get_now(struct tm *out);

/** @brief Current time as a "YYYY-MM-DD HH:MM:SS" string, or the unsynced text. */
esp_err_t calendar_format_now(char *out, size_t len);

/**
 * @brief Fill one entry per day of @p month (1..12).
 *
 * Each entry carries tm_year/tm_mon/tm_mday/tm_wday; @p count is set to the
 * number of days in the month. The clock does not need to be synchronised,
 * because the values are pure calendar arithmetic.
 */
esp_err_t calendar_get_month(int year, int month, struct tm *days, size_t *count);

/** @brief Day of week of the 1st of the month, 0 = Sunday. */
esp_err_t calendar_first_weekday(int year, int month, int *out_wday);

/**
 * @brief Change the timezone (POSIX TZ string such as "CST-8"), persist it and
 *        re-render the clock.
 */
esp_err_t calendar_set_timezone(const char *tz);

/** @brief Currently active POSIX TZ string. */
const char *calendar_get_timezone(void);

/**
 * @brief Override the wall clock manually (settings page / HTTP API).
 *
 * The value is seconds since the Unix epoch; it is not persisted, so a reboot
 * requires a new sync.
 */
esp_err_t calendar_set_time(int64_t unix_seconds);

/** @brief Set the NTP server name and persist it. */
esp_err_t calendar_set_ntp_server(const char *server);

/** @brief Currently configured NTP server. */
const char *calendar_get_ntp_server(void);

/** @brief Publish the /api/v1/calendar/... routes (requirement 8.4). */
esp_err_t calendar_service_register_http(void);

#ifdef __cplusplus
}
#endif
