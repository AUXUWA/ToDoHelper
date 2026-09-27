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
    CALENDAR_EVENT_TIMEZONE_CHANGED,/**< payload: const char *tz */
    /**
     * Result of a *requested* sync, i.e. calendar_sync_ntp_async().
     *
     * payload: esp_err_t (ESP_OK on success, ESP_ERR_TIMEOUT on failure).
     * The unsolicited background sync only posts TIME_SYNCED/SYNC_FAILED; this
     * event exists so a UI that asked for a sync can tell the user the outcome
     * without ever blocking on the network (see calendar_sync_ntp_async()).
     */
    CALENDAR_EVENT_SYNC_RESULT
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
 *
 * @warning **Never call this from the LVGL task.** It blocks for up to
 *          @p timeout_ms on a network round trip, and every LVGL timer/event
 *          callback runs with the adapter lock held (see AGENT.md 9.0.1), so
 *          calling it from a button callback freezes the whole UI for the
 *          entire timeout. Use calendar_sync_ntp_async() from UI code.
 *          It is safe to call from a dedicated worker task (HTTP handlers do
 *          this deliberately when the caller wants a synchronous answer).
 */
esp_err_t calendar_sync_ntp(uint32_t timeout_ms);

/**
 * @brief Request a time synchronisation without blocking the caller.
 *
 * Returns immediately. The work runs on calendar_service's own worker task and
 * the outcome is reported as CALENDAR_EVENT_SYNC_RESULT (payload: esp_err_t),
 * so a page can subscribe with ui_event_subscribe() and show a toast when it
 * lands. This is the variant every UI path must use.
 *
 * @return ESP_OK if the request was queued, ESP_ERR_INVALID_STATE if the
 *         service is not initialised, ESP_ERR_TIMEOUT if a request is already
 *         in flight (the caller may treat that as "already syncing").
 */
esp_err_t calendar_sync_ntp_async(void);

/** @brief True while a calendar_sync_ntp_async() request is in flight. */
bool calendar_sync_ntp_pending(void);

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
