/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sync_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mdns.h"

#include "storage_manager.h"
#include "wifi_manager.h"

static const char *TAG = "sync";

ESP_EVENT_DEFINE_BASE(SYNC_EVENT);

/** Paths that always require a token, even for reads (requirement 8.8). */
static const char *const s_always_auth[] = {
    "/api/v1/device/reboot",
    "/api/v1/device/auth",
    "/api/v1/ai/config",
    "/api/v1/ai/history",
};

/**
 * Concurrent SSE subscribers.
 *
 * Each subscriber costs one httpd session slot *and* one dedicated task, and the
 * server only has `max_open_sockets` (5) sessions in total, so leave at least
 * two slots for the page and the REST calls.
 */
#define SYNC_SSE_MAX_CLIENTS 3
/**
 * Keepalive interval for idle SSE connections.
 *
 * This is also the worst case in which a client that vanished without closing
 * its socket (browser killed, WiFi dropped) is noticed: the streaming task only
 * learns that the peer is gone when a send fails.
 */
#define SYNC_SSE_KEEPALIVE_MS 15000
/** Stack for the per-subscriber streaming task (internal RAM: it talks to LWIP/WiFi). */
#define SYNC_SSE_TASK_STACK 3072
#define SYNC_SSE_TASK_PRIO 4

/* The build system generates these for EMBED_FILES "web/index.html". */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static bool s_initialized;
static httpd_handle_t s_server;
static sync_server_config_t s_cfg;
static char s_url[64];

static QueueHandle_t s_sse_queues[SYNC_SSE_MAX_CLIENTS];
static size_t s_sse_clients;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static void config_defaults(sync_server_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = (uint16_t)sys_config_get_i32_or(SYS_KEY_HTTP_PORT, SYNC_SERVER_DEFAULT_PORT);
    if (cfg->port == 0) {
        cfg->port = SYNC_SERVER_DEFAULT_PORT;
    }
    cfg->mdns_enabled = sys_config_get_bool_or(SYS_KEY_MDNS_ENABLED, true);
    strlcpy(cfg->hostname, SYNC_SERVER_DEFAULT_HOSTNAME, sizeof(cfg->hostname));

    char token[80];
    cfg->auth_enabled = (sys_config_get_str(SYS_KEY_AUTH_TOKEN, token, sizeof(token)) == ESP_OK) &&
                        (token[0] != '\0');
}

static void refresh_url(void)
{
    if (s_cfg.mdns_enabled) {
        snprintf(s_url, sizeof(s_url), "http://%s.local", s_cfg.hostname);
        return;
    }

    char ip[16] = "0.0.0.0";
    (void)wifi_manager_get_ip(ip, sizeof(ip));
    snprintf(s_url, sizeof(s_url), "http://%s", ip);
}

void sync_server_ip_url(char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return;
    }

    /*
     * The plain-IP form of the same address.
     *
     * Needed next to sync_server_url() because mDNS is not actually universal:
     * Windows only resolves `*.local` when Bonjour is installed, and some
     * routers/guest networks block multicast entirely. So the device shows both -
     * the friendly name when mDNS is on, plus the IP the user can always type.
     */
    char ip[16] = { 0 };
    if (wifi_manager_get_ip(ip, sizeof(ip)) != ESP_OK || ip[0] == '\0') {
        snprintf(out, len, "（未连接 WiFi）");
        return;
    }
    snprintf(out, len, "http://%s", ip);
}

bool sync_server_is_running(void)
{
    return s_server != NULL;
}

httpd_handle_t sync_server_handle(void)
{
    return s_server;
}

const char *sync_server_url(void)
{
    /*
     * Recompute every time rather than caching after the first call.
     *
     * The IP is handed out by DHCP, and a lease can change after a router reboot
     * or a long power-off. Since this URL is now shown on the device's settings
     * page for the user to type into a browser, a stale address is worse than a
     * small amount of formatting work: the displayed address must be the one the
     * device actually has. refresh_url() is a snprintf plus one netif query.
     */
    refresh_url();
    return s_url;
}

/* ------------------------------------------------------------------------ */
/* Auth (requirement 8.8)                                                    */
/* ------------------------------------------------------------------------ */

static bool path_always_needs_auth(const char *path)
{
    for (size_t i = 0; i < sizeof(s_always_auth) / sizeof(s_always_auth[0]); i++) {
        if (strcmp(path, s_always_auth[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool token_matches(httpd_req_t *req)
{
    char expected[80];
    if (sys_config_get_str(SYS_KEY_AUTH_TOKEN, expected, sizeof(expected)) != ESP_OK ||
        expected[0] == '\0') {
        /* Authentication enabled without a token cannot be satisfied. */
        return false;
    }

    char provided[80];
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", provided, sizeof(provided)) != ESP_OK) {
        return false;
    }
    return strcmp(expected, provided) == 0;
}

bool sync_server_check_auth(httpd_req_t *req)
{
    char path[128];
    if (http_get_path(req, path, sizeof(path)) != ESP_OK) {
        return false;
    }

    if (path_always_needs_auth(path)) {
        const bool ok = token_matches(req);
        if (!ok) {
            SYS_LOGW(TAG, "auth failed for %s", path);
            (void)sys_event_post_to(SYNC_EVENT, SYNC_EVENT_AUTH_FAILED, path, strlen(path) + 1);
        }
        return ok;
    }

    if (!s_cfg.auth_enabled) {
        return true;
    }

    const bool ok = token_matches(req);
    if (!ok) {
        SYS_LOGW(TAG, "auth failed for %s", path);
        (void)sys_event_post_to(SYNC_EVENT, SYNC_EVENT_AUTH_FAILED, path, strlen(path) + 1);
    }
    return ok;
}

bool sync_server_auth_required(void)
{
    return s_cfg.auth_enabled;
}

esp_err_t sync_server_set_auth_token(const char *token)
{
    if (token == NULL || token[0] == '\0') {
        (void)sys_config_erase(SYS_KEY_AUTH_TOKEN);
        s_cfg.auth_enabled = false;
        SYS_LOGI(TAG, "HTTP authentication disabled");
        return ESP_OK;
    }

    if (strlen(token) > 63) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = sys_config_set_str(SYS_KEY_AUTH_TOKEN, token);
    if (err != ESP_OK) {
        return err;
    }
    s_cfg.auth_enabled = true;
    /* The token itself must never be logged. */
    SYS_LOGI(TAG, "HTTP authentication enabled");
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Routes: static page                                                       */
/* ------------------------------------------------------------------------ */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

/* ------------------------------------------------------------------------ */
/* Routes: device                                                            */
/* ------------------------------------------------------------------------ */

static esp_err_t device_info_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "device info");
    }

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const esp_app_desc_t *app = esp_app_get_description();

    char mac[18] = { 0 };
    (void)wifi_manager_get_mac(mac, sizeof(mac));
    char ip[16] = { 0 };
    (void)wifi_manager_get_ip(ip, sizeof(ip));

    wifi_mgr_state_t wifi_state = WIFI_MGR_DISCONNECTED;
    (void)wifi_manager_get_state(&wifi_state);

    cJSON_AddStringToObject(root, "model", "ESP32-S31-Korvo");
    cJSON_AddNumberToObject(root, "cores", chip.cores);
    cJSON_AddStringToObject(root, "app_name", app->project_name);
    cJSON_AddStringToObject(root, "version", app->version);
    cJSON_AddStringToObject(root, "idf", app->idf_ver);
    cJSON_AddStringToObject(root, "mac", mac);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "hostname", s_cfg.hostname);
    cJSON_AddStringToObject(root, "wifi_state", wifi_manager_state_name(wifi_state));

    /* Deliberately no API keys, no auth token: requirement 8.8 forbids it. */
    cJSON_AddBoolToObject(root, "sd_available", storage_sd_available());
    cJSON_AddBoolToObject(root, "storage_available", storage_available());
    cJSON_AddStringToObject(root, "storage_root", storage_root() ? storage_root() : "");
    cJSON_AddNumberToObject(root, "storage_free_bytes", (double)storage_free_bytes());
    cJSON_AddBoolToObject(root, "auth_enabled", s_cfg.auth_enabled);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    const esp_err_t err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t device_status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "device status");
    }

    sys_status_t st;
    (void)sys_status_get(&st);

    cJSON_AddNumberToObject(root, "uptime_s", (double)(sys_time_ms() / 1000));
    cJSON_AddNumberToObject(root, "free_heap", (double)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "min_free_heap", (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "free_psram",
                            (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "routes", (double)http_router_count());
    cJSON_AddNumberToObject(root, "sse_clients", (double)s_sse_clients);
    cJSON_AddNumberToObject(root, "wifi_rssi", st.wifi_rssi);
    cJSON_AddBoolToObject(root, "wifi_connected", st.wifi_connected);
    cJSON_AddBoolToObject(root, "time_synced", sys_time_synced());
    cJSON_AddNumberToObject(root, "server_ts", (double)sys_time_ts());
    cJSON_AddStringToObject(root, "url", sync_server_url());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    const esp_err_t err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

/** Rebooting from inside the handler would cut the response off. */
static void reboot_timer_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t device_reboot_post_handler(httpd_req_t *req)
{
    esp_err_t err = http_reply_ok(req, "{\"rebooting\":true}");
    if (err != ESP_OK) {
        return err;
    }

    const esp_timer_create_args_t args = {
        .callback = reboot_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "th_reboot",
    };
    esp_timer_handle_t timer = NULL;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        SYS_LOGW(TAG, "reboot requested over HTTP");
        (void)esp_timer_start_once(timer, 700 * 1000);
    }
    return ESP_OK;
}

static esp_err_t device_auth_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return http_reply_bad_request(req, "invalid json");
    }

    const cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "token");
    const char *value = cJSON_IsString(token) ? token->valuestring : NULL;
    const esp_err_t err = sync_server_set_auth_token(value);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "set token");
    }
    return http_reply_ok(req, NULL);
}

/* ------------------------------------------------------------------------ */
/* Server-sent events                                                        */
/* ------------------------------------------------------------------------ */

size_t sync_server_sse_clients(void)
{
    return s_sse_clients;
}

/** Take a free per-client queue slot, or NULL when all are in use. */
static QueueHandle_t sse_register(void)
{
    for (size_t i = 0; i < SYNC_SSE_MAX_CLIENTS; i++) {
        if (s_sse_queues[i] == NULL) {
            QueueHandle_t q = xQueueCreate(8, sizeof(char *));
            if (q == NULL) {
                return NULL;
            }
            s_sse_queues[i] = q;
            s_sse_clients++;
            return q;
        }
    }
    return NULL;
}

static void sse_unregister(QueueHandle_t q)
{
    for (size_t i = 0; i < SYNC_SSE_MAX_CLIENTS; i++) {
        if (s_sse_queues[i] != q) {
            continue;
        }

        char *pending = NULL;
        while (xQueueReceive(q, &pending, 0) == pdTRUE) {
            free(pending);
        }
        vQueueDelete(q);
        s_sse_queues[i] = NULL;
        if (s_sse_clients > 0) {
            s_sse_clients--;
        }
        return;
    }
}

esp_err_t sync_server_broadcast_event(const char *event_name, const char *json_payload)
{
    if (event_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *payload = (json_payload != NULL) ? json_payload : "{}";
    const size_t needed = strlen(event_name) + strlen(payload) + 32;

    char *chunk = malloc(needed);
    if (chunk == NULL) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(chunk, needed, "event: %s\ndata: %s\n\n", event_name, payload);

    size_t delivered = 0;
    for (size_t i = 0; i < SYNC_SSE_MAX_CLIENTS; i++) {
        QueueHandle_t q = s_sse_queues[i];
        if (q == NULL) {
            continue;
        }
        /* Every consumer frees its own copy, so the publisher never has to wait
         * for a reader before returning. */
        char *copy = strdup(chunk);
        if (copy == NULL) {
            continue;
        }
        if (xQueueSend(q, &copy, 0) != pdTRUE) {
            /* Slow client: drop the event rather than blocking the publisher. */
            free(copy);
            continue;
        }
        delivered++;
    }
    free(chunk);

    (void)sys_event_post_to(SYNC_EVENT, SYNC_EVENT_BROADCAST, event_name, strlen(event_name) + 1);
    return ESP_OK;
}

/**
 * A streaming subscriber.
 *
 * `req` is an *async* copy owned by this task. This is essential:
 * esp_http_server runs every URI handler inline in its single server task, so a
 * handler that streams until the client goes away freezes the whole server.
 * Measured before this change, with an SD card and one SSE subscriber attached:
 *
 *     GET /                  -> timeout (no TCP connection at all)
 *     GET /api/v1/words/stats -> timeout
 *     (both answer 200 in ~0.15 s again once the subscriber disconnects)
 *
 * The accept loop stops too, which is exactly the "设备不可达 / 读取失败" banner
 * the browser showed while it was happily holding its stream.
 * httpd_req_async_handler_begin() hands the socket to this task so the handler
 * can return immediately and the server keeps serving everyone else.
 */
typedef struct {
    httpd_req_t *req;       /**< async handle, awaited by the task */
    QueueHandle_t queue;    /**< per-client broadcast queue */
} sse_client_t;

static void sse_client_task(void *arg)
{
    sse_client_t *client = (sse_client_t *)arg;
    httpd_req_t *req = client->req;

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    /* Tell the client to retry after 3 s and announce the channel. */
    bool alive = (httpd_resp_send_chunk(req, "retry: 3000\n\n", HTTPD_RESP_USE_STRLEN) == ESP_OK);
    if (alive) {
        SYS_LOGI(TAG, "SSE client attached (%u/%d)", (unsigned)s_sse_clients,
                 SYNC_SSE_MAX_CLIENTS);
    }

    while (alive) {
        char *chunk = NULL;
        if (xQueueReceive(client->queue, &chunk, pdMS_TO_TICKS(SYNC_SSE_KEEPALIVE_MS)) == pdTRUE) {
            alive = (httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN) == ESP_OK);
            free(chunk);
        } else {
            /* Idle keepalive comment keeps NAT/proxies from closing the socket -
             * and tells us whether the peer is still there. */
            alive = (httpd_resp_send_chunk(req, ": keepalive\n\n", HTTPD_RESP_USE_STRLEN) == ESP_OK);
        }
    }

    sse_unregister(client->queue);
    SYS_LOGI(TAG, "SSE client detached (%u left)", (unsigned)s_sse_clients);

    /* Terminate the chunked body, then give the socket back to the server.
     * Not completing the async request would leak the session slot and, as the
     * header of httpd_req_async_handler_complete() warns, eventually stop the
     * server from accepting connections at all. */
    (void)httpd_resp_send_chunk(req, NULL, 0);
    (void)httpd_req_async_handler_complete(req);
    free(client);
    vTaskDelete(NULL);
}

static esp_err_t events_get_handler(httpd_req_t *req)
{
    QueueHandle_t queue = sse_register();
    if (queue == NULL) {
        return http_reply_unavailable(req, "too many event subscribers");
    }

    sse_client_t *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        sse_unregister(queue);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "sse client");
    }
    client->queue = queue;

    /* Up to here the request may still be answered directly; after this call
     * only `client->req` may be used - `req` is gone once we return. */
    if (httpd_req_async_handler_begin(req, &client->req) != ESP_OK) {
        sse_unregister(queue);
        free(client);
        return http_reply_unavailable(req, "streaming unavailable");
    }

    if (xTaskCreate(sse_client_task, "sse", SYNC_SSE_TASK_STACK, client,
                    SYNC_SSE_TASK_PRIO, NULL) != pdPASS) {
        sse_unregister(queue);
        (void)httpd_resp_set_status(client->req, "503 Service Unavailable");
        (void)httpd_resp_send(client->req, "no memory for streaming task", HTTPD_RESP_USE_STRLEN);
        (void)httpd_req_async_handler_complete(client->req);
        free(client);
        return ESP_OK;
    }

    /* Return immediately: the task owns the stream from here on, which is what
     * keeps the rest of the API responsive while the client stays attached. */
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Route table                                                               */
/* ------------------------------------------------------------------------ */

/** Routes this component owns. Add new device endpoints here. */
static const struct {
    const char *path;
    httpd_method_t method;
    http_route_handler_t handler;
    bool always_auth;
} s_own_routes[] = {
    { "/", HTTP_GET, root_get_handler, false },
    { "/index.html", HTTP_GET, root_get_handler, false },
    { "/api/v1/device/info", HTTP_GET, device_info_get_handler, false },
    { "/api/v1/device/status", HTTP_GET, device_status_get_handler, false },
    { "/api/v1/device/reboot", HTTP_POST, device_reboot_post_handler, true },
    { "/api/v1/device/auth", HTTP_POST, device_auth_post_handler, true },
    { "/api/v1/events", HTTP_GET, events_get_handler, false },
};

/**
 * Endpoints that exist in the specification but are not built yet. They are
 * registered on purpose so clients get 501 instead of 404 (requirement 8.1).
 */
static const struct {
    const char *path;
    httpd_method_t method;
    const char *hint;
} s_placeholder_routes[] = {
    { "/ws/events", HTTP_GET,
      "WebSocket is not available in this build; use /api/v1/events (SSE) instead" },
};

/* ------------------------------------------------------------------------ */
/* Notification relay                                                        */
/* ------------------------------------------------------------------------ */

/**
 * Turns any L0 notification into an SSE push. This is why sync_server does not
 * need to depend on the services that produce the changes.
 */
static void notify_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;

    const sys_notify_t *note = (const sys_notify_t *)data;
    if (note == NULL) {
        return;
    }
    if (s_sse_clients == 0) {
        /* Nobody is listening: skip formatting entirely. */
        return;
    }
    (void)sync_server_broadcast_event(note->name, note->payload);
}

/**
 * Connectivity is not a business change but every web client wants to know, so
 * it is turned into an L0 notification here - which also feeds the idle face's
 * trigger table without sync_server depending on it.
 */
static void connectivity_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;

    const bool *online = (const bool *)data;
    if (online == NULL) {
        return;
    }
    (void)sys_notify(*online ? "net_online" : "net_offline", NULL);
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

static esp_err_t mdns_start(void)
{
    if (!s_cfg.mdns_enabled) {
        SYS_LOGI(TAG, "mDNS disabled in configuration");
        return ESP_OK;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        SYS_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }

    (void)mdns_hostname_set(s_cfg.hostname);
    (void)mdns_instance_name_set("ToDoHelper");

    err = mdns_service_add("ToDoHelper", "_http", "_tcp", s_cfg.port, NULL, 0);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
        return err;
    }

    SYS_LOGI(TAG, "mDNS published as %s.local", s_cfg.hostname);
    return ESP_OK;
}

esp_err_t sync_server_register_route(const char *path, httpd_method_t method,
                                     http_route_handler_t handler)
{
    return http_router_register(path, method, handler);
}

esp_err_t sync_server_register_not_implemented(const char *path, httpd_method_t method,
                                               const char *hint)
{
    return http_router_register_not_implemented(path, method, hint);
}

esp_err_t sync_server_start(const sync_server_config_t *cfg)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    if (cfg != NULL) {
        s_cfg = *cfg;
    } else {
        config_defaults(&s_cfg);
    }
    refresh_url();

    /* The router owns auth policy decisions; sync_server provides the check. */
    http_router_set_auth_check(sync_server_check_auth);

    /* Relay every project notification to the SSE subscribers. */
    static bool notify_subscribed;
    if (!notify_subscribed) {
        if (sys_notify_subscribe(notify_handler, NULL, NULL) == ESP_OK) {
            notify_subscribed = true;
        } else {
            SYS_LOGW(TAG, "notification relay unavailable");
        }
        if (sys_event_subscribe(SYS_EVENT, SYS_EVENT_CONNECTIVITY, connectivity_handler,
                                NULL, NULL) != ESP_OK) {
            SYS_LOGW(TAG, "connectivity relay unavailable");
        }
    }

    for (size_t i = 0; i < sizeof(s_own_routes) / sizeof(s_own_routes[0]); i++) {
        esp_err_t err = http_router_register(s_own_routes[i].path, s_own_routes[i].method,
                                            s_own_routes[i].handler);
        if (err != ESP_OK) {
            SYS_LOGE(TAG, "register %s failed: %s", s_own_routes[i].path, esp_err_to_name(err));
            return err;
        }
        if (s_own_routes[i].always_auth) {
            (void)http_router_require_auth(s_own_routes[i].path, s_own_routes[i].method, true);
        }
    }

    for (size_t i = 0; i < sizeof(s_placeholder_routes) / sizeof(s_placeholder_routes[0]); i++) {
        (void)http_router_register_not_implemented(s_placeholder_routes[i].path,
                                                  s_placeholder_routes[i].method,
                                                  s_placeholder_routes[i].hint);
    }

    if (http_router_count() == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.server_port = s_cfg.port;
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;
    http_cfg.lru_purge_enable = true;
    /* Only the catch-all handler is registered, but leave a little headroom. */
    http_cfg.max_uri_handlers = 4;
    http_cfg.max_open_sockets = 5;
    http_cfg.stack_size = 6144;
    http_cfg.recv_wait_timeout = 5;
    http_cfg.send_wait_timeout = 5;

    esp_err_t err = httpd_start(&s_server, &http_cfg);
    if (err != ESP_OK) {
        SYS_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    err = http_router_attach(s_server);
    if (err != ESP_OK) {
        SYS_LOGE(TAG, "router attach failed: %s", esp_err_to_name(err));
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    (void)mdns_start();
    refresh_url();

    SYS_LOGI(TAG, "listening on port %u, %u route(s), auth=%s, url=%s",
             (unsigned)s_cfg.port, (unsigned)http_router_count(),
             s_cfg.auth_enabled ? "on" : "off", s_url);

    (void)sys_event_post_to(SYNC_EVENT, SYNC_EVENT_STARTED, NULL, 0);
    return ESP_OK;
}

esp_err_t sync_server_stop(void)
{
    for (size_t i = 0; i < SYNC_SSE_MAX_CLIENTS; i++) {
        if (s_sse_queues[i] != NULL) {
            sse_unregister(s_sse_queues[i]);
        }
    }

    if (s_server == NULL) {
        return ESP_OK;
    }

    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    (void)sys_event_post_to(SYNC_EVENT, SYNC_EVENT_STOPPED, NULL, 0);
    SYS_LOGI(TAG, "stopped");
    return err;
}

esp_err_t sync_server_init(void)
{
    if (s_initialized) {
        return sync_server_is_running() ? ESP_OK : sync_server_start(NULL);
    }
    s_initialized = true;

    sync_server_config_t cfg;
    config_defaults(&cfg);

    /* A server that cannot start must not stop the device from booting. */
    esp_err_t err = sync_server_start(&cfg);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "HTTP server unavailable: %s", esp_err_to_name(err));
    }
    return err;
}
