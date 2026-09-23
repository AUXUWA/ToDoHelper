/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_wifi.c
 * @brief FR1: scan, password entry and connection status in one page.
 *
 * Requirement 10.1 keeps all three WiFi screens inside UI_PAGE_WIFI, switched
 * as sub-views, to avoid growing ui_page_id_t for every dialog.
 *
 * Scanning is always started with wifi_manager_start_scan(): the blocking
 * variant would stall the LVGL task for up to 15 s and freeze the UI.
 */

#include "pages_internal.h"

#include <string.h>

#include "wifi_manager.h"

static const char *TAG = "page_wifi";

typedef enum {
    WIFI_VIEW_SCAN = 0,
    WIFI_VIEW_PASSWORD,
    WIFI_VIEW_STATUS,
} wifi_view_t;

static lv_obj_t *s_view[3];
static lv_obj_t *s_scan_status;
static lv_obj_t *s_scan_list;
static lv_obj_t *s_pass_ssid;
static lv_obj_t *s_pass_area;
static lv_obj_t *s_status_detail;
static char s_pending_ssid[WIFI_MANAGER_SSID_MAX + 1];
static wifi_view_t s_view_current = WIFI_VIEW_SCAN;

/* ------------------------------------------------------------------------ */
/* View switching                                                            */
/* ------------------------------------------------------------------------ */

static void show_view(wifi_view_t view)
{
    for (int i = 0; i < 3; i++) {
        if (s_view[i] == NULL) {
            continue;
        }
        if (i == (int)view) {
            lv_obj_remove_flag(s_view[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_view[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_view_current = view;
}

/* ------------------------------------------------------------------------ */
/* Scan view                                                                 */
/* ------------------------------------------------------------------------ */

static void ap_clicked_cb(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (ssid == NULL) {
        return;
    }

    strlcpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
    if (s_pass_ssid != NULL) {
        lv_label_set_text_fmt(s_pass_ssid, "连接到 %s", s_pending_ssid);
    }
    if (s_pass_area != NULL) {
        lv_textarea_set_text(s_pass_area, "");
    }
    show_view(WIFI_VIEW_PASSWORD);
}

static void scan_clicked_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = wifi_manager_start_scan();
    if (err == ESP_OK) {
        if (s_scan_status != NULL) {
            lv_label_set_text(s_scan_status, "正在扫描…");
        }
    } else {
        ui_toast("扫描失败：%s", esp_err_to_name(err));
    }
}

static void render_scan_list(void)
{
    if (s_scan_list == NULL) {
        return;
    }
    lv_obj_clean(s_scan_list);

    wifi_ap_info_t aps[WIFI_MANAGER_MAX_APS];
    size_t count = 0;
    if (wifi_manager_get_scan_result(aps, WIFI_MANAGER_MAX_APS, &count) != ESP_OK || count == 0) {
        lv_obj_t *empty = lv_label_create(s_scan_list);
        lv_label_set_text(empty, "还没有扫描结果，点“重新扫描”。");
        lv_obj_set_style_text_font(empty, ui_font(UI_FONT_BODY), 0);
        lv_obj_set_style_text_color(empty, UI_COLOR_TEXT_MUTED, 0);
        return;
    }

    const size_t shown = (count > WIFI_MANAGER_MAX_APS) ? WIFI_MANAGER_MAX_APS : count;
    for (size_t i = 0; i < shown; i++) {
        /* AP records are only valid until the next scan, so copy the SSID into
         * a per-row buffer that lives as long as the widget. */
        static char ssid_bufs[WIFI_MANAGER_MAX_APS][WIFI_MANAGER_SSID_MAX + 1];
        strlcpy(ssid_bufs[i], aps[i].ssid[0] ? aps[i].ssid : "(隐藏网络)", sizeof(ssid_bufs[i]));

        lv_obj_t *row = lv_button_create(s_scan_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, UI_LIST_ROW_HEIGHT);
        lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, 0);
        lv_obj_set_style_radius(row, UI_CARD_RADIUS, 0);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, ssid_bufs[i]);
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(name, ui_font(UI_FONT_BODY), 0);
        lv_obj_set_style_text_color(name, UI_COLOR_TEXT, 0);

        lv_obj_t *meta = lv_label_create(row);
        lv_label_set_text_fmt(meta, "%d dBm · ch%u · %s", (int)aps[i].rssi, aps[i].channel,
                              aps[i].authmode == WIFI_AUTH_OPEN ? "开放" : "加密");
        lv_obj_set_style_text_font(meta, ui_font(UI_FONT_SMALL), 0);
        lv_obj_set_style_text_color(meta, UI_COLOR_TEXT_MUTED, 0);

        lv_obj_add_event_cb(row, ap_clicked_cb, LV_EVENT_CLICKED, ssid_bufs[i]);
    }
}

static void open_status_cb(lv_event_t *e)
{
    (void)e;
    show_view(WIFI_VIEW_STATUS);
}

static esp_err_t wifi_create_scan_view(lv_obj_t *parent)
{
    s_view[WIFI_VIEW_SCAN] = lv_obj_create(parent);
    if (s_view[WIFI_VIEW_SCAN] == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_view[WIFI_VIEW_SCAN]);
    lv_obj_set_size(s_view[WIFI_VIEW_SCAN], LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_view[WIFI_VIEW_SCAN], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_view[WIFI_VIEW_SCAN], 8, 0);
    lv_obj_remove_flag(s_view[WIFI_VIEW_SCAN], LV_OBJ_FLAG_SCROLLABLE);

    s_scan_status = lv_label_create(s_view[WIFI_VIEW_SCAN]);
    lv_obj_set_style_text_font(s_scan_status, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_scan_status, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_scan_status, "选择要连接的网络");

    s_scan_list = lv_obj_create(s_view[WIFI_VIEW_SCAN]);
    lv_obj_remove_style_all(s_scan_list);
    lv_obj_set_width(s_scan_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_scan_list, 1);
    lv_obj_set_flex_flow(s_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_scan_list, 6, 0);
    lv_obj_set_style_pad_right(s_scan_list, 6, 0);
    lv_obj_add_flag(s_scan_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scan_list, LV_DIR_VER);

    lv_obj_t *bar = lv_obj_create(s_view[WIFI_VIEW_SCAN]);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *scan = lv_button_create(bar);
    lv_obj_set_height(scan, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(scan, 1);
    lv_obj_t *scan_label = lv_label_create(scan);
    lv_label_set_text(scan_label, "重新扫描");
    lv_obj_center(scan_label);
    lv_obj_add_event_cb(scan, scan_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *status = lv_button_create(bar);
    lv_obj_set_height(status, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(status, 1);
    lv_obj_set_style_bg_color(status, UI_COLOR_SURFACE_ALT, 0);
    lv_obj_t *status_label = lv_label_create(status);
    lv_label_set_text(status_label, "连接状态");
    lv_obj_center(status_label);
    lv_obj_add_event_cb(status, open_status_cb, LV_EVENT_CLICKED, NULL);

    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Password view                                                             */
/* ------------------------------------------------------------------------ */

static void connect_clicked_cb(lv_event_t *e)
{
    (void)e;
    const char *password = (s_pass_area != NULL) ? lv_textarea_get_text(s_pass_area) : "";

    esp_err_t err = wifi_manager_connect(s_pending_ssid, password);
    if (err == ESP_OK) {
        ui_toast("正在连接 %s", s_pending_ssid);
        show_view(WIFI_VIEW_STATUS);
    } else {
        ui_toast("连接启动失败：%s", esp_err_to_name(err));
    }
}

static void pass_back_cb(lv_event_t *e)
{
    (void)e;
    show_view(WIFI_VIEW_SCAN);
}

static esp_err_t wifi_create_password_view(lv_obj_t *parent)
{
    s_view[WIFI_VIEW_PASSWORD] = lv_obj_create(parent);
    if (s_view[WIFI_VIEW_PASSWORD] == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_view[WIFI_VIEW_PASSWORD]);
    lv_obj_set_size(s_view[WIFI_VIEW_PASSWORD], LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_view[WIFI_VIEW_PASSWORD], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_view[WIFI_VIEW_PASSWORD], 10, 0);
    lv_obj_remove_flag(s_view[WIFI_VIEW_PASSWORD], LV_OBJ_FLAG_SCROLLABLE);

    s_pass_ssid = lv_label_create(s_view[WIFI_VIEW_PASSWORD]);
    lv_obj_set_style_text_font(s_pass_ssid, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(s_pass_ssid, UI_COLOR_TEXT, 0);
    lv_label_set_text(s_pass_ssid, "连接到");

    s_pass_area = lv_textarea_create(s_view[WIFI_VIEW_PASSWORD]);
    lv_textarea_set_one_line(s_pass_area, true);
    lv_textarea_set_password_mode(s_pass_area, true);
    lv_textarea_set_max_length(s_pass_area, WIFI_MANAGER_PASS_MAX);
    lv_textarea_set_placeholder_text(s_pass_area, "密码（开放网络留空）");
    lv_obj_set_width(s_pass_area, LV_PCT(100));
    lv_obj_set_style_text_font(s_pass_area, ui_font(UI_FONT_BODY), 0);

    lv_obj_t *bar = lv_obj_create(s_view[WIFI_VIEW_PASSWORD]);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ok = lv_button_create(bar);
    lv_obj_set_height(ok, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(ok, 1);
    lv_obj_t *ok_label = lv_label_create(ok);
    lv_label_set_text(ok_label, "连接");
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok, connect_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_button_create(bar);
    lv_obj_set_height(back, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(back, 1);
    lv_obj_set_style_bg_color(back, UI_COLOR_SURFACE_ALT, 0);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, "返回列表");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back, pass_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *keyboard = lv_keyboard_create(s_view[WIFI_VIEW_PASSWORD]);
    lv_obj_set_width(keyboard, LV_PCT(100));
    /* Six rows have to fit; 200 px squeezed them to ~33 px per row, which is below
     * a comfortable touch target. */
    lv_obj_set_height(keyboard, 240);
    lv_keyboard_set_textarea(keyboard, s_pass_area);
    lv_obj_set_style_text_font(keyboard, ui_font(UI_FONT_SMALL), 0);

    /* LVGL's default keyboard is light grey and clashes with the dark theme.
     * Symbols (LV_SYMBOL_BACKSPACE/OK/...) come from the font fallback chain set up
     * in ui_fonts.c - they are not in the embedded CJK subset. */
    lv_obj_set_style_bg_color(keyboard, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(keyboard, 0, 0);
    lv_obj_set_style_pad_all(keyboard, 4, 0);
    lv_obj_set_style_pad_gap(keyboard, 4, 0);

    /* Keys. */
    lv_obj_set_style_bg_color(keyboard, UI_COLOR_SURFACE_ALT, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, UI_COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(keyboard, 6, LV_PART_ITEMS);
    /* Pressed feedback in the accent colour, matching the rest of the UI. */
    lv_obj_set_style_bg_color(keyboard, UI_COLOR_PRIMARY, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keyboard, lv_color_white(), LV_PART_ITEMS | LV_STATE_PRESSED);
    /* Checked keys are the sticky ones (shift / layout switch). */
    lv_obj_set_style_bg_color(keyboard, UI_COLOR_PRIMARY, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(keyboard, lv_color_white(), LV_PART_ITEMS | LV_STATE_CHECKED);
    /* The text area the password is typed into. */
    lv_obj_set_style_bg_color(keyboard, UI_COLOR_BG, LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(keyboard, UI_COLOR_TEXT, LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS | LV_STATE_FOCUSED);

    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Status view                                                               */
/* ------------------------------------------------------------------------ */

static void disconnect_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (wifi_manager_disconnect() == ESP_OK) {
        ui_toast("已断开");
    }
}

static void status_back_cb(lv_event_t *e)
{
    (void)e;
    show_view(WIFI_VIEW_SCAN);
}

static esp_err_t wifi_create_status_view(lv_obj_t *parent)
{
    s_view[WIFI_VIEW_STATUS] = lv_obj_create(parent);
    if (s_view[WIFI_VIEW_STATUS] == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_view[WIFI_VIEW_STATUS]);
    lv_obj_set_size(s_view[WIFI_VIEW_STATUS], LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_view[WIFI_VIEW_STATUS], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_view[WIFI_VIEW_STATUS], 10, 0);
    lv_obj_remove_flag(s_view[WIFI_VIEW_STATUS], LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *body = NULL;
    if (ui_card_create(s_view[WIFI_VIEW_STATUS], "连接状态", &body) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_status_detail = lv_label_create(body);
    lv_obj_set_width(s_status_detail, LV_PCT(100));
    lv_label_set_long_mode(s_status_detail, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_status_detail, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_status_detail, UI_COLOR_TEXT, 0);
    lv_label_set_text(s_status_detail, "…");

    lv_obj_t *bar = lv_obj_create(s_view[WIFI_VIEW_STATUS]);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *disc = lv_button_create(bar);
    lv_obj_set_height(disc, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(disc, 1);
    lv_obj_set_style_bg_color(disc, UI_COLOR_DANGER, 0);
    lv_obj_t *disc_label = lv_label_create(disc);
    lv_label_set_text(disc_label, "断开");
    lv_obj_center(disc_label);
    lv_obj_add_event_cb(disc, disconnect_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_button_create(bar);
    lv_obj_set_height(back, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(back, 1);
    lv_obj_set_style_bg_color(back, UI_COLOR_SURFACE_ALT, 0);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, "切换网络");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back, status_back_cb, LV_EVENT_CLICKED, NULL);

    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Refresh                                                                   */
/* ------------------------------------------------------------------------ */

static void refresh_status(void)
{
    if (s_status_detail == NULL) {
        return;
    }

    wifi_mgr_state_t state = WIFI_MGR_DISCONNECTED;
    (void)wifi_manager_get_state(&state);

    char ip[16] = { 0 };
    char mac[18] = { 0 };
    char ssid[WIFI_MANAGER_SSID_MAX + 1] = { 0 };
    (void)wifi_manager_get_ip(ip, sizeof(ip));
    (void)wifi_manager_get_mac(mac, sizeof(mac));
    (void)wifi_manager_get_ssid(ssid, sizeof(ssid));

    lv_label_set_text_fmt(s_status_detail,
                          "状态：%s\n网络：%s\nIP：%s\n信号：%d dBm\nMAC：%s\n主机名：%s",
                          wifi_manager_state_name(state),
                          ssid[0] ? ssid : "（未连接）",
                          ip,
                          (int)wifi_manager_get_rssi(),
                          mac,
                          WIFI_MANAGER_HOSTNAME);
}

static void refresh_scan_hint(void)
{
    if (s_scan_status == NULL) {
        return;
    }

    wifi_mgr_state_t state = WIFI_MGR_DISCONNECTED;
    (void)wifi_manager_get_state(&state);

    if (state == WIFI_MGR_CONNECTED) {
        char ssid[WIFI_MANAGER_SSID_MAX + 1] = { 0 };
        (void)wifi_manager_get_ssid(ssid, sizeof(ssid));
        lv_label_set_text_fmt(s_scan_status, "已连接 %s，可重新扫描切换", ssid);
    } else if (state == WIFI_MGR_CONNECTING) {
        lv_label_set_text(s_scan_status, "连接中…");
    } else if (state == WIFI_MGR_FAILED) {
        lv_label_set_text(s_scan_status, "上次连接失败，请重试");
    } else {
        lv_label_set_text(s_scan_status, "选择要连接的网络");
    }
}

/* ------------------------------------------------------------------------ */
/* Page                                                                      */
/* ------------------------------------------------------------------------ */

static esp_err_t wifi_create(lv_obj_t *parent)
{
    (void)page_header_create(parent, "WiFi");

    esp_err_t err = wifi_create_scan_view(parent);
    if (err != ESP_OK) {
        return err;
    }
    err = wifi_create_password_view(parent);
    if (err != ESP_OK) {
        return err;
    }
    err = wifi_create_status_view(parent);
    if (err != ESP_OK) {
        return err;
    }

    show_view(WIFI_VIEW_SCAN);
    render_scan_list();
    refresh_scan_hint();
    refresh_status();

    /* Every WiFi event refreshes both the list hint and the status card. */
    (void)ui_event_bind_all(WIFI_MGR_EVENT, UI_PAGE_WIFI, 0);
    SYS_LOGD(TAG, "wifi page built");
    return ESP_OK;
}

static esp_err_t wifi_on_show(void)
{
    refresh_scan_hint();
    refresh_status();
    /* A fresh scan on entry keeps the list current without a manual tap. */
    (void)wifi_manager_start_scan();
    return ESP_OK;
}

static esp_err_t wifi_on_event(int32_t event_id, void *data)
{
    (void)data;
    if (event_id == WIFI_MGR_EVENT_SCAN_DONE) {
        render_scan_list();
    }
    refresh_scan_hint();
    refresh_status();
    return ESP_OK;
}

static esp_err_t wifi_on_tick(void)
{
    refresh_status();
    return ESP_OK;
}

const ui_page_t page_wifi = {
    .id = UI_PAGE_WIFI,
    .name = "wifi",
    .create = wifi_create,
    .on_show = wifi_on_show,
    .on_event = wifi_on_event,
    .on_tick = wifi_on_tick,
    .tick_period_ms = 3000,
};
