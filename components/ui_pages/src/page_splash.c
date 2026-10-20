/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_splash.c
 * @brief Boot banner: the product name plus the live boot log.
 *
 * The panel doubles as a serial monitor while the device starts: sys_utils keeps
 * a ring of the most recent log lines (sys_log_recent_get()) and this page renders
 * them in a list that tails the newest one, so the boot log can be read without
 * attaching a serial console. The console output itself is untouched.
 *
 * Timing belongs to main/app_main.c: it shows this page before the optional
 * services come up and hands over to the home page once they are done. This page
 * therefore has exactly one job - keep the view up to date - and stops its timer
 * in on_hide() so nothing runs while another page is visible.
 *
 * Threading. create()/on_show()/on_hide() all run under the LVGL lock taken by
 * ui_page_switch(), and splash_tick() runs inside the LVGL task, so every static
 * below is only ever touched with the adapter lock held and needs no atomics
 * (AGENT.md 9.0.1).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"

#include "pages_internal.h"
#include "sys_utils.h"

static const char *TAG = "page_splash";

/** How often the log view is rebuilt while new lines arrive.
 *
 * 250 ms (4 Hz) since v1.7.0 (ROADMAP A3). It used to be 100 ms (10 Hz): the
 * boot takes ~6.4 s, so the old timer ran 64 times, and every pass that found
 * a new line called lv_label_set_text_static() on a label holding up to 5 KB of
 * wrapped text - that invalidates the label, so the next layout pass has to
 * re-measure the whole string. The splash is the *only* visible page while the
 * modules come up, so the UI task's time went entirely into that.
 * 4 Hz still looks live: the boot log only grows by a few lines per 250 ms.
 */
#define SPLASH_TICK_MS 250

/** Animated ellipsis: one more dot every this many ms while the boot runs. */
#define SPLASH_DOTS_MS 420

/** Hidden pixels below the viewport that still count as "at the newest line". */
#define SPLASH_TAIL_SLACK 4

/** Render buffer: one row per retained line ("L " + text + newline + NUL). */
#define SPLASH_TEXT_LEN (SYS_LOG_RECENT_CAPACITY * (SYS_LOG_RECENT_LINE_LEN + 3) + 1)

/** Status line buffer: "启动日志 N 条 · 最新 S.Ts · 启动中..." - 128 bytes is
 *  enough for the longest possible line (seq is at most 10 digits, the uptime
 *  at most "4294967.2s"); the text is built in place so a refresh allocates
 *  nothing and the label does not have to copy it. */
#define SPLASH_STATUS_LEN 128

static lv_obj_t *s_status_label;
static lv_obj_t *s_log_box;
static lv_obj_t *s_log_label;
/**
 * Render buffer, PSRAM when possible: it is more than 5 KB and internal RAM is
 * the scarcest resource on this board (AGENT.md 9.0). The label borrows it
 * through lv_label_set_text_static(), so a refresh allocates nothing.
 */
static char *s_text;

/**
 * Status line text, kept in a static buffer so the label can borrow it.
 *
 * It holds the *last rendered* status string: the status line is refreshed on
 * the same 250 ms throttle as the log, and lv_label_set_text_static() always
 * invalidates the label even when the text did not change (AGENT.md 9.0.1.1
 * iron rule 2). So the new text is compared against this buffer first and the
 * label is only touched when it really differs.
 */
static char s_status_text[SPLASH_STATUS_LEN];

/** Refresh timer; NULL while the page is hidden or when no timer was available. */
static lv_timer_t *s_timer;
/** sys_log_recent_seq() the label currently shows. */
static uint32_t s_rendered_seq;
/** Bytes of s_text currently in use (so a new line can be appended, not rebuilt). */
static size_t s_text_used;
/** A tail scroll is owed once the label finishes re-measuring. */
static bool s_tail_pending;

/* Status line state: the ellipsis animates even when no new line arrives, so the
 * page never looks frozen while the (slow) modules come up. */
static uint32_t s_started_ms;      /**< sys_time_ms() when the page was shown */
static uint32_t s_dots_phase;      /**< last rendered dot count, 0..3 */
static bool s_have_newest;         /**< s_newest holds a complete log line */
static sys_log_recent_info_t s_newest;

/* ------------------------------------------------------------------------- */
/* Log rendering                                                             */
/* ------------------------------------------------------------------------- */

static char level_letter(sys_log_level_t level)
{
    switch (level) {
    case SYS_LOG_ERROR:
        return 'E';
    case SYS_LOG_WARN:
        return 'W';
    case SYS_LOG_DEBUG:
        return 'D';
    case SYS_LOG_INFO:
    default:
        return 'I';
    }
}

/** Bounded append of @p src; returns the new offset and keeps @p dst terminated. */
static size_t text_put(char *dst, size_t cap, size_t used, const char *src)
{
    while (*src != '\0' && (used + 1u) < cap) {
        dst[used++] = *src++;
    }
    dst[used] = '\0';
    return used;
}

/** Bounded append of one character; returns the new offset. */
static size_t text_put_char(char *dst, size_t cap, size_t used, char c)
{
    if ((used + 1u) < cap) {
        dst[used++] = c;
        dst[used] = '\0';
    }
    return used;
}

/** Animated ellipsis, so "启动中" never looks like a hung screen. */
static const char *dots_for(uint32_t phase)
{
    static const char *const s_dots[] = { "", ".", "..", "..." };
    return s_dots[phase & 3u];
}

static void splash_update_status(const sys_log_recent_info_t *newest)
{
    if (s_status_label == NULL) {
        return;
    }

    const char *dots = dots_for(s_dots_phase);
    char next[SPLASH_STATUS_LEN];

    if (newest == NULL) {
        (void)snprintf(next, sizeof(next), "正在等待第一条日志%s", dots);
    } else {
        /* The page is up while main/app_main.c brings the optional services up, so it
         * says so instead of just sitting there with a frozen counter. */
        (void)snprintf(next, sizeof(next), "启动日志 %u 条 · 最新 %u.%us · 启动中%s",
                       (unsigned)newest->seq,
                       (unsigned)(newest->uptime_ms / 1000u),
                       (unsigned)((newest->uptime_ms % 1000u) / 100u), dots);
    }

    /* Compare before writing: lv_label_set_text_static() invalidates the label
     * unconditionally, and the status line is refreshed on every tick - writing
     * an identical string would re-measure the label for nothing. */
    if (strcmp(next, s_status_text) == 0) {
        return;
    }
    memcpy(s_status_text, next, strlen(next) + 1u);
    lv_label_set_text_static(s_status_label, s_status_text);
}

/**
 * Append whatever arrived since the last pass, or rebuild when we cannot.
 *
 * Runs with the LVGL lock held (on_show() or the lv_timer), which is what makes
 * rewriting @c s_text in place safe: LVGL only reads it while drawing, and this
 * same task does the drawing.
 *
 * Almost every pass is a pure *append*: sys_log_recent_seq() is monotonic, lines
 * are retained in order, and the ring only ever drops the oldest once it is full.
 * Rebuilding all 64 lines (a >5 KB buffer, re-wrapped by a
 * LV_LABEL_LONG_MODE_WRAP label) on every tick was the single most
 * expensive thing on the boot screen - it also forced lv_obj_update_layout() to
 * run synchronously inside the timer, stealing time from frame composition
 * (smoothness audit P2). The append path is kept: only the *rate* was lowered
 * (100 ms -> 250 ms, ROADMAP v1.7.0 A3), so a tick that finds new lines still
 * only writes the new ones.
 *
 * A full rebuild is still needed in two cases, and both are handled below:
 *   - the first ever pass (nothing rendered yet);
 *   - the ring overflowed and recycled slots, so the oldest retained line is no
 *     longer the oldest line we have rendered.
 */
static void splash_refresh_log(void)
{
    if (s_log_label == NULL || s_log_box == NULL || s_text == NULL) {
        return;
    }

    const uint32_t seq = sys_log_recent_seq();
    if (seq == s_rendered_seq) {
        return;
    }

    /* Remember whether the view was tailing before the text grows: afterwards the
     * newest line is below the fold again and would always look "not at the end". */
    const bool was_at_end = lv_obj_get_scroll_bottom(s_log_box) <= SPLASH_TAIL_SLACK;
    const size_t count = sys_log_recent_count();

    /*
     * How many lines are new? seq counts lines ever emitted; we have rendered up
     * to s_rendered_seq. If everything since then is still retained (the gap is
     * no larger than the ring), an append keeps the buffer valid. Otherwise
     * start over.
     */
    const uint32_t new_lines = seq - s_rendered_seq;
    const bool can_append =
        (s_rendered_seq != 0u) && (new_lines <= count) && (s_text_used != 0u);

    size_t used = can_append ? s_text_used : 0u;
    size_t lines = 0u;
    bool complete = true;
    sys_log_recent_info_t newest = { 0 };
    bool have_newest = false;

    if (can_append) {
        /* Rollback point: if this batch cannot be finished, everything appended
         * below has to go away again (see the comment at the break). */
        const size_t used_before = used;

        /* Only the newest `new_lines` entries, oldest first. */
        for (size_t age = (size_t)new_lines; age > 0u; age--) {
            char line[SYS_LOG_RECENT_LINE_LEN];
            sys_log_recent_info_t info;

            if (!sys_log_recent_get(age - 1u, line, sizeof(line), &info)) {
                /* Not published yet (microseconds): stop here and catch it next
                 * tick rather than leaving a hole in the middle of the buffer. */
                complete = false;
                break;
            }

            used = text_put_char(s_text, SPLASH_TEXT_LEN, used, level_letter(info.level));
            used = text_put_char(s_text, SPLASH_TEXT_LEN, used, ' ');
            used = text_put(s_text, SPLASH_TEXT_LEN, used, line);
            used = text_put_char(s_text, SPLASH_TEXT_LEN, used, '\n');
            lines++;

            if (age == 1u) {
                newest = info;
                have_newest = true;
            }
        }

        if (!complete) {
            /*
             * 回滚这一批。
             *
             * sys_log 的写入方是「先占序号、后发布内容」（sys_log.c 的
             * recent_capture()），读者完全可能读到一条**已被认领但尚未发布**
             * 的行 —— 这时 sys_log_recent_get() 返回 false。
             *
             * 如果把已经追加进去的那几行留在缓冲里，而 s_rendered_seq 又因为
             * complete == false 没有推进，下一拍算出的 new_lines 还是同一批，
             * 于是这几行会被**再追加一遍**：屏幕上出现重复的启动日志，
             * 而且要等环形缓冲溢出触发一次全量重建才会消失。
             *
             * 所以这里把这一批整个撤掉（文本截回追加之前，已用长度还原），
             * 让下一拍从同一起点重来：那时候那一行早发布了，整批能一次追加完。
             */
            s_text[used_before] = '\0';
            s_text_used = used_before;
            return;   /* 不写标签、不改 s_rendered_seq */
        }
    } else {
        /* Full rebuild: oldest retained line first, newest last - the order a
         * serial monitor prints them in, and it puts the newest line at the
         * bottom of the view. */
        for (size_t age = count; age > 0u; age--) {
            char line[SYS_LOG_RECENT_LINE_LEN];
            sys_log_recent_info_t info;

            if (!sys_log_recent_get(age - 1u, line, sizeof(line), &info)) {
                /* Claimed by a writer that has not published yet (microseconds), or
                 * the slot was reused: show what we have and rebuild on the next
                 * tick. */
                complete = false;
                continue;
            }

            used = text_put_char(s_text, SPLASH_TEXT_LEN, used, level_letter(info.level));
            used = text_put_char(s_text, SPLASH_TEXT_LEN, used, ' ');
            used = text_put(s_text, SPLASH_TEXT_LEN, used, line);
            used = text_put_char(s_text, SPLASH_TEXT_LEN, used, '\n');
            lines++;

            if (age == 1u) {
                newest = info;
                have_newest = true;
            }
        }
    }

    if (!can_append && lines == 0u) {
        return; /* nothing published yet: keep the placeholder and retry */
    }
    if (can_append && lines == 0u) {
        /* Nothing could be appended this pass; try again next tick. */
        return;
    }

    s_text_used = used;
    lv_label_set_text_static(s_log_label, s_text);

    /*
     * Scroll to the newest line. The label defers its re-measure to the next
     * layout pass, so the scroll range is only correct afterwards - but that pass
     * is triggered by the text change itself and runs as part of the normal
     * update. Calling lv_obj_update_layout() here forced it *synchronously inside
     * the timer*, which is what made this page cost so much; instead the tail
     * scroll is re-applied from the layout-completed event below.
     */
    s_tail_pending = was_at_end;
    if (was_at_end) {
        lv_obj_scroll_to_y(s_log_box, LV_COORD_MAX, LV_ANIM_OFF);
    }

    /* Only a complete pass may claim this sequence number, so a line missed above
     * is picked up by the next tick. */
    if (complete) {
        s_rendered_seq = seq;
    }

    if (have_newest) {
        s_newest = newest;
        s_have_newest = true;
    }

    /* Keep the newest line around for the ellipsis ticks, which run when no new
     * line arrives and therefore have nothing else to render. */
    splash_update_status(s_have_newest ? &s_newest : NULL);
}

/**
 * Re-apply the tail scroll once the label has actually re-measured.
 *
 * This replaces the old synchronous lv_obj_update_layout() call: LVGL raises this
 * event after the deferred layout completes, which is exactly when the scroll
 * range becomes valid.
 */
static void splash_layout_done_cb(lv_event_t *e)
{
    (void)e;
    if (!s_tail_pending || s_log_box == NULL) {
        return;
    }
    s_tail_pending = false;
    lv_obj_scroll_to_y(s_log_box, LV_COORD_MAX, LV_ANIM_OFF);
}

static void splash_tick(lv_timer_t *timer)
{
    (void)timer;

    /* The ellipsis runs on its own clock: between two log lines nothing else on
     * this page changes, and a frozen status line reads as a hung device.
     * The phase is derived from the wall clock, not from the tick count, so the
     * dot still advances roughly every SPLASH_DOTS_MS (420 ms). Because the tick
     * (250 ms) is shorter than the dot period, the phase can only advance by one
     * per tick - no dot is ever skipped. */
    const uint32_t elapsed = (uint32_t)sys_time_ms() - s_started_ms;
    const uint32_t phase = elapsed / SPLASH_DOTS_MS;
    if (phase != s_dots_phase) {
        s_dots_phase = phase;
        splash_update_status(s_have_newest ? &s_newest : NULL);
    }

    splash_refresh_log();
}

/** Start refreshing. Called from on_show(), with the LVGL lock held. */
static void splash_timer_start(void)
{
    if (s_timer != NULL || s_text == NULL) {
        return;
    }

    s_timer = lv_timer_create(splash_tick, SPLASH_TICK_MS, NULL);
    if (s_timer == NULL) {
        SYS_LOGW(TAG, "no timer available, the boot log will not update");
    }
}

/** Stop refreshing. Called from on_hide(), with the LVGL lock held. */
static void splash_timer_stop(void)
{
    if (s_timer == NULL) {
        return;
    }

    /* Deleting an LVGL timer from outside the LVGL task is safe as long as the
     * adapter lock is held (th_bsp_idle_stop() does the same). */
    lv_timer_delete(s_timer);
    s_timer = NULL;
}

/* ------------------------------------------------------------------------- */
/* Page                                                                      */
/* ------------------------------------------------------------------------- */

static esp_err_t splash_create(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    if (box == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "ToDoHelper");
    lv_obj_set_style_text_font(title, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);

    lv_obj_t *subtitle = lv_label_create(box);
    /* The real firmware version: `CONFIG_APP_PROJECT_VER` only exists when
     * CONFIG_APP_PROJECT_VER_FROM_CONFIG is set, which it is not, so the old code
     * fell back to a hardcoded "0.1.0" and the splash screen lied about the build.
     * version.txt -> esp_app_desc_t is the single source of truth (AGENT.md 8.1). */
    const esp_app_desc_t *app = esp_app_get_description();
    lv_label_set_text_fmt(subtitle, "桌面待办助手 · 启动日志 · 固件 %s",
                          (app != NULL) ? app->version : "?");
    lv_obj_set_style_text_font(subtitle, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(subtitle, UI_COLOR_PRIMARY, 0);

    /* The log takes whatever height is left; the small font keeps ~14 lines
     * visible at once on the 800x480 panel. */
    s_log_box = lv_obj_create(box);
    if (s_log_box == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_log_box);
    lv_obj_set_width(s_log_box, LV_PCT(100));
    lv_obj_set_flex_grow(s_log_box, 1);
    lv_obj_set_style_bg_color(s_log_box, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_log_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_log_box, UI_CARD_RADIUS, 0);
    lv_obj_set_style_pad_all(s_log_box, UI_CARD_PAD, 0);
    lv_obj_set_style_border_width(s_log_box, 0, 0);
    lv_obj_set_scroll_dir(s_log_box, LV_DIR_VER);
    lv_obj_add_flag(s_log_box, LV_OBJ_FLAG_SCROLLABLE);

    s_log_label = lv_label_create(s_log_box);
    if (s_log_label == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_long_mode(s_log_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_log_label, LV_PCT(100));
    lv_obj_set_style_text_font(s_log_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_log_label, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_log_label, "正在读取启动日志…");
    /* The label re-measures its text during a deferred layout pass; this is where
     * the tail scroll is applied instead of forcing the layout synchronously. */
    lv_obj_add_event_cb(s_log_label, splash_layout_done_cb, LV_EVENT_SIZE_CHANGED, NULL);

    s_status_label = lv_label_create(box);
    if (s_status_label == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_text(s_status_label, "正在初始化…");
    lv_obj_set_style_text_font(s_status_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_status_label, UI_COLOR_TEXT_MUTED, 0);

    s_text = heap_caps_malloc(SPLASH_TEXT_LEN, MALLOC_CAP_SPIRAM);
    if (s_text == NULL) {
        s_text = heap_caps_malloc(SPLASH_TEXT_LEN, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_text == NULL) {
        /* The history itself is still captured; only this view is missing. */
        SYS_LOGW(TAG, "%u bytes for the log view unavailable, screen log disabled",
                 (unsigned)SPLASH_TEXT_LEN);
        lv_label_set_text(s_log_label, "内存不足，无法显示启动日志");
    } else {
        s_text[0] = '\0';
    }

    SYS_LOGD(TAG, "splash built (%u lines of history)", (unsigned)SYS_LOG_RECENT_CAPACITY);
    return ESP_OK;
}

static esp_err_t splash_on_show(void)
{
    /* Restart the ellipsis clock: this page is also re-shown after a return from
     * another page, and the dots must not pick up an old phase. */
    s_started_ms = (uint32_t)sys_time_ms();
    s_dots_phase = 0;
    splash_refresh_log();
    splash_update_status(s_have_newest ? &s_newest : NULL);
    splash_timer_start();
    return ESP_OK;
}

static esp_err_t splash_on_hide(void)
{
    /* Do not keep reading the log ring while another page is visible. */
    splash_timer_stop();
    return ESP_OK;
}

const ui_page_t page_splash = {
    .id = UI_PAGE_SPLASH,
    .name = "splash",
    .create = splash_create,
    .on_show = splash_on_show,
    .on_hide = splash_on_hide,
};
