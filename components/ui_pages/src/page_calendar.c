/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_calendar.c
 * @brief FR4: a clock header, a month grid with previous/next month and the day's todos.
 *
 * Layout (top to bottom):
 *   - header row: [返回][大时间 HH:MM / 小字日期][日历 · 星期 + 本月到期数][回到今天][校时]
 *   - month row:  [上月] 2026 年 10 月 [下月]
 *   - month grid: 7 columns, weekday header, one button per day, due dots
 *   - detail panel: the selected day's title + up to 3 todo lines (always on screen)
 *
 * The board has no RTC, so before SNTP succeeds the clock shows "--:--" with
 * "时间未同步" underneath, no day is highlighted as today, and the grid starts on a
 * neutral month; that is the honest behaviour required by FR4.
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
/** The detail panel lists at most this many lines (todo lines + the tail line). */
#define CAL_DETAIL_MAX_LINES 3
/** Hard upper bound for the detail panel; see the budget note below. */
#define CAL_DETAIL_MAX_HEIGHT 96
/** The big time while the wall clock has never been synchronised. */
#define CAL_TIME_UNSYNCED "--:--"

/* ---- header ------------------------------------------------------------- */
static lv_obj_t *s_clock;          /**< big time, HH:MM */
static lv_obj_t *s_mid_title;      /**< middle segment, line 1: 日历 · 星期X */
static lv_obj_t *s_mid_sub;        /**< middle segment, line 2: 本月 N 项到期 */
static lv_obj_t *s_month_title;
static lv_obj_t *s_grid;
static lv_obj_t *s_detail;          /**< selected-day panel (title + item list) */
static lv_obj_t *s_detail_title;
static lv_obj_t *s_detail_list;
static lv_obj_t *s_detail_hint;     /**< shown when no day is selected */

/** The page container: the parent of every object of this page. */
static lv_obj_t *s_parent;
/**
 * The page's scroll offset, remembered across the content's destroy/rebuild.
 *
 * The page container is scrollable (ui_core gives every page container
 * LV_OBJ_FLAG_SCROLLABLE), and destroying the content makes LVGL clamp the
 * offset to the new (much smaller) content height
 * (lv_obj.c -> readjust_scroll_after_layout -> lv_obj_scroll.c).
 * Without remembering and restoring it, every visit would come back at the
 * top - a visible change from before this round.
 */
static int32_t s_scroll_y;
/** Slot of the content block (grid + detail panel) among the page's children. */
static int32_t s_content_index;
/**
 * Week rows the grid was last laid out with.
 *
 * The row count decides how tall a day cell is, so a rebuild has to use the
 * very same number - otherwise the grid would change its height behind the
 * user's back. It is derived from the month on screen (s_year / s_month) and
 * kept here so the rebuild does not have to recompute it.
 */
static int s_week_rows;

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
 * Last values painted into the header.
 *
 * refresh_clock() runs every second (tick_period_ms = 1000) but the big time
 * only shows minutes, so the labels are rewritten only when a field they
 * actually show differs. Without this the 1 Hz write invalidated the labels and
 * re-laid-out the enclosing flex column every second (smoothness audit P1-8).
 */
static bool s_clock_valid;          /**< the header currently shows a real time */
static int s_last_min = -1;
static int s_last_hour = -1;
static int s_last_mday = -1;
static int s_last_mon = -1;
static int s_last_year = -1;

/** Number of unfinished todos due in the *current* month (header middle line). */
static uint16_t s_month_due;
static bool s_month_due_painted;
static uint16_t s_painted_month_due;

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
 * The page is a flex column; the four bands are separated by UI_PAGE_GAP (12):
 *
 *   header row    ~80   big time (48 px font -> ~58 line box) + date line (~20)
 *   month row      44   the two arrow buttons (UI_TOUCH_MIN)
 *   grid          flex   weekday header (~20) + up to 6 week rows
 *   detail panel  26..90 title (~20) + up to 3 todo lines (~20 each)
 *   gaps       3 x 12 = 36
 *
 * => grid = 412 - 80 - 44 - 36 - detail = 252 - detail
 *
 *   detail 26 (no day selected) -> grid 226 -> ~31 px per week row
 *   detail 90 (3 todo lines)     -> grid 162 -> ~20 px per week row
 *
 * Two consequences, both deliberate:
 *
 *  1. The day numbers stay on UI_FONT_SMALL (16 px, ~20 px line box).  The 20 px
 *     font needs a ~25 px line box, which the tightest state (a day with three
 *     todos selected) cannot offer - the digits would touch the due dot.
 *  2. The detail panel is LV_SIZE_CONTENT with an upper bound
 *     (CAL_DETAIL_MAX_HEIGHT), never a fixed height that could push the grid
 *     below the font height.  It also never shows more than
 *     CAL_DETAIL_MAX_LINES lines; a longer day ends with "还有 N 条".
 *
 * What changed against v1.5.0: the clock used to be a 24 px high label holding
 * **two** lines of 20 px text - the second line was always cropped - and the
 * two action buttons lived in a 44 px bar at the bottom.  The clock is now the
 * header row and the buttons moved into the header, so the bottom of the page
 * belongs to the grid and to the (now always visible) detail panel.
 */

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
 * Is this todo due on the given local date?
 *
 * `due_ts` is an epoch stamp, so it has to be converted to local time before it
 * can be compared with a calendar cell - the grid shows local dates.
 */
static bool due_on_day(const todo_item_t *it, int year, int month, int day)
{
    if (it->due_ts <= 0) {
        return false;
    }
    const time_t due = (time_t)it->due_ts;
    struct tm tm_due;
    if (localtime_r(&due, &tm_due) == NULL) {
        return false;
    }
    return (tm_due.tm_year + 1900) == year && (tm_due.tm_mon + 1) == month &&
           tm_due.tm_mday == day;
}

/**
 * Fill s_marks[] with the todo workload for the month on screen and count the
 * unfinished todos of the *current* month for the header.
 *
 * Only items with a real `due_ts` can be placed, and the clock must be synced -
 * before NTP the epoch is unknown, so every due date would land in 1970 and the
 * grid would be nonsense. The caller checks `s_have_date` first.
 *
 * The month is matched in **local** time, because that is what the grid shows.
 * The header counter always refers to the real current month, so it stays
 * meaningful while the user browses through other months.
 */
static void collect_marks(void)
{
    memset(s_marks, 0, sizeof(s_marks));
    s_month_due = 0;

    todo_item_t *items = NULL;
    size_t count = 0;
    if (todo_list(&items, &count) != ESP_OK) {
        return;
    }

    /* The real current month, for the header counter. */
    struct tm now = { 0 };
    const bool have_now = s_have_date && calendar_get_now(&now) == ESP_OK;
    const int now_year = have_now ? (now.tm_year + 1900) : 0;
    const int now_month = have_now ? (now.tm_mon + 1) : 0;

    for (size_t i = 0; i < count; i++) {
        const todo_item_t *it = &items[i];
        if (it->due_ts <= 0) {
            continue;
        }

        /* The day a recurring item lands on is not modelled here; one entry per
         * item, counted once. */
        const time_t due = (time_t)it->due_ts;
        struct tm tm_due;
        if (localtime_r(&due, &tm_due) == NULL) {
            continue;
        }
        const int year = tm_due.tm_year + 1900;
        const int month = tm_due.tm_mon + 1;

        /* The header counter always refers to the real current month, so it
         * stays meaningful while the user browses through other months. */
        if (have_now && !it->done && year == now_year && month == now_month) {
            s_month_due++;
        }

        /* The dots belong to the month on screen. */
        if (year != s_year || month != s_month) {
            continue;
        }
        const int day = tm_due.tm_mday;
        if (day < 1 || day > CAL_MAX_MARKS) {
            continue;
        }

        if (it->done) {
            s_marks[day - 1].done++;
        } else {
            s_marks[day - 1].open++;
        }
    }

    todo_free_list(items);
}

/**
 * Paint the header's "本月 N 项到期" line.
 *
 * Written only when the number changed: this runs on every grid repaint and the
 * label is not fixed width, so re-setting identical text still invalidates it.
 */
static void paint_month_count(void)
{
    if (s_mid_sub == NULL) {
        return;
    }
    if (s_month_due_painted && s_month_due == s_painted_month_due) {
        return;
    }
    s_month_due_painted = true;
    s_painted_month_due = s_month_due;

    if (s_month_due == 0) {
        lv_label_set_text(s_mid_sub, "本月没有到期");
    } else {
        lv_label_set_text_fmt(s_mid_sub, "本月 %u 项到期", (unsigned)s_month_due);
    }
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

/**
 * @brief 按「这个月要占几周」生成网格的行轨道。
 *
 * LVGL 的 FR 轨道会把容器的剩余高度**平均分给所有 FR 行**——哪怕那一行里
 * 一个格子都没有。原来的轨道表固定有 6 个 FR 行，于是只占 5 行的月份
 * （大部分月份）白白浪费 1/6 的高度：真机实测（debug shell 的 widgets 树）
 * 日期格子只有 30 px 高，正好是 225 px 网格除以 6 再减去表头与内边距。
 *
 * 改成按实际周行数生成：多出来的行用 LV_GRID_CONTENT（不占高度），
 * 5 行的月份格子就有 37~45 px，6 行的月份也有 33~39 px。
 *
 * @param week_rows 这个月占的周行数（5 或 6；0 = 还没算出来时按 6 行留位）。
 * @return 轨道数组（调用方传给 lv_obj_set_grid_dsc_array，LVGL 会复制）。
 */
static const int32_t *cal_grid_rows(int week_rows)
{
    if (week_rows < 1 || week_rows > 6) {
        week_rows = 6;
    }
    /* 0 = 星期表头，1..week_rows = 各周，剩下的行不占高度。 */
    static int32_t rows[8];
    rows[0] = LV_GRID_CONTENT;
    for (int i = 0; i < 6; i++) {
        rows[1 + i] = (i < week_rows) ? LV_GRID_FR(1) : LV_GRID_CONTENT;
    }
    rows[7] = LV_GRID_TEMPLATE_LAST;
    return rows;
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
    struct tm now = { 0 };
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
        paint_month_count();
    } else {
        memset(s_marks, 0, sizeof(s_marks));
    }

    const int first_wday = (count > 0) ? days[0].tm_wday : 0;
    /*
     * 这个月要占几周 = 首日星期 + 天数，向上取整到整周。
     * 轨道按它重算，空出来的那一行就不再白吃高度（见 cal_grid_rows 的注释）。
     */
    const int week_rows = (int)((first_wday + (int)count + CAL_COLS - 1) / CAL_COLS);
    /* Remember it: a rebuild of the grid has to use the same row count, or the
     * day cells would come back at a different height. */
    s_week_rows = week_rows;
    {
        static const int32_t cols[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                        LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
        lv_obj_set_grid_dsc_array(s_grid, cols, cal_grid_rows(week_rows));
    }
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

/** One todo line in the detail panel: "· 标题" or "✓ 标题". */
static void detail_row_create(lv_obj_t *parent, const todo_item_t *it)
{
    lv_obj_t *row = lv_label_create(parent);
    if (row == NULL) {
        return;
    }
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
}

/** Rebuild the panel under the grid for s_selected_day. */
static void render_detail(void)
{
    if (s_detail == NULL || s_detail_title == NULL || s_detail_list == NULL ||
        s_detail_hint == NULL) {
        return;
    }

    if (s_selected_day < 1) {
        /* Nothing selected: the panel keeps its place under the grid and only
         * shows one hint line. */
        lv_obj_add_flag(s_detail_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_detail_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_detail_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_remove_flag(s_detail_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_detail_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_detail_list, LV_OBJ_FLAG_HIDDEN);

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

    /* Rebuild the lines: a day holds a handful of items at most, so the churn is
     * not worth an incremental scheme. */
    lv_obj_clean(s_detail_list);

    todo_item_t *items = NULL;
    size_t count = 0;
    if (todo_list(&items, &count) != ESP_OK) {
        return;
    }

    /* First pass: how many todos are due that day (the "共 N 项" in the title). */
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (due_on_day(&items[i], s_year, s_month, s_selected_day)) {
            total++;
        }
    }

    /* Second pass: at most CAL_DETAIL_MAX_LINES lines. When the day holds more
     * todos than that, the last line becomes the "还有 N 条" summary - a fourth
     * line would push the panel past its budget and squeeze the grid. */
    const size_t body = (total > CAL_DETAIL_MAX_LINES) ? (CAL_DETAIL_MAX_LINES - 1) : total;
    size_t drawn = 0;
    for (size_t i = 0; i < count && drawn < body; i++) {
        const todo_item_t *it = &items[i];
        if (!due_on_day(it, s_year, s_month, s_selected_day)) {
            continue;
        }
        detail_row_create(s_detail_list, it);
        drawn++;
    }

    if (total > body) {
        lv_obj_t *more = lv_label_create(s_detail_list);
        if (more != NULL) {
            lv_obj_set_width(more, LV_PCT(100));
            lv_label_set_long_mode(more, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_style_text_font(more, ui_font(UI_FONT_SMALL), 0);
            lv_obj_set_style_text_color(more, UI_COLOR_TEXT_MUTED, 0);
            lv_label_set_text_fmt(more, "还有 %u 条", (unsigned)(total - body));
        }
    } else if (total == 0) {
        lv_obj_t *empty = lv_label_create(s_detail_list);
        if (empty != NULL) {
            lv_obj_set_width(empty, LV_PCT(100));
            lv_label_set_text(empty, "这一天没有到期待办");
            lv_obj_set_style_text_font(empty, ui_font(UI_FONT_SMALL), 0);
            lv_obj_set_style_text_color(empty, UI_COLOR_TEXT_MUTED, 0);
        }
    }

    lv_label_set_text_fmt(s_detail_title, "%d 月 %d 日 · 星期%s · 共 %u 项",
                           s_month, s_selected_day, wd, (unsigned)total);

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
    if (s_clock == NULL || s_mid_title == NULL || s_mid_sub == NULL) {
        return;
    }

    const bool synced = calendar_time_synced();
    if (!synced) {
        /* Requirement FR4: "--:--" and "时间未同步", never 1970. */
        if (s_clock_valid) {
            s_clock_valid = false;
            lv_label_set_text(s_clock, CAL_TIME_UNSYNCED);
            lv_obj_set_style_text_color(s_clock, UI_COLOR_TEXT_MUTED, 0);
            lv_label_set_text(s_mid_title, "日历");
            lv_label_set_text(s_mid_sub, "未校时");
            /* Force the counter to be written again when the clock comes back. */
            s_month_due_painted = false;
        }
        if (s_have_date) {
            s_have_date = false;
            render_grid();
        }
        return;
    }

    struct tm now = { 0 };
    if (calendar_get_now(&now) != ESP_OK) {
        return;
    }

    const bool was_valid = s_clock_valid;
    const bool time_changed = (now.tm_hour != s_last_hour) || (now.tm_min != s_last_min);
    const bool date_changed = (now.tm_mday != s_last_mday) || (now.tm_mon != s_last_mon) ||
                              (now.tm_year != s_last_year);

    /* Only repaint when a *displayed* field changed. The labels are not fixed
     * width, so re-setting identical text still invalidates them and makes the
     * enclosing flex column re-layout - and this runs at 1 Hz (audit P1-8). */
    if (!was_valid || time_changed) {
        lv_label_set_text_fmt(s_clock, "%02d:%02d", now.tm_hour, now.tm_min);
        s_last_hour = now.tm_hour;
        s_last_min = now.tm_min;
        if (!was_valid) {
            lv_obj_set_style_text_color(s_clock, UI_COLOR_TEXT, 0);
        }
    }

    if (!was_valid || date_changed) {
        const char *wd = (now.tm_wday >= 0 && now.tm_wday < 7) ? s_week_names[now.tm_wday] : "?";
        /*
         * 日期与星期都放在中列，而不是压在大时钟下面：
         * 真机实测（debug shell 的 widgets 树）时钟下面那一行日期占 19 px，
         * 而网格的日期格子只有 30 px 高——那 19 px 直接从日期格子上扣。
         * 中列有 391 px 宽，放得下。
         */
        lv_label_set_text_fmt(s_mid_title, "日历 · %d 月 %d 日 星期%s",
                              now.tm_mon + 1, now.tm_mday, wd);
        s_last_mday = now.tm_mday;
        s_last_mon = now.tm_mon;
        s_last_year = now.tm_year;
    }

    if (!s_clock_valid) {
        s_clock_valid = true;
    }

    /* Adopt the real month the first time the clock becomes valid. */
    if (!s_have_date) {
        s_have_date = true;
        s_year = now.tm_year + 1900;
        s_month = now.tm_mon + 1;
        render_grid();
    } else if (date_changed) {
        /* Midnight: today's highlight and the month counter have to follow. This
         * happens once a day, not on the 1 Hz path. */
        render_grid();
    }
}

/* ------------------------------------------------------------------------ */
/* Page                                                                      */
/* ------------------------------------------------------------------------ */

/** A header button: UI_TOUCH_MIN high, width following the text. */
static lv_obj_t *header_button_create(lv_obj_t *parent, const char *text,
                                      lv_color_t bg, lv_color_t fg, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    if (btn == NULL) {
        return NULL;
    }
    lv_obj_set_height(btn, UI_TOUCH_MIN);
    lv_obj_set_width(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_radius(btn, UI_CARD_RADIUS, 0);
    lv_obj_set_style_pad_hor(btn, 12, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(label, fg, 0);
    lv_obj_center(label);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/* ------------------------------------------------------------------------ */
/* Content block (month grid + selected-day panel)                          */
/* ------------------------------------------------------------------------ */

/*
 * The grid and the panel under it are the *content* of this page: they are
 * destroyed when the page is hidden and rebuilt when it comes back
 * (ROADMAP v1.8.3 §1). The chrome around them - the header with the clock and
 * the two action buttons, and the month row with 上月 / 下月 - is built once and
 * stays.
 *
 * The grid is the reason this page is worth splitting: it holds 31 day cells
 * (a button, a label and a due dot each) plus the weekday header, which makes
 * it the biggest batch of objects on the page. ui_core only *hides* the page
 * container, so without this the cells would stay in the heap forever and
 * shred the internal heap into unusable fragments.
 *
 * The rebuild uses the state that was kept in RAM:
 *   - the month on screen (s_year / s_month) and the selected day
 *     (s_selected_day), so the user comes back to the same day of the same
 *     month;
 *   - the week row count (s_week_rows), so the grid comes back with exactly
 *     the same height;
 *   - the todo list, which todo_list() serves from the manager's RAM cache.
 * Nothing is read from the SD card and no request is made on the way back in.
 */

/**
 * Destroy the content block.
 *
 * Only the objects this file created are touched; the header and the month
 * row stay exactly where they are.
 */
static void content_destroy(void)
{
    if (s_grid != NULL) {
        /* The weekday header and the day cells are children of the grid. */
        lv_obj_delete(s_grid);
        s_grid = NULL;
    }
    /* The cell handles are gone with the grid. */
    memset(s_cells, 0, sizeof(s_cells));

    if (s_detail != NULL) {
        /* The title, the line list and the hint are children of the panel. */
        lv_obj_delete(s_detail);
        s_detail = NULL;
    }
    s_detail_title = NULL;
    s_detail_list = NULL;
    s_detail_hint = NULL;
}

/**
 * Build the content block and put it back in its original place.
 *
 * The page container is a flex column, so the order of its children *is* the
 * vertical order of the page: the grid sits above the detail panel.
 *
 * @return false when the objects could not be created (out of memory).
 */
static bool content_create(lv_obj_t *parent)
{
    if (parent == NULL) {
        return false;
    }
    if (s_grid != NULL) {
        /* Already built (first visit): nothing to do. */
        return true;
    }

    /* Weekday header + day cells share one grid, so the columns always line up. */
    s_grid = lv_obj_create(parent);
    if (s_grid == NULL) {
        return false;
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

    /* The same week row count as the last layout, so the day cells come back
     * at the same height (see s_week_rows). */
    static const int32_t cols[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                    LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                    LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    lv_obj_set_grid_dsc_array(s_grid, cols, cal_grid_rows(s_week_rows));

    for (int i = 0; i < CAL_COLS; i++) {
        lv_obj_t *head = lv_label_create(s_grid);
        if (head == NULL) {
            content_destroy();
            return false;
        }
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
        if (btn == NULL) {
            /* Out of memory: stop here instead of walking on with a half-built
             * cell. The empty cells stay hidden (render_grid() skips them). */
            content_destroy();
            return false;
        }
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
        if (label == NULL) {
            content_destroy();
            return false;
        }
        cell->label = label;
        /* UI_FONT_SMALL, not UI_FONT_BODY: the six week rows share the grid
         * height, and in the tightest layout (a day with three todos selected)
         * a row is only ~20 px high - the ~25 px line box of the 20 px font would
         * make the digits touch the due dot. 16 px fits. */
        lv_obj_set_style_text_font(label, ui_font(UI_FONT_SMALL), 0);
        lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);

        /* Due marker: a small dot in the lower part of the cell. */
        lv_obj_t *dot = lv_obj_create(btn);
        if (dot == NULL) {
            content_destroy();
            return false;
        }
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
     * Bottom of the page: the panel with the selected day's todos.
     *
     * It is always on screen (no more swapping with an action bar) and it is
     * sized by its content, with an upper bound, so it can never squeeze the
     * grid below the height the date numbers need - see the budget note at the
     * top of this file.
     */
    s_detail = lv_obj_create(parent);
    if (s_detail == NULL) {
        content_destroy();
        return false;
    }
    lv_obj_remove_style_all(s_detail);
    lv_obj_set_width(s_detail, LV_PCT(100));
    lv_obj_set_height(s_detail, LV_SIZE_CONTENT);
    /* Upper bound: a day with a lot of todos must not eat the grid. */
    lv_obj_set_style_max_height(s_detail, CAL_DETAIL_MAX_HEIGHT, 0);
    lv_obj_set_flex_flow(s_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(s_detail, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_detail, UI_CARD_RADIUS, 0);
    lv_obj_set_style_pad_hor(s_detail, 10, 0);
    lv_obj_set_style_pad_ver(s_detail, 3, 0);
    lv_obj_set_style_pad_row(s_detail, 2, 0);
    lv_obj_remove_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);

    s_detail_title = lv_label_create(s_detail);
    if (s_detail_title == NULL) {
        content_destroy();
        return false;
    }
    lv_obj_set_width(s_detail_title, LV_PCT(100));
    lv_label_set_long_mode(s_detail_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_detail_title, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_detail_title, UI_COLOR_PRIMARY, 0);
    lv_label_set_text(s_detail_title, "");
    lv_obj_add_flag(s_detail_title, LV_OBJ_FLAG_HIDDEN);

    /* The todo lines, one per row. */
    s_detail_list = lv_obj_create(s_detail);
    if (s_detail_list == NULL) {
        content_destroy();
        return false;
    }
    lv_obj_remove_style_all(s_detail_list);
    lv_obj_set_width(s_detail_list, LV_PCT(100));
    lv_obj_set_height(s_detail_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_detail_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_detail_list, 1, 0);
    lv_obj_remove_flag(s_detail_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detail_list, LV_OBJ_FLAG_HIDDEN);

    /* Shown while no day is selected. */
    s_detail_hint = lv_label_create(s_detail);
    if (s_detail_hint == NULL) {
        content_destroy();
        return false;
    }
    lv_obj_set_width(s_detail_hint, LV_PCT(100));
    lv_label_set_long_mode(s_detail_hint, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_detail_hint, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_detail_hint, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_detail_hint, "点日期看当天待办");

    /* Back into the original slots: the grid, then the panel under it. */
    lv_obj_move_to_index(s_grid, s_content_index);
    lv_obj_move_to_index(s_detail, s_content_index + 1);
    return true;
}

static esp_err_t calendar_create(lv_obj_t *parent)
{
    s_parent = parent;

    /*
     * Header: ONE row with three segments.
     *
     * left  = big time HH:MM (UI_FONT_HUGE) with the date underneath
     * middle = page name + weekday, and the number of todos due this month
     * right = the two action buttons (they used to live in a bar at the bottom)
     *
     * The clock used to be a 24 px high label holding two lines of 20 px text -
     * the second line was cropped away. It is a flex row now, so nothing is
     * cropped: each label takes the height it needs.
     */
    lv_obj_t *header = lv_obj_create(parent);
    if (header == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 12, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    /* The calendar is pushed onto the stack from the home page, so it needs a
     * way back. It fits into the header row without costing extra height. */
    (void)page_back_button_create(header);

    /* --- left segment: big time over a small date --- */
    lv_obj_t *clock_col = lv_obj_create(header);
    if (clock_col == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(clock_col);
    lv_obj_set_width(clock_col, LV_SIZE_CONTENT);
    lv_obj_set_height(clock_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(clock_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(clock_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(clock_col, LV_OBJ_FLAG_SCROLLABLE);

    s_clock = lv_label_create(clock_col);
    if (s_clock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_text_font(s_clock, ui_font(UI_FONT_HUGE), 0);
    lv_obj_set_style_text_color(s_clock, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_clock, CAL_TIME_UNSYNCED);

    /* --- middle segment: page name + weekday, and the month counter --- */
    lv_obj_t *mid_col = lv_obj_create(header);
    if (mid_col == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(mid_col);
    lv_obj_set_width(mid_col, LV_SIZE_CONTENT);
    lv_obj_set_height(mid_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(mid_col, 1);
    lv_obj_set_flex_flow(mid_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mid_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(mid_col, LV_OBJ_FLAG_SCROLLABLE);

    s_mid_title = lv_label_create(mid_col);
    if (s_mid_title == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_text_font(s_mid_title, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_mid_title, UI_COLOR_TEXT, 0);
    lv_label_set_text(s_mid_title, "日历");

    s_mid_sub = lv_label_create(mid_col);
    if (s_mid_sub == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_text_font(s_mid_sub, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_mid_sub, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_mid_sub, "未校时");

    /* --- right segment: the two action buttons --- */
    lv_obj_t *actions = lv_obj_create(header);
    if (actions == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(actions);
    lv_obj_set_width(actions, LV_SIZE_CONTENT);
    lv_obj_set_height(actions, UI_TOUCH_MIN);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(actions, 8, 0);
    lv_obj_remove_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    (void)header_button_create(actions, "回到今天", UI_COLOR_SURFACE_ALT, UI_COLOR_TEXT,
                               today_clicked_cb);
    (void)header_button_create(actions, "校时", UI_COLOR_PRIMARY, UI_COLOR_ON_PRIMARY,
                               refresh_clicked_cb);

    /* --- month row --- */
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
        /* 44 px high: the arrows are touch targets too and have to reach
         * UI_TOUCH_MIN (requirement 10.3). */
        lv_obj_set_size(btn, 64, UI_TOUCH_MIN);
        lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE_ALT, 0);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, nav[i].text);
        /* BODY font, matching the other buttons on the page: these used to be the
         * only buttons still on the default font. */
        lv_obj_set_style_text_font(label, ui_font(UI_FONT_BODY), 0);
        lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
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
    /*
     * The content block starts here (grid + selected-day panel). The slot is
     * remembered so a rebuild can put the objects back in the same place -
     * see content_create().
     */
    s_content_index = (int32_t)lv_obj_get_child_count(parent);
    if (!content_create(parent)) {
        return ESP_ERR_NO_MEM;
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
    /*
     * Restore the scroll offset the page had when it was left.
     * lv_obj_scroll_to_y() does lv_obj_update_layout() first, so the content
     * height is already known and the offset is not clamped away.
     */
    if (s_parent != NULL && s_scroll_y > 0 &&
        lv_obj_get_scroll_y(s_parent) != s_scroll_y) {
        lv_obj_scroll_to_y(s_parent, s_scroll_y, LV_ANIM_OFF);
    }

    /*
     * The grid and the detail panel were destroyed when the page was hidden
     * (ROADMAP v1.8.3 §1), so build them again from the state that was kept:
     * the month on screen, the selected day and the week row count. Nothing
     * is read from the SD card and no request is made here.
     */
    if (!content_create(s_parent)) {
        SYS_LOGE(TAG, "cannot rebuild the calendar grid");
        return ESP_ERR_NO_MEM;
    }

    refresh_clock();
    /* Todo marks may have changed while another page was on screen; the bindings
     * only cover changes that happen *while* this page is visible, and they are
     * delivered to the page even when it is hidden - re-rendering once on entry
     * keeps the grid honest either way. */
    render_grid();
    render_detail();
    return ESP_OK;
}

/**
 * Leaving the page: the grid and the detail panel go away with it.
 *
 * They are the biggest batch of objects in the app (31 day cells, each with a
 * button, a label and a due dot) and ui_core only *hides* the page container,
 * so keeping them would take the internal heap down for good. on_show() builds
 * them again from the state kept in RAM.
 */
static esp_err_t calendar_on_hide(void)
{
    /*
     * Remember the scroll offset before the content goes away, and stop the
     * inertia animation first: otherwise we would remember a mid-animation
     * value and the restore would fight with the animation.
     */
    if (s_parent != NULL) {
        lv_anim_delete(s_parent, NULL);
        s_scroll_y = lv_obj_get_scroll_y(s_parent);
    }

    content_destroy();
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
    .on_hide = calendar_on_hide,
    .on_tick = calendar_on_tick,
    .on_event = calendar_on_event,
    .tick_period_ms = 1000,
};
