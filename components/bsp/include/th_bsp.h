/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file th_bsp.h
 * @brief L1 board facade for ESP32-S31-Korvo.
 *
 * Business components must go through this facade instead of calling the
 * out-of-tree Korvo BSP directly, so the board can be swapped in one place.
 *
 * The symbol prefix is `th_bsp_` (not `bsp_`) because the out-of-tree Korvo BSP
 * already exports `bsp_display_*` / `bsp_sdcard_*` / `bsp_led_*`.
 *
 * Display bring-up goes through the existing esp_lvgl_adapter chain: ToDoHelper
 * must never re-initialise the LCD, touch, tear-avoidance or LVGL task itself.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#include "sys_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Status LED policy, mapped to colours by the facade. */
typedef enum {
    TH_BSP_LED_OFF = 0,
    TH_BSP_LED_WIFI,     /**< network activity / connected */
    TH_BSP_LED_SYNC,     /**< todo sync in progress */
    TH_BSP_LED_ERROR,    /**< failed state, needs attention */
} th_bsp_led_state_t;

/**
 * @brief Bring up display + LVGL + touch, then LED and buttons. Idempotent.
 *
 * Uses the same configuration as the factory demo baseline: 800x480 RGB panel,
 * triple-partial tear avoidance, 160 px draw buffer, 16 KB display task stack,
 * PPA acceleration enabled.
 */
esp_err_t th_bsp_init(void);

/** @brief The LVGL display created by the adapter, or NULL before th_bsp_init(). */
lv_display_t *th_bsp_display(void);

/**
 * @brief Take the LVGL adapter lock. The lock is recursive, so nesting inside a
 *        UI callback is safe.
 */
bool th_bsp_lock(int32_t timeout_ms);

/** @brief Release the LVGL adapter lock. */
void th_bsp_unlock(void);

/* ---- Brightness -------------------------------------------------------- */

/**
 * @brief Software brightness in percent (0..100).
 *
 * This board's backlight pin is NC, so real PWM dimming is impossible. The
 * facade dims with a black overlay on lv_layer_top() instead, which also keeps
 * the panel content (and the touch path) intact.
 *
 * May be called from any task; takes the LVGL lock internally.
 */
esp_err_t th_bsp_set_brightness(uint8_t percent);

/** @brief Last brightness value set, in percent. */
uint8_t th_bsp_get_brightness(void);

/** @brief Convenience: restore the user-configured active brightness. */
esp_err_t th_bsp_restore_brightness(void);

/* ---- Input ------------------------------------------------------------- */

/** @brief Register the ADC buttons; each click posts SYS_EVENT_BUTTON. */
esp_err_t th_bsp_buttons_start(void);

/**
 * @brief Start the idle watchdog (500 ms poll) that posts SYS_EVENT_IDLE_TIMEOUT.
 *
 * The timeout comes from NVS key `idle_timeout` (seconds, default 30).
 * Called by th_bsp_init(); exposed so tests can restart it after changing NVS.
 */
esp_err_t th_bsp_idle_init(void);

/** @brief Stop the idle watchdog. */
esp_err_t th_bsp_idle_stop(void);

/**
 * @brief Report user interaction. Feeds the adapter activity tracker and the
 *        project idle timer used by FR6 (auto idle after 30 s).
 */
void th_bsp_report_activity(void);

/** @brief Milliseconds since the last reported user interaction. */
uint32_t th_bsp_idle_ms(void);

/** @brief True while the idle timeout configured in NVS has elapsed. */
bool th_bsp_is_idle(void);

/** @brief Re-evaluate the idle flag; posts SYS_EVENT_IDLE_TIMEOUT on entry. */
void th_bsp_poll_idle(void);

/* ---- Status LED -------------------------------------------------------- */

esp_err_t th_bsp_led_set(th_bsp_led_state_t state);

/* ---- SD card ----------------------------------------------------------- */

/** @brief Mount the SD card. Returns ESP_OK when it is (already) mounted. */
esp_err_t th_bsp_sdcard_mount(void);

esp_err_t th_bsp_sdcard_unmount(void);

bool th_bsp_sdcard_mounted(void);

const char *th_bsp_sdcard_mount_point(void);

/* ---- Audio (lazy) ------------------------------------------------------ */

/**
 * @brief Initialise I2S + codec devices on first use.
 *
 * Audio hardware failures must not block boot, so callers treat a failure as
 * "no speaker/microphone" and degrade (FR7).
 */
esp_err_t th_bsp_audio_init(void);

/** @brief Speaker codec handle, or NULL when unavailable. */
void *th_bsp_audio_speaker(void);

/** @brief Microphone codec handle, or NULL when unavailable. */
void *th_bsp_audio_mic(void);

/** @brief True when the speaker path was successfully created. */
bool th_bsp_audio_available(void);

#ifdef __cplusplus
}
#endif
