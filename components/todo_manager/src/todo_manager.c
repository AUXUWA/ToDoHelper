/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "todo_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "storage_manager.h"

#include "todo_internal.h"

static const char *TAG = "todo";

ESP_EVENT_DEFINE_BASE(TODO_EVENT);

#ifndef CONFIG_TODOHELPER_TODO_CACHE_FILE
#define CONFIG_TODOHELPER_TODO_CACHE_FILE "todos.json"
#endif

#define TODO_CACHE_PATH "cache/" CONFIG_TODOHELPER_TODO_CACHE_FILE
#define TODO_JSON_VERSION 1

/** Maximum nesting depth accepted in todos.json. The file has two levels. */
#define TODO_JSON_MAX_DEPTH 32

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static todo_item_t *s_items;
static size_t s_count;
static size_t s_capacity;
static uint32_t s_next_id = 1;
static bool s_initialized;

/**
 * List version. Bumped on every change of content or order so the pages can
 * skip a full rebuild (they compare it with the value they last drew).
 */
static uint32_t s_version;

/** Guards the item array. */
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

/** Serialises file writes so two writers cannot interleave. */
static StaticSemaphore_t s_io_lock_buf;
static SemaphoreHandle_t s_io_lock;

/* ------------------------------------------------------------------------- */
/* Debounced persistence                                                     */
/* ------------------------------------------------------------------------- */

/*
 * The list used to be written from the caller's context, which for the device
 * UI means the LVGL task: the adapter runs lv_timer_handler() while holding
 * the adapter lock (AGENT.md 9.0.1.1, rule 1), so ticking a checkbox serialised
 * the whole list into JSON and wrote it to the SD card *inside* the lock. The
 * longer the list, the longer the UI froze - and todo_save() even took the IO
 * lock with portMAX_DELAY, so the LVGL task could sit there indefinitely while
 * the httpd task was writing the same file.
 *
 * Now a mutation only records "something changed" and wakes a worker. The
 * worker waits for a quiet period (several taps in a row collapse into one
 * write) and then persists. The LVGL task never touches the card again.
 */
#define TODO_SAVE_TASK_STACK 4096
#define TODO_SAVE_TASK_PRIO  3
/** Quiet period: long enough to swallow a burst of taps, short enough that
 *  losing power right after a change costs at most this much. */
#define TODO_SAVE_DEBOUNCE_MS 400
/** Upper bound for waiting for the IO lock. A writer that takes longer than
 *  this is considered stuck; failing loudly beats freezing the caller. */
#define TODO_IO_LOCK_TIMEOUT_MS 3000

static TaskHandle_t s_save_task;
static StaticSemaphore_t s_save_lock_buf;
static SemaphoreHandle_t s_save_lock;

/**
 * Change counter. Every mutation bumps it; a successful write records the
 * counter it captured, so "s_saved_seq != s_change_seq" means "there are
 * changes that are not on the medium yet".
 */
static uint32_t s_change_seq;
static uint32_t s_saved_seq;

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
    if (s_save_lock == NULL) {
        s_save_lock = xSemaphoreCreateMutexStatic(&s_save_lock_buf);
        if (s_save_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

/**
 * @brief 清单内容或顺序变了：版本号自增。
 *
 * 页面拿 todo_version() 与上次画出来的版本比较，相同就不重建整张列表。
 */
static void bump_version(void)
{
    __atomic_add_fetch(&s_version, 1u, __ATOMIC_RELAXED);
}

/**
 * @brief 把任意整数夹到合法的优先级区间（0 普通 / 1 重要 / 2 紧急）。
 *
 * HTTP 路径与 JSON 导入路径必须用同一套规则，否则同一个文件在网页端和
 * 设备端会排出不同的顺序（导入时写 256 会变成 0，写负数会变成 255）。
 */
static uint8_t clamp_priority(int64_t raw)
{
    if (raw < 0) {
        return 0;
    }
    if (raw > 2) {
        return 2;
    }
    return (uint8_t)raw;
}

/**
 * @brief UTF-8 安全的前缀长度：截断点不会落在多字节序列中间。
 *
 * 标题上限 128 字节，而一个汉字 3 字节 —— 按字节硬切会把最后一个字切成
 * 半个字符，设备上显示乱码，而且这份乱码会被原样写回 todos.json。
 *
 * @param s        待检查的字符串
 * @param len      s 里有效字节数
 * @param max_len  最多保留多少字节
 * @return 可以安全保留的字节数（<= max_len）
 */
static size_t utf8_prefix_len(const char *s, size_t len, size_t max_len)
{
    if (len <= max_len) {
        return len;
    }

    size_t i = 0;
    size_t boundary = 0;
    while (i < len && i < max_len) {
        const unsigned char c = (unsigned char)s[i];
        size_t seq;
        if (c < 0x80) {
            seq = 1;
        } else if ((c & 0xE0) == 0xC0) {
            seq = 2;
        } else if ((c & 0xF0) == 0xE0) {
            seq = 3;
        } else if ((c & 0xF8) == 0xF0) {
            seq = 4;
        } else {
            seq = 1;   /* 非法字节：按单字节处理，别让它吃掉整个串 */
        }
        if (i + seq > max_len) {
            /* 这个字符放不下：停在它之前，残字节一起丢掉。 */
            break;
        }
        i += seq;
        boundary = i;
    }
    return boundary;
}

/**
 * @brief 拷贝文本到定长缓冲，超长时按 UTF-8 字符边界截断。
 *
 * @param src_max 源缓冲最多可以读多少字节（防止源没有结尾 '\0' 时越界读）。
 */
static void copy_text(char *dst, size_t dst_len, const char *src, size_t src_max)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    size_t len = 0;
    while (len < src_max && src[len] != '\0') {
        len++;
    }

    const size_t n = utf8_prefix_len(src, len, dst_len - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
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

    /* Remember which changes this write covers. A change that happens while
     * the file is being written gets a higher counter, so the debounced
     * writer knows it still has work to do. */
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    const uint32_t seq = s_change_seq;
    xSemaphoreGive(s_save_lock);

    char *json = NULL;
    esp_err_t err = todo_export_json(&json, NULL);
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(TODO_IO_LOCK_TIMEOUT_MS)) != pdTRUE) {
        SYS_LOGW(TAG, "another writer holds the file; giving up this round");
        free(json);
        return ESP_ERR_TIMEOUT;
    }
    err = storage_write_cache_pinned(CONFIG_TODOHELPER_TODO_CACHE_FILE, json, strlen(json));
    xSemaphoreGive(s_io_lock);

    free(json);

    if (err == ESP_OK) {
        xSemaphoreTake(s_save_lock, portMAX_DELAY);
        if (s_saved_seq < seq) {
            s_saved_seq = seq;
        }
        xSemaphoreGive(s_save_lock);
    }
    return err;
}

/**
 * @brief 去抖落盘线程：安静期结束后把整份清单写一次。
 *
 * 只在有任务通知时才醒，醒来后先等一个安静期；安静期内又有变更就重新
 * 计时，于是「连点十下复选框」只写一次盘。
 */
static void todo_save_worker(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(TODO_SAVE_DEBOUNCE_MS));
            if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
                /* 安静期里又变了：重新计时。 */
                continue;
            }
            break;
        }

        xSemaphoreTake(s_save_lock, portMAX_DELAY);
        const bool pending = (s_saved_seq != s_change_seq);
        xSemaphoreGive(s_save_lock);

        if (!pending) {
            continue;
        }

        const esp_err_t err = todo_save();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            SYS_LOGW(TAG, "deferred save failed: %s", esp_err_to_name(err));
        }
    }
}

/**
 * @brief 记录一次内容变化（不唤醒写盘线程）。
 *
 * 变更序号与「已落盘序号」不等，就说明还有改动没写进文件。
 */
static void bump_change_seq(void)
{
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    s_change_seq++;
    xSemaphoreGive(s_save_lock);
}

void todo_mark_dirty(void)
{
    if (!s_initialized) {
        return;
    }
    if (lock_init() != ESP_OK) {
        SYS_LOGW(TAG, "no lock for the debounced writer");
        return;
    }

    bump_change_seq();

    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    TaskHandle_t task = s_save_task;
    if (task == NULL) {
        /* First change of this session: bring the writer up. */
        const BaseType_t ok = xTaskCreate(todo_save_worker, "th_todo_save",
                                          TODO_SAVE_TASK_STACK, NULL,
                                          TODO_SAVE_TASK_PRIO, &s_save_task);
        if (ok != pdPASS) {
            s_save_task = NULL;
            xSemaphoreGive(s_save_lock);
            /*
             * No memory for the writer (task stacks come from internal RAM and
             * this board is tight). Falling back to an inline write is the slow
             * path the async design exists to avoid, but losing the change is
             * worse than a stutter.
             */
            SYS_LOGW(TAG, "no memory for the debounced writer; saving inline");
            (void)todo_save();
            return;
        }
        task = s_save_task;
    }
    xSemaphoreGive(s_save_lock);

    xTaskNotifyGive(task);
}

static bool json_read_string(const cJSON *obj, const char *key, char *out, size_t len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    copy_text(out, len, item->valuestring, strlen(item->valuestring));
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
    out->priority = clamp_priority(priority);

    return ESP_OK;
}

/**
 * @brief 导入过程中的 id 是否已经被前面的条目占用。
 *
 * 必须在持有 s_lock 时调用。
 */
static bool id_taken_locked(uint32_t id)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_items[i].id == id) {
            return true;
        }
    }
    return false;
}

esp_err_t todo_import_json(const char *json)
{
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lock_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * todos.json is a file the user can edit on the card, and cJSON's nesting
     * limit is 1000 with ~96 bytes of stack per level - about 128 levels to
     * blow the 12 KB boot task stack. sys_json_parse() measures the depth
     * before handing the text to cJSON, so a broken file can no longer take
     * the device down on every reboot.
     */
    cJSON *root = sys_json_parse(json, TODO_JSON_MAX_DEPTH);
    if (root == NULL) {
        SYS_LOGW(TAG, "todo json is invalid or too deeply nested; keeping the current list");
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        SYS_LOGW(TAG, "todo json has no 'items' array; keeping the current list");
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
        /* A negative or oversized id is as unusable as id 0. */
        const int64_t raw_id_value = json_read_i64(entry, "id", 0);
        const uint32_t raw_id =
            (raw_id_value > 0 && raw_id_value <= (int64_t)UINT32_MAX)
                ? (uint32_t)raw_id_value : 0;
        (void)json_read_string(entry, "title", dst->title, sizeof(dst->title));
        (void)json_read_string(entry, "desc", dst->desc, sizeof(dst->desc));
        dst->due_ts = json_read_i64(entry, "due_ts", 0);
        dst->done = json_read_bool(entry, "done", false);
        dst->priority = clamp_priority(json_read_i64(entry, "priority", 0));
        dst->updated_at = json_read_i64(entry, "updated_at", 0);

        const cJSON *sync = cJSON_GetObjectItemCaseSensitive(entry, "sync_state");
        dst->sync_state = todo_sync_state_from_name(
            cJSON_IsString(sync) ? sync->valuestring : NULL);

        /*
         * A duplicate id would make every later update/delete hit the wrong
         * item, so ids are made unique here: id 0 and ids that are already
         * taken get the next free one.
         */
        if (raw_id == 0 || id_taken_locked(raw_id)) {
            uint32_t candidate = max_id + 1;
            if (candidate == 0) {
                candidate = 1;      /* 回绕：0 是保留值 */
            }
            while (id_taken_locked(candidate)) {
                candidate++;
                if (candidate == 0) {
                    candidate = 1;
                }
            }
            SYS_LOGW(TAG, "import: id %lu is unusable, using %lu",
                     (unsigned long)raw_id, (unsigned long)candidate);
            dst->id = candidate;
        } else {
            dst->id = raw_id;
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
    bump_version();
    xSemaphoreGive(s_lock);
    cJSON_Delete(root);

    SYS_LOGI(TAG, "imported %u items (next id %lu)", (unsigned)s_count, (unsigned long)s_next_id);

    (void)sys_event_post_to(TODO_EVENT, TODO_EVENT_LIST_CHANGED, NULL, 0);

    /*
     * An import replaces the whole list, so it must hit the medium right
     * away: waiting for the debounce window would lose the import on a power
     * cut. The change counter is bumped (so the writer knows the state) and
     * the explicit todo_save() below is the one that actually writes.
     */
    bump_change_seq();
    const esp_err_t save_err = todo_save();
    if (save_err != ESP_OK) {
        SYS_LOGW(TAG, "import could not be persisted: %s", esp_err_to_name(save_err));
    }
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
        /*
         * The list lives in the internal LittleFS, not on the SD card: the
         * storage root follows whatever is mounted, and pulling the card out
         * would make the whole list look like it had vanished. The pinned
         * reader falls back to the storage root when the internal copy is
         * missing, so an upgrade never starts with an empty list.
         */
        err = storage_read_cache_pinned(CONFIG_TODOHELPER_TODO_CACHE_FILE, &json, &len);
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

/**
 * @brief 变更收尾：发布计数并把落盘交给去抖线程。调用时不得持有 s_lock。
 *
 * 这里**不能**直接写盘：调用方可能是 LVGL 任务（持着适配器锁），
 * 同步写 SD 卡会冻住整机（AGENT.md §9.0.1.1 铁律一）。
 */
static void after_change(void)
{
    publish_status();
    todo_mark_dirty();
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
    copy_text(dst->title, sizeof(dst->title), item->title, sizeof(item->title));
    copy_text(dst->desc, sizeof(dst->desc), item->desc, sizeof(item->desc));
    dst->due_ts = item->due_ts;
    dst->done = item->done;
    dst->priority = clamp_priority(item->priority);
    dst->id = s_next_id++;
    dst->updated_at = now_stamp();
    dst->sync_state = TODO_SYNC_PENDING;

    const uint32_t id = dst->id;
    char title_copy[TODO_TITLE_MAX];
    strlcpy(title_copy, dst->title, sizeof(title_copy));
    s_count++;
    /* 版本号在锁内自增：事件处理器读到的版本一定不早于它看到的数据。 */
    bump_version();
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
    copy_text(dst->title, sizeof(dst->title), item->title, sizeof(item->title));
    copy_text(dst->desc, sizeof(dst->desc), item->desc, sizeof(item->desc));
    dst->due_ts = item->due_ts;
    dst->done = item->done;
    dst->priority = clamp_priority(item->priority);
    dst->updated_at = now_stamp();
    dst->sync_state = TODO_SYNC_PENDING;
    bump_version();
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
    bump_version();
    xSemaphoreGive(s_lock);

    post_todo_event(TODO_EVENT_DONE_CHANGED, id);
    after_change();

    /* Ticking something off is worth an expression (FR6 HAPPY). */
    if (done) {
        (void)sys_notify("todo_done", NULL);
    }
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
    bump_version();
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
    if (dropped > 0) {
        bump_version();
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

/**
 * @brief 筛选判定。
 *
 * 「今天到期」必须复用 todo_due_bucket()，否则分档与筛选会各算一套
 * 「今天」的边界（§3.5 的约定）。
 */
static bool match_filter(todo_filter_t filter, const todo_item_t *item)
{
    switch (filter) {
    case TODO_FILTER_ALL:
        return true;
    case TODO_FILTER_OPEN:
        return !item->done;
    case TODO_FILTER_DUE_TODAY:
        return !item->done && todo_due_bucket(item->due_ts) == TODO_DUE_TODAY;
    case TODO_FILTER_DONE:
        return item->done;
    default:
        return true;
    }
}

static esp_err_t snapshot(todo_filter_t filter, bool only_changed, int64_t since,
                           todo_item_t **out, size_t *count)
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
        if (!match_filter(filter, &s_items[i])) {
            continue;
        }
        matched++;
    }

    todo_item_t *copy = NULL;
    if (matched > 0) {
        /* PSRAM first: 256 items are ~100 KB and the internal heap is the
         * scarcest resource on this board (AGENT.md 9.0). */
        copy = heap_caps_malloc_prefer(matched * sizeof(todo_item_t), 2,
                                        MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
        if (copy == NULL) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NO_MEM;
        }
        size_t n = 0;
        for (size_t i = 0; i < s_count; i++) {
            if (only_changed && s_items[i].updated_at <= since) {
                continue;
            }
            if (!match_filter(filter, &s_items[i])) {
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
    return snapshot(TODO_FILTER_ALL, false, 0, items, count);
}

esp_err_t todo_list_filtered(todo_filter_t filter, todo_item_t **items, size_t *count)
{
    return snapshot(filter, false, 0, items, count);
}

esp_err_t todo_list_changed_since(int64_t since_ts, todo_item_t **items, size_t *count)
{
    return snapshot(TODO_FILTER_ALL, true, since_ts, items, count);
}

void todo_free_list(todo_item_t *items)
{
    /* The snapshot arrays come from heap_caps_malloc_prefer(), so they have
     * to go back through heap_caps_free(). */
    heap_caps_free(items);
}

uint32_t todo_version(void)
{
    return __atomic_load_n(&s_version, __ATOMIC_ACQUIRE);
}

/**
 * @brief 公历日期 → 连续的天序号（Howard Hinnant 的 days_from_civil）。
 *
 * 不用 mktime()：夏令时切换那天 mktime() 的结果会差一小时，而「今天」
 * 必须是一个干脆的日历日边界。
 */
static int64_t civil_day_number(int year, int month, int day)
{
    int64_t y = year;
    y -= (month <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                            /* [0, 399] */
    const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;    /* [0, 146096] */
    return era * 146097 + doe - 719468;                          /* 1970-01-01 == 0 */
}

/**
 * @brief 某个时间戳落在本地时区的第几天。
 *
 * 与 calendar_get_now() 一样用 localtime_r()，所以待办的「今天」和日历
 * 页的「今天」永远是同一天。
 */
static bool local_day_number(int64_t ts, int64_t *out)
{
    const time_t t = (time_t)ts;
    struct tm tm_local;
    if (localtime_r(&t, &tm_local) == NULL) {
        return false;
    }
    *out = civil_day_number(tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday);
    return true;
}

todo_due_bucket_t todo_due_bucket(int64_t due_ts)
{
    if (due_ts <= 0) {
        return TODO_DUE_NONE;          /* 没有截止时间 */
    }
    if (!sys_time_synced()) {
        /* 没校过时不能编日期：一律当成「没有截止时间」。 */
        return TODO_DUE_NONE;
    }
    const int64_t now = sys_time_ts();
    if (now <= 0) {
        return TODO_DUE_NONE;
    }

    int64_t now_day = 0;
    int64_t due_day = 0;
    if (!local_day_number(now, &now_day) || !local_day_number(due_ts, &due_day)) {
        return TODO_DUE_NONE;
    }

    const int64_t diff = due_day - now_day;
    if (diff < 0) {
        return TODO_DUE_OVERDUE;
    }
    if (diff == 0) {
        return TODO_DUE_TODAY;
    }
    if (diff == 1) {
        return TODO_DUE_TOMORROW;
    }
    return TODO_DUE_LATER;
}

size_t todo_count_due_today(void)
{
    if (!s_initialized) {
        return 0;
    }

    size_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_count; i++) {
        if (!s_items[i].done && todo_due_bucket(s_items[i].due_ts) == TODO_DUE_TODAY) {
            n++;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
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
    bump_version();
    xSemaphoreGive(s_lock);

    post_todo_event(TODO_EVENT_SYNC_CHANGED, id);
    /*
     * Not a write of its own: the debounced writer merges it with whatever
     * else changed. One HTTP write used to hit the card twice (once for the
     * change, once for the "now it is in sync" flag).
     */
    todo_mark_dirty();
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

    (void)sys_notify("sync_started", NULL);

    size_t changed = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_count; i++) {
        if (s_items[i].sync_state != TODO_SYNC_OK) {
            s_items[i].sync_state = TODO_SYNC_OK;
            changed++;
        }
    }
    if (changed > 0) {
        bump_version();
    }
    xSemaphoreGive(s_lock);

    if (changed > 0) {
        (void)sys_event_post_to(TODO_EVENT, TODO_EVENT_LIST_CHANGED, NULL, 0);
        publish_status();
        todo_mark_dirty();
    }
    (void)sys_notify("sync_done", NULL);
    SYS_LOGD(TAG, "%u item(s) marked in sync", (unsigned)changed);
    return ESP_OK;
}
