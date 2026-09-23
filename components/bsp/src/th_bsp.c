/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "th_bsp.h"

#include <string.h>

#include "bsp/display.h"
#include "bsp/esp32_s31_korvo.h"
#include "esp_lv_adapter.h"

static const char *TAG = "bsp";

/* Same numbers as the verified factory_demo baseline (AGENT.md 2.2). */
#define TH_BSP_DISPLAY_BUFFER_HEIGHT 160
#define TH_BSP_DISPLAY_TASK_STACK    (16 * 1024)
#define TH_BSP_DISPLAY_PPA_ACCEL     true

static bool s_ready;
static lv_display_t *s_display;
static lv_obj_t *s_dim;          /* software dimming overlay on lv_layer_top() */
static uint8_t s_brightness = 100;

/* ------------------------------------------------------------------------- */
/* Locking                                                                   */
/* ------------------------------------------------------------------------- */

bool th_bsp_lock(int32_t timeout_ms)
{
    /* esp_lv_adapter_lock() uses a recursive mutex, so nesting is allowed. */
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

void th_bsp_unlock(void)
{
    esp_lv_adapter_unlock();
}

/* ------------------------------------------------------------------------- */
/* Brightness overlay                                                        */
/* ------------------------------------------------------------------------- */

static esp_err_t dim_ensure_locked(void)
{
    if (s_dim != NULL) {
        return ESP_OK;
    }

    s_dim = lv_obj_create(lv_layer_top());
    if (s_dim == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_remove_style_all(s_dim);
    lv_obj_set_size(s_dim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_dim, 0, 0);
    lv_obj_set_style_bg_color(s_dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dim, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dim, 0, 0);
    lv_obj_set_style_radius(s_dim, 0, 0);
    lv_obj_set_style_pad_all(s_dim, 0, 0);
    /* Must stay transparent to touch so the dimmed UI is still usable. */
    lv_obj_remove_flag(s_dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_dim, LV_OBJ_FLAG_SCROLLABLE);
    return ESP_OK;
}

static esp_err_t apply_brightness_locked(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_brightness = percent;

    esp_err_t err = dim_ensure_locked();
    if (err != ESP_OK) {
        return err;
    }

    /* 100 % -> fully transparent, 0 % -> fully black. */
    const uint8_t opa = (uint8_t)(((100 - percent) * (int)LV_OPA_COVER) / 100);
    lv_obj_set_style_bg_opa(s_dim, opa, 0);
    return ESP_OK;
}

esp_err_t th_bsp_set_brightness(uint8_t percent)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!th_bsp_lock(-1)) {
        return ESP_ERR_TIMEOUT;
    }
    /* The overlay is also the fallback when the panel has no backlight control
     * (BSP_LCD_BACKLIGHT is GPIO_NUM_NC on this board). */
    esp_err_t err = apply_brightness_locked(percent);
    th_bsp_unlock();

    if (err == ESP_OK) {
        sys_events_post(SYS_EVENT_BRIGHTNESS, &percent, sizeof(percent));
    }
    return err;
}

uint8_t th_bsp_get_brightness(void)
{
    return s_brightness;
}

esp_err_t th_bsp_restore_brightness(void)
{
    int32_t stored = sys_config_get_i32_or(SYS_KEY_BRIGHTNESS, 100);
    if (stored < 0) {
        stored = 0;
    }
    if (stored > 100) {
        stored = 100;
    }
    return th_bsp_set_brightness((uint8_t)stored);
}

/* ------------------------------------------------------------------------- */
/* Init                                                                      */
/* ------------------------------------------------------------------------- */

esp_err_t th_bsp_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    bsp_display_config_t cfg = BSP_DISPLAY_DEFAULT_CONFIG();
    cfg.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
    cfg.buffer_height = TH_BSP_DISPLAY_BUFFER_HEIGHT;
    cfg.task_stack_size = TH_BSP_DISPLAY_TASK_STACK;
    cfg.enable_ppa_accel = TH_BSP_DISPLAY_PPA_ACCEL;
    cfg.enable_touch = true;

    s_display = bsp_display_start_with_config(&cfg);
    if (s_display == NULL) {
        SYS_LOGE(TAG, "display bring-up failed");
        return ESP_FAIL;
    }

    s_ready = true;

    /* LED and buttons are conveniences: a failure must not stop the UI. */
    if (bsp_led_init() != ESP_OK) {
        SYS_LOGW(TAG, "status LED unavailable");
    }
    if (th_bsp_buttons_start() != ESP_OK) {
        SYS_LOGW(TAG, "buttons unavailable");
    }
    if (th_bsp_idle_init() != ESP_OK) {
        SYS_LOGW(TAG, "idle watchdog unavailable");
    }

    (void)th_bsp_restore_brightness();

    SYS_LOGI(TAG, "ready: %dx%d, tear-avoid=triple-partial, ppa=%d, brightness=%u%%",
             BSP_LCD_H_RES, BSP_LCD_V_RES, (int)TH_BSP_DISPLAY_PPA_ACCEL, s_brightness);
    return ESP_OK;
}

lv_display_t *th_bsp_display(void)
{
    return s_display;
}
