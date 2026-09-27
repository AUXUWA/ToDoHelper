/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file http_router.h
 * @brief L1 HTTP route table and response envelope.
 *
 * Why this component exists: requirement 7.12 wants business components to
 * publish their own `/api/v1/...` routes without sync_server having to know
 * them. Putting that registry in `sync_server` (L4) would force every L3
 * service to depend upwards on it. Living in L1 instead keeps the dependency
 * direction downward for everybody:
 *
 *   todo_manager (L3) --> http_router (L1) <-- sync_server (L4)
 *
 * The router also owns the project-wide response contract from requirement 8.1
 * so that 404 / 405 / 501 semantics are identical for every route:
 *
 *   {"code": <http status or 0 on success>, "msg": "...", "data": {...}}
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Route handler. Return ESP_OK when a response was sent. */
typedef esp_err_t (*http_route_handler_t)(httpd_req_t *req);

/** Auth hook, installed by sync_server (it owns the token and the policy). */
typedef bool (*http_auth_check_t)(httpd_req_t *req);

/**
 * Maximum number of registered routes.
 *
 * Each (path, method) pair occupies one slot, and registration is
 * first-come-first-served in module order: once the table is full every later
 * route_add() fails with "route table full" and that endpoint simply 404s at
 * runtime - there is no compile-time warning.
 *
 * The table was 48 and 1.3.0 added 6 maimemo routes, which took the total to
 * exactly 48: the table was full with **zero** headroom, so any further endpoint
 * would have been dropped silently. Raised to 64 to leave room for the next
 * feature; the cost is 16 * sizeof(http_route_t) of .bss (route paths are 64 B
 * each, so under 2 KB).
 */
#define HTTP_ROUTER_MAX_ROUTES 64

/* ------------------------------------------------------------------------ */
/* Registration                                                              */
/* ------------------------------------------------------------------------ */

/**
 * @brief Register a handler.
 *
 * @param path Route pattern. Either an exact path such as "/api/v1/todos", or a
 *             pattern containing a single asterisk that stands for any run of
 *             characters: a trailing asterisk after "/api/v1/todos/" addresses
 *             one item, and an asterisk followed by a literal "/done" addresses
 *             an action on one item. The most specific match wins.
 * @param method Method to serve, or HTTP_ANY for all methods.
 */
esp_err_t http_router_register(const char *path, httpd_method_t method,
                               http_route_handler_t handler);

/**
 * @brief Register a known-but-unimplemented route so it answers 501.
 *
 * Requirement 8.1: clients must be able to tell "not built yet" from "wrong
 * URL", so placeholder routes are registered explicitly.
 *
 * @param hint Optional short explanation sent back in `data.detail`.
 */
esp_err_t http_router_register_not_implemented(const char *path, httpd_method_t method,
                                               const char *hint);

/**
 * @brief Force (or clear) the auth requirement for one route.
 *
 * Regardless of this flag, write methods go through the auth hook; this flag
 * additionally protects read methods such as `POST /api/v1/device/reboot`.
 */
esp_err_t http_router_require_auth(const char *path, httpd_method_t method, bool required);

/**
 * @brief Install the catch-all dispatcher on @p server.
 */
esp_err_t http_router_attach(httpd_handle_t server);

/** @brief Number of registered routes, for /api/v1/device/status. */
size_t http_router_count(void);

/* ------------------------------------------------------------------------ */
/* Auth                                                                      */
/* ------------------------------------------------------------------------ */

void http_router_set_auth_check(http_auth_check_t check);

/**
 * @brief Ask the installed hook whether @p req may proceed.
 *
 * Returns true when no hook is installed (fail-open) - sync_server always
 * installs one, so this only affects tests.
 */
bool http_router_check_auth(httpd_req_t *req);

/* ------------------------------------------------------------------------ */
/* Responses (requirement 8.1)                                               */
/* ------------------------------------------------------------------------ */

/**
 * @brief Send `{"code":..,"msg":..,"data":..}`.
 *
 * @param status   HTTP status code. 200/201 make `code` 0, anything else is
 *                 copied into `code`.
 * @param data_json Pre-serialised JSON, or NULL for `null`.
 */
esp_err_t http_reply_json(httpd_req_t *req, int status, const char *msg,
                          const char *data_json);

/** @brief 200 with `code: 0`. */
esp_err_t http_reply_ok(httpd_req_t *req, const char *data_json);

/** @brief Error status with a short message and `data: null`. */
esp_err_t http_reply_err(httpd_req_t *req, int status, const char *msg);

esp_err_t http_reply_bad_request(httpd_req_t *req, const char *msg);
esp_err_t http_reply_not_found(httpd_req_t *req);
esp_err_t http_reply_method_not_allowed(httpd_req_t *req);
esp_err_t http_reply_unauthorized(httpd_req_t *req);
esp_err_t http_reply_unavailable(httpd_req_t *req, const char *msg);

/** @brief 501 with an explanatory detail. */
esp_err_t http_reply_not_implemented(httpd_req_t *req, const char *hint);

/** @brief 500 with the ESP-IDF error name in `data.detail`. */
esp_err_t http_reply_esp_error(httpd_req_t *req, esp_err_t err, const char *context);

/* ------------------------------------------------------------------------ */
/* Request helpers                                                           */
/* ------------------------------------------------------------------------ */

/**
 * @brief Read the whole request body into a NUL-terminated heap buffer.
 *        The caller frees it with free().
 */
esp_err_t http_read_body(httpd_req_t *req, char **out, size_t *len);

/** @brief Copy the request path (URI without the query string) into @p out. */
esp_err_t http_get_path(httpd_req_t *req, char *out, size_t len);

/** @brief Value of `?key=` from the query string; false when absent. */
bool http_query_get(httpd_req_t *req, const char *key, char *out, size_t len);

/** @brief Parse a decimal query parameter; false when absent or malformed. */
bool http_query_get_i64(httpd_req_t *req, const char *key, int64_t *out);

/**
 * @brief Copy the @p index -th '/'-separated segment of the path.
 *
 * `/api/v1/todos/12/done` with index 3 yields "12".
 */
bool http_path_segment(httpd_req_t *req, int index, char *out, size_t len);

/** @brief Parse the @p index -th path segment as an unsigned integer. */
bool http_path_segment_u32(httpd_req_t *req, int index, uint32_t *out);

#ifdef __cplusplus
}
#endif
