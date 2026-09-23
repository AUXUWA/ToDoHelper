/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_calendar.c
 * @brief FR4: clock plus a month grid with previous/next month.
 *
 * The board has no RTC, so before SNTP succeeds the clock area shows
 * UI_STATUS_TIME_UNSYNCED and the month grid falls back to the *current*
 * month via the build date is NOT used - instead the grid simply starts on a
 * neutral month until a real date exists, which is the honest behaviour
 * required by FR4.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "calendar_service.h"
#include "pages_internal.h"

static const char *TAG = "page_calendar";

#define CAL_MAX_DAYS 31
#define CAL_COLS 7

static lv_obj_t *s_clock;
static lv_obj_t *s_month_title;
static lv_obj_t *s_grid_labels[CAL_MAX_DAYS];
static lv_obj_t *s_grid;

static int s_year = 2026;
static int s_month = 1;
static bool s_have_date;

/** UTF-8 weekday names: one entry per column, indexed like tm_wday. */
static const char *const s_week_names[] = { "日", "一", "二", "三", "四", "五", "六" };

/* ------------------------------------------------------------------------ */
/* Grid                                                                      */
/* ------------------------------------------------------------------------ */

static void render_grid(void)
{
    if (s_grid == NULL) {
        return;
    }

    struct tm days[CAL_MAX_DAYS];
    size_t count = 0;
    if (calendar_get_month(s_year, s_month, days, &count) != ESP_OK) {
        return;
    }

    for (int i = 0; i < CAL_MAX_DAYS; i++) {
        if (s_grid_labels[i] != NULL) {
            lv_obj_add_flag(s_grid_labels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_label_set_text_fmt(s_month_title, "%04d 年 %02d 月", s_year, s_month);

    /* Highlight today only when the year/month match what is on screen. */
    int today = -1;
    struct tm now;
    if (s_have_date && calendar_get_now(&now) == ESP_OK &&
        (now.tm_year + 1900) == s_year && (now.tm_mon + 1) == s_month) {
        today = now.tm_mday;
    }

    const int first_wday = (count > 0) ? days[0].tm_wday : 0;
    for (size_t i = 0; i < count; i++) {
        lv_obj_t *cell = s_grid_labels[i];
        if (cell == NULL) {
            continue;
        }

        const int pos = first_wday + (int)i;
        const int row = pos / CAL_COLS;
        const int col = pos % CAL_COLS;

        lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_CENTER, col, 1,
                             LV_GRID_ALIGN_CENTER, row + 1, 1);
        lv_label_set_text_fmt(cell, "%d", days[i].tm_mday);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_HIDDEN);

        const bool is_today = ((int)days[i].tm_mday == today);
        lv_obj_set_style_text_color(cell, is_today ? UI_COLOR_ON_PRIMARY : UI_COLOR_TEXT, 0);
        lv_obj_set_style_bg_opa(cell, is_today ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

/* ------------------------------------------------------------------------ */
/* Navigation                                                                */
/* ------------------------------------------------------------------------ */

static void shift_month(int delta)
{
    s_month += delta;
    while (s_month < 1) {
        s_month += 12;
        s_year--;
    }
    while (s_month > 12) {
        s_month -= 12;
        s_year++;
    }
    render_grid();
}

static void prev_clicked_cb(lv_event_t *e)
{
    (void)e;
    shift_month(-1);
}

static void next_clicked_cb(lv_event_t *e)
{
    (void)e;
    shift_month(1);
}

static void today_clicked_cb(lv_event_t *e)
{
    (void)e;
    struct tm now;
    if (calendar_get_now(&now) != ESP_OK) {
        ui_toast("时间未同步，请先联网校时");
        return;
    }
    s_year = now.tm_year + 1900;
    s_month = now.tm_mon + 1;
    render_grid();
}

static void refresh_clicked_cb(lv_event_t *e)
{
    (void)e;
    ui_toast("正在校时…");
    esp_err_t err = calendar_sync_ntp(0);
    ui_toast(err == ESP_OK ? "校时成功" : "校时失败：%s", esp_err_to_name(err));
}

/* ------------------------------------------------------------------------ */
/* Refresh                                                                   */
/* ------------------------------------------------------------------------ */

static void refresh_clock(void)
{
    if (s_clock == NULL) {
        return;
    }

    const bool synced = calendar_time_synced();
    if (!synced) {
        /* Requirement FR4: "时间未同步", never 1970. */
        lv_label_set_text(s_clock, UI_STATUS_TIME_UNSYNCED);
        lv_obj_set_style_text_color(s_clock, UI_COLOR_TEXT_MUTED, 0);
        if (s_have_date) {
            s_have_date = false;
            render_grid();
        }
        return;
    }

    struct tm now;
    if (calendar_get_now(&now) != ESP_OK) {
        return;
    }

    static const char *const weekdays[] = { "日", "一", "二", "三", "四", "五", "六" };
    const char *wd = (now.tm_wday >= 0 && now.tm_wday < 7) ? weekdays[now.tm_wday] : "?";

    lv_label_set_text_fmt(s_clock, "%02d:%02d:%02d\n%04d-%02d-%02d 星期%s",
                          now.tm_hour, now.tm_min, now.tm_sec,
                          now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, wd);
    lv_obj_set_style_text_color(s_clock, UI_COLOR_TEXT, 0);

    /* Adopt the real month the first time the clock becomes valid. */
    if (!s_have_date) {
        s_have_date = true;
        s_year = now.tm_year + 1900;
        s_month = now.tm_mon + 1;
        render_grid();
    }
}

/* ------------------------------------------------------------------------ */
/* Page                                                                      */
/* ------------------------------------------------------------------------ */

static esp_err_t calendar_create(lv_obj_t *parent)
{
    (void)page_header_create(parent, "日历");

    lv_obj_t *clock_card = NULL;
    lv_obj_t *clock_body = NULL;
    clock_card = ui_card_create(parent, NULL, &clock_body);
    if (clock_card == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_clock = lv_label_create(clock_body);
    lv_obj_set_width(s_clock, LV_PCT(100));
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_clock, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(s_clock, UI_COLOR_TEXT, 0);
    lv_label_set_text(s_clock, UI_STATUS_TIME_UNSYNCED);

    lv_obj_t *month_row = lv_obj_create(parent);
    lv_obj_remove_style_all(month_row);
    lv_obj_set_width(month_row, LV_PCT(100));
    lv_obj_set_height(month_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(month_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(month_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(month_row, 10, 0);
    lv_obj_remove_flag(month_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *prev = lv_button_create(month_row);
    lv_obj_set_size(prev, 64, UI_TOUCH_MIN);
    lv_obj_set_style_bg_color(prev, UI_COLOR_SURFACE_ALT, 0);
    lv_obj_t *prev_label = lv_label_create(prev);
    lv_label_set_text(prev_label, "上月");
    lv_obj_center(prev_label);
    lv_obj_add_event_cb(prev, prev_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_month_title = lv_label_create(month_row);
    lv_obj_set_flex_grow(s_month_title, 1);
    lv_obj_set_style_text_align(s_month_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_month_title, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(s_month_title, UI_COLOR_TEXT, 0);
    lv_label_set_text(s_month_title, "");

    lv_obj_t *next = lv_button_create(month_row);
    lv_obj_set_size(next, 64, UI_TOUCH_MIN);
    lv_obj_set_style_bg_color(next, UI_COLOR_SURFACE_ALT, 0);
    lv_obj_t *next_label = lv_label_create(next);
    lv_label_set_text(next_label, "下月");
    lv_obj_center(next_label);
    lv_obj_add_event_cb(next, next_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* Weekday header + day cells share one grid, so the columns always line up. */
    s_grid = lv_obj_create(parent);
    if (s_grid == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_width(s_grid, LV_PCT(100));
    lv_obj_set_flex_grow(s_grid, 1);
    lv_obj_set_style_bg_color(s_grid, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_grid, UI_CARD_RADIUS, 0);
    lv_obj_set_style_pad_all(s_grid, 6, 0);
    lv_obj_set_style_pad_row(s_grid, 2, 0);
    lv_obj_set_style_pad_column(s_grid, 4, 0);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);

    static const int32_t cols[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                    LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                    LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static const int32_t rows[] = { LV_GRID_CONTENT, LV_GRID_FR(1), LV_GRID_FR(1),
                                    LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                    LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    lv_obj_set_grid_dsc_array(s_grid, cols, rows);

    for (int i = 0; i < CAL_COLS; i++) {
        lv_obj_t *head = lv_label_create(s_grid);
        lv_label_set_text(head, s_week_names[i]);
        lv_obj_set_style_text_font(head, ui_font(UI_FONT_SMALL), 0);
        lv_obj_set_style_text_color(head, UI_COLOR_TEXT_MUTED, 0);
        lv_obj_set_grid_cell(head, LV_GRID_ALIGN_CENTER, i, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    }

    for (int i = 0; i < CAL_MAX_DAYS; i++) {
        lv_obj_t *cell = lv_label_create(s_grid);
        /* UI_FONT_SMALL, not UI_FONT_BODY.
         *
         * The grid shares its height over seven rows (weekday header + six date
         * rows), which on a 480 px panel is roughly 22 px per row. A 20 px font
         * needs ~26 px of line height, so the digits overflowed their cells and
         * adjacent date rows visibly collided (reported from the device). 16 px
         * fits with room to spare. */
        lv_obj_set_style_text_font(cell, ui_font(UI_FONT_SMALL), 0);
        lv_obj_set_style_text_color(cell, UI_COLOR_TEXT, 0);
        lv_obj_set_style_radius(cell, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(cell, UI_COLOR_PRIMARY, 0);
        lv_obj_set_style_pad_all(cell, 2, 0);
        lv_obj_set_style_text_align(cell, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
        s_grid_labels[i] = cell;
    }

    /* No date yet: show a neutral month so the page is never empty. */
    if (!s_have_date) {
        s_year = 2026;
        s_month = 1;
    }
    render_grid();
    refresh_clock();

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
        { "回到今天", today_clicked_cb, UI_COLOR_SURFACE_ALT },
        { "NTP 校时", refresh_clicked_cb, UI_COLOR_PRIMARY },
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

    (void)ui_event_bind(CALENDAR_EVENT, CALENDAR_EVENT_TIME_SYNCED, UI_PAGE_CALENDAR, 0);
    (void)ui_event_bind(CALENDAR_EVENT, CALENDAR_EVENT_TIMEZONE_CHANGED, UI_PAGE_CALENDAR, 0);
    SYS_LOGD(TAG, "calendar page built");
    return ESP_OK;
}

static esp_err_t calendar_on_show(void)
{
    refresh_clock();
    return ESP_OK;
}

static esp_err_t calendar_on_tick(void)
{
    refresh_clock();
    return ESP_OK;
}

const ui_page_t page_calendar = {
    .id = UI_PAGE_CALENDAR,
    .name = "calendar",
    .create = calendar_create,
    .on_show = calendar_on_show,
    .on_tick = calendar_on_tick,
    .tick_period_ms = 1000,
    /* Buttons: "校时" lives in the header area of the page. */
    .on_event = NULL,
};
