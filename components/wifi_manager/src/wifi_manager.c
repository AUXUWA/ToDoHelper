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
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"

#include "th_bsp.h"
#include "esp_attr.h"

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
#define WIFI_SCAN_TASK_STACK  (4 * 1024)
#define WIFI_SCAN_TASK_PRIO   5

static void scan_worker(void *arg);
static void push_status(void);

static bool s_initialized;
static esp_netif_t *s_netif;
/**
 * Scan plumbing.
 *
 * All of this exists because the WiFi event handler runs on the shared `sys_evt`
 * task, whose stack is only ~2.8 KB. Copying scan results there (see
 * scan_collect()) used ~2.7 KB of local array on top of the WiFi logging path and
 * overflowed the stack:
 *
 *     Guru Meditation Error: Core 0 panic'ed (Stack protection fault).
 *     Detected in task "sys_evt"
 *     ... esp_wifi_scan_get_ap_num <- wifi_event_handler (wifi_manager.c:288)
 *
 * So the handler now only signals, and a dedicated worker with a real stack does
 * the copying.
 */
static SemaphoreHandle_t s_scan_done;      /* scan worker -> whoever waited */
static QueueHandle_t s_scan_cmd;           /* commands for the scan worker */
static TaskHandle_t s_scan_task;
static esp_timer_handle_t s_prescan_timer;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_health_timer;

/** Work the scan worker performs. */
typedef enum {
    SCAN_CMD_START,     /**< ask the radio to scan */
    SCAN_CMD_COLLECT,   /**< a scan finished, copy the results into the cache */
} scan_cmd_t;

static wifi_mgr_state_t s_state = WIFI_MGR_DISCONNECTED;
static char s_ssid[WIFI_MANAGER_SSID_MAX + 1];

/*
 * A static address for the station (empty = DHCP).
 *
 * Why: the address the user types into the browser is handed out by DHCP, and
 * it changes with every lease - on this bench it changed three times in one
 * afternoon (192.168.1.50 -> .119 -> .138). With a static address the web
 * console's URL never changes.
 */
static char s_static_ip[16];
static char s_static_gw[16];
static char s_static_mask[16];
static char s_static_dns[16];
static char s_ip[16] = "0.0.0.0";
static int8_t s_rssi;
static uint8_t s_last_reason;
static int s_reconnect_left;
static bool s_allow_reconnect;

static EXT_RAM_BSS_ATTR wifi_ap_info_t s_scan_cache[WIFI_SCAN_CACHE_MAX];
static size_t s_scan_count;

/* ------------------------------------------------------------------------- */
/* Scan worker                                                               */
/* ------------------------------------------------------------------------- */

/**
 * Pull the completed scan's results into the cache.
 *
 * Runs on the scan worker's own stack. The local `records` array is
 * WIFI_SCAN_CACHE_MAX * sizeof(wifi_ap_record_t) - about 2.7 KB with the default
 * of 24 - which is precisely why it must not live in the WiFi event handler.
 */
static void scan_collect(void)
{
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
        if (got > WIFI_SCAN_CACHE_MAX) {
            got = WIFI_SCAN_CACHE_MAX;
        }
        for (uint16_t i = 0; i < got; i++) {
            memset(&s_scan_cache[i], 0, sizeof(s_scan_cache[i]));
            strlcpy(s_scan_cache[i].ssid, (const char *)records[i].ssid,
                    sizeof(s_scan_cache[i].ssid));
            s_scan_cache[i].rssi = records[i].rssi;
            s_scan_cache[i].authmode = records[i].authmode;
            s_scan_cache[i].channel = records[i].primary;
            memcpy(s_scan_cache[i].bssid, records[i].bssid, 6);
        }
        s_scan_count = got;
    } else {
        s_scan_count = 0;
    }

    SYS_LOGI(TAG, "scan done: %u AP(s)", (unsigned)s_scan_count);
}

/**
 * Ask the scan worker to start a scan.
 *
 * All scanning goes through the worker so that no WiFi API is ever driven from
 * the WiFi event handler (~2.8 KB stack) or from an HTTP handler task. In
 * particular, starting a scan straight after esp_wifi_start() returned ESP_OK but
 * never produced SCAN_DONE - the radio was not up yet - which is why the
 * boot-time pre-scan goes through a short timer instead.
 */
static void scan_request(void)
{
    const scan_cmd_t cmd = SCAN_CMD_START;
    if (s_scan_cmd != NULL) {
        (void)xQueueSend(s_scan_cmd, &cmd, 0);
    }
}

static void prescan_timer_cb(void *arg)
{
    (void)arg;
    scan_request();
}

/**
 * Print the link state periodically, independent of the screen.
 *
 * The device once stopped answering ping and HTTP after the screen went idle
 * while logging nothing at all, which made "is the link down, or did the AP drop
 * us, or is the app wedged?" unanswerable. This makes it answerable.
 */
static void health_timer_cb(void *arg)
{
    (void)arg;

    wifi_ap_record_t ap = { 0 };
    const esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err == ESP_OK) {
        s_rssi = ap.rssi;
    }

    sys_status_t st;
    const bool have_st = (sys_status_get(&st) == ESP_OK);

    SYS_LOGI(TAG, "link: state=%s ap_info=%s rssi=%d ip=%s uptime=%llu s",
             wifi_manager_state_name(s_state), esp_err_to_name(err),
             (int)s_rssi, s_ip, (unsigned long long)(sys_time_ms() / 1000));

    /* Keep sys_status fresh so the UI never shows a stale dBm (see the GOT_IP
     * path: set_state() early-returns when the state is unchanged). */
    if (have_st && st.wifi_connected) {
        push_status();
    }
}

static void scan_worker(void *arg)
{
    (void)arg;

    scan_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_scan_cmd, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (cmd == SCAN_CMD_START) {
            const esp_err_t err = esp_wifi_scan_start(NULL, false);
            if (err != ESP_OK) {
                SYS_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
            } else {
                SYS_LOGD(TAG, "scan requested");
            }
            continue;
        }

        scan_collect();
        /* Released last so a waiter that wakes up sees a filled cache. */
        if (s_scan_done != NULL) {
            xSemaphoreGive(s_scan_done);
        }
        (void)sys_event_post_to(WIFI_MGR_EVENT, WIFI_MGR_EVENT_SCAN_DONE, NULL, 0);
    }
}

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

/**
 * Switch the station to the configured static address (or back to DHCP).
 *
 * The gateway and the netmask are not asked of the user: they are taken from
 * the last DHCP lease this device saw (a hotspot or a home router gives the
 * same parameters to everyone), so the user only types the address.
 */
static void apply_address_config(void)
{
    if (s_netif == NULL) {
        return;
    }

    if (s_static_ip[0] == 0) {
        (void)esp_netif_dhcpc_start(s_netif);
        return;
    }

    esp_netif_ip_info_t info = { 0 };
    info.ip.addr = ipaddr_addr(s_static_ip);
    info.gw.addr = ipaddr_addr(s_static_gw[0] ? s_static_gw : "192.168.1.1");
    info.netmask.addr = ipaddr_addr(s_static_mask[0] ? s_static_mask : "255.255.255.0");

    (void)esp_netif_dhcpc_stop(s_netif);
    const esp_err_t err = esp_netif_set_ip_info(s_netif, &info);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "static address %s refused: %s", s_static_ip, esp_err_to_name(err));
        /* Fall back to DHCP rather than being left without an address. */
        (void)esp_netif_dhcpc_start(s_netif);
    } else {
        SYS_LOGI(TAG, "static address %s (gw %s, mask %s)", s_static_ip,
                 s_static_gw[0] ? s_static_gw : "192.168.1.1",
                 s_static_mask[0] ? s_static_mask : "255.255.255.0");
    }

    /*
     * The DNS server has to be set by hand once the address is static.
     *
     * With DHCP the name server comes with the lease; stop the client and
     * lwIP is left with **no resolver at all** - every name lookup fails
     * (getaddrinfo returns 202) and the device cannot reach anything that is
     * not an IP address. That is not just "the weather API stops working":
     * the NTP server is a *name* ("ntp.aliyun.com"), so the clock never
     * syncs either, and the calendar shows "时间未同步" forever.
     *
     * The order of preference:
     *   1. what the user typed (POST /api/v1/wifi/config {"dns":...});
     *   2. the DNS of the last DHCP lease (remembered like the gateway);
     *   3. the gateway - that is where the resolver usually lives
     *      (a home router, or the Windows hotspot's DNS proxy).
     */
    const char *dns_text = s_static_dns[0] ? s_static_dns : (s_static_gw[0] ? s_static_gw : "");
    if (dns_text[0] != 0) {
        const uint32_t parsed = ipaddr_addr(dns_text);
        if (parsed != 0 && parsed != IPADDR_NONE) {
            esp_netif_dns_info_t dns = { 0 };
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = parsed;
            const esp_err_t dns_err = esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
            if (dns_err == ESP_OK) {
                SYS_LOGI(TAG, "DNS server %s%s", dns_text,
                         s_static_dns[0] ? "" : " (from the gateway)");
            } else {
                SYS_LOGW(TAG, "DNS server %s refused: %s", dns_text,
                         esp_err_to_name(dns_err));
            }
        }
    } else {
        SYS_LOGW(TAG, "no DNS server known - names will not resolve "
                      "(the clock cannot sync without it)");
    }
}

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

    /* The address has to be in place before the association, otherwise the
     * lease would overwrite it. */
    apply_address_config();

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
        (void)sys_notify("wifi_failed", NULL);
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

    /*
     * Remember the network's gateway and netmask.
     *
     * A static address needs them, and the user should not have to type all
     * three: a hotspot or a home router hands the same ones to everyone, so
     * the last lease is a good source.
     */
    if (evt->ip_info.gw.addr != 0) {
        char buf[16] = { 0 };
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&evt->ip_info.gw));
        if (strcmp(buf, s_static_gw) != 0) {
            strlcpy(s_static_gw, buf, sizeof(s_static_gw));
            (void)sys_config_set_str(SYS_KEY_WIFI_GW, s_static_gw);
        }
    }
    if (evt->ip_info.netmask.addr != 0) {
        char buf[16] = { 0 };
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&evt->ip_info.netmask));
        if (strcmp(buf, s_static_mask) != 0) {
            strlcpy(s_static_mask, buf, sizeof(s_static_mask));
            (void)sys_config_set_str(SYS_KEY_WIFI_MASK, s_static_mask);
        }
    }
    /*
     * Remember the DNS server as well - it is the third thing a static
     * address needs, and the one that is easiest to forget: without a
     * resolver the device cannot even look up an NTP server, so its clock
     * would never sync (this is what v1.13.2 fixed).
     */
    if (s_static_dns[0] == 0) {
        esp_netif_dns_info_t dns_info = { 0 };
        if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK &&
            dns_info.ip.type == ESP_IPADDR_TYPE_V4 &&
            dns_info.ip.u_addr.ip4.addr != 0) {
            char buf[16] = { 0 };
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
            if (strcmp(buf, s_static_dns) != 0) {
                strlcpy(s_static_dns, buf, sizeof(s_static_dns));
                (void)sys_config_set_str(SYS_KEY_WIFI_DNS, s_static_dns);
                SYS_LOGI(TAG, "DNS server learned: %s", s_static_dns);
            }
        }
    }
    update_rssi();
    s_last_reason = 0;
    /* A successful association resets the retry budget for the next outage. */
    s_reconnect_left = CONFIG_TODOHELPER_WIFI_RECONNECT_ATTEMPTS;

    set_state(WIFI_MGR_CONNECTED);
    /*
     * push_status() explicitly, because set_state() early-returns when the state
     * is unchanged - and WIFI_EVENT_STA_CONNECTED already moved us to CONNECTED
     * (link up, before DHCP). Without this the IP and RSSI gathered just above
     * never reach sys_status, and the UI shows the values from association time
     * forever: the status bar read "WiFi 0 dBm" and the API reported 0.0.0.0
     * while s_rssi was -35 and s_ip still held the stale address.
     */
    push_status();

    SYS_LOGI(TAG, "got ip %s (ssid '%s', %d dBm)", s_ip, s_ssid, (int)s_rssi);

    /* Read back what actually landed in sys_status - the first thing to check if
     * the UI shows stale network state again. */
    sys_status_t st_check;
    if (sys_status_get(&st_check) == ESP_OK) {
        SYS_LOGD(TAG, "status: connected=%d rssi=%d ssid='%s' ip='%s'",
                 (int)st_check.wifi_connected, (int)st_check.wifi_rssi,
                 st_check.wifi_ssid, st_check.wifi_ip);
    }

    (void)sys_event_post_to(WIFI_MGR_EVENT, WIFI_MGR_EVENT_GOT_IP, NULL, 0);
    bool online = true;
    (void)sys_event_post_to(SYS_EVENT, SYS_EVENT_CONNECTIVITY, &online, sizeof(online));
    /* L0 notification so the idle face can celebrate without wifi_manager
     * knowing it exists (see idle_face's trigger table). */
    (void)sys_notify("wifi_connected", NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_SCAN_DONE: {
        /* Signal the worker and return immediately.  This handler runs on the
         * shared ~2.8 KB `sys_evt` stack, so it must not copy scan records or
         * call anything that walks the WiFi logging path - doing that used to
         * overflow the stack and reset the device. */
        const scan_cmd_t cmd = SCAN_CMD_COLLECT;
        if (s_scan_cmd != NULL) {
            (void)xQueueSend(s_scan_cmd, &cmd, 0);
        }
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
    if (s_scan_cmd == NULL) {
        s_scan_cmd = xQueueCreate(4, sizeof(scan_cmd_t));
        if (s_scan_cmd == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_scan_task == NULL) {
        /* Internal RAM on purpose: a task whose stack lives in PSRAM must never
         * perform a flash operation (the cache is disabled then, see th_bsp.c),
         * and the Wi-Fi driver may touch NVS during a scan. It is only 4 KB. */
        const BaseType_t ok = xTaskCreate(scan_worker, "th_wifi_scan",
                                         WIFI_SCAN_TASK_STACK, NULL,
                                         WIFI_SCAN_TASK_PRIO, &s_scan_task);
        if (ok != pdPASS) {
            SYS_LOGE(TAG, "cannot create the scan worker");
            return ESP_ERR_NO_MEM;
        }
        SYS_LOGD(TAG, "scan worker started (%d B stack)", WIFI_SCAN_TASK_STACK);
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
    /*
     * After esp_wifi_start(), not before.
     *
     * With the default power save the PHY is powered down and re-enabled around
     * radio activity, and that re-enable path (pm_wake_up -> esp_phy_enable ->
     * phy_track_pll_init -> esp_timer_create) needs internal RAM - which is the
     * scarcest resource here, so that allocation failed with ESP_ERR_NO_MEM and
     * aborted the device:
     *
     *   ESP_ERROR_CHECK failed: esp_err_t 0x101 (ESP_ERR_NO_MEM)
     *     at phy_track_pll_init (phy_common.c:126)
     *
     * Setting the policy before esp_wifi_start() is not reliable - start applies
     * its own default - so it is done here. This is a mains-powered desktop
     * device: modem sleep buys nothing and costs both internal-RAM churn and
     * latency, and it risks the AP dropping us while the screen is idle.
     */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

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

    /* The static address (if any) is read before the first connection, so the
     * association never gets a DHCP lease in the first place. */
    (void)sys_config_get_str_or(SYS_KEY_WIFI_IP, s_static_ip, sizeof(s_static_ip), "");
    (void)sys_config_get_str_or(SYS_KEY_WIFI_GW, s_static_gw, sizeof(s_static_gw), "");
    (void)sys_config_get_str_or(SYS_KEY_WIFI_MASK, s_static_mask, sizeof(s_static_mask), "");
    (void)sys_config_get_str_or(SYS_KEY_WIFI_DNS, s_static_dns, sizeof(s_static_dns), "");

    s_initialized = true;
    s_allow_reconnect = false;
    push_status();

    char mac[18] = { 0 };
    (void)wifi_manager_get_mac(mac, sizeof(mac));
    SYS_LOGI(TAG, "ready (hostname '%s', mac %s)", WIFI_MANAGER_HOSTNAME, mac);

    esp_err_t http_err = wifi_manager_register_http();
    if (http_err != ESP_OK) {
        SYS_LOGW(TAG, "HTTP routes unavailable: %s", esp_err_to_name(http_err));
    }

    /*
     * Periodic link report.
     *
     * Added because the device stopped answering ping/HTTP after the screen went
     * idle (~30 s in) while still logging nothing, so we could not tell whether
     * the link was actually down or the AP had dropped us. This prints the truth
     * every 30 s regardless of screen state.
     */
    const esp_timer_create_args_t health_args = {
        .callback = health_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_health",
    };
    if (s_health_timer == NULL && esp_timer_create(&health_args, &s_health_timer) == ESP_OK) {
        (void)esp_timer_start_periodic(s_health_timer, 30 * 1000 * 1000);
    }

#if CONFIG_TODOHELPER_WIFI_AUTO_CONNECT
    esp_err_t auto_err = wifi_manager_auto_connect();
    if (auto_err == ESP_ERR_NOT_FOUND) {
        SYS_LOGI(TAG, "no saved network yet");
        /* Nothing to connect to, and the user's first action will be to pick a
         * network - so populate the list before they open the page.
         *
         * Deferred by a timer on purpose: starting a scan immediately after
         * esp_wifi_start() returns ESP_OK but never delivers SCAN_DONE because
         * the radio is still coming up, which left the list permanently empty.
         */
        if (s_prescan_timer == NULL) {
            const esp_timer_create_args_t prescan_args = {
                .callback = prescan_timer_cb,
                .arg = NULL,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "wifi_prescan",
            };
            if (esp_timer_create(&prescan_args, &s_prescan_timer) == ESP_OK) {
                esp_timer_start_once(s_prescan_timer, 2000 * 1000);
                SYS_LOGI(TAG, "pre-scanning for available networks in 2 s");
            } else {
                SYS_LOGW(TAG, "cannot schedule the pre-scan");
                s_prescan_timer = NULL;
            }
        }
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
    /* Handed to the worker: see scan_request() for why the radio is not driven
     * from the caller's task. */
    scan_request();
    return ESP_OK;
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

    /* esp_wifi_scan_start(block=true) returns once SCAN_DONE has been handled, but
     * the handler only *signals* the worker - the cache is filled asynchronously,
     * so wait for the worker before reading it. */
    if (s_scan_done != NULL) {
        if (xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(WIFI_BLOCKING_SCAN_TIMEOUT_MS)) != pdTRUE) {
            SYS_LOGW(TAG, "timed out waiting for the scan worker");
            return ESP_ERR_TIMEOUT;
        }
    }

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

esp_err_t wifi_manager_set_static_ip(const char *ip)
{
    if (ip != NULL && ip[0] != 0) {
        /* A sanity check: it has to look like an address. */
        const uint32_t parsed = ipaddr_addr(ip);
        if (parsed == 0 || parsed == IPADDR_NONE) {
            SYS_LOGW(TAG, "static address '%s' is not a valid IPv4 address", ip);
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(s_static_ip, ip, sizeof(s_static_ip));
        (void)sys_config_set_str(SYS_KEY_WIFI_IP, s_static_ip);
        SYS_LOGI(TAG, "static address set to %s", s_static_ip);
    } else {
        s_static_ip[0] = 0;
        (void)sys_config_erase(SYS_KEY_WIFI_IP);
        SYS_LOGI(TAG, "static address cleared (back to DHCP)");
    }

    /* Take effect now: the address has to be in place before the next
     * association. */
    apply_address_config();
    return ESP_OK;
}

const char *wifi_manager_get_static_ip(void)
{
    return s_static_ip;
}

esp_err_t wifi_manager_set_static_dns(const char *dns)
{
    if (dns != NULL && dns[0] != 0) {
        const uint32_t parsed = ipaddr_addr(dns);
        if (parsed == 0 || parsed == IPADDR_NONE) {
            SYS_LOGW(TAG, "DNS '%s' is not a valid IPv4 address", dns);
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(s_static_dns, dns, sizeof(s_static_dns));
        (void)sys_config_set_str(SYS_KEY_WIFI_DNS, s_static_dns);
        SYS_LOGI(TAG, "DNS server set to %s", s_static_dns);
    } else {
        s_static_dns[0] = 0;
        (void)sys_config_erase(SYS_KEY_WIFI_DNS);
        SYS_LOGI(TAG, "DNS server cleared (fall back to the gateway)");
    }

    /* Take effect now, not at the next association: the user is usually
     * staring at a broken clock and wants it fixed without a reboot. */
    apply_address_config();
    return ESP_OK;
}

const char *wifi_manager_get_static_dns(void)
{
    return s_static_dns;
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
