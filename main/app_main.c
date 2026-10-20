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
#include "debug_shell.h"
#include "idle_face.h"
#include "maimemo_client.h"
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

/**
 * How long the newest boot log line must have been published before the
 * hand-over to the home page.
 *
 * The splash refreshes its log view every 250 ms (SPLASH_TICK_MS in
 * components/ui_pages/src/page_splash.c), so a line that is older than that
 * has been rendered on the panel.
 */
#define BOOT_LOG_ON_SCREEN_MS 300

/**
 * Upper bound of the hand-over wait (roadmap v1.8.0 F1).
 *
 * The old code slept for a fixed 3 s here: 46 % of the whole boot, spent
 * looking at a screen that did not change, because every module had already
 * finished. Now the wait ends as soon as the boot log is on the screen, and
 * it never lasts longer than this - the home page comes up either way.
 */
#define BOOT_HANDOVER_MAX_MS 1000

/** Poll interval of the hand-over wait. */
#define BOOT_HANDOVER_POLL_MS 20

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
    /* Before word_service: the cloud queue (plan A) reads the token and the
     * enrichment cache, so its config and lock must already exist. */
    { .name = "maimemo_client",   .init = maimemo_client_init, .optional = true,  .after_ui = true },
    { .name = "word_service",     .init = word_service_init, .optional = true,  .after_ui = true },
    { .name = "sync_server",      .init = sync_server_init, .optional = true,  .after_ui = true },
    { .name = "idle_face",        .init = idle_face_init, .optional = true,  .after_ui = true },
    { .name = "ai_service",       .init = ai_service_init, .optional = true, .after_ui = true },
    { .name = "voice_service",    .init = voice_service_init, .optional = true, .after_ui = true },
    /* Last, and compiled out unless CONFIG_TODOHELPER_DEBUG_SHELL is set: it is
     * a development aid (/api/v1/debug/exec), not a product feature. */
    { .name = "debug_shell",      .init = debug_shell_init, .optional = true, .after_ui = true },
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

/**
 * Has the newest boot log line been on the splash long enough?
 *
 * The project has no "the home page is ready" flag to ask for: ui_core knows
 * which page is on screen (ui_page_current()) and sys_status knows what the
 * services published, but neither of them says "the boot log the user is
 * supposed to read is on the panel".
 *
 * The log's own metadata is the honest proxy: sys_log_recent_info_t::uptime_ms
 * is the moment the line was published, and the splash draws the newest line
 * every 250 ms, so a line that is older than that has been rendered.
 */
static bool boot_log_on_screen(void)
{
    sys_log_recent_info_t info;
    char line[SYS_LOG_RECENT_LINE_LEN];
    if (!sys_log_recent_get(0, line, sizeof(line), &info)) {
        return false;   /* nothing published yet */
    }
    const uint32_t now = (uint32_t)sys_time_ms();
    return (uint32_t)(now - info.uptime_ms) >= BOOT_LOG_ON_SCREEN_MS;
}

/**
 * Wait until the boot log has been on the screen, but never longer than
 * @p max_wait_ms.
 *
 * The hand-over to the home page must not depend on this: a missing log line
 * (or a slow splash) only costs the user the reading time, never the home
 * page.
 */
static void wait_for_boot_log(uint32_t max_wait_ms)
{
    const int64_t deadline = sys_time_ms() + (int64_t)max_wait_ms;
    while (!boot_log_on_screen()) {
        if (sys_time_ms() >= deadline) {
            SYS_LOGD(TAG, "boot log not on screen after %u ms; home page anyway",
                     (unsigned)max_wait_ms);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(BOOT_HANDOVER_POLL_MS));
    }
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

    /*
     * TEMPORARY migration (remove in the next release).
     *
     * The standby brightness default changed from 20 to 100 so the idle eyes stay
     * pure white, but a value saved before that change wins over the new default.
     * Clear that stale key exactly once.
     *
     * Guarded by a marker key on purpose: nvs_erase_key() returns ESP_OK even when
     * the key does not exist, so an unguarded erase would log every boot *and*
     * would delete a value the user later set with the settings slider.
     */
    bool migrated = false;
    if (sys_config_get_bool(SYS_KEY_BRIGHT_MIGRATED, &migrated) != ESP_OK) {
        (void)sys_config_erase("idle_bright");
        (void)sys_config_set_bool(SYS_KEY_BRIGHT_MIGRATED, true);
        SYS_LOGI(TAG, "migrated: cleared stale 'idle_bright' (standby keeps full brightness)");
    }

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
     * Show the splash before the slow, optional services come up, and keep it
     * up while they come up: their log lines then scroll on the panel, which
     * is the whole point of the boot-log screen.
     *
     * The hand-over to the home page is a **bounded wait**, not a fixed sleep
     * (roadmap v1.8.0 F1). The old `vTaskDelay(pdMS_TO_TICKS(3000))` was pure
     * dead time: nothing at all ran during those 3 s - the services only came
     * up afterwards - so the panel showed a frozen log for 46 % of the boot.
     *
     * The hand-over happens AFTER run_modules(true) on purpose: the home page
     * reads the shared status snapshot (sys_status), so it is created with the
     * services already behind it instead of filling in a second later. The
     * previous revision of this file handed over first and bought the
     * readable log with a plain 3 s delay instead.
     */

    ESP_ERROR_CHECK(ui_page_switch(UI_PAGE_SPLASH));

    /* The hook for services that must be up before the splash. No module is
     * marked that way today, so this is a no-op; it stays because the module
     * table is where that changes. */
    (void)run_modules(false);

    (void)run_modules(true);

    /* Wait until the boot log has actually been on the screen - but never
     * longer than BOOT_HANDOVER_MAX_MS: the home page comes up either way. */
    wait_for_boot_log(BOOT_HANDOVER_MAX_MS);

    if (ui_page_switch(UI_PAGE_HOME) != ESP_OK) {
        SYS_LOGE(TAG, "home page unavailable");
    }

    /*
     * Plan A: today's plan and the real progress are pulled from MaiMemo in
     * the background - but **not** from here. That request is triggered by the
     * "the station got an address" event (roadmap v1.8.0 D3): at boot there is
     * no address yet, so a request sent now can only fail (no DNS) and pays
     * for a full TLS handshake for nothing. The client owns that trigger
     * (components/maimemo_client), so no token or mode is read here either.
     */

    /* Do not touch the status bar from here: ui_core owns it, and LVGL must only
     * be driven from the LVGL task or under the lock. Ask for a refresh instead. */
    (void)sys_status_publish();
    (void)sys_events_post(SYS_EVENT_READY, NULL, 0);

    SYS_LOGI(TAG, "ready (storage=%s)", storage_root() ? storage_root() : "none");
    sys_heap_log("boot complete");
}
