/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_settings.c
 * @brief Settings: brightness, idle behaviour, timezone, city, daily words,
 *        HTTP auth token and a storage/device summary.
 *
 * Everything writes straight to NVS through the owning component's API, so the
 * values survive a reboot without this page knowing any key names.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calendar_service.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pages_internal.h"
#include "storage_manager.h"
#include "sync_server.h"
#include "th_bsp.h"
#include "weather_service.h"
#include "wifi_manager.h"
#include "word_service.h"

static const char *TAG = "page_settings";

static lv_obj_t *s_info_label;
static lv_obj_t *s_auth_label;
/** Web console address, shown large so it can be read off the screen. */
static lv_obj_t *s_url_label;

/* ------------------------------------------------------------------------ */
/* Sliders                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char *title;
    int32_t value;
    int32_t min;
    int32_t max;
    void (*on_change)(int32_t value);
} setting_slider_t;

static void brightness_changed(int32_t value)
{
    (void)th_bsp_set_brightness((uint8_t)value);
    (void)sys_config_set_i32(SYS_KEY_BRIGHTNESS, value);
}

static void idle_brightness_changed(int32_t value)
{
    (void)sys_config_set_i32(SYS_KEY_IDLE_BRIGHT, value);
    /* Only affects the next idle entry, so nothing to apply right now. */
}

static void idle_timeout_changed(int32_t value)
{
    (void)sys_config_set_i32(SYS_KEY_IDLE_TIMEOUT, value);
    /* Re-arm the watchdog so the new value takes effect immediately. */
    (void)th_bsp_idle_init();
}

static void slider_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    const setting_slider_t *def = (const setting_slider_t *)lv_event_get_user_data(e);
    if (def == NULL || def->on_change == NULL) {
        return;
    }
    def->on_change(lv_slider_get_value(slider));
}

static lv_obj_t *add_slider(lv_obj_t *parent, const setting_slider_t *def)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, def->title);
    lv_obj_set_style_text_font(title, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_set_width(title, 150);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_height(slider, 22);
    lv_slider_set_range(slider, def->min, def->max);
    lv_slider_set_value(slider, def->value, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED, (void *)def);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_width(value, 56);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(value, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(value, UI_COLOR_PRIMARY, 0);
    lv_label_set_text_fmt(value, "%ld", (long)def->value);

    /* The shared settings_tick() timer reads the slider back through this
     * pointer, which avoids one callback per slider. */
    lv_obj_set_user_data(slider, value);

    return row;
}

/**
 * LVGL has no "bind label to slider" helper, so a small shared timer refreshes
 * the readouts of the sliders this page created.
 *
 * The timer is owned by the page (s_slider_timer) and paused in on_hide(): it
 * used to be created without keeping the handle, so it kept re-formatting four
 * labels four times a second *forever* - including while the user was on a
 * completely different page (found in the smoothness audit).
 */
#define SETTINGS_MAX_SLIDERS 4
static lv_obj_t *s_slider_objs[SETTINGS_MAX_SLIDERS];
static lv_obj_t *s_slider_values[SETTINGS_MAX_SLIDERS];
static size_t s_slider_count;
static lv_timer_t *s_slider_timer;

static void settings_tick(lv_timer_t *timer)
{
    (void)timer;
    for (size_t i = 0; i < s_slider_count; i++) {
        if (s_slider_objs[i] == NULL || s_slider_values[i] == NULL) {
            continue;
        }
        /* Only touch the label when the value actually changed: writing the same
         * text still invalidates the label and costs a repaint. */
        const int value = (int)lv_slider_get_value(s_slider_objs[i]);
        if (value == (int)(intptr_t)lv_obj_get_user_data(s_slider_values[i])) {
            continue;
        }
        lv_obj_set_user_data(s_slider_values[i], (void *)(intptr_t)value);
        lv_label_set_text_fmt(s_slider_values[i], "%d", value);
    }
}

/* ------------------------------------------------------------------------ */
/* Simple action rows                                                        */
/* ------------------------------------------------------------------------ */

/**
 * Timezone change: the NVS commit is a flash write, so it is handed to a tiny
 * one-shot task instead of running in the LVGL callback (a flash write also
 * disables the cache for its duration). The label is updated optimistically
 * because calendar_set_timezone() only fails on invalid input, which cannot
 * happen here - the strings come from a fixed table.
 */
typedef struct {
    char tz[48];
} tz_request_t;

static void tz_apply_task(void *arg)
{
    tz_request_t *req = (tz_request_t *)arg;
    const esp_err_t err = calendar_set_timezone(req->tz);
    free(req);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "timezone change failed: %s", esp_err_to_name(err));
    }
}

static void tz_clicked_cb(lv_event_t *e)
{
    const char *tz = (const char *)lv_event_get_user_data(e);
    if (tz == NULL) {
        return;
    }

    tz_request_t *req = calloc(1, sizeof(*req));
    if (req == NULL) {
        ui_toast("设置失败：内存不足");
        return;
    }
    strlcpy(req->tz, tz, sizeof(req->tz));

    /* 3072 B stack, priority 3: it only does one NVS write and exits. */
    if (xTaskCreate(tz_apply_task, "th_tz", 3072, req, 3, NULL) != pdPASS) {
        free(req);
        ui_toast("设置失败：无法创建任务");
        return;
    }
    ui_toast("时区已设为 %s", tz);
}

static void sync_ntp_cb(lv_event_t *e)
{
    (void)e;
    /* Async: the old synchronous call waited up to 10 s on the network *with the
     * LVGL lock held*, freezing the whole UI (and the event loop behind it). */
    const esp_err_t err = calendar_sync_ntp_async();
    if (err == ESP_ERR_TIMEOUT) {
        ui_toast("正在校时，请稍候…");
    } else if (err != ESP_OK) {
        ui_toast("校时失败：%s", esp_err_to_name(err));
    } else {
        ui_toast("正在校时…");
    }
}

/** Result of a requested sync, delivered on the LVGL task by ui_core. */
static esp_err_t settings_on_sync_result(int32_t event_id, void *data)
{
    (void)event_id;
    const esp_err_t err = (data != NULL) ? *(const esp_err_t *)data : ESP_FAIL;
    ui_toast(err == ESP_OK ? "校时成功" : "校时失败：%s", esp_err_to_name(err));
    return ESP_OK;
}

static void city_clicked_cb(lv_event_t *e)
{
    (void)e;
    ui_toast("当前城市：%s", weather_get_city()[0] ? weather_get_city() : "未设置");
    ui_toast("请在天气页设置，或用 HTTP API POST /api/v1/weather/city");
}

static void word_count_up_cb(lv_event_t *e)
{
    (void)e;
    uint32_t count = word_get_daily_count() + 5;
    (void)word_set_daily_count(count);
    ui_toast("每日单词数：%u", (unsigned)word_get_daily_count());
}

static void word_count_down_cb(lv_event_t *e)
{
    (void)e;
    uint32_t count = word_get_daily_count();
    count = (count > 5) ? (count - 5) : 1;
    (void)word_set_daily_count(count);
    ui_toast("每日单词数：%u", (unsigned)word_get_daily_count());
}

static void word_reset_cb(lv_event_t *e)
{
    (void)e;
    (void)word_reset_progress();
    ui_toast("单词进度已清空");
}

/* Defined below; the auth callbacks use it to re-render the security card. */
static void refresh_info(void);

static void auth_enable_cb(lv_event_t *e)
{
    (void)e;

    /* Generating the token here is more usable than typing 32 characters on an
     * on-screen keyboard; it is displayed once so it can be copied to the PC. */
    char token[33];
    for (int i = 0; i < 32; i++) {
        token[i] = "0123456789abcdef"[(esp_random() >> (i % 8)) & 0x0F];
    }
    token[32] = '\0';

    esp_err_t err = sync_server_set_auth_token(token);
    if (err == ESP_OK) {
        /*
         * The token is the whole point of this dialog, so it is rendered as a
         * highlighted, wrapped value rather than described in prose. The earlier
         * version told the user to copy "the token below" while showing only
         * explanatory text - and the text was long enough to push the box off
         * the screen.
         *
         * Strings are kept short on purpose: ui_dialog_secret() lays the lead-in
         * and the token out side by side (the panel is 800x480 landscape), so
         * long prose in the left column would squeeze the token column instead of
         * merely making the box taller.
         */
        ui_dialog_secret("HTTP 鉴权已开启",
                         "把下面的 token 填进网页控制台的「访问令牌」。",
                         token,
                         "串口日志里也有；随时可在这里重新生成。",
                         "知道了");
        /* Logged once on purpose: this is the only way to read it off the
         * device, and requirement 8.8 only forbids it appearing in backups and
         * routine logs. Rotate it if that matters to you. */
        SYS_LOGW(TAG, "new HTTP auth token: %s", token);
    } else {
        ui_toast("开启失败：%s", esp_err_to_name(err));
    }
    /* Go through refresh_info() rather than writing the label here, so the
     * dirty-check buffer stays in sync with what is on screen. */
    refresh_info();
}

static void auth_disable_cb(lv_event_t *e)
{
    (void)e;
    (void)sync_server_set_auth_token(NULL);
    ui_toast("HTTP 鉴权已关闭");
    refresh_info();
}

static void reboot_cb(lv_event_t *e)
{
    (void)e;
    ui_toast("重启中…");
    /* Delayed so the toast can actually be drawn before the reset. */
    extern void esp_restart(void);
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

static lv_obj_t *add_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                            const void *user_data, lv_color_t color)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(label, UI_COLOR_ON_PRIMARY, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)user_data);
    return btn;
}

static lv_obj_t *add_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/* ------------------------------------------------------------------------ */
/* Refresh                                                                   */
/* ------------------------------------------------------------------------ */

/**
 * Last rendered info block.
 *
 * The label is a six-line LV_LABEL_LONG_MODE_WRAP at 100% width, so re-setting
 * it forces a full re-wrap and height re-measure of the card. Almost nothing in
 * it changes at the 5 s tick cadence (the firmware version never does), and
 * building it calls storage_free_bytes() - a statvfs on the SD card - so the
 * text is compared before the label is touched (smoothness audit P1-10).
 */
static char s_info_text[320];
/** Dirty-check buffers for the security card's two lines (see refresh_info). */
static char s_url_text[64];
static char s_auth_text[96];

static void refresh_info(void)
{
    if (s_info_label == NULL) {
        return;
    }

    char ip[16] = { 0 };
    char mac[18] = { 0 };
    (void)wifi_manager_get_ip(ip, sizeof(ip));
    (void)wifi_manager_get_mac(mac, sizeof(mac));

    /* Version comes from version.txt at build time (see AGENT.md 8.1); it is the
     * quickest way to tell which firmware is actually flashed on the board. */
    const esp_app_desc_t *app = esp_app_get_description();

    char text[sizeof(s_info_text)];
    snprintf(text, sizeof(text),
             "固件 %s · IDF %s\nIP %s · %s\n存储 %s（%s）\n剩余 %llu KB · 时区 %s\n"
             "天气源 %s · 城市 %s\n单词 %u/日 · 词库 %s",
             (app != NULL) ? app->version : "?",
             (app != NULL) ? app->idf_ver : "?",
             ip, mac,
             storage_root() ? storage_root() : "不可用",
             storage_sd_available() ? "SD 卡" : "内置",
             (unsigned long long)(storage_free_bytes() / 1024),
             calendar_get_timezone(),
             weather_get_provider(),
             weather_get_city()[0] ? weather_get_city() : "未设置",
             (unsigned)word_get_daily_count(),
             word_get_book());

    /*
     * The security card is refreshed here too, not just at creation. Its URL line
     * depends on the DHCP address, and this function is the page's periodic
     * (on_show + tick) refresh point, so this is where a changed address is
     * picked up. Both writes are dirty-checked: a 1 Hz unconditional
     * lv_label_set_text would repaint the page forever.
     */
    if (s_url_label != NULL) {
        /*
         * Show both forms when mDNS is on. `todohelper.local` is nicer to type but
         * only resolves with Bonjour on Windows and not at all where multicast is
         * blocked, so the IP is always shown as well - the user should never have
         * to go hunting for an address that works.
         */
        char ip_url[40];
        sync_server_ip_url(ip_url, sizeof(ip_url));

        char url[128];
        const char *primary = sync_server_url();
        if (strstr(primary, ".local") != NULL) {
            snprintf(url, sizeof(url), "%s\n或 %s", primary, ip_url);
        } else {
            snprintf(url, sizeof(url), "%s", primary);
        }

        if (strcmp(url, s_url_text) != 0) {
            strlcpy(s_url_text, url, sizeof(s_url_text));
            lv_label_set_text(s_url_label, s_url_text);
        }
    }
    if (s_auth_label != NULL) {
        char auth[96];
        snprintf(auth, sizeof(auth), "鉴权：%s · 二维码/网址填这个地址",
                 sync_server_auth_required() ? "已开启（网页需填 token）" : "未开启");
        if (strcmp(auth, s_auth_text) != 0) {
            strlcpy(s_auth_text, auth, sizeof(s_auth_text));
            lv_label_set_text(s_auth_label, s_auth_text);
        }
    }

    if (strcmp(text, s_info_text) == 0) {
        return;
    }
    strlcpy(s_info_text, text, sizeof(s_info_text));
    lv_label_set_text(s_info_label, s_info_text);
}

/* ------------------------------------------------------------------------ */
/* Page                                                                      */
/* ------------------------------------------------------------------------ */

static esp_err_t settings_create(lv_obj_t *parent)
{
    (void)page_header_create(parent, "设置");

    /* ---- Display ---- */
    lv_obj_t *display = NULL;
    if (ui_card_create(parent, "显示与待机", &display) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    static setting_slider_t s_brightness = {
        .title = "屏幕亮度",
        .min = 5,
        .max = 100,
        .on_change = brightness_changed,
    };
    static setting_slider_t s_idle_bright = {
        /* 100 = the idle face keeps its pure white eyes; lower to dim the panel
         * while idle (also the default in ui_core.c). */
        .title = "待机亮度",
        .min = 0,
        .max = 100,
        .on_change = idle_brightness_changed,
    };
    static setting_slider_t s_idle_timeout = {
        .title = "待机超时(秒)",
        .min = 5,
        .max = 300,
        .on_change = idle_timeout_changed,
    };

    s_brightness.value = sys_config_get_i32_or(SYS_KEY_BRIGHTNESS, 100);
    s_idle_bright.value = sys_config_get_i32_or(SYS_KEY_IDLE_BRIGHT, 100);
    s_idle_timeout.value = sys_config_get_i32_or(SYS_KEY_IDLE_TIMEOUT, 30);

    setting_slider_t *defs[] = { &s_brightness, &s_idle_bright, &s_idle_timeout };
    s_slider_count = 0;
    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]) && s_slider_count < SETTINGS_MAX_SLIDERS;
         i++) {
        lv_obj_t *row = add_slider(display, defs[i]);
        /* add_slider stored the value label in the slider's user data. */
        lv_obj_t *slider = lv_obj_get_child(row, 1);
        s_slider_objs[s_slider_count] = slider;
        s_slider_values[s_slider_count] = (slider != NULL) ? lv_obj_get_user_data(slider) : NULL;
        s_slider_count++;
    }

    /* ---- Time ---- */
    lv_obj_t *time_card = NULL;
    if (ui_card_create(parent, "时间与时区", &time_card) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_t *tz_row = add_row(time_card);
    static const char *const tz_presets[] = { "CST-8", "UTC0", "JST-9", "EST5" };
    for (size_t i = 0; i < sizeof(tz_presets) / sizeof(tz_presets[0]); i++) {
        add_button(tz_row, tz_presets[i], tz_clicked_cb, tz_presets[i], UI_COLOR_SURFACE_ALT);
    }

    lv_obj_t *time_row = add_row(time_card);
    add_button(time_row, "NTP 校时", sync_ntp_cb, NULL, UI_COLOR_PRIMARY);
    add_button(time_row, "当前城市", city_clicked_cb, NULL, UI_COLOR_SURFACE_ALT);

    /* ---- Words ---- */
    lv_obj_t *word_card = NULL;
    if (ui_card_create(parent, "单词", &word_card) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_t *word_row = add_row(word_card);
    add_button(word_row, "每日 -5", word_count_down_cb, NULL, UI_COLOR_SURFACE_ALT);
    add_button(word_row, "每日 +5", word_count_up_cb, NULL, UI_COLOR_SURFACE_ALT);
    add_button(word_row, "清空进度", word_reset_cb, NULL, UI_COLOR_DANGER);

    /* ---- Security ---- */
    lv_obj_t *sec_card = NULL;
    if (ui_card_create(parent, "局域网与安全", &sec_card) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * The web console address gets its own prominent row.
     *
     * It is the one string on this page the user has to read and then type into a
     * browser on another device, so it is rendered at the title font in the
     * primary colour on its own line - not buried in the muted info block.
     * Refreshed in refresh_info() so a DHCP change is reflected.
     */
    s_url_label = lv_label_create(sec_card);
    lv_obj_set_width(s_url_label, LV_PCT(100));
    lv_label_set_long_mode(s_url_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_url_label, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(s_url_label, UI_COLOR_PRIMARY, 0);

    s_auth_label = lv_label_create(sec_card);
    lv_obj_set_style_text_font(s_auth_label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_auth_label, UI_COLOR_TEXT, 0);

    lv_obj_t *sec_row = add_row(sec_card);
    add_button(sec_row, "生成并开启 token", auth_enable_cb, NULL, UI_COLOR_PRIMARY);
    add_button(sec_row, "关闭鉴权", auth_disable_cb, NULL, UI_COLOR_SURFACE_ALT);

    /* ---- Info ---- */
    lv_obj_t *info_card = NULL;
    if (ui_card_create(parent, "设备信息", &info_card) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_info_label = lv_label_create(info_card);
    lv_obj_set_width(s_info_label, LV_PCT(100));
    lv_label_set_long_mode(s_info_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_info_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_info_label, UI_COLOR_TEXT_MUTED, 0);

    lv_obj_t *info_row = add_row(info_card);
    add_button(info_row, "重启设备", reboot_cb, NULL, UI_COLOR_DANGER);

    refresh_info();
    /* Owned by the page so on_hide() can stop it (see settings_tick). */
    s_slider_timer = lv_timer_create(settings_tick, 250, NULL);
    if (s_slider_timer != NULL) {
        lv_timer_pause(s_slider_timer);
    }

    /* The NTP outcome is delivered on the LVGL task by ui_core, so the toast can
     * be shown without the page ever blocking on the network. */
    (void)ui_event_subscribe(CALENDAR_EVENT, CALENDAR_EVENT_SYNC_RESULT,
                             sizeof(esp_err_t), settings_on_sync_result);

    SYS_LOGD(TAG, "settings page built");
    return ESP_OK;
}

static esp_err_t settings_on_show(void)
{
    refresh_info();
    if (s_slider_timer != NULL) {
        lv_timer_resume(s_slider_timer);
    }
    return ESP_OK;
}

static esp_err_t settings_on_hide(void)
{
    if (s_slider_timer != NULL) {
        lv_timer_pause(s_slider_timer);
    }
    return ESP_OK;
}

static esp_err_t settings_on_tick(void)
{
    refresh_info();
    return ESP_OK;
}

const ui_page_t page_settings = {
    .id = UI_PAGE_SETTINGS,
    .name = "settings",
    .create = settings_create,
    .on_show = settings_on_show,
    .on_hide = settings_on_hide,
    .on_tick = settings_on_tick,
    .tick_period_ms = 5000,
};
