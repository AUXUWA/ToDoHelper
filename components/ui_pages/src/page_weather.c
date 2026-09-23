/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_weather.c
 * @brief FR3: city, temperature, condition, humidity and update time.
 *
 * The icon comes from the packed assets partition through ui_assets; when it is
 * missing the page falls back to text so a missing asset never blanks the view
 * (requirement 12).
 *
 * Requirement FR3 is explicit that this board has no sensors, so every value
 * here is an API reading, and "no reading yet" is a state the page must render.
 */

#include "pages_internal.h"

#include <string.h>
#include <time.h>

#include "ui_assets.h"
#include "weather_service.h"

static const char *TAG = "page_weather";

static lv_obj_t *s_city_label;
static lv_obj_t *s_temp_label;
static lv_obj_t *s_cond_label;
static lv_obj_t *s_detail_label;
static lv_obj_t *s_icon;
static lv_obj_t *s_icon_fallback;
/** Defined below; the "重新定位" button refreshes the card once IP location lands. */
static void refresh(void);

/**
 * "重新定位": re-derive the city from the public IP.
 *
 * The manual city entry is gone. The panel has no IME, so typing a Chinese city
 * name on it was never practical, and the public IP already tells us where we are
 * (FR3 location priority 2, now the only path - see weather_detect_location()).
 */
static void city_clicked_cb(lv_event_t *e)
{
    (void)e;

    ui_toast("正在通过 IP 定位…");
    esp_err_t err = weather_detect_location();
    if (err != ESP_OK) {
        ui_toast("定位失败：%s", esp_err_to_name(err));
        return;
    }
    refresh();
    ui_toast("已定位到 %s", weather_get_city());
}

/* ------------------------------------------------------------------------ */
/* Refresh                                                                   */
/* ------------------------------------------------------------------------ */

static void refresh(void)
{
    if (s_temp_label == NULL) {
        return;
    }

    weather_data_t data;
    const bool have = (weather_get(&data) == ESP_OK);

    if (!have) {
        lv_label_set_text(s_city_label, weather_get_city()[0] ? weather_get_city() : "未设置城市");
        lv_label_set_text(s_temp_label, "--°");
        lv_label_set_text(s_cond_label, "正在通过 IP 自动定位…");
        lv_obj_set_style_text_color(s_cond_label, UI_COLOR_TEXT_MUTED, 0);
        lv_label_set_text(s_detail_label, "");
        if (s_icon != NULL) {
            lv_obj_add_flag(s_icon, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_icon_fallback != NULL) {
            lv_obj_remove_flag(s_icon_fallback, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    lv_label_set_text(s_city_label, data.city[0] ? data.city : weather_get_city());
    lv_label_set_text_fmt(s_temp_label, "%.0f°C", (double)data.temp);
    lv_label_set_text(s_cond_label, data.condition);
    lv_obj_set_style_text_color(s_cond_label, UI_COLOR_PRIMARY, 0);

    /* Requirement FR3: say when the reading is old instead of hiding it. */
    const int64_t now = sys_time_ts();
    const bool stale = (data.update_ts > 0 && now > 0) &&
                       ((now - data.update_ts) > WEATHER_STALE_AFTER_S);

    char updated[64] = "未知";
    if (data.update_ts > 0) {
        const time_t ts = (time_t)data.update_ts;
        struct tm tm_local;
        if (localtime_r(&ts, &tm_local) != NULL) {
            snprintf(updated, sizeof(updated), "%02d-%02d %02d:%02d",
                     tm_local.tm_mon + 1, tm_local.tm_mday,
                     tm_local.tm_hour, tm_local.tm_min);
        }
    }

    lv_label_set_text_fmt(s_detail_label,
                          "体感 %.0f°C · 湿度 %d%%\n更新 %s%s",
                          (double)data.feels_like, data.humidity, updated,
                          stale ? "（缓存）" : "");

    const char *src = ui_assets_weather_src(data.icon);
    if (s_icon != NULL) {
        if (src != NULL) {
            lv_image_set_src(s_icon, src);
            lv_obj_remove_flag(s_icon, LV_OBJ_FLAG_HIDDEN);
            if (s_icon_fallback != NULL) {
                lv_obj_add_flag(s_icon_fallback, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(s_icon, LV_OBJ_FLAG_HIDDEN);
            if (s_icon_fallback != NULL) {
                lv_label_set_text(s_icon_fallback, data.condition);
                lv_obj_remove_flag(s_icon_fallback, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Buttons                                                                   */
/* ------------------------------------------------------------------------ */

static void refresh_clicked_cb(lv_event_t *e)
{
    (void)e;
    /* Async on purpose: the request must never run in the LVGL task. */
    esp_err_t err = weather_refresh_async();
    if (err == ESP_OK) {
        ui_toast("正在刷新…");
    } else {
        ui_toast("无法刷新：%s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------------ */
/* Page                                                                      */
/* ------------------------------------------------------------------------ */

static esp_err_t weather_create(lv_obj_t *parent)
{
    (void)page_header_create(parent, "天气");

    lv_obj_t *body = NULL;
    if (ui_card_create(parent, NULL, &body) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_t *top = lv_obj_create(body);
    lv_obj_remove_style_all(top);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_height(top, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 12, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left = lv_obj_create(top);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 4, 0);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    s_city_label = lv_label_create(left);
    lv_obj_set_style_text_font(s_city_label, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(s_city_label, UI_COLOR_TEXT, 0);
    lv_label_set_text(s_city_label, "正在定位…");

    s_temp_label = lv_label_create(left);
    lv_obj_set_style_text_font(s_temp_label, ui_font(UI_FONT_HUGE), 0);
    lv_obj_set_style_text_color(s_temp_label, UI_COLOR_TEXT, 0);
    lv_label_set_text(s_temp_label, "--°");

    s_cond_label = lv_label_create(left);
    lv_obj_set_style_text_font(s_cond_label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_cond_label, UI_COLOR_PRIMARY, 0);
    lv_label_set_text(s_cond_label, "");

    lv_obj_t *right = lv_obj_create(top);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, 110, 110);
    lv_obj_center(right);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    s_icon = ui_assets_image_create(right, "w_sunny", 96);
    if (s_icon != NULL) {
        lv_obj_align(s_icon, LV_ALIGN_CENTER, 0, 0);
    }
    s_icon_fallback = lv_label_create(right);
    lv_obj_set_style_text_font(s_icon_fallback, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(s_icon_fallback, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_align(s_icon_fallback, LV_ALIGN_CENTER, 0, 0);
    /* lv_label_create() starts with LVGL's built-in default text, which is the
     * literal string "Text" - so an unpopulated fallback label showed "Text" on
     * the weather card. Start empty and hidden; weather_update() reveals it only
     * when the icon image is unavailable. */
    lv_label_set_text(s_icon_fallback, "");
    lv_obj_add_flag(s_icon_fallback, LV_OBJ_FLAG_HIDDEN);

    s_detail_label = lv_label_create(body);
    lv_obj_set_width(s_detail_label, LV_PCT(100));
    lv_label_set_long_mode(s_detail_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_detail_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_detail_label, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_detail_label, "");

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
        { "重新定位", city_clicked_cb, UI_COLOR_SURFACE_ALT },
        { "立即刷新", refresh_clicked_cb, UI_COLOR_PRIMARY },
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

    (void)ui_event_bind(WEATHER_EVENT, WEATHER_EVENT_UPDATED, UI_PAGE_WEATHER, 0);
    (void)ui_event_bind(WEATHER_EVENT, WEATHER_EVENT_REFRESH_FAILED, UI_PAGE_WEATHER,
                        sizeof(esp_err_t));
    SYS_LOGD(TAG, "weather page built");
    return ESP_OK;
}

static esp_err_t weather_on_show(void)
{
    refresh();
    return ESP_OK;
}

static esp_err_t weather_on_event(int32_t event_id, void *data)
{
    if (event_id == WEATHER_EVENT_REFRESH_FAILED) {
        esp_err_t err = ESP_FAIL;
        if (data != NULL) {
            memcpy(&err, data, sizeof(err));
        }
        /* Offline is a normal state, not an error dialog (requirement 15). */
        ui_toast("刷新失败(%s)，显示缓存", esp_err_to_name(err));
    }
    refresh();
    return ESP_OK;
}

const ui_page_t page_weather = {
    .id = UI_PAGE_WEATHER,
    .name = "weather",
    .create = weather_create,
    .on_show = weather_on_show,
    .on_event = weather_on_event,
};
