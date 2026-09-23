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
#include <time.h>

#include "ui_core.h"
#include "ui_internal.h"
#include "ui_theme.h"

static lv_obj_t *s_wifi_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_sync_label;
static lv_obj_t *s_storage_label;

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

    /* ---- WiFi ---- */
    if (s_wifi_label != NULL) {
        if (st.wifi_connected) {
            lv_label_set_text_fmt(s_wifi_label, "WiFi %d dBm", (int)st.wifi_rssi);
            lv_obj_set_style_text_color(s_wifi_label, UI_COLOR_SUCCESS, 0);
        } else if (st.wifi_connecting) {
            lv_label_set_text(s_wifi_label, "WiFi 连接中");
            lv_obj_set_style_text_color(s_wifi_label, UI_COLOR_WARNING, 0);
        } else {
            lv_label_set_text(s_wifi_label, "WiFi 未连接");
            lv_obj_set_style_text_color(s_wifi_label, UI_COLOR_TEXT_MUTED, 0);
        }
    }

    /* ---- Storage ---- */
    if (s_storage_label != NULL) {
        if (st.sd_available) {
            lv_label_set_text(s_storage_label, "SD");
            lv_obj_set_style_text_color(s_storage_label, UI_COLOR_TEXT_MUTED, 0);
        } else if (st.storage_available) {
            lv_label_set_text(s_storage_label, "内置存储");
            lv_obj_set_style_text_color(s_storage_label, UI_COLOR_TEXT_MUTED, 0);
        } else {
            lv_label_set_text(s_storage_label, "无存储");
            lv_obj_set_style_text_color(s_storage_label, UI_COLOR_DANGER, 0);
        }
    }

    /* ---- Clock ---- */
    if (s_time_label != NULL) {
        if (!sys_time_synced() || st.now_ts <= 0) {
            /* Never show 1970 (requirement FR4). */
            lv_label_set_text(s_time_label, UI_STATUS_TIME_UNSYNCED);
            lv_obj_set_style_text_color(s_time_label, UI_COLOR_TEXT_MUTED, 0);
        } else {
            time_t now = (time_t)st.now_ts;
            struct tm tm_local;
            if (localtime_r(&now, &tm_local) != NULL) {
                lv_label_set_text_fmt(s_time_label, "%02d:%02d:%02d",
                                      tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
            } else {
                lv_label_set_text(s_time_label, UI_STATUS_TIME_UNSYNCED);
            }
            lv_obj_set_style_text_color(s_time_label, UI_COLOR_TEXT, 0);
        }
    }

    /* ---- Sync ---- */
    if (s_sync_label != NULL) {
        if (st.syncing) {
            lv_label_set_text(s_sync_label, "同步中");
            lv_obj_set_style_text_color(s_sync_label, UI_COLOR_PRIMARY, 0);
        } else if (st.todo_pending > 0) {
            lv_label_set_text_fmt(s_sync_label, "待同步 %ld", (long)st.todo_pending);
            lv_obj_set_style_text_color(s_sync_label, UI_COLOR_WARNING, 0);
        } else if (st.todo_total > 0) {
            lv_label_set_text(s_sync_label, "已同步");
            lv_obj_set_style_text_color(s_sync_label, UI_COLOR_SUCCESS, 0);
        } else {
            lv_label_set_text(s_sync_label, "");
        }
    }

    ui_unlock();
    return ESP_OK;
}
