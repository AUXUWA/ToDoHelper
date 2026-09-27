/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "calendar_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_netif_sntp.h"
#include "esp_heap_caps.h"
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "calendar";

ESP_EVENT_DEFINE_BASE(CALENDAR_EVENT);

#ifndef CONFIG_TODOHELPER_NTP_SERVER
#define CONFIG_TODOHELPER_NTP_SERVER "ntp.aliyun.com"
#endif
#ifndef CONFIG_TODOHELPER_TZ_DEFAULT
#define CONFIG_TODOHELPER_TZ_DEFAULT "CST-8"
#endif

#define CALENDAR_DEFAULT_SYNC_TIMEOUT_MS 10000

static bool s_initialized;
static bool s_sntp_started;
static char s_tz[48] = CONFIG_TODOHELPER_TZ_DEFAULT;
static char s_ntp[64] = CONFIG_TODOHELPER_NTP_SERVER;

/*
 * Async sync worker.
 *
 * calendar_sync_ntp() blocks on a network round trip for up to 10 s. Every LVGL
 * timer and widget event callback runs with the adapter lock held (the adapter
 * calls lv_timer_handler() under it, see AGENT.md 9.0.1), so a button callback
 * that waited for NTP used to freeze the entire UI - and, because
 * ui_post_event() also takes that lock, the whole event system behind it.
 *
 * The worker is created once, on first request, and then parks on a task
 * notification. It is intentionally *not* created in init: a device that never
 * asks for a manual sync (the common case - SNTP starts by itself when the link
 * comes up) pays nothing for it.
 */
#define CALENDAR_SYNC_TASK_STACK 3072
#define CALENDAR_SYNC_TASK_PRIO  4

static TaskHandle_t s_sync_task;
static volatile bool s_sync_pending;
/** Serialises lazy worker creation without pulling in a mutex at init time. */
static StaticSemaphore_t s_sync_lock_buf;
static SemaphoreHandle_t s_sync_lock;

static void sync_worker(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* calendar_sync_ntp() posts SYNC_FAILED itself; a *requested* sync also
         * reports back through SYNC_RESULT so the UI can toast the outcome. */
        const esp_err_t err = calendar_sync_ntp(0);

        s_sync_pending = false;
        (void)sys_event_post_to(CALENDAR_EVENT, CALENDAR_EVENT_SYNC_RESULT,
                                &err, sizeof(err));
    }
}

esp_err_t calendar_sync_ntp_async(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sync_pending) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_sync_lock == NULL) {
        s_sync_lock = xSemaphoreCreateMutexStatic(&s_sync_lock_buf);
        if (s_sync_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_sync_lock, portMAX_DELAY);
    if (s_sync_task == NULL) {
        /* Internal RAM on purpose: this task never writes flash, but it is small
         * and the pool that matters on this board is internal (AGENT.md 9.0). */
        const BaseType_t ok = xTaskCreate(sync_worker, "th_ntp",
                                          CALENDAR_SYNC_TASK_STACK, NULL,
                                          CALENDAR_SYNC_TASK_PRIO, &s_sync_task);
        if (ok != pdPASS) {
            s_sync_task = NULL;
            xSemaphoreGive(s_sync_lock);
            SYS_LOGE(TAG, "cannot create the sync worker");
            return ESP_ERR_NO_MEM;
        }
    }
    TaskHandle_t task = s_sync_task;
    xSemaphoreGive(s_sync_lock);

    s_sync_pending = true;
    SYS_LOGI(TAG, "async NTP sync requested");
    xTaskNotifyGive(task);
    return ESP_OK;
}

bool calendar_sync_ntp_pending(void)
{
    return s_sync_pending;
}

/* ------------------------------------------------------------------------ */
/* Timezone                                                                  */
/* ------------------------------------------------------------------------ */

static void apply_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

const char *calendar_get_timezone(void)
{
    return s_tz;
}

esp_err_t calendar_set_timezone(const char *tz)
{
    if (tz == NULL || tz[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(tz) >= sizeof(s_tz)) {
        return ESP_ERR_INVALID_SIZE;
    }

    strlcpy(s_tz, tz, sizeof(s_tz));
    apply_tz(s_tz);

    const esp_err_t err = sys_config_set_str(SYS_KEY_TZ, s_tz);
    SYS_LOGI(TAG, "timezone set to '%s'", s_tz);

    (void)sys_event_post_to(CALENDAR_EVENT, CALENDAR_EVENT_TIMEZONE_CHANGED,
                            s_tz, strlen(s_tz) + 1);
    (void)sys_status_publish();
    return err;
}

/* ------------------------------------------------------------------------ */
/* SNTP                                                                      */
/* ------------------------------------------------------------------------ */

static void sntp_sync_cb(struct timeval *tv)
{
    /* SNTP has already applied the offset to the system clock; anchor
     * sys_utils' monotonic-based wall clock to the same value so that
     * sys_time_ts() starts returning real Unix seconds. */
    const int64_t ts = (tv != NULL) ? (int64_t)tv->tv_sec : (int64_t)time(NULL);
    sys_time_set_ts(ts);

    SYS_LOGI(TAG, "time synchronised: %lld", (long long)ts);
    (void)sys_event_post_to(CALENDAR_EVENT, CALENDAR_EVENT_TIME_SYNCED, &ts, sizeof(ts));
    (void)sys_status_publish();
}

static esp_err_t sntp_start(void)
{
    if (s_sntp_started) {
        return ESP_OK;
    }

    /*
     * Warm up esp_libc's timekeeping lock before the network stack needs it.
     *
     * lwIP's SNTP client calls settimeofday() **from its tcpip_thread** as soon as
     * the first reply arrives, and esp_libc creates the lock for its timekeeping
     * lazily on that first call. That lazy creation returned NULL on the device
     * and IDF aborts when it does (esp_libc/src/locks.c:77):
     *
     *   sntp_recv -> sntp_set_system_time -> settimeofday
     *     -> esp_libc_timekeeping_set_realtime_us -> lock_acquire_generic
     *     -> lock_init_generic -> xQueueCreateMutex() == NULL -> abort()
     *
     * Doing it here, on a normal task with the scheduler settled and plenty of
     * stack, means the first call from the network stack finds the lock already
     * created and performs no allocation. The value written is the clock's
     * current value, so this is a semantic no-op.
     */
    struct timeval now;
    if (gettimeofday(&now, NULL) == 0) {
        (void)settimeofday(&now, NULL);
    }
    if (!heap_caps_check_integrity_all(false)) {
        SYS_LOGE(TAG, "heap integrity check FAILED before starting SNTP - "
                      "expect the libc lock creation to fail");
    }

    const esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp);
    esp_sntp_config_t active = cfg;
    active.sync_cb = sntp_sync_cb;
    /* Do not block the caller: the sync callback reports completion. */
    active.wait_for_sync = false;

    esp_err_t err = esp_netif_sntp_init(&active);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Already initialised by a previous call. */
        s_sntp_started = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_sntp_started = true;
    SYS_LOGI(TAG, "SNTP started (%s)", s_ntp);
    return ESP_OK;
}

/* Start SNTP as soon as we have connectivity, whenever that happens. */
static void connectivity_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;

    const bool *online = (const bool *)data;
    if (online == NULL || !*online) {
        return;
    }
    if (calendar_time_synced()) {
        return;
    }
    (void)sntp_start();
}

esp_err_t calendar_sync_ntp(uint32_t timeout_ms)
{
    if (calendar_time_synced()) {
        return ESP_OK;
    }

    esp_err_t err = sntp_start();
    if (err != ESP_OK) {
        return err;
    }

    const uint32_t timeout = (timeout_ms > 0) ? timeout_ms : CALENDAR_DEFAULT_SYNC_TIMEOUT_MS;
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout));
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "NTP sync not completed within %u ms", (unsigned)timeout);
        (void)sys_event_post_to(CALENDAR_EVENT, CALENDAR_EVENT_SYNC_FAILED, NULL, 0);
        return ESP_ERR_TIMEOUT;
    }
    return calendar_time_synced() ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool calendar_time_synced(void)
{
    return sys_time_synced();
}

/* ------------------------------------------------------------------------ */
/* Queries                                                                   */
/* ------------------------------------------------------------------------ */

esp_err_t calendar_get_now(struct tm *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sys_time_synced()) {
        /* Requirement FR4: never fabricate 1970. */
        return ESP_ERR_INVALID_STATE;
    }

    const time_t now = (time_t)sys_time_ts();
    if (localtime_r(&now, out) == NULL) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t calendar_format_now(char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    struct tm now;
    if (calendar_get_now(&now) != ESP_OK) {
        strlcpy(out, "时间未同步", len);
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(out, len, "%04d-%02d-%02d %02d:%02d:%02d",
             now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
             now.tm_hour, now.tm_min, now.tm_sec);
    return ESP_OK;
}

/**
 * Days in @p month. Works for the Gregorian calendar, which is what the device
 * clock uses; no need for anything more clever.
 */
static int days_in_month(int year, int month)
{
    static const int table[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2) {
        const bool leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return table[month - 1];
}

esp_err_t calendar_first_weekday(int year, int month, int *out_wday)
{
    if (out_wday == NULL || month < 1 || month > 12) {
        return ESP_ERR_INVALID_ARG;
    }

    struct tm first = { 0 };
    first.tm_year = year - 1900;
    first.tm_mon = month - 1;
    first.tm_mday = 1;
    first.tm_hour = 12;   /* midday avoids DST edge cases around midnight */
    first.tm_isdst = -1;

    if (mktime(&first) == (time_t)-1) {
        return ESP_FAIL;
    }
    *out_wday = first.tm_wday;
    return ESP_OK;
}

esp_err_t calendar_get_month(int year, int month, struct tm *days, size_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (month < 1 || month > 12 || year < 1970 || year > 2200) {
        return ESP_ERR_INVALID_ARG;
    }

    const int total = days_in_month(year, month);
    int wday = 0;
    esp_err_t err = calendar_first_weekday(year, month, &wday);
    if (err != ESP_OK) {
        return err;
    }

    if (days != NULL) {
        for (int day = 0; day < total; day++) {
            struct tm *entry = &days[day];
            memset(entry, 0, sizeof(*entry));
            entry->tm_year = year - 1900;
            entry->tm_mon = month - 1;
            entry->tm_mday = day + 1;
            entry->tm_wday = (wday + day) % 7;
            entry->tm_isdst = -1;
        }
    }

    *count = (size_t)total;
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Manual override                                                           */
/* ------------------------------------------------------------------------ */

esp_err_t calendar_set_time(int64_t unix_seconds)
{
    if (unix_seconds <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const struct timeval tv = { .tv_sec = (time_t)unix_seconds, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }

    sys_time_set_ts(unix_seconds);
    SYS_LOGI(TAG, "clock set manually to %lld", (long long)unix_seconds);
    (void)sys_event_post_to(CALENDAR_EVENT, CALENDAR_EVENT_TIME_SYNCED,
                            &unix_seconds, sizeof(unix_seconds));
    (void)sys_status_publish();
    return ESP_OK;
}

const char *calendar_get_ntp_server(void)
{
    return s_ntp;
}

esp_err_t calendar_set_ntp_server(const char *server)
{
    if (server == NULL || server[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(server) >= sizeof(s_ntp)) {
        return ESP_ERR_INVALID_SIZE;
    }

    strlcpy(s_ntp, server, sizeof(s_ntp));
    const esp_err_t err = sys_config_set_str(SYS_KEY_NTP_SERVER, s_ntp);
    SYS_LOGI(TAG, "NTP server set to '%s'", s_ntp);

    /* A running SNTP client keeps the old server until restarted. */
    if (s_sntp_started) {
        esp_netif_sntp_deinit();
        s_sntp_started = false;
        (void)sntp_start();
    }
    return err;
}

/* ------------------------------------------------------------------------ */
/* Init                                                                      */
/* ------------------------------------------------------------------------ */

esp_err_t calendar_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    (void)sys_config_get_str_or(SYS_KEY_TZ, s_tz, sizeof(s_tz), CONFIG_TODOHELPER_TZ_DEFAULT);
    (void)sys_config_get_str_or(SYS_KEY_NTP_SERVER, s_ntp, sizeof(s_ntp),
                               CONFIG_TODOHELPER_NTP_SERVER);
    apply_tz(s_tz);

    s_initialized = true;

    /* Watch connectivity so SNTP starts as soon as the link is up, even if that
     * happens long after boot. */
    esp_err_t err = sys_event_subscribe(SYS_EVENT, SYS_EVENT_CONNECTIVITY,
                                        connectivity_handler, NULL, NULL);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "connectivity subscription failed: %s", esp_err_to_name(err));
    }

    /* If WiFi is already connected, start immediately instead of waiting for
     * the next connectivity event. */
    sys_status_t st;
    if (sys_status_get(&st) == ESP_OK && (st.wifi_connected || st.online)) {
        (void)sntp_start();
    }

    esp_err_t http_err = calendar_service_register_http();
    if (http_err != ESP_OK) {
        SYS_LOGW(TAG, "HTTP routes unavailable: %s", esp_err_to_name(http_err));
    }

    SYS_LOGI(TAG, "ready (tz '%s', ntp '%s', synced=%d)", s_tz, s_ntp, calendar_time_synced());
    return ESP_OK;
}
