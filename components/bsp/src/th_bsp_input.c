/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "th_bsp.h"

#include "bsp/esp32_s31_korvo.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "iot_button.h"

static const char *TAG = "bsp_input";

/** Idle timeout used when the NVS key is absent (FR6: 30 s). */
#define TH_BSP_IDLE_TIMEOUT_DEFAULT_S 30

/** How often the idle state is re-evaluated. */
#define TH_BSP_IDLE_POLL_US (500 * 1000)

static button_handle_t s_buttons[BSP_BUTTON_NUM];
static int s_button_count;

static volatile uint32_t s_last_activity_ms;
static bool s_idle;
static uint32_t s_idle_timeout_ms = TH_BSP_IDLE_TIMEOUT_DEFAULT_S * 1000;
static esp_timer_handle_t s_idle_timer;

/* ------------------------------------------------------------------------- */
/* Buttons                                                                   */
/* ------------------------------------------------------------------------- */

/**
 * iot_button callbacks run outside the UI task, so they only translate the
 * hardware event into a project event. Nothing here may touch LVGL.
 */
static void button_event_cb(void *button_handle, void *usr_data)
{
    const int index = (int)(intptr_t)usr_data;
    if (index < 0 || index >= BSP_BUTTON_NUM) {
        return;
    }

    const button_event_t event = iot_button_get_event((button_handle_t)button_handle);

    sys_button_event_t payload = {
        .id = (sys_button_id_t)index,
        .action = SYS_BUTTON_PRESS,
    };

    if (event == BUTTON_LONG_PRESS_START || event == BUTTON_LONG_PRESS_HOLD) {
        payload.action = SYS_BUTTON_LONG_PRESS;
    }

    th_bsp_report_activity();

    /* Only the two "logical" events are forwarded; press/release edges would
     * spam the shared event queue for no benefit. */
    if (event == BUTTON_SINGLE_CLICK || event == BUTTON_LONG_PRESS_START) {
        SYS_LOGD(TAG, "button %d action %d", payload.id, payload.action);
        sys_events_post(SYS_EVENT_BUTTON, &payload, sizeof(payload));
    }
}

esp_err_t th_bsp_buttons_start(void)
{
    if (s_button_count > 0) {
        return ESP_OK;
    }

    esp_err_t err = bsp_iot_button_create(s_buttons, &s_button_count, BSP_BUTTON_NUM);
    if (err != ESP_OK) {
        return err;
    }
    if (s_button_count <= 0) {
        return ESP_ERR_NOT_FOUND;
    }

    for (int i = 0; i < s_button_count; i++) {
        if (s_buttons[i] == NULL) {
            continue;
        }
        void *arg = (void *)(intptr_t)i;
        iot_button_register_cb(s_buttons[i], BUTTON_SINGLE_CLICK, NULL, button_event_cb, arg);
        iot_button_register_cb(s_buttons[i], BUTTON_LONG_PRESS_START, NULL, button_event_cb, arg);
    }

    SYS_LOGI(TAG, "%d ADC buttons registered", s_button_count);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Idle tracking                                                             */
/* ------------------------------------------------------------------------- */

void th_bsp_report_activity(void)
{
    s_last_activity_ms = (uint32_t)sys_time_ms();

    if (s_idle) {
        s_idle = false;
        sys_events_post(SYS_EVENT_WAKEUP, NULL, 0);
    }

    /* Keep the adapter's own activity tracker in sync so its auto-sleep logic
     * (and any future backlight policy) sees real user activity. */
    (void)esp_lv_adapter_report_activity();
}

uint32_t th_bsp_idle_ms(void)
{
    const uint32_t now = (uint32_t)sys_time_ms();
    return now - s_last_activity_ms;
}

bool th_bsp_is_idle(void)
{
    return s_idle;
}

void th_bsp_poll_idle(void)
{
    if (s_idle) {
        return;
    }
    if (s_idle_timeout_ms == 0) {
        return;
    }
    if (th_bsp_idle_ms() >= s_idle_timeout_ms) {
        s_idle = true;
        SYS_LOGI(TAG, "idle timeout reached after %u ms", (unsigned)s_idle_timeout_ms);
        sys_events_post(SYS_EVENT_IDLE_TIMEOUT, NULL, 0);
    }
}

static void idle_timer_cb(void *arg)
{
    (void)arg;
    th_bsp_poll_idle();
}

esp_err_t th_bsp_idle_init(void)
{
    int32_t timeout_s = sys_config_get_i32_or(SYS_KEY_IDLE_TIMEOUT, TH_BSP_IDLE_TIMEOUT_DEFAULT_S);
    if (timeout_s < 0) {
        timeout_s = 0;
    }
    if (timeout_s > 3600) {
        timeout_s = 3600;
    }
    s_idle_timeout_ms = (uint32_t)timeout_s * 1000;

    th_bsp_report_activity();

    if (s_idle_timer != NULL) {
        return ESP_OK;
    }

    const esp_timer_create_args_t args = {
        .callback = idle_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "th_idle",
    };

    esp_err_t err = esp_timer_create(&args, &s_idle_timer);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_timer_start_periodic(s_idle_timer, TH_BSP_IDLE_POLL_US);
    if (err != ESP_OK) {
        esp_timer_delete(s_idle_timer);
        s_idle_timer = NULL;
        return err;
    }

    SYS_LOGI(TAG, "idle timeout %u s", (unsigned)(s_idle_timeout_ms / 1000));
    return ESP_OK;
}

esp_err_t th_bsp_idle_stop(void)
{
    if (s_idle_timer == NULL) {
        return ESP_OK;
    }
    esp_timer_stop(s_idle_timer);
    esp_timer_delete(s_idle_timer);
    s_idle_timer = NULL;
    s_idle = false;
    return ESP_OK;
}
