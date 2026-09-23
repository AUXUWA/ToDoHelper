/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sys_status.h
 * @brief Read-only snapshot of system state, published by L3 services.
 *
 * The status bar (L2) must not depend on wifi_manager / todo_manager (L3), so
 * services instead publish their state here and everyone else reads it. This is
 * the only sanctioned "shared state" in the project; everything else is events.
 *
 * Writers use sys_status_update() which mutates under a lock and then posts
 * SYS_EVENT_STATUS_CHANGED. Readers use sys_status_get().
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "sys_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_STATUS_SSID_LEN 33
#define SYS_STATUS_IP_LEN   16

typedef struct {
    /* WiFi (FR1) */
    bool wifi_connected;
    bool wifi_connecting;
    char wifi_ssid[SYS_STATUS_SSID_LEN];
    char wifi_ip[SYS_STATUS_IP_LEN];
    int8_t wifi_rssi;

    /** Internet reachable (heuristic: got IP + at least one successful request). */
    bool online;

    /* Time (FR4). false => UI must show "时间未同步", never 1970. */
    bool time_synced;
    int64_t now_ts;

    /* Todo sync (FR2) */
    int32_t todo_total;
    int32_t todo_pending;   /**< items whose sync_state != TODO_SYNC_OK */
    bool syncing;

    /* Storage (FR2/FR3 degradation) */
    bool sd_available;
    bool storage_available;
} sys_status_t;

/**
 * @brief Copy the current snapshot.
 */
esp_err_t sys_status_get(sys_status_t *out);

/**
 * @brief Mutate the snapshot under a lock, then publish SYS_EVENT_STATUS_CHANGED.
 *
 * @param mutator Called with the live struct; must not block or allocate much.
 */
esp_err_t sys_status_update(void (*mutator)(sys_status_t *status, void *arg), void *arg);

/**
 * @brief Force SYS_EVENT_STATUS_CHANGED (after a service updated state itself).
 */
esp_err_t sys_status_publish(void);

#ifdef __cplusplus
}
#endif
