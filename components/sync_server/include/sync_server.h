/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sync_server.h
 * @brief L4 LAN HTTP server: lifecycle, device API, SSE push, browser page.
 *
 * The server itself owns no business logic. Business components register their
 * own routes on the L1 http_router, so adding an API never means editing this
 * component (docs/TODOHELPER_FRAMEWORK.md 5.6 / 7.4).
 *
 * HTTP contract (requirement 8.1): every response is
 * `{"code":<status or 0>,"msg":"...","data":{...}}`; a registered but
 * unimplemented route answers 501, an unknown path answers 404.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"

#include "http_router.h"
#include "sys_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(SYNC_EVENT);

typedef enum {
    SYNC_EVENT_STARTED = 0,       /**< no payload */
    SYNC_EVENT_STOPPED,           /**< no payload */
    SYNC_EVENT_CLIENT_CONNECTED,  /**< no payload: see /api/v1/device/status */
    SYNC_EVENT_AUTH_FAILED,       /**< payload: const char *path */
    SYNC_EVENT_BROADCAST,         /**< payload: const char *event name */
} sync_event_id_t;

/** Default HTTP port. */
#define SYNC_SERVER_DEFAULT_PORT 80
/** Default mDNS hostname, giving http://todohelper.local */
#define SYNC_SERVER_DEFAULT_HOSTNAME "todohelper"

typedef struct {
    uint16_t port;
    bool mdns_enabled;
    bool auth_enabled;
    char hostname[32];
} sync_server_config_t;

/**
 * @brief Prepare and start the server using NVS configuration.
 *
 * Idempotent. Treats a failure as recoverable: the rest of the device keeps
 * working without HTTP (requirement 15).
 */
esp_err_t sync_server_init(void);

/** @brief Start (or restart) with an explicit configuration. */
esp_err_t sync_server_start(const sync_server_config_t *cfg);

/** @brief Stop the server and unpublish mDNS. */
esp_err_t sync_server_stop(void);

/** @brief True while the HTTP server is listening. */
bool sync_server_is_running(void);

/** @brief "http://todohelper.local" or "http://192.168.1.23"; static storage. */
const char *sync_server_url(void);

/** @brief The httpd handle, or NULL when stopped. */
httpd_handle_t sync_server_handle(void);

/* ---- Routes ------------------------------------------------------------ */

/**
 * @brief Publish a route. Thin wrapper over http_router_register().
 */
esp_err_t sync_server_register_route(const char *path, httpd_method_t method,
                                     http_route_handler_t handler);

/** @brief Publish a "known but not built yet" route that answers 501. */
esp_err_t sync_server_register_not_implemented(const char *path, httpd_method_t method,
                                               const char *hint);

/* ---- Auth (requirement 8.8) -------------------------------------------- */

/**
 * @brief Set and persist the auth token; also enables authentication.
 *
 * Passing NULL or "" disables authentication.
 */
esp_err_t sync_server_set_auth_token(const char *token);

/** @brief True when a write operation currently requires a token. */
bool sync_server_auth_required(void);

/**
 * @brief Auth hook used by the router. Reads `X-Auth-Token` only - never a
 *        query parameter, because query strings end up in logs.
 */
bool sync_server_check_auth(httpd_req_t *req);

/* ---- Server-sent events (requirement 8.7) ------------------------------ */

/**
 * @brief Push an event to every connected SSE client.
 *
 * WebSocket is not available in this IDF build, so `/api/v1/events` provides
 * the equivalent push channel; `/ws/events` stays registered as 501 so clients
 * can detect the difference (requirement 8.7).
 *
 * @param event_name Short event name, e.g. "todo_changed".
 * @param json_payload Serialised JSON, or NULL for `{}`.
 */
esp_err_t sync_server_broadcast_event(const char *event_name, const char *json_payload);

/** @brief Number of clients currently subscribed to /api/v1/events. */
size_t sync_server_sse_clients(void);

#ifdef __cplusplus
}
#endif
