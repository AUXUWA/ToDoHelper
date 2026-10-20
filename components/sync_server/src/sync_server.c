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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"

#include "storage_manager.h"
#include "wifi_manager.h"

static const char *TAG = "sync";

ESP_EVENT_DEFINE_BASE(SYNC_EVENT);

/**
 * The auth model, in one place.
 *
 * - **No token configured** → the device is open on the LAN. That is the
 *   long-standing default (requirement 8.8) and the state the unit ships in;
 *   demanding a token on some routes while no token exists would only block
 *   the three ways to *get* going: setting the first token, configuring the
 *   AI key, and using the debug shell. (All three were 401 in v1.5.0 - the
 *   "strict" list was written as "these always need a token", but with no
 *   token configured there is nothing to compare against, so it only ever
 *   locked the user out of their own device.)
 * - **A token is configured** → *every* route needs it, **reads included**.
 *   That is the hole v1.5.0 closed: before, `GET /api/v1/todos` and
 *   `GET /api/v1/ai/history` were readable by anyone on the LAN even with
 *   authentication "enabled", while the web page told the user the opposite.
 *
 * The owner can always clear the token from the device's own settings page,
 * so a LAN client that sets one first cannot lock them out for good.
 */

/**
 * Read routes that have to go through the auth hook as well.
 *
 * The router only forces the auth hook for write methods
 * (`need_auth = route->auth_required || is_write_method()`), so a GET route is
 * protected only when it is *marked* with http_router_require_auth(). These
 * carry the user's own data: the todo list and the AI conversation log.
 *
 * The SSE stream (/api/v1/events) is marked the same way - it is owned by this
 * component, see s_own_routes[].
 */
static const struct {
    const char *path;
    httpd_method_t method;
} s_protected_routes[] = {
    { "/api/v1/todos", HTTP_GET },
    { "/api/v1/todos/*", HTTP_GET },
    { "/api/v1/ai/history", HTTP_GET },
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

/**
 * Cached HTTP auth token.
 *
 * token_matches() runs on every request that needs a token. Reading the token
 * from NVS there costs two nvs_get_str() calls *and* the global config lock
 * (`xSemaphoreTake(s_lock, portMAX_DELAY)`) - and that lock is also taken by
 * the UI task, so a burst of requests can park the httpd task behind a flash
 * write. The token only changes when it is set, so it is cached here and
 * refreshed on every change instead.
 *
 * The token is a secret: it is never logged and never read back through an API
 * (same rule as the AI key).
 */
#define SYNC_AUTH_TOKEN_MAX 80
static char s_auth_token[SYNC_AUTH_TOKEN_MAX];

/**
 * Whether a token is configured.
 *
 * Kept apart from the token itself so a token that does not fit into the cache
 * (only possible when somebody wrote NVS behind our back) fails *closed*
 * instead of silently opening the device up.
 */
static bool s_auth_enabled;

static QueueHandle_t s_sse_queues[SYNC_SSE_MAX_CLIENTS];

/**
 * Lock for the subscriber table.
 *
 * sse_register() runs in the httpd task while sse_unregister() runs in the SSE
 * task: without a lock a single lost update makes the counter disagree with
 * the real number of connections, and notify_handler() uses "nobody is
 * listening" as the reason to drop the event - so one lost update would
 * silently stop every push while the web page still shows "推送中".
 *
 * The critical sections are a couple of pointer assignments long and never
 * block.
 */
static SemaphoreHandle_t s_sse_lock;

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

    /*
     * The auth state is not read here: it comes from the token cache, which
     * auth_cache_refresh() fills. NVS is the single source of truth for the
     * token, so the flag is derived from it instead of being read twice.
     */
    cfg->auth_enabled = s_auth_enabled;
}

/**
 * Re-read the token from NVS into the cache and update the auth flag.
 *
 * Called when the server starts and whenever the token is set or cleared. The
 * token itself is never written to the log.
 */
static void auth_cache_refresh(void)
{
    char token[SYNC_AUTH_TOKEN_MAX] = { 0 };
    const esp_err_t err = sys_config_get_str(SYS_KEY_AUTH_TOKEN, token, sizeof(token));

    if (err == ESP_OK) {
        strlcpy(s_auth_token, token, sizeof(s_auth_token));
        s_auth_enabled = (s_auth_token[0] != '\0');
    } else if (err == ESP_ERR_INVALID_SIZE) {
        /* The stored token is longer than the cache: it can never match a
         * request, so fail closed (auth on, nobody gets in) instead of
         * treating the device as unprotected. */
        s_auth_token[0] = '\0';
        s_auth_enabled = true;
    } else {
        s_auth_token[0] = '\0';
        s_auth_enabled = false;
    }

    s_cfg.auth_enabled = s_auth_enabled;
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

/**
 * Compare the request's token with the cached one.
 *
 * Only the `X-Auth-Token` header is accepted - a token in the URL query would
 * end up in the access log.
 */
static bool token_matches(httpd_req_t *req)
{
    char provided[SYNC_AUTH_TOKEN_MAX];
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", provided, sizeof(provided)) != ESP_OK) {
        /* No header at all: fail without touching NVS. */
        return false;
    }

    if (s_auth_enabled && s_auth_token[0] != '\0' && strcmp(s_auth_token, provided) == 0) {
        return true;   /* cache hit: the happy path never reads NVS */
    }

    /*
     * Cache miss. Either the client sent the wrong token, or the token was
     * changed behind our back (the debug shell writes NVS directly). Re-read
     * once so a fresh token cannot be locked out by a stale cache - this only
     * happens on the failure path.
     */
    auth_cache_refresh();
    return s_auth_enabled && s_auth_token[0] != '\0' && strcmp(s_auth_token, provided) == 0;
}

/**
 * @brief Auth decision for one request (requirement 8.8).
 *
 * Two layers:
 *  1. **no token configured** -> open. That is the long-standing LAN default
 *     and the state the unit ships in. Requiring a token on some routes while
 *     no token exists only blocks the three ways to get going: setting the
 *     first token, configuring the AI key, using the debug shell.
 *  2. **a token is configured** -> every route needs it, reads included.
 *  3. the token itself is only read from the `X-Auth-Token` header.
 */
bool sync_server_check_auth(httpd_req_t *req)
{
    char path[128];
    if (http_get_path(req, path, sizeof(path)) != ESP_OK) {
        return false;
    }

    if (!s_auth_enabled) {
        /* LAN default: no token configured -> open (requirement 8.8). */
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
    return s_auth_enabled;
}

esp_err_t sync_server_set_auth_token(const char *token)
{
    if (token == NULL || token[0] == '\0') {
        (void)sys_config_erase(SYS_KEY_AUTH_TOKEN);
        s_auth_token[0] = '\0';
        s_auth_enabled = false;
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

    /* Refresh the cache right away: the next request must see the new token
     * without waiting for a reboot. */
    strlcpy(s_auth_token, token, sizeof(s_auth_token));
    s_auth_enabled = true;
    s_cfg.auth_enabled = true;
    /* The token itself must never be logged. */
    SYS_LOGI(TAG, "HTTP authentication enabled");
    return ESP_OK;
}

/**
 * Mark the read routes that have to be authenticated.
 *
 * http_router_require_auth() looks the route up by its exact (path, method)
 * pair, so it only works *after* the owner registered the route. The owners
 * register in their own init - todo_manager before sync_server, ai_service
 * after it - so the marks are applied twice: once when the server starts and
 * once when every module has finished initialising (SYS_EVENT_READY).
 */
static void apply_auth_marks(void)
{
    for (size_t i = 0; i < sizeof(s_protected_routes) / sizeof(s_protected_routes[0]); i++) {
        const esp_err_t err = http_router_require_auth(s_protected_routes[i].path,
                                                       s_protected_routes[i].method, true);
        if (err != ESP_OK) {
            /* The owning component has not registered the route (yet). */
            SYS_LOGD(TAG, "auth mark skipped for %s (%s)",
                     s_protected_routes[i].path, esp_err_to_name(err));
        }
    }
}

/**
 * Every module has finished initialising.
 *
 * The components that start after sync_server (ai_service) have registered
 * their routes by now, so their routes can be marked as auth-required too.
 */
static void ready_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;

    apply_auth_marks();
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
    cJSON_AddNumberToObject(root, "sse_clients", (double)sync_server_sse_clients());
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

/** Take the subscriber lock. No-op when it could not be created. */
static void sse_lock(void)
{
    if (s_sse_lock != NULL) {
        (void)xSemaphoreTake(s_sse_lock, portMAX_DELAY);
    }
}

static void sse_unlock(void)
{
    if (s_sse_lock != NULL) {
        (void)xSemaphoreGive(s_sse_lock);
    }
}

/** Number of attached subscribers. The caller must hold the lock. */
static size_t sse_clients_locked(void)
{
    size_t count = 0;
    for (size_t i = 0; i < SYNC_SSE_MAX_CLIENTS; i++) {
        if (s_sse_queues[i] != NULL) {
            count++;
        }
    }
    return count;
}

size_t sync_server_sse_clients(void)
{
    sse_lock();
    const size_t count = sse_clients_locked();
    sse_unlock();
    return count;
}

/** Take a free per-client queue slot, or NULL when all are in use. */
static QueueHandle_t sse_register(void)
{
    /* Created outside the lock: the queue allocation must not happen while
     * the table is locked. */
    QueueHandle_t q = xQueueCreate(8, sizeof(char *));
    if (q == NULL) {
        return NULL;
    }

    sse_lock();
    QueueHandle_t slot = NULL;
    for (size_t i = 0; i < SYNC_SSE_MAX_CLIENTS; i++) {
        if (s_sse_queues[i] == NULL) {
            s_sse_queues[i] = q;
            slot = q;
            break;
        }
    }
    sse_unlock();

    if (slot == NULL) {
        vQueueDelete(q);
        return NULL;
    }
    return slot;
}

static void sse_unregister(QueueHandle_t q)
{
    if (q == NULL) {
        return;
    }

    /*
     * Detach the queue from the table first: a broadcaster that is already
     * inside its critical section holds the lock, so it cannot be looking at
     * a queue that is about to disappear.
     */
    sse_lock();
    for (size_t i = 0; i < SYNC_SSE_MAX_CLIENTS; i++) {
        if (s_sse_queues[i] == q) {
            s_sse_queues[i] = NULL;
            break;
        }
    }
    sse_unlock();

    /* The queue is owned by whoever registered it, so it is always torn down
     * here - even when it was detached from the table already (server stop). */
    char *pending = NULL;
    while (xQueueReceive(q, &pending, 0) == pdTRUE) {
        free(pending);
    }
    vQueueDelete(q);
}

/**
 * Detach every subscriber queue from the table without deleting it.
 *
 * Used when the server stops: the SSE tasks are still blocked on their
 * queues, and deleting a queue a task is waiting on is undefined behaviour.
 * The tasks own their queue and delete it when they notice the dead socket.
 */
static void sse_detach_all(void)
{
    sse_lock();
    for (size_t i = 0; i < SYNC_SSE_MAX_CLIENTS; i++) {
        s_sse_queues[i] = NULL;
    }
    sse_unlock();
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

    /*
     * The whole fan-out happens under the lock: the SSE task may tear a
     * queue down at any moment, and a send on a deleted queue is a crash.
     * The sends never block, so the critical section stays short.
     */
    sse_lock();
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
    }
    sse_unlock();

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
        SYS_LOGI(TAG, "SSE client attached (%u/%d)", (unsigned)sync_server_sse_clients(),
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
    SYS_LOGI(TAG, "SSE client detached (%u left)", (unsigned)sync_server_sse_clients());

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
    bool require_auth;   /**< 标记为"读方法也要过鉴权钩子" */
} s_own_routes[] = {
    { "/", HTTP_GET, root_get_handler, false },
    { "/index.html", HTTP_GET, root_get_handler, false },
    { "/api/v1/device/info", HTTP_GET, device_info_get_handler, false },
    { "/api/v1/device/status", HTTP_GET, device_status_get_handler, false },
    { "/api/v1/device/reboot", HTTP_POST, device_reboot_post_handler, true },
    { "/api/v1/device/auth", HTTP_POST, device_auth_post_handler, true },
    /* The stream carries the very data the read routes protect, so it is
     * marked as well: the web page sends X-Auth-Token on the SSE connection
     * (fetch based stream) and falls back to polling when it gets a 401. */
    { "/api/v1/events", HTTP_GET, events_get_handler, true },
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
    if (sync_server_sse_clients() == 0) {
        /* Nobody is listening: skip formatting entirely. The count is taken
         * under the lock, so it cannot drift away from the real number of
         * subscribers (which used to silently swallow every push). */
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

    /*
     * Fill the token cache before anything can ask for authentication: NVS is
     * the single source of truth, and s_cfg.auth_enabled is derived from it
     * (a caller-provided auth_enabled is overwritten on purpose - otherwise
     * the flag and the token could disagree).
     */
    auth_cache_refresh();

    refresh_url();

    /* The router owns auth policy decisions; sync_server provides the check. */
    http_router_set_auth_check(sync_server_check_auth);

    /* The subscriber table is touched by the httpd task and by the SSE tasks. */
    if (s_sse_lock == NULL) {
        s_sse_lock = xSemaphoreCreateMutex();
        if (s_sse_lock == NULL) {
            /* Not fatal: the web server still works, only the subscriber count
             * could drift again. */
            SYS_LOGE(TAG, "SSE lock unavailable");
        }
    }

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
        /*
         * The components that start after sync_server (ai_service) register
         * their routes later, so their auth marks are applied once every
         * module has finished initialising.
         */
        if (sys_event_subscribe(SYS_EVENT, SYS_EVENT_READY, ready_handler,
                                NULL, NULL) != ESP_OK) {
            SYS_LOGW(TAG, "auth mark hook unavailable");
        }
    }

    for (size_t i = 0; i < sizeof(s_own_routes) / sizeof(s_own_routes[0]); i++) {
        esp_err_t err = http_router_register(s_own_routes[i].path, s_own_routes[i].method,
                                            s_own_routes[i].handler);
        if (err != ESP_OK) {
            SYS_LOGE(TAG, "register %s failed: %s", s_own_routes[i].path, esp_err_to_name(err));
            return err;
        }
        if (s_own_routes[i].require_auth) {
            (void)http_router_require_auth(s_own_routes[i].path, s_own_routes[i].method, true);
        }
    }

    /* Routes of the components that registered before us are marked right
     * away; the others are marked on SYS_EVENT_READY. */
    apply_auth_marks();

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
    /*
     * Only detach the queues: the SSE tasks are still waiting on them, and a
     * queue that is deleted under a waiting task is undefined behaviour. Each
     * task deletes its own queue when the socket dies.
     */
    sse_detach_all();

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
