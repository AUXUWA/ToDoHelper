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

/*
 * Display bring-up numbers.
 *
 * The RGB panel's own frame buffers live in PSRAM, but the *partial* tear-avoid
 * path allocates its LVGL draw buffer from **internal RAM** - the capability is
 * hardcoded to non-PSRAM in esp_lvgl_adapter (display_manager.c:1095):
 *
 *     draw buffer bytes = hor_res * buffer_height * 2 = 800 * H * 2
 *
 * Internal RAM on this part is only ~310 KB in total and esp_wifi_init() needs
 * one large contiguous internal block. The factory demo's H = 160 therefore ate
 * 256 KB of that 310 KB, which is fine for a demo that never starts WiFi - but
 * ToDoHelper also needs internal memory for net80211, mDNS, I2S DMA and task
 * stacks, and with H = 160 every one of them failed with ESP_ERR_NO_MEM:
 *
 *     E wifi:alloc net80211 funcs fail        -> esp_wifi_init ESP_ERR_NO_MEM
 *     E mdns_service: failed to allocate task stack
 *     E i2s_common: allocate DMA description failed
 *     E module 'ai_service' unavailable: ESP_ERR_NO_MEM
 *
 * H = 50 is the adapter's own default for the RGB profile (80 KB), leaving
 * ~215 KB for the network stack. Raising it is possible, but re-measure with the
 * heap trace in the boot log (sys_heap_log) before doing so.
 */
#define TH_BSP_DISPLAY_BUFFER_HEIGHT 50
/*
 * LVGL task stack.
 *
 * This is the same task that *renders*, because LV_DRAW_SW_DRAW_UNIT_CNT = 1
 * means LVGL has no separate draw thread. With FreeType enabled, glyph
 * rasterisation runs on this stack, and LVGL's own configuration note says
 * "If FreeType or ThorVG is enabled, it is recommended to set it to 32KB or
 * more". At 16 KB the device panicked non-deterministically as soon as the first
 * page with a FreeType font was drawn (Load access fault / Load address
 * misaligned / Stack protection fault on alternating cores).
 *
 * Note ESP_LVGL_ADAPTER_FREETYPE_SMALL_RENDER_POOL removes LVGL's build-time
 * check for exactly this, so it does not warn for you - keep the two in sync.
 */
#define TH_BSP_DISPLAY_TASK_STACK    (32 * 1024)
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
    /*
     * TRIPLE_FULL rather than TRIPLE_PARTIAL.
     *
     * In the PARTIAL modes the adapter allocates LVGL's draw buffer from
     * INTERNAL RAM (the capability is hardcoded, display_manager.c:1095):
     * hor_res * buffer_height * 2 = 800 * 50 * 2 = 80 KB. With only ~310 KB of
     * internal RAM in total and esp_wifi_init() needing a large contiguous
     * internal block, that 80 KB left internal free at 32 KB with a *largest
     * free block of 0 KB* - so SDMMC and I2S DMA allocation failed even though
     * "free" looked non-zero:
     *
     *     E sdmmc_cmd: allocate_dma_buf: not enough mem, err=0x101
     *     E i2s_common: i2s_alloc_dma_desc: allocate DMA description failed
     *
     * FULL mode reuses the panel's own PSRAM frame buffers as the LVGL draw
     * buffers, so it costs zero internal RAM and needs no extra memory. The
     * panel is already configured with fb_count = 3, which is what this mode
     * requires.
     */
    cfg.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL;
    cfg.buffer_height = TH_BSP_DISPLAY_BUFFER_HEIGHT;
    cfg.task_stack_size = TH_BSP_DISPLAY_TASK_STACK;
    cfg.enable_ppa_accel = TH_BSP_DISPLAY_PPA_ACCEL;
    cfg.enable_touch = true;

    /*
     * Take over the LVGL adapter configuration before the BSP initialises it.
     *
     * bsp_display_start_with_config() only calls esp_lv_adapter_init() when the
     * adapter is not already up, so initialising it here is the supported way to
     * choose the render task's stack placement. The BSP's own default puts it in
     * internal RAM; moving it to PSRAM keeps internal RAM for DMA.
     */
    if (!esp_lv_adapter_is_initialized()) {
        esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
        adapter_cfg.task_stack_size = TH_BSP_DISPLAY_TASK_STACK;
        adapter_cfg.stack_in_psram = true;
        esp_err_t aerr = esp_lv_adapter_init(&adapter_cfg);
        if (aerr != ESP_OK) {
            SYS_LOGE(TAG, "LVGL adapter init failed: %s", esp_err_to_name(aerr));
            return aerr;
        }
        SYS_LOGI(TAG, "LVGL adapter pre-initialised (task stack %u KB in PSRAM)",
                 (unsigned)(TH_BSP_DISPLAY_TASK_STACK / 1024));
    }

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

    SYS_LOGI(TAG, "ready: %dx%d, tear-avoid=triple-full (draw buffers in PSRAM), ppa=%d, brightness=%u%%",
             BSP_LCD_H_RES, BSP_LCD_V_RES, (int)TH_BSP_DISPLAY_PPA_ACCEL, s_brightness);
    return ESP_OK;
}

lv_display_t *th_bsp_display(void)
{
    return s_display;
}
