/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "bsp/esp32_s31_korvo.h"
#include "coffee_assets.h"
#include "factory_demo_audio.h"
#include "factory_demo_camera.h"
#include "factory_demo_usb_hid.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui.h"

#define UI_REFRESH_PERIOD_MS     100
#define UI_DISPLAY_BUFFER_HEIGHT 160
#define UI_DISPLAY_TASK_STACK_SIZE (16 * 1024)
#define UI_SCREEN_SWITCH_TASK_STACK_SIZE 4096
#define UI_SCREEN_SWITCH_TASK_PRIORITY 5
#define UI_DISPLAY_ENABLE_PPA_ACCEL true
#define UI_CARD_RADIUS           22
#define UI_BORDER_WIDTH          1
#define UI_STATUS_TEXT_LEN       96
#define UI_LED_NAME_LEN          24
#define UI_CAMERA_FRAME_WIDTH    240
#define UI_CAMERA_FRAME_HEIGHT   240
#define UI_MAIN_ROW_Y            110
#define UI_MAIN_ROW_H            258
#define UI_BOTTOM_ROW_Y          382
#define UI_BOTTOM_ROW_H          92
#define UI_RIGHT_COL_X           524
#define UI_RIGHT_COL_W           252
#define UI_COMPACT_CARD_PAD      10
#define UI_COMPACT_CONTENT_Y     30
#define UI_COMPACT_BUTTON_Y      32
#define UI_COMPACT_STATUS_Y      66
#define UI_HW_BUTTON_COUNT       4
#define UI_AUDIO_BUTTON_Y        78
#define UI_AUDIO_BUTTON_W        104
#define UI_AUDIO_BUTTON_H        40
#define UI_AUDIO_BUTTON_GAP      8
#define UI_SDCARD_MOUNT_RETRY_DELAY_MS 100
#define UI_SDCARD_MOUNT_MAX_RETRY      2
#define UI_CAMERA_FIRST_FRAME_TIMEOUT_MS 3000

typedef struct {
    lv_display_t *disp;
    factory_audio_t audio;
    factory_camera_t camera;
    factory_usb_hid_t usb_hid;
    SemaphoreHandle_t lock;
    button_handle_t buttons[BSP_BUTTON_NUM];
    int button_count;
    lv_obj_t *camera_status_label;
    lv_obj_t *camera_canvas;
    lv_draw_buf_t camera_canvas_draw_buf;
    lv_obj_t *audio_status_label;
    lv_obj_t *audio_btn_label;
    lv_obj_t *audio_progress;
    bool audio_play_pending;
    lv_obj_t *storage_card;
    lv_obj_t *storage_status_label;
    lv_obj_t *usb_hid_card;
    lv_obj_t *usb_hid_status_label;
    lv_obj_t *usb_hid_input_label;
    lv_obj_t *button_led_buttons[UI_HW_BUTTON_COUNT];
    lv_obj_t *button_status_label;
    lv_obj_t *footer_label;
    lv_obj_t *factory_screen;
    void *camera_canvas_buf;
    uint8_t led_r;
    uint8_t led_g;
    uint8_t led_b;
    bool sd_mounted;
    bool coffee_transition_pending;
    bool hw_button_pressed[UI_HW_BUTTON_COUNT];
    int active_led_button;
    char led_name[UI_LED_NAME_LEN];
    char button_status[UI_STATUS_TEXT_LEN];
    char sd_status[UI_STATUS_TEXT_LEN];
} factory_demo_app_t;

static const char *TAG = "factory_demo";
static factory_demo_app_t *s_app;

static const lv_color_t COLOR_BG = LV_COLOR_MAKE(0x07, 0x12, 0x22);
static const lv_color_t COLOR_PANEL = LV_COLOR_MAKE(0x10, 0x1F, 0x32);
static const lv_color_t COLOR_PANEL_CAMERA = LV_COLOR_MAKE(0x14, 0x26, 0x3D);
static const lv_color_t COLOR_PANEL_AUDIO = LV_COLOR_MAKE(0x12, 0x24, 0x36);
static const lv_color_t COLOR_PANEL_BUTTONS = LV_COLOR_MAKE(0x11, 0x21, 0x33);
static const lv_color_t COLOR_PANEL_USB = LV_COLOR_MAKE(0x12, 0x22, 0x34);
static const lv_color_t COLOR_PANEL_STORAGE = LV_COLOR_MAKE(0x11, 0x23, 0x35);
static const lv_color_t COLOR_TEXT = LV_COLOR_MAKE(0xF4, 0xF5, 0xF7);
static const lv_color_t COLOR_MUTED = LV_COLOR_MAKE(0x9E, 0xAE, 0xBF);
static const lv_color_t COLOR_ACCENT = LV_COLOR_MAKE(0xFF, 0x8A, 0x5B);
static const lv_color_t COLOR_SKY = LV_COLOR_MAKE(0x61, 0xA8, 0xFF);
static const lv_color_t COLOR_STATE_OK = LV_COLOR_MAKE(0x12, 0x4B, 0x38);
static const lv_color_t COLOR_STATE_WARN = LV_COLOR_MAKE(0x5A, 0x44, 0x12);
static const lv_color_t COLOR_STATE_ERR = LV_COLOR_MAKE(0x58, 0x20, 0x26);
static const lv_color_t COLOR_STATE_IDLE = LV_COLOR_MAKE(0x17, 0x42, 0x6A);

typedef struct {
    bsp_button_t button;
    const char *button_name;
    const char *led_name;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    lv_color_t ui_color;
} factory_button_led_map_t;

static const factory_button_led_map_t BUTTON_LED_MAP[UI_HW_BUTTON_COUNT] = {
    { BSP_BUTTON_SET, "set", "Amber", 28, 18, 4, LV_COLOR_MAKE(0xFF, 0xB0, 0x47) },
    { BSP_BUTTON_MODE, "mode", "Blue", 6, 14, 28, LV_COLOR_MAKE(0x61, 0xA8, 0xFF) },
    { BSP_BUTTON_VOLDOWN, "vol-", "Green", 4, 28, 10, LV_COLOR_MAKE(0x63, 0xD5, 0xB4) },
    { BSP_BUTTON_VOLUP, "vol+", "Red", 28, 4, 4, LV_COLOR_MAKE(0xFF, 0x6B, 0x5C) },
};

static void factory_ui_refresh_now(factory_demo_app_t *app);
static void factory_show_coffee_screen(factory_demo_app_t *app);
static void factory_show_factory_screen(factory_demo_app_t *app);
static void factory_switch_to_coffee_task(void *arg);
static void factory_app_set_button_status(factory_demo_app_t *app, const char *text);
static void factory_update_button_led_ui(factory_demo_app_t *app, const bool pressed[UI_HW_BUTTON_COUNT],
                                         int active_led_button);
static void factory_update_button_led_ui_now(factory_demo_app_t *app);

static const char *factory_button_name(bsp_button_t button)
{
    switch (button) {
    case BSP_BUTTON_VOLUP:
        return "vol+";
    case BSP_BUTTON_VOLDOWN:
        return "vol-";
    case BSP_BUTTON_MODE:
        return "mode";
    case BSP_BUTTON_SET:
        return "set";
    default:
        return "UNKNOWN";
    }
}

static const char *factory_button_event_name(button_event_t event)
{
    switch (event) {
    case BUTTON_PRESS_DOWN:
        return "DOWN";
    case BUTTON_PRESS_UP:
        return "UP";
    case BUTTON_SINGLE_CLICK:
        return "CLICK";
    case BUTTON_DOUBLE_CLICK:
        return "DBL";
    case BUTTON_PRESS_END:
        return "END";
    default:
        return "EVENT";
    }
}

static const char *factory_audio_state_name(factory_audio_state_t state)
{
    switch (state) {
    case FACTORY_AUDIO_STATE_IDLE:
        return "Idle";
    case FACTORY_AUDIO_STATE_RECORDING:
        return "Recording";
    case FACTORY_AUDIO_STATE_PLAYING:
        return "Playing";
    case FACTORY_AUDIO_STATE_ERROR:
        return "Error";
    default:
        return "Unknown";
    }
}

static const char *factory_camera_short_status(const factory_camera_snapshot_t *camera)
{
    if (camera->state == FACTORY_CAMERA_STATE_ERROR) {
        return "ERR";
    }
    if (camera->state == FACTORY_CAMERA_STATE_PREVIEW && camera->frame_count > 0) {
        return "OK";
    }
    if (camera->state == FACTORY_CAMERA_STATE_STARTING || camera->state == FACTORY_CAMERA_STATE_PREVIEW) {
        return "WAIT";
    }
    return "IDLE";
}

static int factory_button_map_index(bsp_button_t button)
{
    for (int i = 0; i < UI_HW_BUTTON_COUNT; i++) {
        if (BUTTON_LED_MAP[i].button == button) {
            return i;
        }
    }
    return -1;
}

static void factory_app_set_led_status(factory_demo_app_t *app, uint8_t red, uint8_t green, uint8_t blue, const char *name)
{
    if (!app) {
        return;
    }

    esp_err_t ret;
    if (red == 0 && green == 0 && blue == 0) {
        ret = bsp_led_clear();
    } else {
        ret = bsp_led_set_rgb(BSP_LED_STATUS, red, green, blue);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED update failed (%s)", esp_err_to_name(ret));
        return;
    }

    xSemaphoreTake(app->lock, portMAX_DELAY);
    app->led_r = red;
    app->led_g = green;
    app->led_b = blue;
    strlcpy(app->led_name, name, sizeof(app->led_name));
    xSemaphoreGive(app->lock);

    ESP_LOGI(TAG, "LED set to %s (%u,%u,%u)", name, red, green, blue);
}

static void factory_app_set_led_by_button(factory_demo_app_t *app, int index, const char *source)
{
    if (!app || index < 0 || index >= UI_HW_BUTTON_COUNT) {
        return;
    }

    const factory_button_led_map_t *map = &BUTTON_LED_MAP[index];
    factory_app_set_led_status(app, map->red, map->green, map->blue, map->led_name);

    char status[UI_STATUS_TEXT_LEN];
    snprintf(status, sizeof(status), "%s: %s -> %s LED", source, map->button_name, map->led_name);
    factory_app_set_button_status(app, status);

    xSemaphoreTake(app->lock, portMAX_DELAY);
    app->active_led_button = index;
    xSemaphoreGive(app->lock);
}

static void factory_app_restore_led(factory_demo_app_t *app)
{
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    xSemaphoreTake(app->lock, portMAX_DELAY);
    red = app->led_r;
    green = app->led_g;
    blue = app->led_b;
    xSemaphoreGive(app->lock);

    if (red == 0 && green == 0 && blue == 0) {
        (void)bsp_led_clear();
    } else {
        (void)bsp_led_set_rgb(BSP_LED_STATUS, red, green, blue);
    }
}

static void factory_app_set_button_status(factory_demo_app_t *app, const char *text)
{
    xSemaphoreTake(app->lock, portMAX_DELAY);
    strlcpy(app->button_status, text, sizeof(app->button_status));
    xSemaphoreGive(app->lock);
}

static const char *factory_sd_short_status(const char *status)
{
    if (strstr(status, "OK")) {
        return "OK";
    }
    if (strstr(status, "failed") || strstr(status, "Failed")) {
        return "ERR";
    }
    return "IDLE";
}

static void factory_app_set_sd_status(factory_demo_app_t *app, const char *text)
{
    xSemaphoreTake(app->lock, portMAX_DELAY);
    strlcpy(app->sd_status, text, sizeof(app->sd_status));
    xSemaphoreGive(app->lock);
}

static void factory_app_set_sd_mounted(factory_demo_app_t *app, bool mounted)
{
    xSemaphoreTake(app->lock, portMAX_DELAY);
    app->sd_mounted = mounted;
    xSemaphoreGive(app->lock);
}

static esp_err_t factory_app_ensure_sdcard_mounted(factory_demo_app_t *app)
{
    if (bsp_sdcard_get_card()) {
        factory_app_set_sd_mounted(app, true);
        return ESP_OK;
    }

    factory_app_set_sd_mounted(app, false);

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= UI_SDCARD_MOUNT_MAX_RETRY; attempt++) {
        sdmmc_card_t *card = NULL;
        ret = bsp_sdcard_mount(NULL, &card);
        if (ret == ESP_OK) {
            factory_app_set_sd_mounted(app, true);
            ESP_LOGI(TAG, "SD card mounted, keep it active for factory checks");
            return ESP_OK;
        }

        ESP_LOGW(TAG, "SD card mount attempt %d/%d failed (%s)",
                 attempt, UI_SDCARD_MOUNT_MAX_RETRY, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(UI_SDCARD_MOUNT_RETRY_DELAY_MS));
    }

    return ret;
}

static esp_err_t factory_app_write_sdcard_probe_file(void)
{
    char file_path[96];
    snprintf(file_path, sizeof(file_path), "%s/factory_demo.txt", bsp_sdcard_get_mount_point());

    FILE *file = fopen(file_path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "Open SD card test file failed");
        return ESP_FAIL;
    }

    fprintf(file, "factory demo ok %" PRIu64 "\n", (uint64_t)esp_timer_get_time());
    if (fflush(file) != 0) {
        fclose(file);
        ESP_LOGE(TAG, "Flush SD card test file failed");
        return ESP_FAIL;
    }
    if (fsync(fileno(file)) != 0) {
        fclose(file);
        ESP_LOGE(TAG, "Sync SD card test file failed");
        return ESP_FAIL;
    }
    fclose(file);

    return ESP_OK;
}

static void factory_app_probe_sdcard(factory_demo_app_t *app)
{
    esp_err_t ret = factory_app_ensure_sdcard_mounted(app);
    if (ret != ESP_OK) {
        factory_app_set_sd_status(app, "Mount failed");
        ESP_LOGW(TAG, "SD card probe failed during mount (%s)", esp_err_to_name(ret));
        return;
    }

    ret = factory_app_write_sdcard_probe_file();
    if (ret != ESP_OK) {
        factory_app_set_sd_status(app, "R/W failed");
        factory_app_set_sd_mounted(app, false);
        (void)bsp_sdcard_unmount();
        ESP_LOGW(TAG, "SD card probe failed during file check (%s)", esp_err_to_name(ret));
        return;
    }

    factory_app_set_sd_status(app, "OK");
    ESP_LOGI(TAG, "SD card probe finished");
}

static void factory_button_event_cb(void *button_handle, void *user_data)
{
    factory_demo_app_t *app = s_app;
    bsp_button_t button = (bsp_button_t)(intptr_t)user_data;
    button_event_t event = iot_button_get_event(button_handle);
    char status[UI_STATUS_TEXT_LEN];
    int button_index = factory_button_map_index(button);

    snprintf(status, sizeof(status), "%s: %s", factory_button_name(button), factory_button_event_name(event));
    factory_app_set_button_status(app, status);
    ESP_LOGI(TAG, "%s", status);

    if (event == BUTTON_PRESS_DOWN) {
        if (button_index >= 0) {
            xSemaphoreTake(app->lock, portMAX_DELAY);
            app->hw_button_pressed[button_index] = true;
            xSemaphoreGive(app->lock);
            factory_app_set_led_by_button(app, button_index, "HW");
        }
    } else if (event == BUTTON_PRESS_UP || event == BUTTON_PRESS_END) {
        if (button_index >= 0) {
            xSemaphoreTake(app->lock, portMAX_DELAY);
            app->hw_button_pressed[button_index] = false;
            xSemaphoreGive(app->lock);
        }
        factory_app_restore_led(app);
    }
}

static void factory_register_buttons(factory_demo_app_t *app)
{
    ESP_ERROR_CHECK(bsp_iot_button_create(app->buttons, &app->button_count, BSP_BUTTON_NUM));
    for (int i = 0; i < app->button_count; i++) {
        iot_button_register_cb(app->buttons[i], BUTTON_PRESS_DOWN, NULL, factory_button_event_cb, (void *)(intptr_t)i);
        iot_button_register_cb(app->buttons[i], BUTTON_PRESS_UP, NULL, factory_button_event_cb, (void *)(intptr_t)i);
        iot_button_register_cb(app->buttons[i], BUTTON_SINGLE_CLICK, NULL, factory_button_event_cb, (void *)(intptr_t)i);
        iot_button_register_cb(app->buttons[i], BUTTON_DOUBLE_CLICK, NULL, factory_button_event_cb, (void *)(intptr_t)i);
        iot_button_register_cb(app->buttons[i], BUTTON_PRESS_END, NULL, factory_button_event_cb, (void *)(intptr_t)i);
    }
    ESP_LOGI(TAG, "Registered %d board buttons", app->button_count);
}

static void factory_style_card(lv_obj_t *obj, lv_color_t color)
{
    lv_obj_set_style_radius(obj, UI_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_grad_color(obj, color, 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, UI_BORDER_WIDTH, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x324B66), 0);
    lv_obj_set_style_shadow_width(obj, 18, 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_black(), 0);
    lv_obj_set_style_pad_all(obj, 18, 0);
}

static void factory_style_compact_card(lv_obj_t *obj, lv_color_t color)
{
    factory_style_card(obj, color);
    lv_obj_set_style_shadow_width(obj, 14, 0);
    lv_obj_set_style_pad_all(obj, UI_COMPACT_CARD_PAD, 0);
}

static lv_obj_t *factory_create_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                                     const char *title, const char *subtitle, lv_color_t color)
{
    const lv_font_t *title_font = (h <= 96 && w < 320) ? &lv_font_montserrat_18 : &lv_font_montserrat_24;
    const lv_font_t *subtitle_font = &lv_font_montserrat_14;
    lv_coord_t subtitle_width = (h <= 96 && w > 260) ? (w - 230) : (w - 36);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    factory_style_card(card, color);

    lv_obj_t *title_label = lv_label_create(card);
    lv_obj_set_style_text_font(title_label, title_font, 0);
    lv_obj_set_style_text_color(title_label, COLOR_TEXT, 0);
    lv_label_set_text(title_label, title);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    if (subtitle) {
        lv_obj_t *subtitle_label = lv_label_create(card);
        lv_obj_set_width(subtitle_label, subtitle_width);
        lv_obj_set_style_text_font(subtitle_label, subtitle_font, 0);
        lv_obj_set_style_text_color(subtitle_label, COLOR_MUTED, 0);
        lv_label_set_text(subtitle_label, subtitle);
        lv_obj_align_to(subtitle_label, title_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
    }

    return card;
}

static lv_obj_t *factory_create_compact_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                                             const char *title, lv_color_t color)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    factory_style_compact_card(card, color);

    lv_obj_t *title_label = lv_label_create(card);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title_label, COLOR_TEXT, 0);
    lv_label_set_text(title_label, title);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    return card;
}

static void on_audio_button(lv_event_t *e)
{
    factory_demo_app_t *app = (factory_demo_app_t *)lv_event_get_user_data(e);
    factory_audio_snapshot_t audio = {0};
    factory_audio_get_snapshot(&app->audio, &audio);

    esp_err_t ret = ESP_OK;
    if (audio.state == FACTORY_AUDIO_STATE_IDLE) {
        app->audio_play_pending = false;
        ret = factory_audio_start_record(&app->audio);
    } else if (audio.state == FACTORY_AUDIO_STATE_RECORDING) {
        app->audio_play_pending = true;
        ret = factory_audio_stop(&app->audio);
    } else {
        app->audio_play_pending = false;
        ret = factory_audio_stop(&app->audio);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Audio button action failed (%s)", esp_err_to_name(ret));
    }
}

static void on_sd_probe_button(lv_event_t *e)
{
    factory_app_probe_sdcard((factory_demo_app_t *)lv_event_get_user_data(e));
}

static void on_coffee_demo_button(lv_event_t *e)
{
    factory_show_coffee_screen((factory_demo_app_t *)lv_event_get_user_data(e));
}

static void on_button_led_clicked(lv_event_t *e)
{
    int button_index = (int)(intptr_t)lv_event_get_user_data(e);
    factory_demo_app_t *app = s_app;

    if (app && button_index >= 0 && button_index < UI_HW_BUTTON_COUNT) {
        bool pressed[UI_HW_BUTTON_COUNT] = {0};
        xSemaphoreTake(app->lock, portMAX_DELAY);
        app->active_led_button = button_index;
        memcpy(pressed, app->hw_button_pressed, sizeof(pressed));
        xSemaphoreGive(app->lock);
        factory_update_button_led_ui(app, pressed, button_index);
    }

    factory_app_set_led_by_button(s_app, button_index, "UI");
    factory_update_button_led_ui_now(s_app);
}

static void on_factory_back_button(lv_event_t *e)
{
    factory_show_factory_screen((factory_demo_app_t *)lv_event_get_user_data(e));
}

static lv_obj_t *factory_create_action_button(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                                              const char *text, lv_event_cb_t cb, void *user_data, lv_color_t color)
{
    const lv_font_t *button_font = (w < 96) ? &lv_font_montserrat_14 : &lv_font_montserrat_18;

    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, button_font, 0);
    lv_obj_set_style_text_color(label, COLOR_TEXT, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

static lv_obj_t *factory_create_button_led_key(lv_obj_t *parent, int index, lv_coord_t x, lv_coord_t y)
{
    const factory_button_led_map_t *map = &BUTTON_LED_MAP[index];
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, 108, 34);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A2D43), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, map->ui_color, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn, map->ui_color, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x344D67), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn, lv_color_white(), LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, on_button_led_clicked, LV_EVENT_PRESSED, (void *)(intptr_t)index);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, COLOR_TEXT, 0);
    lv_label_set_text(label, map->button_name);
    lv_obj_center(label);

    return btn;
}

static void factory_ui_refresh(lv_timer_t *timer)
{
    factory_ui_refresh_now((factory_demo_app_t *)lv_timer_get_user_data(timer));
}

static void factory_update_button_led_ui(factory_demo_app_t *app, const bool pressed[UI_HW_BUTTON_COUNT],
                                         int active_led_button)
{
    if (!app || !pressed) {
        return;
    }

    for (int i = 0; i < UI_HW_BUTTON_COUNT; i++) {
        if (!app->button_led_buttons[i]) {
            continue;
        }
        if (pressed[i] || active_led_button == i) {
            lv_obj_add_state(app->button_led_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(app->button_led_buttons[i], LV_STATE_CHECKED);
        }
    }
}

static void factory_update_button_led_ui_now(factory_demo_app_t *app)
{
    if (!app || !app->disp || !app->button_status_label) {
        return;
    }
    if (!bsp_display_lock(0)) {
        return;
    }
    if (lv_disp_get_scr_act(app->disp) != app->factory_screen) {
        bsp_display_unlock();
        return;
    }

    bool pressed[UI_HW_BUTTON_COUNT];
    int active_led_button;
    char button_status[UI_STATUS_TEXT_LEN];
    char led_name[UI_LED_NAME_LEN];

    xSemaphoreTake(app->lock, portMAX_DELAY);
    memcpy(pressed, app->hw_button_pressed, sizeof(pressed));
    active_led_button = app->active_led_button;
    strlcpy(button_status, app->button_status, sizeof(button_status));
    strlcpy(led_name, app->led_name, sizeof(led_name));
    xSemaphoreGive(app->lock);

    factory_update_button_led_ui(app, pressed, active_led_button);
    lv_label_set_text_fmt(app->button_status_label, "%s | LED: %s", button_status, led_name);

    bsp_display_unlock();
}

static void factory_ui_refresh_now(factory_demo_app_t *app)
{
    if (!app || lv_disp_get_scr_act(app->disp) != app->factory_screen) {
        return;
    }

    factory_audio_snapshot_t audio = {0};
    factory_camera_snapshot_t camera = {0};
    factory_usb_hid_snapshot_t usb_hid = {0};
    char button_status[UI_STATUS_TEXT_LEN];
    char sd_status[UI_STATUS_TEXT_LEN];
    char led_name[UI_LED_NAME_LEN];
    bool hw_button_pressed[UI_HW_BUTTON_COUNT];
    int active_led_button;

    factory_audio_get_snapshot(&app->audio, &audio);
    factory_camera_get_snapshot(&app->camera, &camera);
    factory_usb_hid_get_snapshot(&app->usb_hid, &usb_hid);

    xSemaphoreTake(app->lock, portMAX_DELAY);
    strlcpy(button_status, app->button_status, sizeof(button_status));
    strlcpy(sd_status, app->sd_status, sizeof(sd_status));
    strlcpy(led_name, app->led_name, sizeof(led_name));
    memcpy(hw_button_pressed, app->hw_button_pressed, sizeof(hw_button_pressed));
    active_led_button = app->active_led_button;
    xSemaphoreGive(app->lock);

    lv_label_set_text_fmt(app->camera_status_label,
                          "CAM: %s\n%"PRIu32"x%"PRIu32,
                          factory_camera_short_status(&camera),
                          camera.frame_width ? camera.frame_width : UI_CAMERA_FRAME_WIDTH,
                          camera.frame_height ? camera.frame_height : UI_CAMERA_FRAME_HEIGHT);

    if (app->audio_play_pending && audio.state == FACTORY_AUDIO_STATE_IDLE && audio.recorded_size > 0) {
        app->audio_play_pending = false;
        factory_audio_start_playback(&app->audio);
        factory_audio_get_snapshot(&app->audio, &audio);
    }

    const char *audio_btn_text = "Record";
    if (audio.state == FACTORY_AUDIO_STATE_RECORDING || audio.state == FACTORY_AUDIO_STATE_PLAYING) {
        audio_btn_text = "Stop";
    } else if (audio.state == FACTORY_AUDIO_STATE_ERROR) {
        audio_btn_text = "Reset";
    }
    lv_label_set_text(app->audio_btn_label, audio_btn_text);
    lv_label_set_text(app->audio_status_label, factory_audio_state_name(audio.state));
    int audio_pct = 0;
    if (audio.state == FACTORY_AUDIO_STATE_RECORDING && audio.capacity > 0) {
        audio_pct = (int)((audio.recorded_size * 100) / audio.capacity);
        if (audio_pct > 100) audio_pct = 100;
    }
    lv_bar_set_value(app->audio_progress, audio_pct, LV_ANIM_OFF);

    const char *sd_state = factory_sd_short_status(sd_status);
    lv_color_t sd_color;
    if (sd_state[0] == 'O') {
        sd_color = COLOR_STATE_OK;
    } else if (sd_state[0] == 'E') {
        sd_color = COLOR_STATE_ERR;
    } else {
        sd_color = COLOR_STATE_IDLE;
    }
    lv_obj_set_style_bg_color(app->storage_card, sd_color, 0);
    lv_obj_set_style_bg_grad_color(app->storage_card, sd_color, 0);
    if (app->storage_status_label) {
        lv_label_set_text(app->storage_status_label, sd_status);
    }

    lv_color_t usb_color;
    switch (usb_hid.state) {
    case FACTORY_USB_HID_STATE_CONNECTED:
        usb_color = COLOR_STATE_OK; break;
    case FACTORY_USB_HID_STATE_WAITING:
        usb_color = COLOR_STATE_WARN; break;
    case FACTORY_USB_HID_STATE_ERROR:
        usb_color = COLOR_STATE_ERR; break;
    default:
        usb_color = COLOR_STATE_IDLE; break;
    }
    lv_obj_set_style_bg_color(app->usb_hid_card, usb_color, 0);
    lv_obj_set_style_bg_grad_color(app->usb_hid_card, usb_color, 0);
    lv_label_set_text_fmt(app->usb_hid_status_label, "%s: %s", usb_hid.device, usb_hid.status);
    lv_label_set_text_fmt(app->usb_hid_input_label, "Input:%"PRIu32"  %s",
                          usb_hid.input_count,
                          usb_hid.last_input);

    factory_update_button_led_ui(app, hw_button_pressed, active_led_button);

    lv_label_set_text_fmt(app->button_status_label, "%s | LED: %s", button_status, led_name);
}

static void factory_show_coffee_screen(factory_demo_app_t *app)
{
    if (!app || !ui_Screen_Splash_screen) {
        ESP_LOGW(TAG, "Coffee UI is not ready");
        return;
    }

    xSemaphoreTake(app->lock, portMAX_DELAY);
    if (app->coffee_transition_pending) {
        xSemaphoreGive(app->lock);
        return;
    }
    app->coffee_transition_pending = true;
    xSemaphoreGive(app->lock);

    BaseType_t ok = xTaskCreate(factory_switch_to_coffee_task, "factory_to_coffee",
                                UI_SCREEN_SWITCH_TASK_STACK_SIZE, app,
                                UI_SCREEN_SWITCH_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        xSemaphoreTake(app->lock, portMAX_DELAY);
        app->coffee_transition_pending = false;
        xSemaphoreGive(app->lock);
        ESP_LOGE(TAG, "Create Coffee switch task failed");
    }
}

static void factory_show_factory_screen(factory_demo_app_t *app)
{
    if (!app || !app->factory_screen) {
        ESP_LOGW(TAG, "Factory screen is not ready");
        return;
    }

    ESP_LOGI(TAG, "Switching to Factory demo");
    ESP_ERROR_CHECK_WITHOUT_ABORT(factory_camera_set_render_enabled(&app->camera, true));
    lv_screen_load_anim(app->factory_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

static void factory_switch_to_coffee_task(void *arg)
{
    factory_demo_app_t *app = (factory_demo_app_t *)arg;

    ESP_LOGI(TAG, "Switching to Coffee demo");
    ESP_ERROR_CHECK_WITHOUT_ABORT(factory_camera_set_render_enabled(&app->camera, false));

    if (bsp_display_lock(-1)) {
        lv_screen_load_anim(ui_Screen_Splash_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "LVGL lock failed while switching to Coffee demo");
    }

    xSemaphoreTake(app->lock, portMAX_DELAY);
    app->coffee_transition_pending = false;
    xSemaphoreGive(app->lock);

    vTaskDelete(NULL);
}

static void factory_add_coffee_back_button(lv_obj_t *screen, factory_demo_app_t *app)
{
    if (!screen) {
        return;
    }

    lv_obj_t *btn = lv_btn_create(screen);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(btn, 120, 40);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFF8A5B), 0);
    lv_obj_add_event_cb(btn, on_factory_back_button, LV_EVENT_CLICKED, app);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, "Factory");
    lv_obj_center(label);
    lv_obj_move_foreground(btn);
}

static void factory_add_coffee_back_buttons(factory_demo_app_t *app)
{
    factory_add_coffee_back_button(ui_Screen_Splash_screen, app);
    factory_add_coffee_back_button(ui_Screen_Home, app);
    factory_add_coffee_back_button(ui_Screen_Home_ristretto, app);
    factory_add_coffee_back_button(ui_Screen_Home_espresso, app);
    factory_add_coffee_back_button(ui_Screen_Customize, app);
    factory_add_coffee_back_button(ui_Screen_Customize2, app);
    factory_add_coffee_back_button(ui_Screen_Brewing, app);
    factory_add_coffee_back_button(ui_Screen_Brewing_Done, app);
    factory_add_coffee_back_button(ui_Screen_Settings, app);
    factory_add_coffee_back_button(ui_Screen_Add_water, app);
    factory_add_coffee_back_button(ui_Screen_Add_coffee, app);
    factory_add_coffee_back_button(ui_Screen_Ready, app);
}

static void factory_ui_create(factory_demo_app_t *app)
{
    lv_obj_t *scr = lv_disp_get_scr_act(app->disp);
    app->factory_screen = scr;
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *orb_a = lv_obj_create(scr);
    lv_obj_remove_style_all(orb_a);
    lv_obj_set_size(orb_a, 260, 260);
    lv_obj_set_pos(orb_a, -60, -40);
    lv_obj_set_style_radius(orb_a, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(orb_a, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(orb_a, LV_OPA_20, 0);

    lv_obj_t *orb_b = lv_obj_create(scr);
    lv_obj_remove_style_all(orb_b);
    lv_obj_set_size(orb_b, 220, 220);
    lv_obj_set_pos(orb_b, 620, 290);
    lv_obj_set_style_radius(orb_b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(orb_b, COLOR_SKY, 0);
    lv_obj_set_style_bg_opa(orb_b, LV_OPA_20, 0);

    lv_obj_t *header = factory_create_card(scr, 24, 20, 752, 72,
                                           "Korvo Factory Demo",
                                           "LCD / Camera / Audio / LED / SD / Buttons",
                                           COLOR_PANEL);
    lv_obj_t *header_chip = lv_btn_create(header);
    lv_obj_set_size(header_chip, 174, 34);
    lv_obj_align(header_chip, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_radius(header_chip, 17, 0);
    lv_obj_set_style_bg_color(header_chip, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(header_chip, LV_OPA_90, 0);
    lv_obj_set_style_border_width(header_chip, 0, 0);
    lv_obj_set_style_pad_all(header_chip, 0, 0);
    lv_obj_set_style_shadow_width(header_chip, 0, 0);
    lv_obj_add_event_cb(header_chip, on_coffee_demo_button, LV_EVENT_CLICKED, app);
    lv_obj_t *chip_label = lv_label_create(header_chip);
    lv_obj_set_style_text_color(chip_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(chip_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(chip_label, "Coffee Demo");
    lv_obj_center(chip_label);

    lv_obj_t *camera_card = lv_obj_create(scr);
    lv_obj_set_pos(camera_card, 24, UI_MAIN_ROW_Y);
    lv_obj_set_size(camera_card, 484, UI_MAIN_ROW_H);
    lv_obj_clear_flag(camera_card, LV_OBJ_FLAG_SCROLLABLE);
    factory_style_card(camera_card, COLOR_PANEL_CAMERA);
    lv_obj_set_style_shadow_width(camera_card, 10, 0);
    lv_obj_set_style_pad_all(camera_card, 8, 0);

    lv_obj_t *camera_frame = lv_obj_create(camera_card);
    lv_obj_set_pos(camera_frame, 0, 0);
    lv_obj_set_size(camera_frame, UI_CAMERA_FRAME_WIDTH, UI_CAMERA_FRAME_HEIGHT);
    lv_obj_clear_flag(camera_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(camera_frame, 20, 0);
    lv_obj_set_style_bg_color(camera_frame, lv_color_hex(0x05080E), 0);
    lv_obj_set_style_bg_opa(camera_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(camera_frame, lv_color_hex(0x2D405A), 0);
    lv_obj_set_style_border_width(camera_frame, 1, 0);
    lv_obj_set_style_pad_all(camera_frame, 0, 0);

    lv_obj_t *camera_frame_tag = lv_obj_create(camera_frame);
    lv_obj_set_size(camera_frame_tag, 88, 26);
    lv_obj_align(camera_frame_tag, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_radius(camera_frame_tag, 13, 0);
    lv_obj_set_style_bg_color(camera_frame_tag, lv_color_hex(0x1A2E48), 0);
    lv_obj_set_style_bg_opa(camera_frame_tag, LV_OPA_80, 0);
    lv_obj_set_style_border_width(camera_frame_tag, 0, 0);
    lv_obj_set_style_pad_all(camera_frame_tag, 0, 0);

    lv_obj_t *camera_frame_tag_label = lv_label_create(camera_frame_tag);
    lv_obj_set_style_text_font(camera_frame_tag_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(camera_frame_tag_label, COLOR_TEXT, 0);
    lv_label_set_text(camera_frame_tag_label, "240 preview");
    lv_obj_center(camera_frame_tag_label);

    app->camera_canvas = lv_canvas_create(camera_frame);
    lv_canvas_set_draw_buf(app->camera_canvas, &app->camera_canvas_draw_buf);
    lv_canvas_fill_bg(app->camera_canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_center(app->camera_canvas);
    lv_obj_move_foreground(camera_frame_tag);

    lv_obj_t *camera_title = lv_label_create(camera_card);
    lv_obj_set_style_text_font(camera_title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(camera_title, COLOR_TEXT, 0);
    lv_label_set_text(camera_title, "Camera");
    lv_obj_set_pos(camera_title, 258, 4);

    lv_obj_t *camera_note = lv_label_create(camera_card);
    lv_obj_set_width(camera_note, 190);
    lv_obj_set_style_text_font(camera_note, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(camera_note, COLOR_MUTED, 0);
    lv_label_set_long_mode(camera_note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(camera_note, "Auto camera, 240 x 240 preview");
    lv_obj_set_pos(camera_note, 258, 34);

    app->camera_status_label = lv_label_create(camera_card);
    lv_obj_set_width(app->camera_status_label, 190);
    lv_obj_set_style_text_font(app->camera_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(app->camera_status_label, COLOR_TEXT, 0);
    lv_label_set_long_mode(app->camera_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(app->camera_status_label, 258, 72);
    lv_label_set_text(app->camera_status_label, "");

    lv_obj_t *audio_card = factory_create_card(scr, UI_RIGHT_COL_X, UI_MAIN_ROW_Y, UI_RIGHT_COL_W, 160,
                                               "Audio Test",
                                               NULL,
                                               COLOR_PANEL_AUDIO);
    lv_obj_set_style_shadow_width(audio_card, 10, 0);
    app->audio_progress = lv_bar_create(audio_card);
    lv_obj_set_size(app->audio_progress, 216, 14);
    lv_obj_set_pos(app->audio_progress, 0, 32);
    lv_bar_set_range(app->audio_progress, 0, 100);
    lv_bar_set_value(app->audio_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(app->audio_progress, lv_color_hex(0x1E3248), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(app->audio_progress, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(app->audio_progress, COLOR_ACCENT, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(app->audio_progress, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(app->audio_progress, 7, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    lv_obj_t *audio_btn = factory_create_action_button(audio_card, 0, 54, 216, UI_AUDIO_BUTTON_H,
                                                       "Record", on_audio_button, app, COLOR_ACCENT);
    app->audio_btn_label = lv_obj_get_child(audio_btn, 0);

    app->audio_status_label = lv_label_create(audio_card);
    lv_obj_set_style_text_font(app->audio_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(app->audio_status_label, COLOR_MUTED, 0);
    lv_label_set_text(app->audio_status_label, "Idle");
    lv_obj_set_pos(app->audio_status_label, 0, 100);

    app->storage_card = factory_create_card(scr, UI_RIGHT_COL_X, 286, UI_RIGHT_COL_W, 82,
                                            "Storage",
                                            NULL,
                                            COLOR_PANEL_STORAGE);
    lv_obj_set_style_shadow_width(app->storage_card, 10, 0);
    app->storage_status_label = lv_label_create(app->storage_card);
    lv_obj_set_width(app->storage_status_label, 136);
    lv_obj_set_height(app->storage_status_label, 24);
    lv_obj_set_style_text_font(app->storage_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(app->storage_status_label, COLOR_TEXT, 0);
    lv_label_set_long_mode(app->storage_status_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(app->storage_status_label, "Not checked");
    lv_obj_set_pos(app->storage_status_label, 0, 34);
    factory_create_action_button(app->storage_card, 148, 22, 78, 34, "Probe", on_sd_probe_button, app, COLOR_SKY);

    lv_obj_t *button_led_card = factory_create_compact_card(scr, 24, UI_BOTTOM_ROW_Y, 486, UI_BOTTOM_ROW_H,
                                                            "Buttons / LED",
                                                            COLOR_PANEL_BUTTONS);
    lv_obj_set_style_shadow_width(button_led_card, 10, 0);
    app->button_led_buttons[0] = factory_create_button_led_key(button_led_card, 0, 0, 24);
    app->button_led_buttons[1] = factory_create_button_led_key(button_led_card, 1, 116, 24);
    app->button_led_buttons[2] = factory_create_button_led_key(button_led_card, 2, 232, 24);
    app->button_led_buttons[3] = factory_create_button_led_key(button_led_card, 3, 348, 24);

    app->button_status_label = lv_label_create(button_led_card);
    lv_obj_set_width(app->button_status_label, 456);
    lv_obj_set_height(app->button_status_label, 20);
    lv_obj_set_style_text_font(app->button_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(app->button_status_label, COLOR_MUTED, 0);
    lv_label_set_long_mode(app->button_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(app->button_status_label, 0, 60);

    app->usb_hid_card = factory_create_compact_card(scr, 526, UI_BOTTOM_ROW_Y, 250, UI_BOTTOM_ROW_H,
                                                    "USB HID",
                                                    COLOR_PANEL_USB);
    lv_obj_set_style_shadow_width(app->usb_hid_card, 10, 0);
    app->usb_hid_status_label = lv_label_create(app->usb_hid_card);
    lv_obj_set_width(app->usb_hid_status_label, 226);
    lv_obj_set_height(app->usb_hid_status_label, 22);
    lv_obj_set_style_text_font(app->usb_hid_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(app->usb_hid_status_label, COLOR_TEXT, 0);
    lv_label_set_long_mode(app->usb_hid_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(app->usb_hid_status_label, 0, 26);

    app->usb_hid_input_label = lv_label_create(app->usb_hid_card);
    lv_obj_set_width(app->usb_hid_input_label, 226);
    lv_obj_set_height(app->usb_hid_input_label, 22);
    lv_obj_set_style_text_font(app->usb_hid_input_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(app->usb_hid_input_label, COLOR_MUTED, 0);
    lv_label_set_long_mode(app->usb_hid_input_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(app->usb_hid_input_label, 0, 52);

    lv_timer_create(factory_ui_refresh, UI_REFRESH_PERIOD_MS, app);
    factory_ui_refresh_now(app);
}

static esp_err_t factory_wait_camera_first_frame(factory_camera_t *camera, uint32_t timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        factory_camera_snapshot_t snapshot = {0};
        factory_camera_get_snapshot(camera, &snapshot);
        if (snapshot.frame_count > 0) {
            ESP_LOGI(TAG, "Camera first frame ready before UI init");
            return ESP_OK;
        }
        if (snapshot.state == FACTORY_CAMERA_STATE_ERROR) {
            ESP_LOGE(TAG, "Camera entered error state before first frame");
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGW(TAG, "Camera first frame not ready after %"PRIu32" ms", timeout_ms);
    return ESP_ERR_TIMEOUT;
}

void app_main(void)
{
    static factory_demo_app_t app;
    const size_t camera_canvas_buf_size = LV_DRAW_BUF_SIZE(UI_CAMERA_FRAME_WIDTH, UI_CAMERA_FRAME_HEIGHT, LV_COLOR_FORMAT_RGB565);
    memset(&app, 0, sizeof(app));
    s_app = &app;

    app.lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(app.lock ? ESP_OK : ESP_ERR_NO_MEM);

    strlcpy(app.button_status, "Idle", sizeof(app.button_status));
    strlcpy(app.sd_status, "Not checked", sizeof(app.sd_status));
    strlcpy(app.led_name, "Off", sizeof(app.led_name));
    app.active_led_button = -1;

    ESP_LOGI(TAG, "Starting integrated factory demo");

    bsp_display_config_t display_cfg = BSP_DISPLAY_DEFAULT_CONFIG();
    display_cfg.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
    display_cfg.buffer_height = UI_DISPLAY_BUFFER_HEIGHT;
    display_cfg.task_stack_size = UI_DISPLAY_TASK_STACK_SIZE;
    display_cfg.enable_ppa_accel = UI_DISPLAY_ENABLE_PPA_ACCEL;

    /* Initialize camera before LVGL touch so camera owns the shared I2C setup first. */
    ESP_ERROR_CHECK(factory_camera_init(&app.camera, NULL));
    esp_err_t camera_ret = factory_camera_start_preview(&app.camera);
    if (camera_ret != ESP_OK) {
        ESP_LOGW(TAG, "Camera start failed (%s)", esp_err_to_name(camera_ret));
    } else {
        (void)factory_wait_camera_first_frame(&app.camera, UI_CAMERA_FIRST_FRAME_TIMEOUT_MS);
    }

    app.disp = bsp_display_start_with_config(&display_cfg);
    if (!app.disp) {
        ESP_LOGE(TAG, "Display start failed");
        return;
    }
    ESP_ERROR_CHECK(coffee_assets_init());

    ESP_ERROR_CHECK(bsp_led_init());
    factory_register_buttons(&app);
    ESP_ERROR_CHECK(factory_audio_init(&app.audio));
    ESP_ERROR_CHECK(factory_camera_set_display(&app.camera, app.disp));
    ESP_ERROR_CHECK(factory_usb_hid_init(&app.usb_hid));
    app.camera_canvas_buf = heap_caps_aligned_calloc(64, 1, camera_canvas_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(app.camera_canvas_buf ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(lv_draw_buf_init(&app.camera_canvas_draw_buf,
                                     UI_CAMERA_FRAME_WIDTH,
                                     UI_CAMERA_FRAME_HEIGHT,
                                     LV_COLOR_FORMAT_RGB565,
                                     LV_STRIDE_AUTO,
                                     app.camera_canvas_buf,
                                     camera_canvas_buf_size) == LV_RESULT_OK ? ESP_OK : ESP_FAIL);
    factory_app_set_led_status(&app, 0, 0, 0, "Off");

    if (bsp_display_lock(-1)) {
        factory_ui_create(&app);
        ESP_ERROR_CHECK(factory_camera_attach_canvas(&app.camera, app.camera_canvas, app.camera_canvas_buf,
                                                     UI_CAMERA_FRAME_WIDTH, UI_CAMERA_FRAME_HEIGHT,
                                                     app.camera_canvas_draw_buf.header.stride));
        ui_init();
        factory_add_coffee_back_buttons(&app);
        lv_disp_load_scr(app.factory_screen);
        factory_ui_refresh_now(&app);
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL lock failed");
        return;
    }

    ESP_LOGI(TAG, "Factory demo is ready");
}
