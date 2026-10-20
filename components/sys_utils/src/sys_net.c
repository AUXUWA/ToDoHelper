/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sys_net.c
 * @brief One HTTPS at a time: a shared lock for the outbound network requests.
 *
 * ## Why this exists
 *
 * On this board the internal heap is the scarce resource, and a TLS handshake
 * is not a small allocation: `mbedtls_ssl_setup()` grabs the session buffers
 * (mbedTLS sizes them from MBEDTLS_SSL_IN_CONTENT_LEN / OUT_CONTENT_LEN,
 * 16 KB each by default), plus the handshake and certificate state.
 *
 * The device used to start three HTTPS requests within a couple of seconds of
 * each other when the WiFi address arrived:
 *
 *   - the weather refresh (open-meteo),
 *   - the MaiMemo progress sync,
 *   - the cloud word queue (which talks to MaiMemo).
 *
 * With two handshakes in flight the internal heap could not satisfy the
 * allocation and mbedTLS reported it as
 *
 *   E esp-tls-mbedtls: mbedtls_ssl_setup returned -0x008D
 *
 * -0x008D is -141 = PSA_ERROR_INSUFFICIENT_MEMORY
 * (= MBEDTLS_ERR_SSL_ALLOC_FAILED) - **not** a certificate problem and not a
 * DNS problem. The request then failed with ESP_ERR_HTTP_CONNECT and had to
 * wait for a backoff window (up to 60 s) before it could try again.
 *
 * The fix is not "retry harder", it is "do not run two handshakes at once".
 * Every component that talks to the network takes this lock around its
 * request, so the radio and the heap see one connection at a time.
 *
 * ## The rules
 *
 * - The lock is **recursive**: a task that already owns it may take it again
 *   (FreeRTOS mutexes are recursive by default).
 * - The wait is **bounded**: a caller that cannot get the lock in time gets
 *   ESP_ERR_TIMEOUT instead of blocking forever, so one slow request can never
 *   wedge the others.
 * - **Nothing else may be done while holding the lock** - no SD card writes,
 *   no LVGL calls. The lock is for the network round trip only.
 */

#include "sys_net.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sys_net";

/**
 * How long a caller waits for the lock before giving up.
 *
 * A TLS handshake plus a small HTTPS request is a couple of seconds on this
 * radio (measured: the maimemo handshake 6.6 s from start to "Certificate
 * validated" on a cold DNS). 20 s leaves room for two queued requests and
 * still fails fast enough that a stuck holder cannot wedge the boot.
 */
#define SYS_NET_LOCK_WAIT_MS 20000

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

static SemaphoreHandle_t net_lock_ensure(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "cannot create the network lock");
        }
    }
    return s_lock;
}

esp_err_t sys_net_lock(uint32_t timeout_ms)
{
    SemaphoreHandle_t lock = net_lock_ensure();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const TickType_t ticks = (timeout_ms == 0)
        ? portMAX_DELAY
        : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(lock, ticks) != pdTRUE) {
        /* Somebody else is holding it for a long time. Say so instead of
         * waiting forever - the caller can decide what to do. */
        ESP_LOGW(TAG, "the network lock is busy after %u ms", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void sys_net_unlock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}
