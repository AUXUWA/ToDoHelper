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

/**
 * @brief 一次 ack 最多处理多少个 id。
 *
 * ack 的 body 来自网络，数组长度不设上限的话，一个失控的客户端就能让
 * httpd 任务在这里空转。256 与 TODO_MAX_ITEMS 同量级，正常用法（一次
 * 合并本轮收到的变更）远小于它。
 */
#define TODO_ACK_MAX_IDS 256

/**
 * @brief 解析外部 JSON 时允许的最大嵌套深度。
 *
 * 8 层对本组件的报文绰绰有余（ack 是 2 层，导出文件是 3 层），但 cJSON 的
 * 递归解析每层要吃约 96 字节栈，httpd 任务的栈只有 6 KB —— 限深是为了
 * 让畸形输入止步于「回 400」，而不是把 httpd 任务打爆（AGENT.md §9.0.5）。
 */
#define TODO_HTTP_JSON_MAX_DEPTH 8

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

    cJSON *root = sys_json_parse(body, TODO_HTTP_JSON_MAX_DEPTH);
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

    cJSON *root = sys_json_parse(body, TODO_HTTP_JSON_MAX_DEPTH);
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

    cJSON *root = sys_json_parse(body, TODO_HTTP_JSON_MAX_DEPTH);
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
/* B9: ack 回执                                                               */
/* ------------------------------------------------------------------------ */

/**
 * @brief 客户端确认「这些 id 的最新版本我已经收下了」。
 *
 * 设备端改动一条待办只会把它置成 TODO_SYNC_PENDING；以前只有
 * `POST /api/v1/sync` 能把 PENDING 洗回 OK，于是设备端勾选一次，待同步
 * 计数就再也回不到 0。现在客户端把改动吃进本地列表后回一个 ack，设备
 * 只把**被点名的那几条**改判为 OK。
 *
 * 注意「没人在线就不洗」是正确的：没人收到过的东西本来就不该标成已同步。
 */
static esp_err_t todos_ack_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing or oversized body");
    }

    cJSON *root = sys_json_parse(body, TODO_HTTP_JSON_MAX_DEPTH);
    free(body);
    if (root == NULL) {
        return http_reply_bad_request(req, "invalid json");
    }

    const cJSON *ids = cJSON_GetObjectItemCaseSensitive(root, "ids");
    if (!cJSON_IsArray(ids)) {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "field 'ids' must be an array");
    }

    cJSON *acked = cJSON_CreateArray();
    cJSON *missing = cJSON_CreateArray();
    if (acked == NULL || missing == NULL) {
        cJSON_Delete(acked);
        cJSON_Delete(missing);
        cJSON_Delete(root);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "ack todos");
    }

    int handled = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, ids) {
        if (handled >= TODO_ACK_MAX_IDS) {
            SYS_LOGW(TAG, "ack list truncated at %d ids", TODO_ACK_MAX_IDS);
            break;
        }
        if (!cJSON_IsNumber(entry)) {
            continue;
        }
        const double raw = entry->valuedouble;
        /* id 从 1 开始；0 与越界值都不是合法 id，直接跳过而不是让
         * todo_mark_synced() 去处理一个莫名其妙的目标。 */
        if (raw < 1.0 || raw > (double)UINT32_MAX) {
            continue;
        }
        const uint32_t id = (uint32_t)raw;
        handled++;

        const esp_err_t err = todo_mark_synced(id);
        if (err == ESP_OK) {
            cJSON_AddItemToArray(acked, cJSON_CreateNumber((double)id));
        } else if (err == ESP_ERR_NOT_FOUND) {
            /* 客户端可能还在确认一条已经被删掉的待办 —— 不算错误，
             * 但要让网页端看得到「哪几条没对上」。 */
            cJSON_AddItemToArray(missing, cJSON_CreateNumber((double)id));
        } else {
            SYS_LOGW(TAG, "ack id %lu failed: %s", (unsigned long)id, esp_err_to_name(err));
        }
    }
    cJSON_Delete(root);

    const int acked_count = cJSON_GetArraySize(acked);

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        cJSON_Delete(acked);
        cJSON_Delete(missing);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "ack todos");
    }
    cJSON_AddItemToObject(resp, "acked", acked);
    cJSON_AddItemToObject(resp, "missing", missing);
    cJSON_AddNumberToObject(resp, "count", (double)acked_count);

    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    esp_err_t err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

/* ------------------------------------------------------------------------ */
/* B12: 导出 / 导入                                                          */
/* ------------------------------------------------------------------------ */

/**
 * @brief 判断查询串里有没有 `?key=1` 形式的开关。
 */
static bool query_flag(httpd_req_t *req, const char *key)
{
    char buf[8];
    return http_query_get(req, key, buf, sizeof(buf)) && buf[0] == '1';
}

/**
 * @brief 导出整份清单。
 *
 * 默认**直接返回清单本体**（不带项目统一信封），并带上
 * `Content-Disposition: attachment`，浏览器存下来的文件可以原样 POST 回
 * `/api/v1/todos/import`，导出→导入是闭环的。网页端如果只想把 JSON 打到
 * 屏幕上而不是下载，加 `?envelope=1` 换回标准信封。
 */
static esp_err_t todos_export_get_handler(httpd_req_t *req)
{
    char *json = NULL;
    size_t len = 0;
    esp_err_t err = todo_export_json(&json, &len);
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "export todos");
    }

    if (query_flag(req, "envelope")) {
        /* 信封模式：把清单本体当作 data 交给 http_reply_ok() 统一封口。
         * http_reply_json() 是把 data_json 原样嵌进信封的，所以这里不需要
         * 重新解析一遍刚生成的 JSON。 */
        err = http_reply_ok(req, json);
        free(json);
        return err;
    }

    /* 下载模式：裸清单 + attachment 头。 */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"todos.json\"");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_status(req, "200");

    err = httpd_resp_send(req, json, len);
    free(json);
    return err;
}

/**
 * @brief 用一份导出文件整体替换清单。
 *
 * 语义与 `todo_import_json()` 一致：**整体替换**，不是合并。
 */
static esp_err_t todos_import_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing or oversized body");
    }

    /* 先自己解析一遍做校验：非法 JSON 必须回 400，不能让内核拿着
     * 半截文本去改清单。 */
    cJSON *root = sys_json_parse(body, TODO_HTTP_JSON_MAX_DEPTH);
    if (root == NULL) {
        free(body);
        return http_reply_bad_request(req, "invalid json");
    }

    char *text = NULL;      /* 交给 todo_import_json() 的文本 */
    bool owned = false;     /* text 是否需要自己释放 */

    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (cJSON_IsArray(items)) {
        text = body;
    } else {
        /* 兼容信封：如果网页端拿到的导出结果被 api() 漏斗套了一层
         * {"code":..,"msg":..,"data":{...}}，就把 data 里那份清单取出来。 */
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsObject(data) &&
            cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(data, "items"))) {
            text = cJSON_PrintUnformatted(data);
            owned = true;
        }
    }

    if (text == NULL) {
        cJSON_Delete(root);
        free(body);
        return http_reply_bad_request(req, "field 'items' must be an array");
    }

    esp_err_t err = todo_import_json(text);
    cJSON_Delete(root);
    if (owned) {
        cJSON_free(text);
    }
    free(body);

    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_ARG) {
            return http_reply_bad_request(req, "invalid todo document");
        }
        return http_reply_esp_error(req, err, "import todos");
    }

    todo_stats_t stats;
    (void)todo_stats(&stats);

    char data[96];
    snprintf(data, sizeof(data), "{\"imported\":%u,\"total\":%u,\"pending\":%u}",
             (unsigned)stats.total, (unsigned)stats.total, (unsigned)stats.pending);
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
        /* 导出/导入/ack 都排在单条通配路由之前注册：路由表是定长的
         * 静态数组（HTTP_ROUTER_MAX_ROUTES = 64），而且匹配是「最具体者胜」
         * —— 精确路径的分数高于通配，所以 export/import/ack 不会被
         * 通配的「单条待办」路由抢走。 */
        { "/api/v1/todos/export", HTTP_GET, todos_export_get_handler },
        { "/api/v1/todos/import", HTTP_POST, todos_import_post_handler },
        { "/api/v1/todos/ack", HTTP_POST, todos_ack_post_handler },
        { "/api/v1/todos/*/done", HTTP_PATCH, todos_item_done_handler },
        { "/api/v1/todos/*", HTTP_GET, todos_item_get_handler },
        { "/api/v1/todos/*", HTTP_PUT, todos_item_put_handler },
        { "/api/v1/todos/*", HTTP_DELETE, todos_item_delete_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = http_router_register(routes[i].path, routes[i].method, routes[i].handler);
        if (err != ESP_OK) {
            SYS_LOGE(TAG, "register %s (method %d) failed: %s", routes[i].path,
                     (int)routes[i].method, esp_err_to_name(err));
            return err;
        }
    }

    SYS_LOGI(TAG, "%u todo routes registered", (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
