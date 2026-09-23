/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file todo_manager_http.c
 * @brief Requirement 8.2: the Todo REST API.
 *
 * Registered on the L1 http_router by todo_manager_register_http(), so this
 * component never has to know that sync_server exists.
 *
 * Every response goes through the router's envelope helper, which is what makes
 * 400/401/404/405/501 behave identically across all APIs.
 */

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "http_router.h"
#include "todo_internal.h"
#include "todo_manager.h"

static const char *TAG = "todo_http";

/* Path segments, following http_router's convention:
 *   /api/v1/todos/12/done
 *    0    1   2    3  4
 */
#define SEG_ID 3

/* ------------------------------------------------------------------------ */
/* Handlers                                                                  */
/* ------------------------------------------------------------------------ */

static esp_err_t todos_get_handler(httpd_req_t *req)
{
    int64_t updated_since = 0;
    const bool incremental = http_query_get_i64(req, "updated_since", &updated_since);

    todo_item_t *items = NULL;
    size_t count = 0;
    esp_err_t err = incremental ? todo_list_changed_since(updated_since, &items, &count)
                                : todo_list(&items, &count);
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "list todos");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    if (root == NULL || array == NULL) {
        todo_free_list(items);
        cJSON_Delete(root);
        cJSON_Delete(array);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "list todos");
    }
    cJSON_AddItemToObject(root, "items", array);
    for (size_t i = 0; i < count; i++) {
        cJSON *obj = todo_item_to_json(&items[i]);
        if (obj != NULL) {
            cJSON_AddItemToArray(array, obj);
        }
    }
    cJSON_AddNumberToObject(root, "count", (double)count);
    cJSON_AddBoolToObject(root, "incremental", incremental);
    cJSON_AddNumberToObject(root, "server_ts", (double)sys_time_ts());
    todo_free_list(items);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t todos_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing or oversized body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return http_reply_bad_request(req, "invalid json");
    }

    todo_item_t item;
    esp_err_t err = todo_item_from_json(root, &item, true);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return http_reply_bad_request(req, "field 'title' is required");
    }

    uint32_t id = 0;
    err = todo_add(&item, &id);
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "add todo");
    }

    /* The item came from a client, so the client already has this version. */
    (void)todo_mark_synced(id);

    todo_item_t created;
    if (todo_get(id, &created) != ESP_OK) {
        return http_reply_ok(req, "{\"id\":0}");
    }

    cJSON *obj = todo_item_to_json(&created);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    err = http_reply_json(req, 201, "created", json);
    cJSON_free(json);
    return err;
}

static esp_err_t todos_item_get_handler(httpd_req_t *req)
{
    uint32_t id = 0;
    if (!http_path_segment_u32(req, SEG_ID, &id)) {
        return http_reply_bad_request(req, "invalid id");
    }

    todo_item_t item;
    esp_err_t err = todo_get(id, &item);
    if (err == ESP_ERR_NOT_FOUND) {
        return http_reply_not_found(req);
    }
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "get todo");
    }

    cJSON *obj = todo_item_to_json(&item);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t todos_item_put_handler(httpd_req_t *req)
{
    uint32_t id = 0;
    if (!http_path_segment_u32(req, SEG_ID, &id)) {
        return http_reply_bad_request(req, "invalid id");
    }

    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing or oversized body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return http_reply_bad_request(req, "invalid json");
    }

    todo_item_t item;
    esp_err_t err = todo_item_from_json(root, &item, true);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return http_reply_bad_request(req, "field 'title' is required");
    }

    err = todo_update(id, &item);
    if (err == ESP_ERR_NOT_FOUND) {
        return http_reply_not_found(req);
    }
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "update todo");
    }

    (void)todo_mark_synced(id);
    return todos_item_get_handler(req);
}

static esp_err_t todos_item_done_handler(httpd_req_t *req)
{
    uint32_t id = 0;
    if (!http_path_segment_u32(req, SEG_ID, &id)) {
        return http_reply_bad_request(req, "invalid id");
    }

    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return http_reply_bad_request(req, "invalid json");
    }

    const cJSON *done = cJSON_GetObjectItemCaseSensitive(root, "done");
    if (!cJSON_IsBool(done)) {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "field 'done' must be a boolean");
    }
    const bool value = cJSON_IsTrue(done);
    cJSON_Delete(root);

    esp_err_t err = todo_set_done(id, value);
    if (err == ESP_ERR_NOT_FOUND) {
        return http_reply_not_found(req);
    }
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "set done");
    }

    (void)todo_mark_synced(id);
    return todos_item_get_handler(req);
}

static esp_err_t todos_item_delete_handler(httpd_req_t *req)
{
    uint32_t id = 0;
    if (!http_path_segment_u32(req, SEG_ID, &id)) {
        return http_reply_bad_request(req, "invalid id");
    }

    esp_err_t err = todo_delete(id);
    if (err == ESP_ERR_NOT_FOUND) {
        return http_reply_not_found(req);
    }
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "delete todo");
    }

    char data[48];
    snprintf(data, sizeof(data), "{\"id\":%lu,\"deleted\":true}", (unsigned long)id);
    return http_reply_ok(req, data);
}

static esp_err_t sync_post_handler(httpd_req_t *req)
{
    esp_err_t err = todo_mark_all_synced();
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "sync");
    }

    todo_stats_t stats;
    (void)todo_stats(&stats);

    char data[96];
    snprintf(data, sizeof(data), "{\"total\":%u,\"pending\":%u,\"synced\":true}",
             (unsigned)stats.total, (unsigned)stats.pending);
    return http_reply_ok(req, data);
}

/* ------------------------------------------------------------------------ */
/* Registration                                                              */
/* ------------------------------------------------------------------------ */

esp_err_t todo_manager_register_http(void)
{
    /* Collection routes first, then the item routes. Order does not matter for
     * matching (the router scores the most specific pattern), but registering
     * the collection first keeps /api/v1/todos from being shadowed. */
    static const struct {
        const char *path;
        httpd_method_t method;
        http_route_handler_t handler;
    } routes[] = {
        { "/api/v1/todos", HTTP_GET, todos_get_handler },
        { "/api/v1/todos", HTTP_POST, todos_post_handler },
        { "/api/v1/sync", HTTP_POST, sync_post_handler },
        { "/api/v1/todos/*/done", HTTP_PATCH, todos_item_done_handler },
        { "/api/v1/todos/*", HTTP_GET, todos_item_get_handler },
        { "/api/v1/todos/*", HTTP_PUT, todos_item_put_handler },
        { "/api/v1/todos/*", HTTP_DELETE, todos_item_delete_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = http_router_register(routes[i].path, routes[i].method, routes[i].handler);
        if (err != ESP_OK) {
            SYS_LOGE(TAG, "register %s failed: %s", routes[i].path, esp_err_to_name(err));
            return err;
        }
    }

    SYS_LOGI(TAG, "%u todo routes registered", (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
