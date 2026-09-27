/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_calendar.c
 * @brief FR4: clock plus a month grid with previous/next month and the day's todos.
 *
 * Layout (top to bottom): one compact clock line, the month row, the month grid,
 * an action bar, and a details panel for the selected day.
 *
 * The board has no RTC, so before SNTP succeeds the clock area shows
 * UI_STATUS_TIME_UNSYNCED and the grid starts on a neutral month; that is the
 * honest behaviour required by FR4.
 *
 * Day cells are real buttons, not bare labels: todo_list() already carries a
 * `due_ts` per item, so a day that has work due is marked in the grid and tapping
 * it lists that day's items. Nothing new was needed on the todo side.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "calendar_service.h"
#include "pages_internal.h"
#include "todo_manager.h"

static const char *TAG = "page_calendar";

#define CAL_MAX_DAYS 31
#define CAL_COLS 7
/** Days of the grid that carry todo marks; only one month is ever on screen. */
#define CAL_MAX_MARKS 32

static lv_obj_t *s_clock;
static lv_obj_t *s_month_title;
static lv_obj_t *s_grid;
static lv_obj_t *s_detail;          /**< selected-day panel (title + item list) */
static lv_obj_t *s_detail_title;
static lv_obj_t *s_detail_list;
/** The action bar; hidden while the selected-day panel takes its place. */
static lv_obj_t *s_action_bar;

/**
 * Day cells.
 *
 * Each is a button so the whole cell is a touch target (a bare label only
 * responds on its glyphs, which on a 16 px font is a few pixels wide - reported
 * from the device as "hard to hit").
 */
typedef struct {
    lv_obj_t *btn;
    lv_obj_t *label;
    lv_obj_t *dot;      /**< small marker: this day has work due */
} cal_cell_t;

static cal_cell_t s_cells[CAL_MAX_DAYS];

static int s_year = 2026;
static int s_month = 1;
static int s_selected_day;          /**< 0 = nothing selected */
static bool s_have_date;

/**
 * Last values painted into the clock label.
 *
 * refresh_clock() runs every second (tick_period_ms = 1000) but only the seconds
 * field changes that often, so the label is rewritten only when a field it
 * actually shows differs. Without this the 1 Hz write invalidated the label and
 * re-laid-out the enclosing flex column every second (smoothness audit P1-8).
 */
static bool s_clock_text_synced;
static int s_last_sec = -1;
static int s_last_min = -1;
static int s_last_hour = -1;
static int s_last_mday = -1;
static int s_last_mon = -1;
static int s_last_year = -1;

/** UTF-8 weekday names: one entry per column, indexed like tm_wday. */
static const char *const s_week_names[] = { "日", "一", "二", "三", "四", "五", "六" };

/*
 * Vertical budget for this page, measured in pixels on the 480 px panel.
 *
 *   panel                                 480
 *   - status bar (UI_STATUSBAR_HEIGHT)     40
 *   - page padding top+bottom (14 x2)      28
 *   = usable                              412
 *
 * The page is a flex column, so anything that grows here steals from the grid:
 *
 *   header ~30 + clock 24 + month row 40 + bar 44 + 5 gaps (12) = 198
 *   => the grid gets ~214 px, i.e. ~30 px for each of its 7 rows, which clears
 *      the 16 px font's ~22 px line height with room for the due dot.
 *
 * The selected-day panel deliberately **replaces the action bar** (see
 * calendar_show_detail()) instead of stacking above it: with both visible the
 * grid would drop to ~17 px per row and the date rows collided - which is exactly
 * what happened on the first attempt at this feature.
 */
#define CAL_DETAIL_HEIGHT 44
#define CAL_CLOCK_HEIGHT  24

/* Defined in the sections below; the day-selection callback needs them. */
static void refresh_clock(void);
static void render_grid(void);
static void render_detail(void);


/* ------------------------------------------------------------------------ */
/* Due-date marks                                                            */
/* ------------------------------------------------------------------------ */

/** Per-day summary of what is due inside the month currently on screen. */
typedef struct {
    uint16_t open;      /**< not-done items due that day */
    uint16_t done;      /**< done items due that day */
} cal_mark_t;

static cal_mark_t s_marks[CAL_MAX_MARKS];

/**
 * Fill s_marks[] with the todo workload for the month on screen.
 *
 * Only items with a real `due_ts` can be placed, and the clock must be synced -
 * before NTP the epoch is unknown, so every due date would land in 1970 and the
 * grid would be nonsense. The caller checks `s_have_date` first.
 *
 * The month is matched in **local** time, because that is what the grid shows.
 */
static void collect_marks(void)
{
    memset(s_marks, 0, sizeof(s_marks));

    todo_item_t *items = NULL;
    size_t count = 0;
    if (todo_list(&items, &count) != ESP_OK) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        const todo_item_t *it = &items[i];
        if (it->due_ts <= 0) {
            continue;
        }

        const time_t due = (time_t)it->due_ts;
        struct tm tm_due;
        if (localtime_r(&due, &tm_due) == NULL) {
            continue;
        }
        if ((tm_due.tm_year + 1900) != s_year || (tm_due.tm_mon + 1) != s_month) {
            continue;
        }
        const int day = tm_due.tm_mday;
        if (day < 1 || day > CAL_MAX_MARKS) {
            continue;
        }

        /* The day a recurring item lands on is not modelled here; one entry per
         * item, counted once. */
        if (it->done) {
            s_marks[day - 1].done++;
        } else {
            s_marks[day - 1].open++;
        }
    }

    todo_free_list(items);
}

/* ------------------------------------------------------------------------ */
/* Grid                                                                      */
/* ------------------------------------------------------------------------ */

static void day_clicked_cb(lv_event_t *e)
{
    const int day = (int)(intptr_t)lv_event_get_user_data(e);
    if (day < 1) {
        return;
    }
    /* Tapping the selected day again clears the selection (and the panel). */
    s_selected_day = (s_selected_day == day) ? 0 : day;
    render_grid();
    render_detail();
}

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
        if (s_cells[i].btn != NULL) {
            lv_obj_add_flag(s_cells[i].btn, LV_OBJ_FLAG_HIDDEN);
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

    /* A selected day that is no longer in view must not survive a month change. */
    if (s_selected_day > (int)count) {
        s_selected_day = 0;
    }

    if (s_have_date) {
        collect_marks();
    } else {
        memset(s_marks, 0, sizeof(s_marks));
    }

    const int first_wday = (count > 0) ? days[0].tm_wday : 0;
    for (size_t i = 0; i < count; i++) {
        cal_cell_t *cell = &s_cells[i];
        if (cell->btn == NULL) {
            continue;
        }

        const int day = days[i].tm_mday;
        const int pos = first_wday + (int)i;
        const int row = pos / CAL_COLS;
        const int col = pos % CAL_COLS;

        lv_obj_set_grid_cell(cell->btn, LV_GRID_ALIGN_STRETCH, col, 1,
                             LV_GRID_ALIGN_STRETCH, row + 1, 1);
        lv_label_set_text_fmt(cell->label, "%d", day);
        lv_obj_remove_flag(cell->btn, LV_OBJ_FLAG_HIDDEN);

        const bool is_today = (day == today);
        const bool is_selected = (day == s_selected_day);
        const cal_mark_t *mark = &s_marks[(day >= 1 && day <= CAL_MAX_MARKS) ? day - 1 : 0];

        /* Fill priority: selected > today > plain. Text colour follows, so a
         * filled cell never keeps dark text on the accent colour. */
        if (is_selected) {
            lv_obj_set_style_bg_opa(cell->btn, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(cell->btn, UI_COLOR_PRIMARY, 0);
            lv_obj_set_style_text_color(cell->label, UI_COLOR_ON_PRIMARY, 0);
        } else if (is_today) {
            lv_obj_set_style_bg_opa(cell->btn, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(cell->btn, UI_COLOR_SURFACE_ALT, 0);
            lv_obj_set_style_outline_width(cell->btn, 2, 0);
            lv_obj_set_style_outline_color(cell->btn, UI_COLOR_PRIMARY, 0);
            lv_obj_set_style_text_color(cell->label, UI_COLOR_TEXT, 0);
        } else {
            lv_obj_set_style_bg_opa(cell->btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(cell->label, UI_COLOR_TEXT, 0);
        }
        if (!is_today) {
            lv_obj_set_style_outline_width(cell->btn, 0, 0);
        }

        /* The due marker is a small dot: orange while work is outstanding, muted
         * once everything due that day is done. Shape + colour together, so it
         * does not rely on colour alone. */
        if (cell->dot != NULL) {
            if (mark->open > 0) {
                lv_obj_set_style_bg_color(cell->dot, UI_COLOR_WARNING, 0);
                lv_obj_set_style_bg_opa(cell->dot, LV_OPA_COVER, 0);
                lv_obj_remove_flag(cell->dot, LV_OBJ_FLAG_HIDDEN);
            } else if (mark->done > 0) {
                lv_obj_set_style_bg_color(cell->dot, UI_COLOR_SUCCESS, 0);
                lv_obj_set_style_bg_opa(cell->dot, LV_OPA_COVER, 0);
                lv_obj_remove_flag(cell->dot, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(cell->dot, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Selected-day detail                                                       */
/* ------------------------------------------------------------------------ */

/** Rebuild the panel under the grid for s_selected_day. */
static void render_detail(void)
{
    if (s_detail == NULL || s_detail_title == NULL || s_detail_list == NULL) {
        return;
    }

    /*
     * Exactly one of the panel / action bar is visible.
     *
     * They are the same height and live in the same flex position, so swapping
     * them keeps the page height - and therefore the grid rows - constant whether
     * or not a day is selected.
     */
    if (s_selected_day < 1) {
        lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
        if (s_action_bar != NULL) {
            lv_obj_remove_flag(s_action_bar, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    lv_obj_remove_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    if (s_action_bar != NULL) {
        lv_obj_add_flag(s_action_bar, LV_OBJ_FLAG_HIDDEN);
    }

    /* Weekday of the selected date: mktime() resolves it without needing the
     * clock, because the date itself is known. */
    struct tm sel = { 0 };
    sel.tm_year = s_year - 1900;
    sel.tm_mon = s_month - 1;
    sel.tm_mday = s_selected_day;
    sel.tm_hour = 12;
    sel.tm_isdst = -1;
    const char *wd = "?";
    if (mktime(&sel) != (time_t)-1 && sel.tm_wday >= 0 && sel.tm_wday < 7) {
        wd = s_week_names[sel.tm_wday];
    }

    lv_label_set_text_fmt(s_detail_title, "%02d 月 %02d 日 · 星期%s", s_month, s_selected_day, wd);

    /* Rebuild the lines: a day holds a handful of items at most, so the churn is
     * not worth an incremental scheme (and the panel is usually hidden). */
    lv_obj_clean(s_detail_list);

    todo_item_t *items = NULL;
    size_t count = 0;
    if (todo_list(&items, &count) != ESP_OK) {
        return;
    }

    size_t shown = 0;
    for (size_t i = 0; i < count; i++) {
        const todo_item_t *it = &items[i];
        if (it->due_ts <= 0) {
            continue;
        }
        const time_t due = (time_t)it->due_ts;
        struct tm tm_due;
        if (localtime_r(&due, &tm_due) == NULL) {
            continue;
        }
        if ((tm_due.tm_year + 1900) != s_year || (tm_due.tm_mon + 1) != s_month ||
            tm_due.tm_mday != s_selected_day) {
            continue;
        }

        lv_obj_t *row = lv_label_create(s_detail_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_label_set_long_mode(row, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(row, ui_font(UI_FONT_SMALL), 0);
        /* Mark completion with a leading check and a muted colour: LVGL has no
         * text-decoration, same convention as the todo page. */
        if (it->done) {
            lv_label_set_text_fmt(row, "✓ %s", it->title);
            lv_obj_set_style_text_color(row, UI_COLOR_TEXT_MUTED, 0);
        } else {
            lv_label_set_text_fmt(row, "· %s", it->title);
            lv_obj_set_style_text_color(row, UI_COLOR_TEXT, 0);
        }
        shown++;
    }

    if (shown == 0) {
        lv_obj_t *empty = lv_label_create(s_detail_list);
        lv_label_set_text(empty, "这一天没有到期待办");
        lv_obj_set_style_text_font(empty, ui_font(UI_FONT_SMALL), 0);
        lv_obj_set_style_text_color(empty, UI_COLOR_TEXT_MUTED, 0);
    }

    todo_free_list(items);
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
    /* The selected day belongs to the month that was on screen; drop it rather
     * than silently reinterpreting the number in the new month. */
    s_selected_day = 0;
    render_grid();
    render_detail();
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
    s_selected_day = now.tm_mday;
    render_grid();
    render_detail();
}

static void refresh_clicked_cb(lv_event_t *e)
{
    (void)e;
    /* Async: the synchronous variant waits up to 10 s on the network while the
     * LVGL lock is held, which froze the whole UI (smoothness audit P0). The
     * outcome arrives as CALENDAR_EVENT_SYNC_RESULT. */
    const esp_err_t err = calendar_sync_ntp_async();
    if (err == ESP_ERR_TIMEOUT) {
        ui_toast("正在校时，请稍候…");
    } else if (err != ESP_OK) {
        ui_toast("校时失败：%s", esp_err_to_name(err));
    } else {
        ui_toast("正在校时…");
    }
}

/** Result of a requested sync, marshalled onto the LVGL task by ui_core. */
static esp_err_t calendar_on_sync_result(int32_t event_id, void *data)
{
    (void)event_id;
    const esp_err_t err = (data != NULL) ? *(const esp_err_t *)data : ESP_FAIL;
    ui_toast(err == ESP_OK ? "校时成功" : "校时失败：%s", esp_err_to_name(err));
    if (err == ESP_OK) {
        refresh_clock();
    }
    return ESP_OK;
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
        if (s_clock_text_synced) {
            s_clock_text_synced = false;
            lv_label_set_text(s_clock, UI_STATUS_TIME_UNSYNCED);
            lv_obj_set_style_text_color(s_clock, UI_COLOR_TEXT_MUTED, 0);
        }
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

    /* Only repaint when a *displayed* field changed. The label is not fixed
     * width, so re-setting identical text still invalidates it and makes the
     * enclosing flex column re-layout - and this runs at 1 Hz (audit P1-8). */
    if (!s_clock_text_synced || now.tm_sec != s_last_sec || now.tm_min != s_last_min ||
        now.tm_hour != s_last_hour || now.tm_mday != s_last_mday ||
        now.tm_mon != s_last_mon || now.tm_year != s_last_year) {
        lv_label_set_text_fmt(s_clock, "%02d:%02d:%02d\n%04d-%02d-%02d 星期%s",
                              now.tm_hour, now.tm_min, now.tm_sec,
                              now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, wd);
        s_last_sec = now.tm_sec;
        s_last_min = now.tm_min;
        s_last_hour = now.tm_hour;
        s_last_mday = now.tm_mday;
        s_last_mon = now.tm_mon;
        s_last_year = now.tm_year;
        if (!s_clock_text_synced) {
            s_clock_text_synced = true;
            lv_obj_set_style_text_color(s_clock, UI_COLOR_TEXT, 0);
        }
    }

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

    /*
     * Clock: ONE line.
     *
     * It used to be a card of its own holding two lines of 28 px text, sitting
     * directly above a month row that repeated the year and month - three bands of
     * the 480 px panel spent on the same information, which squeezed the grid and
     * left no room for anything else.
     */
    s_clock = lv_label_create(parent);
    lv_obj_set_width(s_clock, LV_PCT(100));
    /* Fixed one-line height: the label must not be able to grow and squeeze the
     * grid below it (see the budget note at the top of this file). */
    lv_obj_set_height(s_clock, CAL_CLOCK_HEIGHT);
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_clock, ui_font(UI_FONT_BODY), 0);
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

    const struct {
        const char *text;
        lv_event_cb_t cb;
    } nav[] = { { "上月", prev_clicked_cb }, { "下月", next_clicked_cb } };

    for (size_t i = 0; i < sizeof(nav) / sizeof(nav[0]); i++) {
        lv_obj_t *btn = lv_button_create(month_row);
        lv_obj_set_size(btn, 64, 40);
        lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE_ALT, 0);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, nav[i].text);
        /* BODY font, matching the action bar below: these used to be the only
         * buttons on the page still on the default font. */
        lv_obj_set_style_text_font(label, ui_font(UI_FONT_BODY), 0);
        lv_obj_center(label);
        lv_obj_add_event_cb(btn, nav[i].cb, LV_EVENT_CLICKED, NULL);

        if (i == 0) {
            /* The title sits between the two arrows. */
            s_month_title = lv_label_create(month_row);
            lv_obj_set_flex_grow(s_month_title, 1);
            lv_obj_set_style_text_align(s_month_title, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(s_month_title, ui_font(UI_FONT_TITLE), 0);
            lv_obj_set_style_text_color(s_month_title, UI_COLOR_TEXT, 0);
            lv_label_set_text(s_month_title, "");
        }
    }

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
        /* Weekend columns read as weekend before any date is picked. */
        const bool weekend = (i == 0 || i == 6);
        lv_obj_set_style_text_color(head, weekend ? UI_COLOR_WARNING : UI_COLOR_TEXT_MUTED, 0);
        lv_obj_set_grid_cell(head, LV_GRID_ALIGN_CENTER, i, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    }

    for (int i = 0; i < CAL_MAX_DAYS; i++) {
        cal_cell_t *cell = &s_cells[i];

        /*
         * A real button stretched across its whole grid cell.
         *
         * The cell used to be a bare label sized to its text, so a 16 px digit was
         * the entire touch target - the device reported it as hard to hit. The
         * grid uses ALIGN_STRETCH and the button fills the cell, giving roughly
         * 60x40 px per day; LV_GRID_ALIGN_CENTER would shrink it back to content.
         */
        lv_obj_t *btn = lv_button_create(s_grid);
        cell->btn = btn;
        lv_obj_remove_style_all(btn);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_all(btn, 2, 0);
        lv_obj_set_style_bg_color(btn, UI_COLOR_PRIMARY, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_outline_color(btn, UI_COLOR_PRIMARY, 0);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(btn, day_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));

        lv_obj_t *label = lv_label_create(btn);
        cell->label = label;
        /* UI_FONT_SMALL, not UI_FONT_BODY: seven rows share the grid height, which
         * on a 480 px panel is roughly 22 px each. A 20 px font needs ~26 px of
         * line height, so digits overflowed and adjacent date rows collided
         * (reported from the device). 16 px fits with room to spare. */
        lv_obj_set_style_text_font(label, ui_font(UI_FONT_SMALL), 0);
        lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);

        /* Due marker: a small dot in the lower part of the cell. */
        lv_obj_t *dot = lv_obj_create(btn);
        cell->dot = dot;
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, 0);
    }

    /*
     * Bottom of the page: the selected-day panel and the action bar.
     *
     * They are **siblings with a fixed height each**, and render_detail() makes
     * exactly one of them visible. They must not be wrapped in a shared
     * fixed-height container with flex_grow on both: grow() only distributes
     * *free* space, so two growing children of a 44 px box simply split those
     * 44 px and pushed the panel's own content out of it, which is what squeezed
     * the layout the moment a day was tapped.
     *
     * As siblings in the page column, hiding one makes the other take its place
     * and the grid above keeps its ~30 px rows either way.
     */
    const int32_t slot_h = UI_TOUCH_MIN;

    /* --- selected-day panel (hidden until a date is tapped) --- */
    s_detail = lv_obj_create(parent);
    if (s_detail == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_detail);
    lv_obj_set_width(s_detail, LV_PCT(100));
    lv_obj_set_height(s_detail, slot_h);
    lv_obj_set_flex_flow(s_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(s_detail, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_detail, UI_CARD_RADIUS, 0);
    lv_obj_set_style_pad_hor(s_detail, 10, 0);
    lv_obj_set_style_pad_ver(s_detail, 3, 0);
    lv_obj_set_style_pad_row(s_detail, 1, 0);
    lv_obj_remove_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);

    s_detail_title = lv_label_create(s_detail);
    lv_obj_set_width(s_detail_title, LV_PCT(100));
    lv_label_set_long_mode(s_detail_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_detail_title, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_detail_title, UI_COLOR_PRIMARY, 0);
    lv_label_set_text(s_detail_title, "");

    /* The items scroll horizontally on one line, so the panel never needs to grow
     * taller than the action bar it replaces. */
    s_detail_list = lv_obj_create(s_detail);
    lv_obj_remove_style_all(s_detail_list);
    lv_obj_set_width(s_detail_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_detail_list, 1);
    lv_obj_set_flex_flow(s_detail_list, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_detail_list, 10, 0);
    lv_obj_set_scroll_dir(s_detail_list, LV_DIR_HOR);
    lv_obj_add_flag(s_detail_list, LV_OBJ_FLAG_SCROLLABLE);

    /* --- action bar --- */
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, slot_h);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    s_action_bar = bar;

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
    /* The grid marks come from todo_list(), so a change made on the web page (or
     * on the todo page) has to repaint this grid too. */
    (void)ui_event_bind(TODO_EVENT, TODO_EVENT_LIST_CHANGED, UI_PAGE_CALENDAR, 0);
    /* Outcome of a *requested* sync (this page's NTP button). */
    (void)ui_event_subscribe(CALENDAR_EVENT, CALENDAR_EVENT_SYNC_RESULT,
                             sizeof(esp_err_t), calendar_on_sync_result);

    /* No date yet: show a neutral month so the page is never empty. */
    if (!s_have_date) {
        s_year = 2026;
        s_month = 1;
    }
    s_selected_day = 0;
    render_grid();
    render_detail();
    refresh_clock();

    SYS_LOGD(TAG, "calendar page built");
    return ESP_OK;
}


static esp_err_t calendar_on_show(void)
{
    refresh_clock();
    /* Todo marks may have changed while another page was on screen; the bindings
     * only cover changes that happen *while* this page is visible, and they are
     * delivered to the page even when it is hidden - re-rendering once on entry
     * keeps the grid honest either way. */
    render_grid();
    render_detail();
    return ESP_OK;
}

static esp_err_t calendar_on_tick(void)
{
    refresh_clock();
    return ESP_OK;
}

/**
 * Events delivered by ui_core (in the LVGL task).
 *
 * TIME_SYNCED / TIMEZONE_CHANGED come from the calendar service; LIST_CHANGED
 * comes from todo_manager, and is what makes a todo added on the web page show up
 * as a dot on the grid without the user doing anything.
 */
static esp_err_t calendar_on_event(int32_t event_id, void *data)
{
    (void)event_id;
    (void)data;
    /* refresh_clock() adopts the real month the first time the clock is valid, so
     * a sync that lands after the page was built also fixes the month on screen. */
    refresh_clock();
    render_grid();
    render_detail();
    return ESP_OK;
}

const ui_page_t page_calendar = {
    .id = UI_PAGE_CALENDAR,
    .name = "calendar",
    .create = calendar_create,
    .on_show = calendar_on_show,
    .on_tick = calendar_on_tick,
    .on_event = calendar_on_event,
    .tick_period_ms = 1000,
};
