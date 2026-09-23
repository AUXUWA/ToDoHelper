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

/**
 * @brief Load the persisted list. Safe to call when storage is unavailable:
 *        the component then runs in memory-only mode.
 */
esp_err_t todo_manager_init(void);

/** @brief Force a persist of the current list. */
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
