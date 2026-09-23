/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/ip_addr.h"

#include "th_bsp.h"

static const char *TAG = "wifi";

ESP_EVENT_DEFINE_BASE(WIFI_MGR_EVENT);

#ifndef CONFIG_TODOHELPER_WIFI_RECONNECT_ATTEMPTS
#define CONFIG_TODOHELPER_WIFI_RECONNECT_ATTEMPTS 5
#endif
#ifndef CONFIG_TODOHELPER_WIFI_RECONNECT_DELAY_MS
#define CONFIG_TODOHELPER_WIFI_RECONNECT_DELAY_MS 3000
#endif
#ifndef CONFIG_TODOHELPER_WIFI_SCAN_MAX_APS
#define CONFIG_TODOHELPER_WIFI_SCAN_MAX_APS 24
#endif

#define WIFI_SCAN_CACHE_MAX  CONFIG_TODOHELPER_WIFI_SCAN_MAX_APS
#define WIFI_BLOCKING_SCAN_TIMEOUT_MS 15000

static bool s_initialized;
static esp_netif_t *s_netif;
static SemaphoreHandle_t s_scan_done;      /* signalled by the SCAN_DONE handler */
static esp_timer_handle_t s_reconnect_timer;

static wifi_mgr_state_t s_state = WIFI_MGR_DISCONNECTED;
static char s_ssid[WIFI_MANAGER_SSID_MAX + 1];
static char s_ip[16] = "0.0.0.0";
static int8_t s_rssi;
static uint8_t s_last_reason;
static int s_reconnect_left;
static bool s_allow_reconnect;

static wifi_ap_info_t s_scan_cache[WIFI_SCAN_CACHE_MAX];
static size_t s_scan_count;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

const char *wifi_manager_state_name(wifi_mgr_state_t state)
{
    switch (state) {
    case WIFI_MGR_DISCONNECTED:
        return "disconnected";
    case WIFI_MGR_CONNECTING:
        return "connecting";
    case WIFI_MGR_CONNECTED:
        return "connected";
    case WIFI_MGR_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

/* ------------------------------------------------------------------------- */
/* sys_status publication                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    wifi_mgr_state_t state;
    const char *ip;
    const char *ssid;
    int8_t rssi;
    bool online;
} status_push_t;

static void status_mutator(sys_status_t *st, void *arg)
{
    const status_push_t *push = (const status_push_t *)arg;
    st->wifi_connected = (push->state == WIFI_MGR_CONNECTED);
    st->wifi_connecting = (push->state == WIFI_MGR_CONNECTING);
    st->online = push->online;
    st->wifi_rssi = push->rssi;

    if (push->ssid != NULL) {
        strlcpy(st->wifi_ssid, push->ssid, sizeof(st->wifi_ssid));
    }
    if (push->ip != NULL) {
        strlcpy(st->wifi_ip, push->ip, sizeof(st->wifi_ip));
    }
}

static void push_status(void)
{
    const status_push_t push = {
        .state = s_state,
        .ip = s_ip,
        .ssid = s_ssid,
        .rssi = s_rssi,
        .online = (s_state == WIFI_MGR_CONNECTED),
    };
    (void)sys_status_update(status_mutator, (void *)&push);
}

static void set_state(wifi_mgr_state_t state)
{
    if (s_state == state) {
        return;
    }
    s_state = state;
    SYS_LOGI(TAG, "state -> %s", wifi_manager_state_name(state));

    switch (state) {
    case WIFI_MGR_CONNECTING:
        (void)th_bsp_led_set(TH_BSP_LED_WIFI);
        break;
    case WIFI_MGR_CONNECTED:
        (void)th_bsp_led_set(TH_BSP_LED_WIFI);
        break;
    case WIFI_MGR_FAILED:
        (void)th_bsp_led_set(TH_BSP_LED_ERROR);
        break;
    case WIFI_MGR_DISCONNECTED:
    default:
        (void)th_bsp_led_set(TH_BSP_LED_OFF);
        break;
    }

    push_status();
}

static void update_rssi(void)
{
    wifi_ap_record_t ap = { 0 };
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_rssi = ap.rssi;
    }
}

static bool stored_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    ssid[0] = '\0';
    pass[0] = '\0';

    if (sys_config_get_str(SYS_KEY_WIFI_SSID, ssid, ssid_len) != ESP_OK || ssid[0] == '\0') {
        return false;
    }
    /* An open network legitimately has no stored password. */
    (void)sys_config_get_str_or(SYS_KEY_WIFI_PASS, pass, pass_len, "");
    return true;
}

/* ------------------------------------------------------------------------- */
/* Connect                                                                   */
/* ------------------------------------------------------------------------- */

static esp_err_t connect_with(const char *ssid, const char *password, bool remember)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) > WIFI_MANAGER_SSID_MAX ||
        (password != NULL && strlen(password) > WIFI_MANAGER_PASS_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (password != NULL && password[0] != '\0') {
        strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    }
    cfg.sta.threshold.authmode = (password != NULL && password[0] != '\0')
                                     ? WIFI_AUTH_WPA_PSK
                                     : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        SYS_LOGE(TAG, "set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    if (remember) {
        (void)wifi_manager_save_config(ssid, password);
    }

    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    s_reconnect_left = CONFIG_TODOHELPER_WIFI_RECONNECT_ATTEMPTS;
    s_allow_reconnect = true;
    set_state(WIFI_MGR_CONNECTING);
    (void)sys_event_post_to(WIFI_MGR_EVENT, WIFI_MGR_EVENT_CONNECTING, NULL, 0);

    /* esp_wifi_connect() can fail immediately (e.g. still scanning); retrying it
     * is exactly what the reconnect timer is for. */
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "connect start failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_allow_reconnect) {
        return;
    }
    if (s_reconnect_left <= 0) {
        SYS_LOGE(TAG, "giving up after %d attempts (last reason %u)",
                 CONFIG_TODOHELPER_WIFI_RECONNECT_ATTEMPTS, (unsigned)s_last_reason);
        set_state(WIFI_MGR_FAILED);
        (void)sys_event_post_to(WIFI_MGR_EVENT, WIFI_MGR_EVENT_FAILED,
                                &s_last_reason, sizeof(s_last_reason));
        return;
    }

    s_reconnect_left--;
    SYS_LOGI(TAG, "reconnecting (%d attempts left)", s_reconnect_left);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "reconnect failed: %s", esp_err_to_name(err));
    }
}

static void schedule_reconnect(void)
{
    if (!s_allow_reconnect || s_reconnect_timer == NULL) {
        return;
    }
    (void)esp_timer_stop(s_reconnect_timer);
    (void)esp_timer_start_once(s_reconnect_timer,
                               (uint64_t)CONFIG_TODOHELPER_WIFI_RECONNECT_DELAY_MS * 1000);
}

/* ------------------------------------------------------------------------- */
/* Event handling                                                            */
/* ------------------------------------------------------------------------- */

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    const ip_event_got_ip_t *evt = (const ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
    update_rssi();
    s_last_reason = 0;
    /* A successful association resets the retry budget for the next outage. */
    s_reconnect_left = CONFIG_TODOHELPER_WIFI_RECONNECT_ATTEMPTS;

    set_state(WIFI_MGR_CONNECTED);
    SYS_LOGI(TAG, "got ip %s (ssid '%s', %d dBm)", s_ip, s_ssid, (int)s_rssi);

    (void)sys_event_post_to(WIFI_MGR_EVENT, WIFI_MGR_EVENT_GOT_IP, NULL, 0);
    bool online = true;
    (void)sys_event_post_to(SYS_EVENT, SYS_EVENT_CONNECTIVITY, &online, sizeof(online));
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_SCAN_DONE: {
        uint16_t num = 0;
        if (esp_wifi_scan_get_ap_num(&num) != ESP_OK) {
            num = 0;
        }
        if (num > WIFI_SCAN_CACHE_MAX) {
            num = WIFI_SCAN_CACHE_MAX;
        }

        wifi_ap_record_t records[WIFI_SCAN_CACHE_MAX];
        uint16_t got = num;
        if (got > 0 && esp_wifi_scan_get_ap_records(&got, records) == ESP_OK) {
            s_scan_count = got;
            for (uint16_t i = 0; i < got; i++) {
                memset(&s_scan_cache[i], 0, sizeof(s_scan_cache[i]));
                strlcpy(s_scan_cache[i].ssid, (const char *)records[i].ssid,
                        sizeof(s_scan_cache[i].ssid));
                s_scan_cache[i].rssi = records[i].rssi;
                s_scan_cache[i].authmode = records[i].authmode;
                s_scan_cache[i].channel = records[i].primary;
                memcpy(s_scan_cache[i].bssid, records[i].bssid, 6);
            }
        } else {
            s_scan_count = 0;
        }

        SYS_LOGI(TAG, "scan done: %u AP(s)", (unsigned)s_scan_count);
        if (s_scan_done != NULL) {
            xSemaphoreGive(s_scan_done);
        }
        (void)sys_event_post_to(WIFI_MGR_EVENT, WIFI_MGR_EVENT_SCAN_DONE, NULL, 0);
        break;
    }

    case WIFI_EVENT_STA_START:
        SYS_LOGD(TAG, "station started");
        break;

    case WIFI_EVENT_STA_CONNECTED:
        set_state(WIFI_MGR_CONNECTED);
        (void)sys_event_post_to(WIFI_MGR_EVENT, WIFI_MGR_EVENT_CONNECTED, NULL, 0);
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *evt =
            (const wifi_event_sta_disconnected_t *)data;
        s_last_reason = evt ? evt->reason : 0;
        s_ip[0] = '\0';
        strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));
        s_rssi = 0;

        set_state(WIFI_MGR_DISCONNECTED);
        SYS_LOGW(TAG, "disconnected (reason %u)", (unsigned)s_last_reason);

        (void)sys_event_post_to(WIFI_MGR_EVENT, WIFI_MGR_EVENT_DISCONNECTED,
                                &s_last_reason, sizeof(s_last_reason));
        bool online = false;
        (void)sys_event_post_to(SYS_EVENT, SYS_EVENT_CONNECTIVITY, &online, sizeof(online));

        if (s_allow_reconnect) {
            schedule_reconnect();
        }
        break;
    }

    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        SYS_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* A missing default event loop is fine: this component only uses the
     * project loop from sys_utils. */
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        SYS_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_scan_done == NULL) {
        s_scan_done = xSemaphoreCreateBinary();
        if (s_scan_done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        SYS_LOGE(TAG, "create default WiFi STA netif failed");
        return ESP_FAIL;
    }
    (void)esp_netif_set_hostname(s_netif, WIFI_MANAGER_HOSTNAME);

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        SYS_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       ip_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_reconn",
    };
    if (s_reconnect_timer == NULL) {
        err = esp_timer_create(&timer_args, &s_reconnect_timer);
        if (err != ESP_OK) {
            SYS_LOGW(TAG, "reconnect timer unavailable: %s", esp_err_to_name(err));
            s_reconnect_timer = NULL;
        }
    }

    s_initialized = true;
    s_allow_reconnect = false;
    push_status();

    char mac[18] = { 0 };
    (void)wifi_manager_get_mac(mac, sizeof(mac));
    SYS_LOGI(TAG, "ready (hostname '%s', mac %s)", WIFI_MANAGER_HOSTNAME, mac);

#if CONFIG_TODOHELPER_WIFI_AUTO_CONNECT
    esp_err_t auto_err = wifi_manager_auto_connect();
    if (auto_err == ESP_ERR_NOT_FOUND) {
        SYS_LOGI(TAG, "no saved network yet");
    } else if (auto_err != ESP_OK) {
        SYS_LOGW(TAG, "auto connect not started: %s", esp_err_to_name(auto_err));
    }
#endif

    return ESP_OK;
}

esp_err_t wifi_manager_start_scan(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scan_done != NULL) {
        /* Drop a stale completion so the next wait cannot return early. */
        (void)xSemaphoreTake(s_scan_done, 0);
    }
    return esp_wifi_scan_start(NULL, false);
}

esp_err_t wifi_manager_scan(wifi_ap_info_t *aps, size_t max, size_t *count)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_scan_done != NULL) {
        (void)xSemaphoreTake(s_scan_done, 0);
    }

    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "blocking scan failed: %s", esp_err_to_name(err));
        return err;
    }

    /* esp_wifi_scan_start(block=true) already waited for SCAN_DONE; the handler
     * filled the cache on the way. */
    return wifi_manager_get_scan_result(aps, max, count);
}

esp_err_t wifi_manager_get_scan_result(wifi_ap_info_t *aps, size_t max, size_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t n = s_scan_count;
    if (n > max) {
        n = max;
    }
    if (aps != NULL && n > 0) {
        memcpy(aps, s_scan_cache, n * sizeof(wifi_ap_info_t));
    }
    *count = s_scan_count;
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    return connect_with(ssid, password, true);
}

esp_err_t wifi_manager_disconnect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_allow_reconnect = false;
    if (s_reconnect_timer != NULL) {
        (void)esp_timer_stop(s_reconnect_timer);
    }

    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        err = ESP_OK;
    }

    set_state(WIFI_MGR_DISCONNECTED);
    s_ip[0] = '\0';
    strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));
    push_status();
    return err;
}

esp_err_t wifi_manager_get_state(wifi_mgr_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *state = s_state;
    return ESP_OK;
}

esp_err_t wifi_manager_get_ip(char *ip, size_t len)
{
    if (ip == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_netif != NULL) {
        esp_netif_ip_info_t info = { 0 };
        if (esp_netif_get_ip_info(s_netif, &info) == ESP_OK && info.ip.addr != 0) {
            snprintf(ip, len, IPSTR, IP2STR(&info.ip));
            strlcpy(s_ip, ip, sizeof(s_ip));
            return ESP_OK;
        }
    }
    strlcpy(ip, "0.0.0.0", len);
    return ESP_OK;
}

esp_err_t wifi_manager_get_mac(char *mac, size_t len)
{
    if (mac == NULL || len < 18) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[6] = { 0 };
    if (esp_wifi_get_mac(WIFI_IF_STA, raw) != ESP_OK) {
        strlcpy(mac, "00:00:00:00:00:00", len);
        return ESP_FAIL;
    }
    snprintf(mac, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
    return ESP_OK;
}

esp_err_t wifi_manager_get_ssid(char *ssid, size_t len)
{
    if (ssid == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(ssid, s_ssid, len);
    return ESP_OK;
}

int8_t wifi_manager_get_rssi(void)
{
    update_rssi();
    return s_rssi;
}

esp_err_t wifi_manager_save_config(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = sys_config_set_str(SYS_KEY_WIFI_SSID, ssid);
    if (err != ESP_OK) {
        return err;
    }
    err = sys_config_set_str(SYS_KEY_WIFI_PASS, (password != NULL) ? password : "");
    if (err != ESP_OK) {
        return err;
    }
    /* Deliberately never logged: requirement 8.8 forbids leaking credentials. */
    SYS_LOGI(TAG, "credentials for '%s' saved", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_clear_config(void)
{
    (void)sys_config_erase(SYS_KEY_WIFI_SSID);
    (void)sys_config_erase(SYS_KEY_WIFI_PASS);
    SYS_LOGI(TAG, "stored credentials cleared");
    return ESP_OK;
}

esp_err_t wifi_manager_auto_connect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!sys_config_get_bool_or(SYS_KEY_WIFI_AUTO, true)) {
        SYS_LOGI(TAG, "auto connect disabled in config");
        return ESP_ERR_INVALID_STATE;
    }

    char ssid[WIFI_MANAGER_SSID_MAX + 1];
    char pass[WIFI_MANAGER_PASS_MAX + 1];
    if (!stored_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        return ESP_ERR_NOT_FOUND;
    }

    SYS_LOGI(TAG, "auto connecting to stored network '%s'", ssid);
    return connect_with(ssid, pass, false);
}

bool wifi_manager_is_connected(void)
{
    return s_state == WIFI_MGR_CONNECTED;
}
