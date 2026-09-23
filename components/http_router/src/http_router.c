/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "http_router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "http_router";

#define ROUTER_PATH_MAX 64

typedef struct {
    char path[ROUTER_PATH_MAX];
    httpd_method_t method;
    http_route_handler_t handler;
    const char *hint;         /**< non-NULL for 501 placeholders */
    bool auth_required;
    bool used;
} http_route_t;

static http_route_t s_routes[HTTP_ROUTER_MAX_ROUTES];
static size_t s_route_count;
static http_auth_check_t s_auth_check;

/* ------------------------------------------------------------------------ */
/* Registration                                                              */
/* ------------------------------------------------------------------------ */

static http_route_t *find_route(const char *path, httpd_method_t method)
{
    for (size_t i = 0; i < HTTP_ROUTER_MAX_ROUTES; i++) {
        if (!s_routes[i].used) {
            continue;
        }
        if (s_routes[i].method == method && strcmp(s_routes[i].path, path) == 0) {
            return &s_routes[i];
        }
    }
    return NULL;
}

static esp_err_t route_add(const char *path, httpd_method_t method,
                           http_route_handler_t handler, const char *hint)
{
    if (path == NULL || path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(path) >= ROUTER_PATH_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    http_route_t *existing = find_route(path, method);
    if (existing != NULL) {
        /* Re-registration replaces the handler, which keeps component
         * re-init idempotent. */
        existing->handler = handler;
        existing->hint = hint;
        return ESP_OK;
    }

    for (size_t i = 0; i < HTTP_ROUTER_MAX_ROUTES; i++) {
        if (s_routes[i].used) {
            continue;
        }
        strlcpy(s_routes[i].path, path, sizeof(s_routes[i].path));
        s_routes[i].method = method;
        s_routes[i].handler = handler;
        s_routes[i].hint = hint;
        s_routes[i].auth_required = false;
        s_routes[i].used = true;
        s_route_count++;
        return ESP_OK;
    }

    ESP_LOGE(TAG, "route table full (%d)", HTTP_ROUTER_MAX_ROUTES);
    return ESP_ERR_NO_MEM;
}

esp_err_t http_router_register(const char *path, httpd_method_t method,
                               http_route_handler_t handler)
{
    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return route_add(path, method, handler, NULL);
}

esp_err_t http_router_register_not_implemented(const char *path, httpd_method_t method,
                                               const char *hint)
{
    return route_add(path, method, NULL, hint);
}

esp_err_t http_router_require_auth(const char *path, httpd_method_t method, bool required)
{
    http_route_t *route = find_route(path, method);
    if (route == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    route->auth_required = required;
    return ESP_OK;
}

size_t http_router_count(void)
{
    return s_route_count;
}

/* ------------------------------------------------------------------------ */
/* Matching                                                                  */
/* ------------------------------------------------------------------------ */

/**
 * Pattern match with one optional asterisk.
 *
 * A trailing asterisk after "/api/v1/todos/" matches "/api/v1/todos/12"; an
 * asterisk followed by a literal "/done" matches "/api/v1/todos/12/done".
 */
static bool pattern_match(const char *pattern, const char *path, size_t *score)
{
    const char *star = strchr(pattern, '*');
    const size_t path_len = strlen(path);

    if (star == NULL) {
        if (strcmp(pattern, path) != 0) {
            return false;
        }
        /* Exact matches always beat wildcards. */
        if (score != NULL) {
            *score = 1000 + path_len;
        }
        return true;
    }

    const size_t prefix_len = (size_t)(star - pattern);
    if (strncmp(pattern, path, prefix_len) != 0) {
        return false;
    }

    const char *suffix = star + 1;
    const size_t suffix_len = strlen(suffix);
    if (path_len < prefix_len + suffix_len) {
        return false;
    }
    if (suffix_len > 0 && strcmp(path + path_len - suffix_len, suffix) != 0) {
        return false;
    }

    /* The asterisk must consume at least one character, so a trailing-asterisk
     * pattern does not also swallow the collection path itself. */
    if (path_len == prefix_len + suffix_len) {
        return false;
    }

    if (score != NULL) {
        *score = prefix_len + suffix_len;
    }
    return true;
}

static bool is_write_method(httpd_method_t method)
{
    switch (method) {
    case HTTP_POST:
    case HTTP_PUT:
    case HTTP_PATCH:
    case HTTP_DELETE:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------ */
/* Dispatch                                                                  */
/* ------------------------------------------------------------------------ */

static esp_err_t send_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods",
                       "GET,POST,PUT,PATCH,DELETE,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers",
                       "Content-Type,X-Auth-Token");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t router_dispatch(httpd_req_t *req)
{
    char path[256];
    if (http_get_path(req, path, sizeof(path)) != ESP_OK) {
        return http_reply_bad_request(req, "uri too long");
    }

    if (req->method == HTTP_OPTIONS) {
        return send_options(req);
    }

    /* Pass 1: best match that also serves this method.
     * Pass 2 (only if nothing matched): detect a path match with the wrong
     * method so we can answer 405 instead of 404. */
    const http_route_t *best = NULL;
    size_t best_score = 0;
    bool path_exists = false;

    for (size_t i = 0; i < HTTP_ROUTER_MAX_ROUTES; i++) {
        const http_route_t *route = &s_routes[i];
        if (!route->used) {
            continue;
        }

        size_t score = 0;
        if (!pattern_match(route->path, path, &score)) {
            continue;
        }

        path_exists = true;

        if (route->method != HTTP_ANY && route->method != (httpd_method_t)req->method) {
            continue;
        }
        if (best == NULL || score > best_score) {
            best = route;
            best_score = score;
        }
    }

    if (best == NULL) {
        if (path_exists) {
            return http_reply_method_not_allowed(req);
        }
        return http_reply_not_found(req);
    }

    if (best->hint != NULL || best->handler == NULL) {
        return http_reply_not_implemented(req, best->hint);
    }

    const bool need_auth = best->auth_required || is_write_method((httpd_method_t)req->method);
    if (need_auth && !http_router_check_auth(req)) {
        return http_reply_unauthorized(req);
    }

    return best->handler(req);
}

esp_err_t http_router_attach(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t catch_all = {
        .uri = "/*",
        .method = HTTP_ANY,
        .handler = router_dispatch,
        .user_ctx = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(server, &catch_all);
    if (err == ESP_ERR_HTTPD_HANDLER_EXISTS) {
        /* Already attached (idempotent restart). */
        return ESP_OK;
    }
    return err;
}

/* ------------------------------------------------------------------------ */
/* Auth                                                                      */
/* ------------------------------------------------------------------------ */

void http_router_set_auth_check(http_auth_check_t check)
{
    s_auth_check = check;
}

bool http_router_check_auth(httpd_req_t *req)
{
    if (s_auth_check == NULL) {
        return true;
    }
    return s_auth_check(req);
}

/* ------------------------------------------------------------------------ */
/* Responses                                                                 */
/* ------------------------------------------------------------------------ */

/** Escape the characters that would break a JSON string. */
static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    if (out_len == 0) {
        return;
    }
    for (size_t i = 0; in != NULL && in[i] != '\0' && o + 2 < out_len; i++) {
        const char c = in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = c;
        } else if (c == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (c == '\r') {
            out[o++] = '\\';
            out[o++] = 'r';
        } else if (c == '\t') {
            out[o++] = '\\';
            out[o++] = 't';
        } else if ((unsigned char)c < 0x20) {
            /* Drop other control characters rather than emitting invalid JSON. */
            continue;
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

esp_err_t http_reply_json(httpd_req_t *req, int status, const char *msg,
                          const char *data_json)
{
    char safe_msg[128];
    json_escape(msg != NULL ? msg : "", safe_msg, sizeof(safe_msg));

    const char *data = (data_json != NULL) ? data_json : "null";
    const int code = (status == 200 || status == 201) ? 0 : status;

    const size_t needed = strlen(safe_msg) + strlen(data) + 64;
    char *body = malloc(needed);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(body, needed, "{\"code\":%d,\"msg\":\"%s\",\"data\":%s}", code, safe_msg, data);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char status_str[16];
    snprintf(status_str, sizeof(status_str), "%d", status);
    httpd_resp_set_status(req, status_str);

    const esp_err_t err = httpd_resp_send(req, body, strlen(body));
    free(body);
    return err;
}

esp_err_t http_reply_ok(httpd_req_t *req, const char *data_json)
{
    return http_reply_json(req, 200, "ok", data_json);
}

esp_err_t http_reply_err(httpd_req_t *req, int status, const char *msg)
{
    return http_reply_json(req, status, msg, NULL);
}

esp_err_t http_reply_bad_request(httpd_req_t *req, const char *msg)
{
    return http_reply_err(req, 400, msg ? msg : "bad request");
}

esp_err_t http_reply_not_found(httpd_req_t *req)
{
    return http_reply_err(req, 404, "not found");
}

esp_err_t http_reply_method_not_allowed(httpd_req_t *req)
{
    return http_reply_err(req, 405, "method not allowed");
}

esp_err_t http_reply_unauthorized(httpd_req_t *req)
{
    return http_reply_err(req, 401, "unauthorized");
}

esp_err_t http_reply_unavailable(httpd_req_t *req, const char *msg)
{
    return http_reply_err(req, 503, msg ? msg : "unavailable");
}

esp_err_t http_reply_not_implemented(httpd_req_t *req, const char *hint)
{
    if (hint == NULL) {
        return http_reply_err(req, 501, "not implemented");
    }

    char safe[192];
    json_escape(hint, safe, sizeof(safe));

    char data[256];
    snprintf(data, sizeof(data), "{\"detail\":\"not implemented\",\"hint\":\"%s\"}", safe);
    return http_reply_json(req, 501, "not implemented", data);
}

esp_err_t http_reply_esp_error(httpd_req_t *req, esp_err_t err, const char *context)
{
    char data[192];
    snprintf(data, sizeof(data), "{\"detail\":\"%s\",\"esp_err\":\"%s\"}",
             context ? context : "", esp_err_to_name(err));
    return http_reply_json(req, 500, "internal error", data);
}

/* ------------------------------------------------------------------------ */
/* Request helpers                                                           */
/* ------------------------------------------------------------------------ */

esp_err_t http_get_path(httpd_req_t *req, char *out, size_t len)
{
    if (req == NULL || out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *uri = req->uri;
    size_t n = 0;
    while (uri[n] != '\0' && uri[n] != '?' && n + 1 < len) {
        out[n] = uri[n];
        n++;
    }
    out[n] = '\0';
    return (uri[n] == '\0' || uri[n] == '?') ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t http_read_body(httpd_req_t *req, char **out, size_t *len)
{
    if (req == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    const size_t total = req->content_len;
    if (total == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (total > 16 * 1024) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc(total + 1);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    while (received < total) {
        const int got = httpd_req_recv(req, buf + received, total - received);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            return ESP_FAIL;
        }
        received += (size_t)got;
    }
    buf[received] = '\0';

    *out = buf;
    if (len != NULL) {
        *len = received;
    }
    return ESP_OK;
}

/** Extract `?key=value` from req->uri. Returns the value length or -1. */
static int query_value(const char *uri, const char *key, const char **out)
{
    const char *q = strchr(uri, '?');
    if (q == NULL) {
        return -1;
    }
    q++;

    const size_t key_len = strlen(key);
    const char *p = q;
    while (*p != '\0') {
        const char *amp = strchr(p, '&');
        const size_t seg_len = (amp != NULL) ? (size_t)(amp - p) : strlen(p);

        if (seg_len > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            *out = p + key_len + 1;
            return (int)(seg_len - key_len - 1);
        }
        if (amp == NULL) {
            break;
        }
        p = amp + 1;
    }
    return -1;
}

bool http_query_get(httpd_req_t *req, const char *key, char *out, size_t len)
{
    if (req == NULL || key == NULL || out == NULL || len == 0) {
        return false;
    }

    const char *value = NULL;
    const int value_len = query_value(req->uri, key, &value);
    if (value_len < 0) {
        return false;
    }

    size_t n = (size_t)value_len;
    if (n >= len) {
        n = len - 1;
    }
    memcpy(out, value, n);
    out[n] = '\0';
    return true;
}

bool http_query_get_i64(httpd_req_t *req, const char *key, int64_t *out)
{
    char buf[24];
    if (out == NULL || !http_query_get(req, key, buf, sizeof(buf))) {
        return false;
    }
    char *end = NULL;
    const long long value = strtoll(buf, &end, 10);
    if (end == buf) {
        return false;
    }
    *out = (int64_t)value;
    return true;
}

bool http_path_segment(httpd_req_t *req, int index, char *out, size_t len)
{
    if (req == NULL || out == NULL || len == 0 || index < 0) {
        return false;
    }

    char path[256];
    if (http_get_path(req, path, sizeof(path)) != ESP_OK) {
        return false;
    }

    /* Segment 0 is the first path element: "/api/v1/todos/12" is
     * 0="api" 1="v1" 2="todos" 3="12". */
    const char *p = path;
    if (*p == '/') {
        p++;
    }

    int current = 0;
    while (*p != '\0') {
        const char *slash = strchr(p, '/');
        const size_t seg_len = (slash != NULL) ? (size_t)(slash - p) : strlen(p);

        if (current == index) {
            if (seg_len == 0 || seg_len >= len) {
                return false;
            }
            memcpy(out, p, seg_len);
            out[seg_len] = '\0';
            return true;
        }

        if (slash == NULL) {
            break;
        }
        p = slash + 1;
        current++;
    }
    return false;
}

bool http_path_segment_u32(httpd_req_t *req, int index, uint32_t *out)
{
    char buf[16];
    if (out == NULL || !http_path_segment(req, index, buf, sizeof(buf))) {
        return false;
    }
    char *end = NULL;
    const unsigned long value = strtoul(buf, &end, 10);
    if (end == buf || *end != '\0') {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}
