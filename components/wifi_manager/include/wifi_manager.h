/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wifi_manager.h
 * @brief FR1: WiFi scan, connect, persist and auto-reconnect.
 *
 * The component owns the station netif and publishes its state to
 * sys_status_* so that the status bar (L2) never has to call into it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"

#include "sys_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(WIFI_MGR_EVENT);

typedef enum {
    WIFI_MGR_EVENT_SCAN_DONE = 0,  /**< no payload */
    WIFI_MGR_EVENT_CONNECTING,     /**< no payload */
    WIFI_MGR_EVENT_CONNECTED,      /**< no payload: associated, no IP yet */
    WIFI_MGR_EVENT_GOT_IP,         /**< no payload: read wifi_manager_get_ip() */
    WIFI_MGR_EVENT_DISCONNECTED,   /**< payload: uint8_t reason */
    WIFI_MGR_EVENT_FAILED,         /**< payload: uint8_t last disconnect reason */
} wifi_mgr_event_id_t;

/** One scan result. Field names follow docs/TODOHELPER_REQUIREMENTS.md 7.2. */
typedef struct {
    char ssid[33];
    int8_t rssi;
    wifi_auth_mode_t authmode;
    uint8_t channel;
    uint8_t bssid[6];
} wifi_ap_info_t;

typedef enum {
    WIFI_MGR_DISCONNECTED = 0,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,
    WIFI_MGR_FAILED,
} wifi_mgr_state_t;

/** Maximum number of access points kept from one scan. */
#define WIFI_MANAGER_MAX_APS 24

/** Longest SSID accepted from callers (NUL not included). */
#define WIFI_MANAGER_SSID_MAX 32
/** Longest passphrase accepted from callers. */
#define WIFI_MANAGER_PASS_MAX 64

/** Default hostname advertised over DHCP and used for mDNS. */
#define WIFI_MANAGER_HOSTNAME "todohelper"

/**
 * @brief Create the station netif, init WiFi and register event handlers.
 *
 * Idempotent. Does not connect; call wifi_manager_auto_connect() for that.
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Start a scan and return immediately. Posts WIFI_MGR_EVENT_SCAN_DONE.
 */
esp_err_t wifi_manager_start_scan(void);

/**
 * @brief Blocking scan. Convenient for HTTP handlers and tests.
 *
 * @param[out] aps   Destination array, may be NULL when only the count matters.
 * @param[in]  max   Capacity of @p aps.
 * @param[out] count Number of results stored.
 *
 * @warning Must not be called from the WiFi event task or the shared event loop.
 */
esp_err_t wifi_manager_scan(wifi_ap_info_t *aps, size_t max, size_t *count);

/**
 * @brief Read the cached result of the last completed scan.
 */
esp_err_t wifi_manager_get_scan_result(wifi_ap_info_t *aps, size_t max, size_t *count);

/**
 * @brief Connect to @p ssid. Returns as soon as the attempt is started.
 *
 * On success the credentials are persisted (requirement FR1: remember the
 * network and reconnect on boot).
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/** @brief Disconnect and stop auto-reconnect until the next explicit connect. */
esp_err_t wifi_manager_disconnect(void);

esp_err_t wifi_manager_get_state(wifi_mgr_state_t *state);

/** @brief Current IPv4 address as a string, or "0.0.0.0" when not connected. */
esp_err_t wifi_manager_get_ip(char *ip, size_t len);

/** @brief Station MAC address as "aa:bb:cc:dd:ee:ff". */
esp_err_t wifi_manager_get_mac(char *mac, size_t len);

/** @brief SSID of the network we are connected to (or last tried), "" if unknown. */
esp_err_t wifi_manager_get_ssid(char *ssid, size_t len);

/** @brief Last measured RSSI in dBm, 0 when unknown. */
int8_t wifi_manager_get_rssi(void);

/** @brief Persist credentials without connecting. Password may be NULL for open APs. */
esp_err_t wifi_manager_save_config(const char *ssid, const char *password);

/** @brief Forget the stored credentials. */
esp_err_t wifi_manager_clear_config(void);

/**
 * @brief Connect using the stored credentials when `wifi_auto` is enabled.
 *
 * Returns ESP_ERR_NOT_FOUND when nothing is stored, so app_main can treat this
 * as a normal "not configured yet" state rather than an error.
 */
esp_err_t wifi_manager_auto_connect(void);

/** @brief True when the station has an IP address. */
bool wifi_manager_is_connected(void);

/** @brief Human readable name of a state, for logs and JSON. */
const char *wifi_manager_state_name(wifi_mgr_state_t state);

#ifdef __cplusplus
}
#endif
