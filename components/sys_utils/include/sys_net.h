/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sys_net.h
 * @brief A shared lock that keeps the device from running two HTTPS requests
 *        at the same time.
 *
 * ## Why
 *
 * A TLS handshake is a large allocation (mbedTLS sizes the session buffers
 * from MBEDTLS_SSL_IN_CONTENT_LEN / OUT_CONTENT_LEN, 16 KB each by default),
 * and the internal heap on this board is the scarce resource - the WiFi PHY
 * needs a ~30 KB contiguous block to re-enable (AGENT.md 9.0.3).
 *
 * When the WiFi address arrived, three HTTPS requests used to start within a
 * couple of seconds of each other (the weather refresh, the MaiMemo progress
 * sync and the cloud word queue). Two handshakes in flight were enough to
 * make mbedTLS fail with
 *
 *   E esp-tls-mbedtls: mbedtls_ssl_setup returned -0x008D
 *
 * (-0x008D = PSA_ERROR_INSUFFICIENT_MEMORY, **not** a certificate error),
 * and the request then sat in a backoff window for up to 60 s.
 *
 * So every component that talks to the network takes this lock around its
 * request. The radio and the heap then see one connection at a time.
 *
 * ## How to use
 *
 * @code
 * if (sys_net_lock(SYS_NET_LOCK_DEFAULT_MS) == ESP_OK) {
 *     err = do_the_https_request();
 *     sys_net_unlock();
 * }
 * @endcode
 *
 * The lock is **recursive** (FreeRTOS mutexes are), so a task that already
 * owns it may take it again. The wait is **bounded**: a caller that cannot get
 * the lock in time gets ESP_ERR_TIMEOUT rather than blocking forever.
 *
 * **Never do anything else while holding it** - no SD card writes, no LVGL
 * calls, no long computations. It is for the network round trip only.
 */

#ifndef SYS_NET_H
#define SYS_NET_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default wait for the network lock (ms). */
#define SYS_NET_LOCK_DEFAULT_MS 20000

/**
 * @brief Take the shared network lock.
 *
 * @param timeout_ms How long to wait at most.
 *                   0 means "wait forever" - use that only where blocking is
 *                   genuinely fine (never in an LVGL callback).
 * @return ESP_OK when the lock was taken, ESP_ERR_TIMEOUT when it was busy,
 *         ESP_ERR_NO_MEM when the lock could not be created.
 */
esp_err_t sys_net_lock(uint32_t timeout_ms);

/**
 * @brief Release the shared network lock.
 *
 * Safe to call when the lock was not taken by this task: it is a no-op then.
 */
void sys_net_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* SYS_NET_H */
