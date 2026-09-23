/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_home.c
 * @brief Main menu: status summary plus navigation tiles.
 */

#include <time.h>

#include "pages_internal.h"

static const char *TAG = "page_home";

static lv_obj_t *s_summary_label;
static lv_obj_t *s_clock_label;

typedef struct {
    const char *title;
    const char *subtitle;
    ui_page_id_t target;
} home_tile_def_t;

static const home_tile_def_t s_tiles[] = {
    { "WiFi",   "扫描 · 连接 · 自动重连", UI_PAGE_WIFI },
    { "待办",   "与电脑端同步清单",       UI_PAGE_TODO },
    { "天气",   "城市 · 温度 · 湿度",     UI_PAGE_WEATHER },
    { "日历",   "月历 · NTP 校时",        UI_PAGE_CALENDAR },
    { "单词",   "卡片背诵 · 复习进度",    UI_PAGE_WORD },
    { "设置",   "亮度 · 时区 · 密钥",     UI_PAGE_SETTINGS },
};

static void home_refresh(void)
{
    sys_status_t st;
    if (sys_status_get(&st) != ESP_OK) {
        return;
    }

    if (s_summary_label != NULL) {
        lv_label_set_text_fmt(s_summary_label, "待办 %ld 项 · 待同步 %ld 项 · 存储 %s",
                              (long)st.todo_total, (long)st.todo_pending,
                              st.sd_available ? "SD 卡"
                                              : (st.storage_available ? "内置" : "不可用"));
    }

    if (s_clock_label == NULL) {
        return;
    }

    if (!sys_time_synced() || st.now_ts <= 0) {
        /* Requirement FR4: never render an epoch date before NTP succeeds. */
        lv_label_set_text(s_clock_label, UI_STATUS_TIME_UNSYNCED);
        return;
    }

    const time_t now = (time_t)st.now_ts;
    struct tm tm_local;
    if (localtime_r(&now, &tm_local) == NULL) {
        lv_label_set_text(s_clock_label, UI_STATUS_TIME_UNSYNCED);
        return;
    }

    static const char *const weekdays[] = { "日", "一", "二", "三", "四", "五", "六" };
    const char *wd = (tm_local.tm_wday >= 0 && tm_local.tm_wday < 7) ? weekdays[tm_local.tm_wday] : "?";
    lv_label_set_text_fmt(s_clock_label, "%04d-%02d-%02d 星期%s  %02d:%02d",
                          tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday, wd,
                          tm_local.tm_hour, tm_local.tm_min);
}

static esp_err_t home_create(lv_obj_t *parent)
{
    lv_obj_t *head = lv_obj_create(parent);
    if (head == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(head, 4, 0);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *greeting = lv_label_create(head);
    lv_label_set_text(greeting, "你好，今天要做什么？");
    lv_obj_set_style_text_font(greeting, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(greeting, UI_COLOR_TEXT, 0);

    s_clock_label = lv_label_create(head);
    lv_label_set_text(s_clock_label, UI_STATUS_TIME_UNSYNCED);
    lv_obj_set_style_text_font(s_clock_label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_clock_label, UI_COLOR_PRIMARY, 0);

    s_summary_label = lv_label_create(head);
    lv_label_set_text(s_summary_label, "");
    lv_obj_set_style_text_font(s_summary_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_summary_label, UI_COLOR_TEXT_MUTED, 0);

    /* Tile grid: a wrapping flex row keeps this free of absolute coordinates
     * (requirement 10.3). */
    lv_obj_t *grid = lv_obj_create(parent);
    if (grid == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, UI_PAGE_GAP, 0);
    lv_obj_set_style_pad_column(grid, UI_PAGE_GAP, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < sizeof(s_tiles) / sizeof(s_tiles[0]); i++) {
        lv_obj_t *tile = page_tile_create(grid, s_tiles[i].title, s_tiles[i].subtitle,
                                         s_tiles[i].target);
        if (tile == NULL) {
            return ESP_ERR_NO_MEM;
        }
        /* Three columns per row on an 800 px wide panel. */
        lv_obj_set_width(tile, LV_PCT(32));
    }

    home_refresh();
    SYS_LOGD(TAG, "home built with %u tiles", (unsigned)(sizeof(s_tiles) / sizeof(s_tiles[0])));
    return ESP_OK;
}

static esp_err_t home_on_show(void)
{
    home_refresh();
    return ESP_OK;
}

static esp_err_t home_on_event(int32_t event_id, void *data)
{
    (void)event_id;
    (void)data;
    home_refresh();
    return ESP_OK;
}

static esp_err_t home_on_tick(void)
{
    home_refresh();
    return ESP_OK;
}

const ui_page_t page_home = {
    .id = UI_PAGE_HOME,
    .name = "home",
    .create = home_create,
    .on_show = home_on_show,
    .on_event = home_on_event,
    .on_tick = home_on_tick,
    .tick_period_ms = 1000,
};
