/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_todo.c
 * @brief FR2: the device side of the todo list — a read-only viewer.
 *
 * All todo creation/editing/completion/deletion happens on the PC web page.
 * The touch panel has no keyboard worth typing Chinese on, and letting two
 * writers race on the same record made the sync state hard to reason about, so
 * this page deliberately exposes **no** control that can change a todo. It
 * stays useful as a viewer: the list shows title, done state and sync state,
 * the detail page shows the remaining fields, and both point the user at the
 * web page.
 *
 * Binds TODO_EVENT_LIST_CHANGED so a change made from the PC browser (which
 * arrives through sync_server -> todo_manager) redraws this page immediately.
 * The rows are rebuilt from a snapshot each time; with <= 256 items that is
 * cheaper and far less error-prone than delta-updating widgets.
 *
 * Every LVGL call below runs in the LVGL task: create()/on_show()/on_event()
 * are invoked by ui_core, and ui_event_bind() marshals into that task — see
 * AGENT.md 9.0.1.
 */

#include "pages_internal.h"

#include "todo_manager.h"

static const char *TAG = "page_todo";

/** Shown in place of the removed editing controls. */
#define TODO_READONLY_HINT "此页可勾选完成 / 删除 | 新增请到电脑网页端"

static lv_obj_t *s_list;
static lv_obj_t *s_stats;

/* ------------------------------------------------------------------------ */
/* Row callbacks                                                             */
/* ------------------------------------------------------------------------ */

/** Tapping a row is navigation only: it opens the read-only detail page. */
static void row_open_cb(lv_event_t *e)
{
    const uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    page_todo_detail_set_id(id);
    (void)ui_nav_push(UI_PAGE_TODO_DETAIL);
}

/* ------------------------------------------------------------------------ */
/* Render                                                                    */
/* ------------------------------------------------------------------------ */

/** One muted line telling the user where a todo can actually be changed. */
/*
 * Marking done and deleting stay on the device: both are single taps, and a tap is
 * the one input this panel is good at. *Creating and editing* stay on the web page
 * - they need Chinese text entry and this board has no IME, which is the whole
 * reason the add overlay was removed (see README).
 *
 * Both callbacks are safe to run even though the resulting
 * TODO_EVENT_LIST_CHANGED refreshes this very list: the event is posted, not
 * delivered inline, so the rows are only rebuilt after the callback returns and
 * LVGL is never asked to delete the object that is currently handling an event.
 */
static void row_toggle_cb(lv_event_t *e)
{
    lv_obj_t *checkbox = lv_event_get_target(e);
    const uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    const bool done = lv_obj_has_state(checkbox, LV_STATE_CHECKED);

    esp_err_t err = todo_set_done(id, done);
    if (err != ESP_OK) {
        /* Put the box back rather than leaving the UI showing a state the store
         * does not have. */
        if (done) {
            lv_obj_remove_state(checkbox, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(checkbox, LV_STATE_CHECKED);
        }
        ui_toast("操作失败：%s", esp_err_to_name(err));
    }
}

static void row_delete_cb(lv_event_t *e)
{
    const uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    esp_err_t err = todo_delete(id);
    if (err == ESP_OK) {
        ui_toast("已删除");
    } else {
        ui_toast("删除失败：%s", esp_err_to_name(err));
    }
}

static void readonly_hint_create(lv_obj_t *parent)
{
    lv_obj_t *hint = lv_label_create(parent);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(hint, TODO_READONLY_HINT);
    lv_obj_set_style_text_font(hint, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(hint, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

static void render_list(void)
{
    if (s_list == NULL) {
        return;
    }

    lv_obj_clean(s_list);

    todo_item_t *items = NULL;
    size_t count = 0;
    if (todo_list(&items, &count) != ESP_OK) {
        return;
    }

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "还没有待办。\n请在电脑网页端新增（浏览器打开设备地址）。");
        lv_obj_set_style_text_font(empty, ui_font(UI_FONT_BODY), 0);
        lv_obj_set_style_text_color(empty, UI_COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_pad_all(empty, 24, 0);
        todo_free_list(items);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        const todo_item_t *item = &items[i];

        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, UI_LIST_ROW_HEIGHT, 0);
        lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, UI_CARD_RADIUS, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_ver(row, 6, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* The checkbox *is* a control: marking a todo done is one tap. */
        lv_obj_t *checkbox = lv_checkbox_create(row);
        lv_checkbox_set_text(checkbox, "");
        lv_obj_set_size(checkbox, 34, 34);
        lv_obj_add_flag(checkbox, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(checkbox, row_toggle_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(uintptr_t)item->id);
        if (item->done) {
            lv_obj_add_state(checkbox, LV_STATE_CHECKED);
        }

        lv_obj_t *title = lv_label_create(row);
        lv_label_set_text(title, item->title);
        lv_obj_set_flex_grow(title, 1);
        lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(title, ui_font(UI_FONT_BODY), 0);
        lv_obj_set_style_text_color(title, item->done ? UI_COLOR_TEXT_MUTED : UI_COLOR_TEXT, 0);
        if (item->done) {
            /* LVGL has no text-decoration: mark completion with the colour and
             * a leading check instead of a strike-through. */
            lv_label_set_text_fmt(title, "✓ %s", item->title);
        }

        if (item->sync_state != TODO_SYNC_OK) {
            lv_obj_t *badge = lv_label_create(row);
            lv_label_set_text(badge, item->sync_state == TODO_SYNC_FAILED ? "失败" : "待同步");
            lv_obj_set_style_text_font(badge, ui_font(UI_FONT_SMALL), 0);
            lv_obj_set_style_text_color(badge, item->sync_state == TODO_SYNC_FAILED
                                                   ? UI_COLOR_DANGER
                                                   : UI_COLOR_WARNING,
                                        0);
        }

        /* One-tap delete. Kept small and last so it is never hit while reaching
         * for the row. */
        lv_obj_t *del = lv_button_create(row);
        lv_obj_set_size(del, 52, 34);
        lv_obj_set_style_bg_color(del, UI_COLOR_DANGER, 0);
        lv_obj_t *del_label = lv_label_create(del);
        lv_label_set_text(del_label, "删除");
        lv_obj_set_style_text_font(del_label, ui_font(UI_FONT_SMALL), 0);
        lv_obj_center(del_label);
        lv_obj_add_event_cb(del, row_delete_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)item->id);

        /* Tapping the row (or its title) opens the read-only detail page. */
        lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(title, row_open_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)item->id);
        lv_obj_add_event_cb(row, row_open_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)item->id);
    }

    todo_free_list(items);
}

static void render_stats(void)
{
    if (s_stats == NULL) {
        return;
    }

    todo_stats_t stats;
    if (todo_stats(&stats) != ESP_OK) {
        return;
    }
    lv_label_set_text_fmt(s_stats, "共 %u 项 · 已完成 %u · 待同步 %u",
                          (unsigned)stats.total, (unsigned)stats.done,
                          (unsigned)stats.pending);
    lv_obj_set_style_text_color(s_stats,
                                stats.pending > 0 ? UI_COLOR_WARNING : UI_COLOR_TEXT_MUTED, 0);
}

static void refresh(void)
{
    render_stats();
    render_list();
}

/* ------------------------------------------------------------------------ */
/* Page                                                                      */
/* ------------------------------------------------------------------------ */

static esp_err_t todo_create(lv_obj_t *parent)
{
    (void)page_header_create(parent, "待办清单");

    s_stats = lv_label_create(parent);
    lv_obj_set_style_text_font(s_stats, ui_font(UI_FONT_SMALL), 0);
    lv_label_set_text(s_stats, "");

    s_list = lv_obj_create(parent);
    if (s_list == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_style_pad_right(s_list, 6, 0);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    /* The "新增 / 标记已同步 / 清除已完成" toolbar used to live here; editing is
     * done on the PC web page, so its slot now carries that hint instead. */
    readonly_hint_create(parent);

    refresh();

    /* Data can change from the HTTP task, so only refresh through events. */
    (void)ui_event_bind(TODO_EVENT, TODO_EVENT_LIST_CHANGED, UI_PAGE_TODO, 0);
    SYS_LOGD(TAG, "todo page built (read-only)");
    return ESP_OK;
}

static esp_err_t todo_on_show(void)
{
    refresh();
    return ESP_OK;
}

static esp_err_t todo_on_event(int32_t event_id, void *data)
{
    (void)event_id;
    (void)data;
    refresh();
    return ESP_OK;
}

const ui_page_t page_todo = {
    .id = UI_PAGE_TODO,
    .name = "todo",
    .create = todo_create,
    .on_show = todo_on_show,
    .on_event = todo_on_event,
};

/* ------------------------------------------------------------------------ */
/* Detail page (FR2 / UI_PAGE_TODO_DETAIL) — display only                    */
/* ------------------------------------------------------------------------ */

static uint32_t s_detail_id;
static lv_obj_t *s_detail_title;
static lv_obj_t *s_detail_meta;
static lv_obj_t *s_detail_desc;

void page_todo_detail_set_id(uint32_t id)
{
    s_detail_id = id;
}

static void detail_refresh(void)
{
    if (s_detail_title == NULL) {
        return;
    }

    todo_item_t item;
    if (todo_get(s_detail_id, &item) != ESP_OK) {
        lv_label_set_text(s_detail_title, "该待办已不存在");
        lv_label_set_text(s_detail_meta, "");
        lv_label_set_text(s_detail_desc, "");
        return;
    }

    lv_label_set_text(s_detail_title, item.title);
    lv_label_set_text(s_detail_desc, item.desc[0] != '\0' ? item.desc : "（无备注）");

    static const char *const sync_names[] = { "未同步", "待同步", "已同步", "同步失败" };
    const char *sync = (item.sync_state < 4) ? sync_names[item.sync_state] : "?";

    if (item.due_ts > 0) {
        lv_label_set_text_fmt(s_detail_meta, "#%lu · %s · 优先级 %u · %s · 到期 %lld",
                              (unsigned long)item.id, item.done ? "已完成" : "未完成",
                              item.priority, sync, (long long)item.due_ts);
    } else {
        lv_label_set_text_fmt(s_detail_meta, "#%lu · %s · 优先级 %u · %s · 无到期时间",
                              (unsigned long)item.id, item.done ? "已完成" : "未完成",
                              item.priority, sync);
    }
}

static esp_err_t todo_detail_create(lv_obj_t *parent)
{
    (void)page_header_create(parent, "待办详情");

    lv_obj_t *body = NULL;
    if (ui_card_create(parent, NULL, &body) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_detail_title = lv_label_create(body);
    lv_obj_set_width(s_detail_title, LV_PCT(100));
    lv_label_set_long_mode(s_detail_title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_detail_title, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(s_detail_title, UI_COLOR_TEXT, 0);

    s_detail_meta = lv_label_create(body);
    lv_obj_set_style_text_font(s_detail_meta, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_detail_meta, UI_COLOR_TEXT_MUTED, 0);

    s_detail_desc = lv_label_create(body);
    lv_obj_set_width(s_detail_desc, LV_PCT(100));
    lv_label_set_long_mode(s_detail_desc, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_detail_desc, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_detail_desc, UI_COLOR_TEXT, 0);

    /* The "完成/取消 / 优先级 / 删除" bar used to live here; the detail page is
     * a viewer now, so its slot carries the read-only hint instead. */
    readonly_hint_create(parent);

    detail_refresh();
    (void)ui_event_bind(TODO_EVENT, TODO_EVENT_LIST_CHANGED, UI_PAGE_TODO_DETAIL, 0);
    return ESP_OK;
}

static esp_err_t todo_detail_on_show(void)
{
    detail_refresh();
    return ESP_OK;
}

static esp_err_t todo_detail_on_event(int32_t event_id, void *data)
{
    (void)event_id;
    (void)data;
    detail_refresh();
    return ESP_OK;
}

const ui_page_t page_todo_detail = {
    .id = UI_PAGE_TODO_DETAIL,
    .name = "todo_detail",
    .create = todo_detail_create,
    .on_show = todo_detail_on_show,
    .on_event = todo_detail_on_event,
};
