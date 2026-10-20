/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file todo_manager.h
 * @brief FR2: todo list with local persistence and PC synchronisation.
 *
 * The device is the authoritative store: it keeps the list in
 * `<storage>/todohelper/cache/todos.json` so it survives power loss, keeps
 * working while offline, and serves the list to the PC over HTTP (sync_server).
 *
 * Sync semantics (see docs/API.md):
 *   - a change made on the device marks the item TODO_SYNC_PENDING;
 *   - a change pushed by a client is already in sync, so sync_server calls
 *     todo_mark_synced() right after applying it;
 *   - a client that completed a full synchronisation calls
 *     todo_mark_all_synced() (POST /api/v1/sync).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#include "sys_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(TODO_EVENT);

typedef enum {
    TODO_EVENT_ADDED = 0,     /**< payload: uint32_t id */
    TODO_EVENT_UPDATED,       /**< payload: uint32_t id */
    TODO_EVENT_DONE_CHANGED,  /**< payload: uint32_t id */
    TODO_EVENT_DELETED,       /**< payload: uint32_t id */
    TODO_EVENT_SYNC_CHANGED,  /**< payload: uint32_t id */
    TODO_EVENT_LIST_CHANGED,  /**< no payload: coarse "redraw" notification */
} todo_event_id_t;

typedef enum {
    TODO_SYNC_NONE = 0,   /**< unknown / never reported */
    TODO_SYNC_PENDING,    /**< changed on the device, not yet picked up by a client */
    TODO_SYNC_OK,         /**< a client has the current version */
    TODO_SYNC_FAILED,     /**< the last transfer attempt failed */
} todo_sync_state_t;

/** Text limits, mirroring docs/TODOHELPER_REQUIREMENTS.md 7.3. */
#define TODO_TITLE_MAX 128
#define TODO_DESC_MAX  256

/** Hard cap on the number of items kept in RAM (bounds memory, not a UI limit). */
#define TODO_MAX_ITEMS 256

typedef struct {
    uint32_t id;
    char title[TODO_TITLE_MAX];
    char desc[TODO_DESC_MAX];
    int64_t due_ts;            /**< 0 = no due date; Unix seconds when the clock is synced */
    bool done;
    uint8_t priority;          /**< 0 = normal, 1 = important, 2 = urgent */
    int64_t updated_at;        /**< Unix seconds, or seconds-since-boot before NTP */
    todo_sync_state_t sync_state;
} todo_item_t;

/** Counters used by the status bar and the home page. */
typedef struct {
    size_t total;
    size_t done;
    size_t pending;            /**< sync_state != TODO_SYNC_OK */
} todo_stats_t;

/** 待办的截止时间分档。设备端用它决定徽标文案与颜色。 */
typedef enum {
    TODO_DUE_NONE = 0,   /**< 没有截止时间 */
    TODO_DUE_OVERDUE,    /**< 已逾期 */
    TODO_DUE_TODAY,      /**< 今天到期（本地时区 00:00~23:59:59） */
    TODO_DUE_TOMORROW,   /**< 明天到期 */
    TODO_DUE_LATER,      /**< 更晚 */
} todo_due_bucket_t;

/** 列表筛选口径。 */
typedef enum {
    TODO_FILTER_ALL = 0,   /**< 全部 */
    TODO_FILTER_OPEN,      /**< 只看未完成 */
    TODO_FILTER_DUE_TODAY, /**< 只看今天到期且未完成 */
    TODO_FILTER_DONE,      /**< 只看已完成 */
} todo_filter_t;

/**
 * @brief Load the persisted list. Safe to call when storage is unavailable:
 *        the component then runs in memory-only mode.
 */
esp_err_t todo_manager_init(void);

/**
 * @brief Force a persist of the current list.
 *
 * Synchronous: it writes the file right away and bypasses the debounced
 * writer. Use it where the data must be on the medium *now* (right after an
 * import, after the web client changed something). Ordinary mutations call
 * todo_mark_dirty() instead, so the LVGL task never touches the card.
 */
esp_err_t todo_save(void);

/**
 * @brief Add an item. @p out_id may be NULL.
 *
 * The id, updated_at and sync_state fields of @p item are ignored.
 */
esp_err_t todo_add(const todo_item_t *item, uint32_t *out_id);

/**
 * @brief Replace the mutable fields of an existing item.
 *
 * id, updated_at and sync_state are managed by the component.
 */
esp_err_t todo_update(uint32_t id, const todo_item_t *item);

esp_err_t todo_set_done(uint32_t id, bool done);

esp_err_t todo_delete(uint32_t id);

/** @brief Delete every completed item. Returns the number removed via @p removed. */
esp_err_t todo_clear_done(size_t *removed);

esp_err_t todo_get(uint32_t id, todo_item_t *out);

/**
 * @brief Snapshot the list, sorted by (done asc, priority desc, due_ts asc, id asc).
 *
 * The caller owns the returned array and must release it with todo_free_list().
 */
esp_err_t todo_list(todo_item_t **items, size_t *count);

/** @brief Snapshot of items whose updated_at is greater than @p since_ts. */
esp_err_t todo_list_changed_since(int64_t since_ts, todo_item_t **items, size_t *count);

void todo_free_list(todo_item_t *items);

esp_err_t todo_stats(todo_stats_t *out);

/* ------------------------------------------------------------------------ */
/* 列表视图与脏检查                                                          */
/* ------------------------------------------------------------------------ */

/**
 * @brief 按本地时区给 due_ts 分档。设备未校时（sys_time_synced() 为假）时，
 *        除 TODO_DUE_NONE 外一律返回 TODO_DUE_NONE —— 不要编造日期。
 */
todo_due_bucket_t todo_due_bucket(int64_t due_ts);

/**
 * @brief 按筛选口径取快照，排序与 todo_list() 完全一致
 *        （未完成在前 → 优先级高在前 → 截止早在前 → id 升序）。
 *
 * 返回的数组由调用方用 todo_free_list() 释放。
 */
esp_err_t todo_list_filtered(todo_filter_t filter, todo_item_t **items, size_t *count);

/**
 * @brief 清单版本号。任何内容或结构变化（增删改、状态变化）都会自增。
 *        页面拿它做脏检查：版本没变就不用整表重建。
 */
uint32_t todo_version(void);

/**
 * @brief 标记清单已变更，交给后台线程去抖落盘。
 *        LVGL 回调里必须调这个而不是直接写盘（AGENT.md §9.0.1.1 铁律一）。
 *        HTTP 侧若需要立刻落盘，调 todo_save()（同步）。
 */
void todo_mark_dirty(void);

/**
 * @brief 今天到期且未完成的条数（首页卡片用）。未校时时返回 0。
 */
size_t todo_count_due_today(void);

/**
 * @brief Mark one item as in sync with the clients.
 */
esp_err_t todo_mark_synced(uint32_t id);

/** @brief Mark every item as in sync (client completed a full sync). */
esp_err_t todo_mark_all_synced(void);

esp_err_t todo_set_sync_state(uint32_t id, todo_sync_state_t state);

/**
 * @brief Serialise the whole list as
 *        {"version":1,"next_id":N,"items":[...]}.
 *        The caller frees @p json with free().
 */
esp_err_t todo_export_json(char **json, size_t *len);

/**
 * @brief Replace the list from a JSON document produced by todo_export_json().
 *
 * Unknown fields are ignored so the format can grow without breaking clients.
 */
esp_err_t todo_import_json(const char *json);

/** @brief Machine readable name of a sync state, used by the HTTP API. */
const char *todo_sync_state_name(todo_sync_state_t state);

/** @brief Parse the name produced by todo_sync_state_name(). */
todo_sync_state_t todo_sync_state_from_name(const char *name);

/* ------------------------------------------------------------------------ */
/* HTTP (requirement 8.2)                                                    */
/* ------------------------------------------------------------------------ */

/**
 * @brief Publish the /api/v1/todos* routes on the shared HTTP router.
 *
 * Called by todo_manager_init(). Kept separate so the component still builds
 * and runs on a device without an HTTP server.
 *
 * | Method | Path                        | Notes |
 * |---|---|---|
 * | GET    | /api/v1/todos               | `?updated_since=ts` for incremental sync |
 * | POST   | /api/v1/todos               | body: {title,desc,due_ts,priority} |
 * | GET    | /api/v1/todos/{id}          | single item |
 * | PUT    | /api/v1/todos/{id}          | full update |
 * | PATCH  | /api/v1/todos/{id}/done     | body: {"done":true} |
 * | DELETE | /api/v1/todos/{id}          | remove |
 * | POST   | /api/v1/sync                | client declares itself in sync |
 */
esp_err_t todo_manager_register_http(void);

#ifdef __cplusplus
}
#endif
