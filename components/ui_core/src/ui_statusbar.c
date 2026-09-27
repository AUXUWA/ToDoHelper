/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_statusbar.c
 * @brief Always-present top bar: WiFi, clock, sync and storage state.
 *
 * The bar is created once by ui_core_init() and never rebuilt on page switch.
 * It only ever reads sys_status_get(), so it has no dependency on the L3
 * services that produce the data (docs/TODOHELPER_FRAMEWORKS 5.1 / FR1, FR4).
 *
 * Requirement 10.2: while the clock has never been synchronised the bar must
 * show UI_STATUS_TIME_UNSYNCED instead of 1970.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ui_core.h"
#include "ui_internal.h"
#include "ui_theme.h"

static lv_obj_t *s_wifi_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_sync_label;
static lv_obj_t *s_storage_label;

/**
 * Last text painted into each label, so ui_statusbar_refresh() can skip writes
 * that would not change anything.
 *
 * lv_label_set_text() invalidates the object even when the string is identical,
 * and this runs at 1 Hz forever, so without these the whole status bar was
 * repainted every second for nothing (smoothness audit P1).
 */
static char s_wifi_text[32];
static char s_storage_text[16];
static char s_sync_text[32];
/** Clock: whether the label shows a real time, plus the fields it last showed. */
static bool s_clock_synced;
static int s_last_sec = -1;
static int s_last_min = -1;
static int s_last_hour = -1;

static lv_obj_t *make_group(lv_obj_t *parent, lv_obj_t **out_label)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, 10, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(box);
    lv_obj_set_style_text_font(label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(label, "");

    if (out_label != NULL) {
        *out_label = label;
    }
    return box;
}

lv_obj_t *ui_statusbar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    if (bar == NULL) {
        return NULL;
    }

    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, UI_STATUSBAR_HEIGHT);
    lv_obj_set_style_bg_color(bar, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, UI_PAGE_PAD, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    /* Left cluster: connectivity + storage */
    lv_obj_t *left = lv_obj_create(bar);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 18, 0);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_CLICKABLE);

    (void)make_group(left, &s_wifi_label);
    (void)make_group(left, &s_storage_label);

    /* Centre: clock */
    (void)make_group(bar, &s_time_label);
    if (s_time_label != NULL) {
        lv_obj_set_style_text_font(s_time_label, ui_font(UI_FONT_BODY), 0);
        lv_obj_set_style_text_color(s_time_label, UI_COLOR_TEXT, 0);
    }

    /* Right cluster: sync state */
    (void)make_group(bar, &s_sync_label);

    lv_label_set_text(s_time_label, UI_STATUS_TIME_UNSYNCED);
    return bar;
}

esp_err_t ui_statusbar_refresh(void)
{
    if (s_time_label == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    sys_status_t st;
    if (sys_status_get(&st) != ESP_OK) {
        return ESP_FAIL;
    }

    /* Take the LVGL lock here instead of trusting the caller. This function is
     * reached from three places with different locking situations: the
     * once-a-second lv_timer (already inside the LVGL task; the adapter lock is
     * recursive so this is safe), an event marshalled into the LVGL task, and
     * app_main. The app_main path used to run without the lock, which corrupted
     * LVGL's object lists while the render task walked them and wedged the main
     * task hard enough to trip the task watchdog. */
    if (!ui_lock(-1)) {
        return ESP_ERR_TIMEOUT;
    }

    /*
     * Every branch below only writes when the *rendered* value changed.
     *
     * This runs at 1 Hz forever (ui_core's clock_tick_cb) and used to rewrite
     * all four labels plus four colours unconditionally. lv_label_set_text()
     * invalidates the label's area even when the text is byte-identical, so the
     * full-width status bar was being repainted once a second with nothing on it
     * changing - a permanent tax on every other animation (smoothness audit P1).
     */

    /* ---- WiFi ---- */
    if (s_wifi_label != NULL) {
        const int rssi = (int)st.wifi_rssi;
        char text[32];
        lv_color_t color;
        if (st.wifi_connected) {
            snprintf(text, sizeof(text), "WiFi %d dBm", rssi);
            color = UI_COLOR_SUCCESS;
        } else if (st.wifi_connecting) {
            strlcpy(text, "WiFi 连接中", sizeof(text));
            color = UI_COLOR_WARNING;
        } else {
            strlcpy(text, "WiFi 未连接", sizeof(text));
            color = UI_COLOR_TEXT_MUTED;
        }
        if (strcmp(text, s_wifi_text) != 0) {
            strlcpy(s_wifi_text, text, sizeof(s_wifi_text));
            lv_label_set_text(s_wifi_label, s_wifi_text);
            lv_obj_set_style_text_color(s_wifi_label, color, 0);
        }
    }

    /* ---- Storage ---- */
    if (s_storage_label != NULL) {
        const char *text;
        lv_color_t color = UI_COLOR_TEXT_MUTED;
        if (st.sd_available) {
            text = "SD";
        } else if (st.storage_available) {
            text = "内置存储";
        } else {
            text = "无存储";
            color = UI_COLOR_DANGER;
        }
        if (strcmp(text, s_storage_text) != 0) {
            strlcpy(s_storage_text, text, sizeof(s_storage_text));
            lv_label_set_text(s_storage_label, s_storage_text);
            lv_obj_set_style_text_color(s_storage_label, color, 0);
        }
    }

    /* ---- Clock ---- */
    if (s_time_label != NULL) {
        if (!sys_time_synced() || st.now_ts <= 0) {
            /* Never show 1970 (requirement FR4). */
            if (s_clock_synced) {
                s_clock_synced = false;
                lv_label_set_text(s_time_label, UI_STATUS_TIME_UNSYNCED);
                lv_obj_set_style_text_color(s_time_label, UI_COLOR_TEXT_MUTED, 0);
            }
        } else {
            const time_t now = (time_t)st.now_ts;
            struct tm tm_local;
            if (localtime_r(&now, &tm_local) != NULL) {
                /* Only the seconds field changes at this cadence. */
                if (!s_clock_synced || tm_local.tm_sec != s_last_sec ||
                    tm_local.tm_min != s_last_min || tm_local.tm_hour != s_last_hour) {
                    lv_label_set_text_fmt(s_time_label, "%02d:%02d:%02d",
                                          tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
                    s_last_sec = tm_local.tm_sec;
                    s_last_min = tm_local.tm_min;
                    s_last_hour = tm_local.tm_hour;
                    if (!s_clock_synced) {
                        s_clock_synced = true;
                        lv_obj_set_style_text_color(s_time_label, UI_COLOR_TEXT, 0);
                    }
                }
            } else if (s_clock_synced) {
                s_clock_synced = false;
                lv_label_set_text(s_time_label, UI_STATUS_TIME_UNSYNCED);
                lv_obj_set_style_text_color(s_time_label, UI_COLOR_TEXT_MUTED, 0);
            }
        }
    }

    /* ---- Sync ---- */
    if (s_sync_label != NULL) {
        char text[32];
        lv_color_t color;
        if (st.syncing) {
            strlcpy(text, "同步中", sizeof(text));
            color = UI_COLOR_PRIMARY;
        } else if (st.todo_pending > 0) {
            snprintf(text, sizeof(text), "待同步 %ld", (long)st.todo_pending);
            color = UI_COLOR_WARNING;
        } else if (st.todo_total > 0) {
            strlcpy(text, "已同步", sizeof(text));
            color = UI_COLOR_SUCCESS;
        } else {
            text[0] = '\0';
            color = UI_COLOR_TEXT_MUTED;
        }
        if (strcmp(text, s_sync_text) != 0) {
            strlcpy(s_sync_text, text, sizeof(s_sync_text));
            lv_label_set_text(s_sync_label, s_sync_text);
            lv_obj_set_style_text_color(s_sync_label, color, 0);
        }
    }

    ui_unlock();
    return ESP_OK;
}
