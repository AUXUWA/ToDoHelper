/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_todo.c
 * @brief FR2: the device side of the todo list.
 *
 * Binds TODO_EVENT_LIST_CHANGED so a change made from the PC browser (which
 * arrives through sync_server -> todo_manager) redraws this page immediately.
 * The rows are rebuilt from a snapshot each time; with <= 256 items that is
 * cheaper and far less error-prone than delta-updating widgets.
 */

#include "pages_internal.h"

#include <string.h>

#include "todo_manager.h"
#include "ui_assets.h"

static const char *TAG = "page_todo";

static lv_obj_t *s_list;
static lv_obj_t *s_stats;
static lv_obj_t *s_input_overlay;
static lv_obj_t *s_input_area;

/* ------------------------------------------------------------------------ */
/* Row callbacks                                                             */
/* ------------------------------------------------------------------------ */

static void row_toggle_cb(lv_event_t *e)
{
    const uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *checkbox = lv_event_get_target(e);
    const bool checked = lv_obj_has_state(checkbox, LV_STATE_CHECKED);

    esp_err_t err = todo_set_done(id, checked);
    if (err == ESP_ERR_NOT_FOUND) {
        ui_toast("该待办已不存在");
        return;
    }
    if (err != ESP_OK) {
        ui_toast("更新失败：%s", esp_err_to_name(err));
        return;
    }

    /* Completing something is a moment worth celebrating (FR6 HAPPY). */
    if (checked) {
        ui_toast("已完成");
    }
}

static void row_delete_cb(lv_event_t *e)
{
    const uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    esp_err_t err = todo_delete(id);
    if (err == ESP_ERR_NOT_FOUND) {
        ui_toast("该待办已不存在");
    } else if (err != ESP_OK) {
        ui_toast("删除失败：%s", esp_err_to_name(err));
    } else {
        ui_toast("已删除");
    }
}

static void row_open_cb(lv_event_t *e)
{
    const uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    page_todo_detail_set_id(id);
    (void)ui_nav_push(UI_PAGE_TODO_DETAIL);
}

/* ------------------------------------------------------------------------ */
/* Input overlay                                                             */
/* ------------------------------------------------------------------------ */

static void input_close(void)
{
    if (s_input_overlay != NULL) {
        lv_obj_delete(s_input_overlay);
        s_input_overlay = NULL;
        s_input_area = NULL;
    }
}

static void input_cancel_cb(lv_event_t *e)
{
    (void)e;
    input_close();
}

static void input_submit_cb(lv_event_t *e)
{
    (void)e;
    if (s_input_area == NULL) {
        return;
    }

    const char *text = lv_textarea_get_text(s_input_area);
    if (text == NULL || text[0] == '\0') {
        ui_toast("请输入内容");
        return;
    }

    todo_item_t item;
    memset(&item, 0, sizeof(item));
    strlcpy(item.title, text, sizeof(item.title));

    uint32_t id = 0;
    esp_err_t err = todo_add(&item, &id);
    input_close();

    if (err == ESP_OK) {
        ui_toast("已添加 #%lu", (unsigned long)id);
    } else {
        ui_toast("添加失败：%s", esp_err_to_name(err));
    }
}

static void input_open(void)
{
    if (s_input_overlay != NULL) {
        return;
    }

    s_input_overlay = lv_obj_create(lv_layer_top());
    if (s_input_overlay == NULL) {
        return;
    }
    lv_obj_remove_style_all(s_input_overlay);
    lv_obj_set_size(s_input_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_input_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_input_overlay, LV_OPA_70, 0);
    lv_obj_set_flex_flow(s_input_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_input_overlay, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_input_overlay, 8, 0);
    lv_obj_set_style_pad_row(s_input_overlay, 8, 0);
    lv_obj_remove_flag(s_input_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row = lv_obj_create(s_input_overlay);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    s_input_area = lv_textarea_create(row);
    lv_textarea_set_one_line(s_input_area, true);
    lv_textarea_set_max_length(s_input_area, TODO_TITLE_MAX - 1);
    lv_textarea_set_placeholder_text(s_input_area, "新待办（英文/数字，中文请用电脑网页）");
    lv_obj_set_flex_grow(s_input_area, 1);
    lv_obj_set_style_text_font(s_input_area, ui_font(UI_FONT_BODY), 0);

    lv_obj_t *ok = lv_button_create(row);
    lv_obj_set_height(ok, UI_TOUCH_MIN);
    lv_obj_t *ok_label = lv_label_create(ok);
    lv_label_set_text(ok_label, "添加");
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok, input_submit_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel = lv_button_create(row);
    lv_obj_set_height(cancel, UI_TOUCH_MIN);
    lv_obj_set_style_bg_color(cancel, UI_COLOR_SURFACE_ALT, 0);
    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, input_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *keyboard = lv_keyboard_create(s_input_overlay);
    lv_obj_set_width(keyboard, LV_PCT(100));
    lv_obj_set_height(keyboard, 220);
    lv_keyboard_set_textarea(keyboard, s_input_area);
    lv_obj_set_style_text_font(keyboard, ui_font(UI_FONT_SMALL), 0);
}

/* ------------------------------------------------------------------------ */
/* Toolbar callbacks                                                         */
/* ------------------------------------------------------------------------ */

static void add_clicked_cb(lv_event_t *e)
{
    (void)e;
    input_open();
}

static void sync_clicked_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = todo_mark_all_synced();
    if (err == ESP_OK) {
        ui_toast("已标记为同步");
    } else {
        ui_toast("同步失败：%s", esp_err_to_name(err));
    }
}

static void clear_done_clicked_cb(lv_event_t *e)
{
    (void)e;
    size_t removed = 0;
    if (todo_clear_done(&removed) == ESP_OK) {
        ui_toast(removed > 0 ? "已清除 %u 项" : "没有已完成项", (unsigned)removed);
    }
}

/* ------------------------------------------------------------------------ */
/* Render                                                                    */
/* ------------------------------------------------------------------------ */

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
        lv_label_set_text(empty, "还没有待办。\n在电脑浏览器打开设备地址即可添加。");
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

        lv_obj_t *checkbox = lv_checkbox_create(row);
        lv_checkbox_set_text(checkbox, "");
        lv_obj_set_size(checkbox, 34, 34);
        if (item->done) {
            lv_obj_add_state(checkbox, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(checkbox, row_toggle_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(uintptr_t)item->id);

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

        /* Tapping the title opens the detail page; the delete button sits at the
         * end so it is never hit by accident while scrolling. */
        lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(title, row_open_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)item->id);
        lv_obj_add_event_cb(row, row_open_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)item->id);

        lv_obj_t *del = lv_button_create(row);
        lv_obj_set_size(del, 60, UI_TOUCH_MIN);
        lv_obj_set_style_bg_color(del, UI_COLOR_SURFACE_ALT, 0);
        lv_obj_t *del_label = lv_label_create(del);
        lv_label_set_text(del_label, "删除");
        lv_obj_set_style_text_font(del_label, ui_font(UI_FONT_SMALL), 0);
        lv_obj_center(del_label);
        lv_obj_add_event_cb(del, row_delete_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)item->id);

        lv_obj_move_foreground(row);
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

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    const struct {
        const char *text;
        lv_event_cb_t cb;
        lv_color_t color;
    } buttons[] = {
        { "新增", add_clicked_cb, UI_COLOR_PRIMARY },
        { "标记已同步", sync_clicked_cb, UI_COLOR_SURFACE_ALT },
        { "清除已完成", clear_done_clicked_cb, UI_COLOR_SURFACE_ALT },
    };

    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        lv_obj_t *btn = lv_button_create(bar);
        lv_obj_set_height(btn, UI_TOUCH_MIN);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_bg_color(btn, buttons[i].color, 0);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, buttons[i].text);
        lv_obj_set_style_text_font(label, ui_font(UI_FONT_BODY), 0);
        lv_obj_center(label);
        lv_obj_add_event_cb(btn, buttons[i].cb, LV_EVENT_CLICKED, NULL);
    }

    refresh();

    /* Data can change from the HTTP task, so only refresh through events. */
    (void)ui_event_bind(TODO_EVENT, TODO_EVENT_LIST_CHANGED, UI_PAGE_TODO, 0);
    SYS_LOGD(TAG, "todo page built");
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
/* Detail page (FR2 / UI_PAGE_TODO_DETAIL)                                   */
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
        lv_label_set_text_fmt(s_detail_meta, "#%lu · 优先级 %u · %s · 到期 %lld",
                              (unsigned long)item.id, item.priority, sync,
                              (long long)item.due_ts);
    } else {
        lv_label_set_text_fmt(s_detail_meta, "#%lu · 优先级 %u · %s · 无到期时间",
                              (unsigned long)item.id, item.priority, sync);
    }
}

static void detail_toggle_cb(lv_event_t *e)
{
    (void)e;
    todo_item_t item;
    if (todo_get(s_detail_id, &item) != ESP_OK) {
        ui_toast("该待办已不存在");
        return;
    }
    if (todo_set_done(s_detail_id, !item.done) == ESP_OK) {
        ui_toast(item.done ? "已取消完成" : "已完成");
    }
    detail_refresh();
}

static void detail_priority_cb(lv_event_t *e)
{
    (void)e;
    todo_item_t item;
    if (todo_get(s_detail_id, &item) != ESP_OK) {
        return;
    }
    item.priority = (uint8_t)((item.priority + 1) % 3);
    if (todo_update(s_detail_id, &item) == ESP_OK) {
        ui_toast("优先级：%u", item.priority);
    }
    detail_refresh();
}

static void detail_delete_cb(lv_event_t *e)
{
    (void)e;
    if (todo_delete(s_detail_id) == ESP_OK) {
        ui_toast("已删除");
    }
    (void)ui_nav_back();
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

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    const struct {
        const char *text;
        lv_event_cb_t cb;
        lv_color_t color;
    } buttons[] = {
        { "完成/取消", detail_toggle_cb, UI_COLOR_SUCCESS },
        { "优先级", detail_priority_cb, UI_COLOR_SURFACE_ALT },
        { "删除", detail_delete_cb, UI_COLOR_DANGER },
    };

    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        lv_obj_t *btn = lv_button_create(bar);
        lv_obj_set_height(btn, UI_TOUCH_MIN);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_bg_color(btn, buttons[i].color, 0);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, buttons[i].text);
        lv_obj_set_style_text_font(label, ui_font(UI_FONT_BODY), 0);
        lv_obj_center(label);
        lv_obj_add_event_cb(btn, buttons[i].cb, LV_EVENT_CLICKED, NULL);
    }

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
