/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file app_main.c
 * @brief Module orchestrator. Contains no business logic.
 *
 * Boot order (docs/TODOHELPER_FRAMEWORK.md 9):
 *   NVS/config -> event loop -> board -> storage -> UI core + page table
 *   -> splash -> optional services -> home -> SYS_EVENT_READY
 *
 * A module marked @c optional may fail without preventing the UI from coming
 * up: requirement 15 demands that a missing SD card, no network or an absent
 * API key degrade instead of blocking.
 */

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ai_service.h"
#include "calendar_service.h"
#include "idle_face.h"
#include "storage_manager.h"
#include "sync_server.h"
#include "sys_utils.h"
#include "th_bsp.h"
#include "todo_manager.h"
#include "ui_assets.h"
#include "ui_core.h"
#include "ui_pages.h"
#include "ui_theme.h"
#include "weather_service.h"
#include "wifi_manager.h"
#include "voice_service.h"
#include "word_service.h"

static const char *TAG = "app";

typedef struct {
    const char *name;
    esp_err_t (*init)(void);
    bool optional;      /**< failure is logged, not fatal */
    bool after_ui;      /**< run after the UI is up (splash is already visible) */
} app_module_t;

/**
 * The module table. Adding a feature component = adding one row here.
 */
static const app_module_t s_modules[] = {
    { .name = "wifi_manager",     .init = wifi_manager_init, .optional = true,  .after_ui = true },
    { .name = "todo_manager",     .init = todo_manager_init, .optional = true,  .after_ui = true },
    { .name = "weather_service",  .init = weather_service_init, .optional = true,  .after_ui = true },
    { .name = "calendar_service", .init = calendar_service_init, .optional = true, .after_ui = true },
    { .name = "word_service",     .init = word_service_init, .optional = true,  .after_ui = true },
    { .name = "sync_server",      .init = sync_server_init, .optional = true,  .after_ui = true },
    { .name = "idle_face",        .init = idle_face_init, .optional = true,  .after_ui = true },
    { .name = "ai_service",       .init = ai_service_init, .optional = true, .after_ui = true },
    { .name = "voice_service",    .init = voice_service_init, .optional = true, .after_ui = true },
};

static esp_err_t run_modules(bool after_ui)
{
    esp_err_t first_error = ESP_OK;

    for (size_t i = 0; i < sizeof(s_modules) / sizeof(s_modules[0]); i++) {
        const app_module_t *m = &s_modules[i];
        if (m->after_ui != after_ui) {
            continue;
        }
        if (m->init == NULL) {
            SYS_LOGD(TAG, "module '%s' not linked yet, skipped", m->name);
            continue;
        }

        const esp_err_t err = m->init();
        if (err == ESP_OK) {
            SYS_LOGI(TAG, "module '%s' ready", m->name);
            /* This board has only ~310 KB of internal RAM against 12.4 MB of
             * PSRAM, and WiFi/mDNS/I2S all need large *internal* blocks. Tracing
             * every stage is how we find out which module eats the scarce pool;
             * see sys_heap_log() and AGENT.md section 9. */
            sys_heap_log(m->name);
            continue;
        }

        if (m->optional) {
            SYS_LOGW(TAG, "module '%s' unavailable: %s", m->name, esp_err_to_name(err));
        } else {
            SYS_LOGE(TAG, "module '%s' failed: %s", m->name, esp_err_to_name(err));
        }
        if (first_error == ESP_OK) {
            first_error = err;
        }
    }

    return first_error;
}

static void log_reset_reason(void)
{
    const esp_reset_reason_t reason = esp_reset_reason();
    const char *name = "unknown";
    switch (reason) {
    case ESP_RST_POWERON:
        name = "power-on";
        break;
    case ESP_RST_SW:
        name = "software";
        break;
    case ESP_RST_PANIC:
        name = "panic";
        break;
    case ESP_RST_INT_WDT:
        name = "interrupt watchdog";
        break;
    case ESP_RST_TASK_WDT:
        name = "task watchdog";
        break;
    case ESP_RST_WDT:
        name = "watchdog";
        break;
    case ESP_RST_BROWNOUT:
        name = "brownout";
        break;
    case ESP_RST_DEEPSLEEP:
        name = "deep sleep";
        break;
    case ESP_RST_EXT:
        name = "external";
        break;
    default:
        break;
    }
    SYS_LOGI(TAG, "reset reason: %s (%d)", name, (int)reason);
}

void app_main(void)
{
    /* First, so every later line is filtered by our level/quiet-tag policy. */
    sys_log_init();

    /* Before anything parses JSON: cJSON's per-element allocations must come
     * from PSRAM, not from the ~310 KB internal pool. Parsing the 546 KB CET-6
     * word book from internal RAM empties that pool and makes the concurrently
     * connecting WiFi driver abort with ESP_ERR_NO_MEM (AGENT.md 9.0.5). */
    sys_json_use_psram();

    SYS_LOGI(TAG, "ToDoHelper starting");

    ESP_ERROR_CHECK(sys_config_init(NULL));
    ESP_ERROR_CHECK(sys_events_init());

    log_reset_reason();
    sys_heap_log("startup");

    /* Board first: everything visual depends on the display/LVGL chain. */
    ESP_ERROR_CHECK(th_bsp_init());
    sys_heap_log("bsp (display+LVGL)");

    /* Storage is optional by design: no SD card means the internal LittleFS,
     * and if that fails too the app still runs without persistence. */
    ESP_ERROR_CHECK(storage_manager_init());
    sys_heap_log("storage");

    /* Packed icons (weather symbols). Missing assets only degrade the UI. */
    if (ui_assets_init() != ESP_OK) {
        SYS_LOGW(TAG, "UI assets unavailable, pages will use text fallbacks");
    }
    sys_heap_log("ui_assets");

    ESP_ERROR_CHECK(ui_core_init());
    sys_heap_log("ui_core");

    ESP_ERROR_CHECK(ui_pages_init());
    sys_heap_log("ui_pages");

    /*
     * Show the splash before the slow, optional services come up and keep it up for
     * a fixed 3 s so the boot log is readable (user request).
     *
     * The hand-off deliberately happens BEFORE run_modules(true), so the splash's
     * log-refresh lv_timer is not alive while the modules initialise. That
     * ordering was once suspected of causing the startup abort(); it did not.
     * The real cause was internal RAM exhaustion inside a module's JSON parse,
     * fixed by sys_json_use_psram() above - see AGENT.md 9.0.5.
     */

    ESP_ERROR_CHECK(ui_page_switch(UI_PAGE_SPLASH));

    (void)run_modules(false);

    vTaskDelay(pdMS_TO_TICKS(3000));

    if (ui_page_switch(UI_PAGE_HOME) != ESP_OK) {
        SYS_LOGE(TAG, "home page unavailable");
    }

    (void)run_modules(true);

    /* Do not touch the status bar from here: ui_core owns it, and LVGL must only
     * be driven from the LVGL task or under the lock. Ask for a refresh instead. */
    (void)sys_status_publish();
    (void)sys_events_post(SYS_EVENT_READY, NULL, 0);

    SYS_LOGI(TAG, "ready (storage=%s)", storage_root() ? storage_root() : "none");
    sys_heap_log("boot complete");
}
