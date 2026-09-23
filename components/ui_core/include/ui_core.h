/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_core.h
 * @brief L2 UI framework: page registry, navigation, status bar, LVGL locking.
 *
 * Design rules (docs/TODOHELPER_FRAMEWORK.md):
 *   - one LVGL screen owned by ui_core; pages never call lv_screen_load()/create
 *     their own screen, they fill the container handed to create();
 *   - every LVGL access happens in the LVGL task. Other tasks use ui_lock() or
 *     ui_run_async();
 *   - pages receive service events through ui_event_bind(), which marshals the
 *     event into the LVGL task and calls ui_page_t::on_event.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "lvgl.h"

#include "sys_utils.h"
#include "th_bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Pages                                                                     */
/* ------------------------------------------------------------------------ */

/**
 * @brief Page identifiers.
 *
 * The order must match docs/TODOHELPER_REQUIREMENTS.md 10.1 and the page table
 * in components/ui_pages/ui_pages.c.
 */
typedef enum {
    UI_PAGE_SPLASH = 0,   /**< boot banner + init progress */
    UI_PAGE_HOME,         /**< card style main menu */
    UI_PAGE_WIFI,         /**< scan list / password / status sub-views */
    UI_PAGE_TODO,         /**< todo list */
    UI_PAGE_TODO_DETAIL,  /**< todo detail + sync state */
    UI_PAGE_WEATHER,      /**< weather */
    UI_PAGE_CALENDAR,     /**< month calendar */
    UI_PAGE_WORD,         /**< word flashcards */
    UI_PAGE_AI_CHAT,      /**< AI chat (P2) */
    UI_PAGE_SETTINGS,     /**< settings */
    UI_PAGE_IDLE_FACE,    /**< idle eyes animation */
    UI_PAGE_MAX,
} ui_page_id_t;

/**
 * @brief One entry of the page table.
 *
 * Only @c id and @c create are mandatory. Pages are created lazily on first
 * show, unless @c keep_alive is set (used by the idle face, which must react to
 * events even while another page is visible).
 */
typedef struct {
    ui_page_id_t id;
    const char *name;
    /** Build the page inside @p parent. Called once, in the LVGL task. */
    esp_err_t (*create)(lv_obj_t *parent);
    /** Optional teardown when the page is removed from the cache. */
    esp_err_t (*destroy)(void);
    /** Page became visible. */
    esp_err_t (*on_show)(void);
    /** Page stopped being visible. */
    esp_err_t (*on_hide)(void);
    /** Bound service/system event, already marshalled into the LVGL task. */
    esp_err_t (*on_event)(int32_t event_id, void *data);
    /** Called periodically while the page is visible (0 = disabled). */
    esp_err_t (*on_tick)(void);
    uint32_t tick_period_ms;
} ui_page_t;

/**
 * @brief Bring up the UI root: theme, fonts, status bar, LVGL lock, tick timer.
 *
 * Must be called after th_bsp_init() and after sys_events_init().
 */
esp_err_t ui_core_init(void);

/**
 * @brief Register a page table. May be called more than once for extra modules;
 *        later registrations must not reuse an existing ui_page_id_t.
 */
esp_err_t ui_page_register(const ui_page_t *pages, size_t count);

/**
 * @brief Look up a registered page; NULL when not registered.
 */
const ui_page_t *ui_page_get(ui_page_id_t id);

/**
 * @brief Switch to a page. Creates it on first use.
 *
 * Safe to call from any task.
 */
esp_err_t ui_page_switch(ui_page_id_t id);

/** @brief Currently visible page, UI_PAGE_MAX when none. */
ui_page_id_t ui_page_current(void);

/**
 * @brief Container of a page (its create() parent), or NULL when not created.
 *        Only valid while holding the LVGL lock.
 */
lv_obj_t *ui_page_parent(ui_page_id_t id);

/** @brief Push the current page and switch to @p id. */
esp_err_t ui_nav_push(ui_page_id_t id);

/** @brief Return to the previous page; no-op when the stack is empty. */
esp_err_t ui_nav_back(void);

/** @brief True when there is a page to go back to. */
bool ui_nav_can_back(void);

/* ------------------------------------------------------------------------ */
/* Threading                                                                 */
/* ------------------------------------------------------------------------ */

/**
 * @brief Take the LVGL lock (recursive, so nesting inside a callback is safe).
 */
bool ui_lock(int32_t timeout_ms);

/** @brief Release the LVGL lock. */
void ui_unlock(void);

/**
 * @brief Run @p fn in the LVGL task, from any task.
 *
 * @p arg is passed through unchanged; @p fn runs with the LVGL lock held and
 * must not block.
 */
esp_err_t ui_run_async(void (*fn)(void *arg), void *arg);

/* ------------------------------------------------------------------------ */
/* Event binding                                                             */
/* ------------------------------------------------------------------------ */

/**
 * @brief UI event handler, always invoked in the LVGL task.
 *
 * @p data points at a ui_core owned copy of the event payload and is only valid
 * for the duration of the call.
 */
typedef esp_err_t (*ui_event_fn_t)(int32_t event_id, void *data);

/**
 * @brief Deliver (base, event_id) to @p page's on_event, in the LVGL task.
 *
 * One handler per event base is registered on the shared loop; ui_core copies
 * @p data_size payload bytes, marshals them with lv_async_call() and drops the
 * event when the UI is too busy (these are notifications, not a transport).
 *
 * @param data_size Payload bytes to copy; pass 0 for events without a payload.
 */
esp_err_t ui_event_bind(esp_event_base_t base, int32_t event_id, ui_page_id_t page,
                        size_t data_size);

/** @brief Convenience: bind every event id of @p base to @p page. */
esp_err_t ui_event_bind_all(esp_event_base_t base, ui_page_id_t page, size_t data_size);

/**
 * @brief Subscribe a plain handler to a shared-loop event, marshalled into the
 *        LVGL task. Used by ui_core itself and by non-page UI modules such as
 *        idle_face.
 */
esp_err_t ui_event_subscribe(esp_event_base_t base, int32_t event_id, size_t data_size,
                             ui_event_fn_t handler);

/* ------------------------------------------------------------------------ */
/* Common widgets                                                            */
/* ------------------------------------------------------------------------ */

/**
 * @brief Rounded card with an optional title.
 *
 * @param[out] out_body Optional; receives the child container for content.
 * @return the card object, or NULL on allocation failure.
 */
lv_obj_t *ui_card_create(lv_obj_t *parent, const char *title, lv_obj_t **out_body);

/**
 * @brief Transient bottom message. Auto-hides after 2 s; safe from any task.
 */
esp_err_t ui_toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** @brief Show a persistent message box. @p on_close may be NULL. */
lv_obj_t *ui_dialog(const char *title, const char *text,
                    const char *ok_text, lv_event_cb_t on_close, void *user_data);

/** @brief Screen size helpers (kept here so pages never hardcode 800x480). */
int32_t ui_screen_width(void);
int32_t ui_screen_height(void);

/** @brief Height of the status bar in pixels. */
int32_t ui_statusbar_height(void);

/* ------------------------------------------------------------------------ */
/* Status bar                                                                */
/* ------------------------------------------------------------------------ */

/**
 * @brief Refresh the status bar from sys_status_get().
 *
 * ui_core subscribes to SYS_EVENT_STATUS_CHANGED and calls this automatically,
 * so pages never need to.
 */
esp_err_t ui_statusbar_refresh(void);

/** @brief Text shown when the wall clock has never been synchronised. */
#define UI_STATUS_TIME_UNSYNCED "时间未同步"

#ifdef __cplusplus
}
#endif
