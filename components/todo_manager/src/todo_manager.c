/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "todo_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "storage_manager.h"

#include "todo_internal.h"

static const char *TAG = "todo";

ESP_EVENT_DEFINE_BASE(TODO_EVENT);

#ifndef CONFIG_TODOHELPER_TODO_CACHE_FILE
#define CONFIG_TODOHELPER_TODO_CACHE_FILE "todos.json"
#endif

#define TODO_CACHE_PATH "cache/" CONFIG_TODOHELPER_TODO_CACHE_FILE
#define TODO_JSON_VERSION 1

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static todo_item_t *s_items;
static size_t s_count;
static size_t s_capacity;
static uint32_t s_next_id = 1;
static bool s_initialized;

/** Guards the item array. */
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

/** Serialises file writes so two writers cannot interleave. */
static StaticSemaphore_t s_io_lock_buf;
static SemaphoreHandle_t s_io_lock;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

const char *todo_sync_state_name(todo_sync_state_t state)
{
    switch (state) {
    case TODO_SYNC_PENDING:
        return "pending";
    case TODO_SYNC_OK:
        return "ok";
    case TODO_SYNC_FAILED:
        return "failed";
    case TODO_SYNC_NONE:
    default:
        return "none";
    }
}

todo_sync_state_t todo_sync_state_from_name(const char *name)
{
    if (name == NULL) {
        return TODO_SYNC_NONE;
    }
    if (strcmp(name, "pending") == 0) {
        return TODO_SYNC_PENDING;
    }
    if (strcmp(name, "ok") == 0) {
        return TODO_SYNC_OK;
    }
    if (strcmp(name, "failed") == 0) {
        return TODO_SYNC_FAILED;
    }
    return TODO_SYNC_NONE;
}

/**
 * Unix seconds when the clock is synchronised, otherwise seconds since boot.
 * Before NTP the value is far below any real Unix timestamp, so a client doing
 * incremental sync simply receives everything - which is the safe direction.
 */
static int64_t now_stamp(void)
{
    const int64_t ts = sys_time_ts();
    if (ts > 0) {
        return ts;
    }
    return sys_time_ms() / 1000;
}

static esp_err_t lock_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_io_lock == NULL) {
        s_io_lock = xSemaphoreCreateMutexStatic(&s_io_lock_buf);
        if (s_io_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

/** Payload used to publish counters into the shared system status. */
typedef struct {
    int32_t total;
    int32_t pending;
} todo_status_push_t;

static void todo_status_mutator(sys_status_t *st, void *arg)
{
    const todo_status_push_t *push = (const todo_status_push_t *)arg;
    st->todo_total = push->total;
    st->todo_pending = push->pending;
}

/**
 * Refresh the counters the status bar and home page read. Must be called
 * without holding s_lock.
 */
static void publish_status(void)
{
    todo_stats_t stats;
    if (todo_stats(&stats) != ESP_OK) {
        return;
    }
    const todo_status_push_t push = {
        .total = (int32_t)stats.total,
        .pending = (int32_t)stats.pending,
    };
    (void)sys_status_update(todo_status_mutator, (void *)&push);
}

static size_t find_index_locked(uint32_t id)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_items[i].id == id) {
            return i;
        }
    }
    return (size_t)-1;
}

static esp_err_t reserve_locked(size_t needed)
{
    if (needed <= s_capacity) {
        return ESP_OK;
    }

    size_t cap = (s_capacity == 0) ? 16 : s_capacity;
    while (cap < needed) {
        cap *= 2;
    }
    if (cap > TODO_MAX_ITEMS) {
        cap = TODO_MAX_ITEMS;
    }
    if (cap < needed) {
        return ESP_ERR_NO_MEM;
    }

    todo_item_t *grown = realloc(s_items, cap * sizeof(todo_item_t));
    if (grown == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_items = grown;
    s_capacity = cap;
    return ESP_OK;
}

static void post_todo_event(int32_t id, uint32_t item_id)
{
    (void)sys_event_post_to(TODO_EVENT, id, &item_id, sizeof(item_id));
    (void)sys_event_post_to(TODO_EVENT, TODO_EVENT_LIST_CHANGED, NULL, 0);

    /* L0 change channel: sync_server turns this into an SSE push for the
     * browser page, without this component knowing it exists. */
    (void)sys_notify("todo_changed", NULL);
}

/* ------------------------------------------------------------------------- */
/* Persistence                                                               */
/* ------------------------------------------------------------------------- */

cJSON *todo_item_to_json(const todo_item_t *item)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(obj, "id", (double)item->id);
    cJSON_AddStringToObject(obj, "title", item->title);
    cJSON_AddStringToObject(obj, "desc", item->desc);
    cJSON_AddNumberToObject(obj, "due_ts", (double)item->due_ts);
    cJSON_AddBoolToObject(obj, "done", item->done);
    cJSON_AddNumberToObject(obj, "priority", item->priority);
    cJSON_AddNumberToObject(obj, "updated_at", (double)item->updated_at);
    cJSON_AddStringToObject(obj, "sync_state", todo_sync_state_name(item->sync_state));
    return obj;
}

esp_err_t todo_export_json(char **json, size_t *len)
{
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *json = NULL;
    if (lock_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    if (root == NULL || array == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(array);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(root, "items", array);
    cJSON_AddNumberToObject(root, "version", TODO_JSON_VERSION);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddNumberToObject(root, "next_id", (double)s_next_id);
    for (size_t i = 0; i < s_count; i++) {
        cJSON *obj = todo_item_to_json(&s_items[i]);
        if (obj != NULL) {
            cJSON_AddItemToArray(array, obj);
        }
    }
    xSemaphoreGive(s_lock);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *json = text;
    if (len != NULL) {
        *len = strlen(text);
    }
    return ESP_OK;
}

esp_err_t todo_save(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!storage_available()) {
        /* In-memory only: not an error, the UI keeps working (FR2 offline path). */
        return ESP_OK;
    }
    if (lock_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    char *json = NULL;
    esp_err_t err = todo_export_json(&json, NULL);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    err = storage_write_cache(CONFIG_TODOHELPER_TODO_CACHE_FILE, json, strlen(json));
    xSemaphoreGive(s_io_lock);

    free(json);
    return err;
}

static bool json_read_string(const cJSON *obj, const char *key, char *out, size_t len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    strlcpy(out, item->valuestring, len);
    return true;
}

static int64_t json_read_i64(const cJSON *obj, const char *key, int64_t fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) {
        return fallback;
    }
    return (int64_t)item->valuedouble;
}

static bool json_read_bool(const cJSON *obj, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    if (cJSON_IsNumber(item)) {
        return item->valuedouble != 0;
    }
    return fallback;
}

esp_err_t todo_item_from_json(const cJSON *obj, todo_item_t *out, bool require_title)
{
    if (obj == NULL || out == NULL || !cJSON_IsObject(obj)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    const bool has_title = json_read_string(obj, "title", out->title, sizeof(out->title));
    if (require_title && (!has_title || out->title[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    (void)json_read_string(obj, "desc", out->desc, sizeof(out->desc));
    out->due_ts = json_read_i64(obj, "due_ts", 0);
    out->done = json_read_bool(obj, "done", false);

    const int64_t priority = json_read_i64(obj, "priority", 0);
    out->priority = (uint8_t)((priority < 0) ? 0 : ((priority > 2) ? 2 : priority));

    return ESP_OK;
}

esp_err_t todo_import_json(const char *json)
{
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lock_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const size_t incoming = (size_t)cJSON_GetArraySize(items);
    if (incoming > TODO_MAX_ITEMS) {
        SYS_LOGW(TAG, "import truncated to %d items", TODO_MAX_ITEMS);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = 0;

    uint32_t max_id = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, items) {
        if (s_count >= TODO_MAX_ITEMS) {
            break;
        }
        if (!cJSON_IsObject(entry)) {
            continue;
        }
        if (reserve_locked(s_count + 1) != ESP_OK) {
            break;
        }

        todo_item_t *dst = &s_items[s_count];
        memset(dst, 0, sizeof(*dst));
        dst->id = (uint32_t)json_read_i64(entry, "id", 0);
        (void)json_read_string(entry, "title", dst->title, sizeof(dst->title));
        (void)json_read_string(entry, "desc", dst->desc, sizeof(dst->desc));
        dst->due_ts = json_read_i64(entry, "due_ts", 0);
        dst->done = json_read_bool(entry, "done", false);
        dst->priority = (uint8_t)json_read_i64(entry, "priority", 0);
        dst->updated_at = json_read_i64(entry, "updated_at", 0);

        const cJSON *sync = cJSON_GetObjectItemCaseSensitive(entry, "sync_state");
        dst->sync_state = todo_sync_state_from_name(
            cJSON_IsString(sync) ? sync->valuestring : NULL);

        if (dst->id == 0) {
            dst->id = max_id + 1;
        }
        if (dst->id > max_id) {
            max_id = dst->id;
        }
        s_count++;
    }

    const int64_t stored_next = json_read_i64(root, "next_id", 0);
    s_next_id = (uint32_t)((stored_next > (int64_t)max_id) ? stored_next : (int64_t)(max_id + 1));
    if (s_next_id == 0) {
        s_next_id = 1;
    }

    xSemaphoreGive(s_lock);
    cJSON_Delete(root);

    SYS_LOGI(TAG, "imported %u items (next id %lu)", (unsigned)s_count, (unsigned long)s_next_id);

    (void)sys_event_post_to(TODO_EVENT, TODO_EVENT_LIST_CHANGED, NULL, 0);
    todo_save();
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Init                                                                      */
/* ------------------------------------------------------------------------- */

esp_err_t todo_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = lock_init();
    if (err != ESP_OK) {
        return err;
    }

    s_items = NULL;
    s_count = 0;
    s_capacity = 0;
    s_next_id = 1;
    s_initialized = true;

    if (storage_available()) {
        char *json = NULL;
        size_t len = 0;
        err = storage_read_cache(CONFIG_TODOHELPER_TODO_CACHE_FILE, &json, &len);
        if (err == ESP_OK && json != NULL) {
            esp_err_t parse = todo_import_json(json);
            if (parse != ESP_OK) {
                SYS_LOGW(TAG, "cache file unreadable (%s), starting empty",
                         esp_err_to_name(parse));
            }
            free(json);
        } else {
            SYS_LOGI(TAG, "no todo cache yet, starting empty");
        }
    } else {
        SYS_LOGW(TAG, "storage unavailable: todos will not survive a reboot");
    }

    SYS_LOGI(TAG, "ready with %u item(s)", (unsigned)s_count);

    /* Publish the REST API. The router is only a table, so this is harmless
     * when no HTTP server is running (requirement 8.2 + framework 7.4). */
    esp_err_t http_err = todo_manager_register_http();
    if (http_err != ESP_OK) {
        SYS_LOGW(TAG, "HTTP routes unavailable: %s", esp_err_to_name(http_err));
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Mutations                                                                 */
/* ------------------------------------------------------------------------- */

/** Shared epilogue: publish counters and persist. Caller must not hold s_lock. */
static void after_change(void)
{
    publish_status();
    todo_save();
}

esp_err_t todo_add(const todo_item_t *item, uint32_t *out_id)
{
    if (item == NULL || item->title[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count >= TODO_MAX_ITEMS) {
        xSemaphoreGive(s_lock);
        SYS_LOGW(TAG, "list full (%d items)", TODO_MAX_ITEMS);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = reserve_locked(s_count + 1);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }

    todo_item_t *dst = &s_items[s_count];
    memset(dst, 0, sizeof(*dst));
    strlcpy(dst->title, item->title, sizeof(dst->title));
    strlcpy(dst->desc, item->desc, sizeof(dst->desc));
    dst->due_ts = item->due_ts;
    dst->done = item->done;
    dst->priority = item->priority;
    dst->id = s_next_id++;
    dst->updated_at = now_stamp();
    dst->sync_state = TODO_SYNC_PENDING;

    const uint32_t id = dst->id;
    char title_copy[TODO_TITLE_MAX];
    strlcpy(title_copy, dst->title, sizeof(title_copy));
    s_count++;
    xSemaphoreGive(s_lock);

    SYS_LOGI(TAG, "added #%lu '%s'", (unsigned long)id, title_copy);
    post_todo_event(TODO_EVENT_ADDED, id);
    after_change();

    if (out_id != NULL) {
        *out_id = id;
    }
    return ESP_OK;
}

esp_err_t todo_update(uint32_t id, const todo_item_t *item)
{
    if (item == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t idx = find_index_locked(id);
    if (idx == (size_t)-1) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    todo_item_t *dst = &s_items[idx];
    strlcpy(dst->title, item->title, sizeof(dst->title));
    strlcpy(dst->desc, item->desc, sizeof(dst->desc));
    dst->due_ts = item->due_ts;
    dst->done = item->done;
    dst->priority = item->priority;
    dst->updated_at = now_stamp();
    dst->sync_state = TODO_SYNC_PENDING;
    xSemaphoreGive(s_lock);

    post_todo_event(TODO_EVENT_UPDATED, id);
    after_change();
    return ESP_OK;
}

esp_err_t todo_set_done(uint32_t id, bool done)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t idx = find_index_locked(id);
    if (idx == (size_t)-1) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_items[idx].done == done) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    s_items[idx].done = done;
    s_items[idx].updated_at = now_stamp();
    s_items[idx].sync_state = TODO_SYNC_PENDING;
    xSemaphoreGive(s_lock);

    post_todo_event(TODO_EVENT_DONE_CHANGED, id);
    after_change();
    return ESP_OK;
}

esp_err_t todo_delete(uint32_t id)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t idx = find_index_locked(id);
    if (idx == (size_t)-1) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    memmove(&s_items[idx], &s_items[idx + 1], (s_count - idx - 1) * sizeof(todo_item_t));
    s_count--;
    xSemaphoreGive(s_lock);

    SYS_LOGI(TAG, "deleted #%lu", (unsigned long)id);
    post_todo_event(TODO_EVENT_DELETED, id);
    after_change();
    return ESP_OK;
}

esp_err_t todo_clear_done(size_t *removed)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t dropped = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_count;) {
        if (!s_items[i].done) {
            i++;
            continue;
        }
        memmove(&s_items[i], &s_items[i + 1], (s_count - i - 1) * sizeof(todo_item_t));
        s_count--;
        dropped++;
    }
    xSemaphoreGive(s_lock);

    if (removed != NULL) {
        *removed = dropped;
    }
    if (dropped > 0) {
        (void)sys_event_post_to(TODO_EVENT, TODO_EVENT_LIST_CHANGED, NULL, 0);
        after_change();
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Queries                                                                   */
/* ------------------------------------------------------------------------- */

esp_err_t todo_get(uint32_t id, todo_item_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t idx = find_index_locked(id);
    if (idx == (size_t)-1) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    *out = s_items[idx];
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/** Order: unfinished first, then priority high->low, then earliest due, then id. */
static int compare_items(const void *a, const void *b)
{
    const todo_item_t *x = (const todo_item_t *)a;
    const todo_item_t *y = (const todo_item_t *)b;

    if (x->done != y->done) {
        return x->done ? 1 : -1;
    }
    if (x->priority != y->priority) {
        return (int)y->priority - (int)x->priority;
    }
    if (x->due_ts != y->due_ts) {
        /* Items without a due date sort last among their peers. */
        if (x->due_ts == 0) {
            return 1;
        }
        if (y->due_ts == 0) {
            return -1;
        }
        return (x->due_ts < y->due_ts) ? -1 : 1;
    }
    return (x->id < y->id) ? -1 : (x->id > y->id ? 1 : 0);
}

static esp_err_t snapshot(bool only_changed, int64_t since, todo_item_t **out, size_t *count)
{
    if (out == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    *count = 0;
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    size_t matched = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (only_changed && s_items[i].updated_at <= since) {
            continue;
        }
        matched++;
    }

    todo_item_t *copy = NULL;
    if (matched > 0) {
        copy = malloc(matched * sizeof(todo_item_t));
        if (copy == NULL) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NO_MEM;
        }
        size_t n = 0;
        for (size_t i = 0; i < s_count; i++) {
            if (only_changed && s_items[i].updated_at <= since) {
                continue;
            }
            copy[n++] = s_items[i];
        }
    }
    xSemaphoreGive(s_lock);

    if (copy != NULL) {
        qsort(copy, matched, sizeof(todo_item_t), compare_items);
    }
    *out = copy;
    *count = matched;
    return ESP_OK;
}

esp_err_t todo_list(todo_item_t **items, size_t *count)
{
    return snapshot(false, 0, items, count);
}

esp_err_t todo_list_changed_since(int64_t since_ts, todo_item_t **items, size_t *count)
{
    return snapshot(true, since_ts, items, count);
}

void todo_free_list(todo_item_t *items)
{
    free(items);
}

esp_err_t todo_stats(todo_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    todo_stats_t stats = { 0 };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stats.total = s_count;
    for (size_t i = 0; i < s_count; i++) {
        if (s_items[i].done) {
            stats.done++;
        }
        if (s_items[i].sync_state != TODO_SYNC_OK) {
            stats.pending++;
        }
    }
    xSemaphoreGive(s_lock);

    *out = stats;
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Sync state                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t todo_set_sync_state(uint32_t id, todo_sync_state_t state)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t idx = find_index_locked(id);
    if (idx == (size_t)-1) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_items[idx].sync_state == state) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_items[idx].sync_state = state;
    xSemaphoreGive(s_lock);

    post_todo_event(TODO_EVENT_SYNC_CHANGED, id);
    todo_save();
    return ESP_OK;
}

esp_err_t todo_mark_synced(uint32_t id)
{
    return todo_set_sync_state(id, TODO_SYNC_OK);
}

esp_err_t todo_mark_all_synced(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t changed = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_count; i++) {
        if (s_items[i].sync_state != TODO_SYNC_OK) {
            s_items[i].sync_state = TODO_SYNC_OK;
            changed++;
        }
    }
    xSemaphoreGive(s_lock);

    if (changed > 0) {
        (void)sys_event_post_to(TODO_EVENT, TODO_EVENT_LIST_CHANGED, NULL, 0);
        publish_status();
        todo_save();
    }
    SYS_LOGD(TAG, "%u item(s) marked in sync", (unsigned)changed);
    return ESP_OK;
}
