/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file word_service_http.c
 * @brief Requirement 8.4: the /api/v1/words/... endpoints.
 */

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "http_router.h"
#include "word_service.h"

static const char *TAG = "word_http";

static cJSON *word_to_json(const word_item_t *word)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(obj, "id", (double)word->id);
    cJSON_AddStringToObject(obj, "spelling", word->spelling);
    cJSON_AddStringToObject(obj, "phonetic", word->phonetic);
    cJSON_AddStringToObject(obj, "meaning", word->meaning);
    cJSON_AddStringToObject(obj, "example", word->example);
    cJSON_AddNumberToObject(obj, "level", word->level);
    cJSON_AddNumberToObject(obj, "next_review_ts", (double)word->next_review_ts);
    return obj;
}

static cJSON *stats_to_json(const word_stats_t *stats)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(obj, "total", (double)stats->total);
    cJSON_AddNumberToObject(obj, "new", (double)stats->new_words);
    cJSON_AddNumberToObject(obj, "due", (double)stats->due);
    cJSON_AddNumberToObject(obj, "learned", (double)stats->learned);
    cJSON_AddNumberToObject(obj, "reviewed_total", (double)stats->reviewed_total);
    cJSON_AddNumberToObject(obj, "known_total", (double)stats->known_total);
    cJSON_AddNumberToObject(obj, "fuzzy_total", (double)stats->fuzzy_total);
    cJSON_AddNumberToObject(obj, "unknown_total", (double)stats->unknown_total);
    cJSON_AddStringToObject(obj, "book", word_get_book());
    cJSON_AddNumberToObject(obj, "daily_count", (double)word_get_daily_count());
    return obj;
}

static esp_err_t today_get_handler(httpd_req_t *req)
{
    const size_t limit = word_get_daily_count();
    word_item_t *words = calloc(limit, sizeof(word_item_t));
    if (words == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "today's words");
    }

    size_t count = 0;
    esp_err_t err = word_get_today(words, limit, &count);
    if (err != ESP_OK) {
        free(words);
        return http_reply_esp_error(req, err, "today's words");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    if (root == NULL || array == NULL) {
        free(words);
        cJSON_Delete(root);
        cJSON_Delete(array);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "today's words");
    }
    cJSON_AddItemToObject(root, "items", array);
    for (size_t i = 0; i < count && i < limit; i++) {
        cJSON *obj = word_to_json(&words[i]);
        if (obj != NULL) {
            cJSON_AddItemToArray(array, obj);
        }
    }
    free(words);

    cJSON_AddNumberToObject(root, "count", (double)count);
    cJSON_AddStringToObject(root, "book", word_get_book());
    cJSON_AddNumberToObject(root, "server_ts", (double)sys_time_ts());

    word_stats_t stats;
    if (word_stats(&stats) == ESP_OK) {
        cJSON *stats_obj = stats_to_json(&stats);
        if (stats_obj != NULL) {
            cJSON_AddItemToObject(root, "stats", stats_obj);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t stats_get_handler(httpd_req_t *req)
{
    word_stats_t stats;
    esp_err_t err = word_stats(&stats);
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "word stats");
    }

    cJSON *obj = stats_to_json(&stats);
    if (obj == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "word stats");
    }
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t next_get_handler(httpd_req_t *req)
{
    word_item_t word;
    esp_err_t err = word_next(&word);
    if (err == ESP_ERR_NOT_FOUND) {
        return http_reply_err(req, 404, "no words loaded");
    }
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "next word");
    }

    cJSON *obj = word_to_json(&word);
    if (obj == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "next word");
    }

    size_t position = 0;
    size_t total = 0;
    (void)word_queue_position(&position, &total);
    cJSON_AddNumberToObject(obj, "position", (double)position);
    cJSON_AddNumberToObject(obj, "queue_total", (double)total);

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t review_post_handler(httpd_req_t *req)
{
    uint32_t id = 0;
    if (!http_path_segment_u32(req, 3, &id)) {
        return http_reply_bad_request(req, "invalid word id");
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

    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    word_review_result_t grade;
    if (!cJSON_IsString(result) || !word_review_from_name(result->valuestring, &grade)) {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "field 'result' must be known|fuzzy|unknown");
    }
    cJSON_Delete(root);

    esp_err_t err = word_review(id, grade);
    if (err == ESP_ERR_NOT_FOUND) {
        return http_reply_not_found(req);
    }
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "review word");
    }

    word_item_t word;
    if (word_get(id, &word) != ESP_OK) {
        return http_reply_ok(req, NULL);
    }

    cJSON *obj = word_to_json(&word);
    if (obj == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "review word");
    }
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t config_post_handler(httpd_req_t *req)
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

    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily_count");
    const cJSON *book = cJSON_GetObjectItemCaseSensitive(root, "book");
    const cJSON *reset = cJSON_GetObjectItemCaseSensitive(root, "reset");

    if (cJSON_IsNumber(daily)) {
        (void)word_set_daily_count((uint32_t)daily->valuedouble);
    }
    const bool reset_requested = cJSON_IsTrue(reset);
    char book_copy[32] = { 0 };
    const bool book_given = cJSON_IsString(book);
    if (book_given) {
        strlcpy(book_copy, book->valuestring, sizeof(book_copy));
    }
    cJSON_Delete(root);

    if (book_given) {
        esp_err_t err = word_set_book(book_copy);
        if (err != ESP_OK) {
            return http_reply_bad_request(req, "book not found or unreadable");
        }
    }
    if (reset_requested) {
        (void)word_reset_progress();
    }

    word_stats_t stats;
    (void)word_stats(&stats);
    cJSON *obj = stats_to_json(&stats);
    if (obj == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "word config");
    }
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    const esp_err_t err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

esp_err_t word_service_register_http(void)
{
    static const struct {
        const char *path;
        httpd_method_t method;
        http_route_handler_t handler;
    } routes[] = {
        { "/api/v1/words/today", HTTP_GET, today_get_handler },
        { "/api/v1/words/stats", HTTP_GET, stats_get_handler },
        { "/api/v1/words/next", HTTP_GET, next_get_handler },
        { "/api/v1/words/*/review", HTTP_POST, review_post_handler },
        { "/api/v1/words/config", HTTP_POST, config_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = http_router_register(routes[i].path, routes[i].method, routes[i].handler);
        if (err != ESP_OK) {
            SYS_LOGE(TAG, "register %s failed: %s", routes[i].path, esp_err_to_name(err));
            return err;
        }
    }
    SYS_LOGD(TAG, "%u word routes registered", (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
