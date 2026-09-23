/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sys_utils.h
 * @brief L0 foundation: logging, shared event loop, time, NVS configuration.
 *
 * Every other component depends on this one. It is the first module that must
 * come up in app_main (see docs/TODOHELPER_FRAMEWORK.md section 9).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#include "sys_err.h"
#include "sys_keys.h"
#include "sys_log.h"
#include "sys_notify.h"
#include "sys_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */

/** System-wide event base. Component specific bases are declared by the components. */
ESP_EVENT_DECLARE_BASE(SYS_EVENT);

typedef enum {
    SYS_EVENT_READY = 0,        /**< all app modules finished init */
    SYS_EVENT_BUTTON,           /**< payload: sys_button_event_t */
    SYS_EVENT_BRIGHTNESS,       /**< payload: uint8_t percent */
    SYS_EVENT_IDLE_TIMEOUT,     /**< no payload: idle timeout reached */
    SYS_EVENT_WAKEUP,           /**< no payload: user interaction after idle */
    SYS_EVENT_STORAGE_READY,    /**< no payload: storage root resolved */
    SYS_EVENT_CONNECTIVITY,     /**< payload: bool online */
    SYS_EVENT_STATUS_CHANGED,   /**< no payload: read sys_status_get() */
} sys_event_id_t;

typedef enum {
    SYS_BUTTON_VOLUP = 0,
    SYS_BUTTON_VOLDOWN,
    SYS_BUTTON_MODE,
    SYS_BUTTON_SET,
    SYS_BUTTON_MAX,
} sys_button_id_t;

typedef enum {
    SYS_BUTTON_PRESS = 0,       /**< short press / click */
    SYS_BUTTON_LONG_PRESS,      /**< long press */
} sys_button_action_t;

typedef struct {
    sys_button_id_t id;
    sys_button_action_t action;
} sys_button_event_t;

/**
 * @brief Create the project-wide event loop. Idempotent.
 */
esp_err_t sys_events_init(void);

/**
 * @brief Handle of the shared event loop. NULL before sys_events_init().
 *
 * Components register handlers with
 * esp_event_handler_instance_register_with(sys_events_get_loop(), BASE, ...).
 */
esp_event_loop_handle_t sys_events_get_loop(void);

/**
 * @brief Post an event on the shared loop with an explicit base. Non-blocking.
 */
esp_err_t sys_event_post_to(esp_event_base_t base, int32_t event_id,
                            const void *data, size_t data_size);

/**
 * @brief Post an event with base SYS_EVENT. Non-blocking.
 */
esp_err_t sys_events_post(int32_t event_id, const void *data, size_t data_size);

/**
 * @brief Thin wrapper over esp_event_handler_instance_register_with().
 */
esp_err_t sys_event_subscribe(esp_event_base_t base, int32_t event_id,
                              esp_event_handler_t handler, void *handler_arg,
                              esp_event_handler_instance_t *instance);

/**
 * @brief Counterpart of sys_event_subscribe().
 */
esp_err_t sys_event_unsubscribe(esp_event_base_t base, int32_t event_id,
                                esp_event_handler_instance_t instance);

/* ------------------------------------------------------------------------ */
/* Time                                                                      */
/* ------------------------------------------------------------------------ */

/**
 * @brief Monotonic milliseconds since boot. Used for timeouts and animation.
 */
int64_t sys_time_ms(void);

/**
 * @brief Unix timestamp in seconds.
 *
 * Returns 0 while the clock has never been synchronised (NTP not yet applied).
 * Callers MUST check sys_time_synced() before formatting a date, otherwise the
 * UI would show 1970 (docs/TODOHELPER_REQUIREMENTS.md FR4).
 */
int64_t sys_time_ts(void);

/**
 * @brief True once a real wall-clock time has been established.
 */
bool sys_time_synced(void);

/**
 * @brief Anchor the wall clock to @p ts (Unix seconds). Called by calendar_service
 *        after a successful SNTP sync, or after restoring a stored time.
 */
void sys_time_set_ts(int64_t ts);

/**
 * @brief Drop the wall clock back to "unknown" (e.g. after a failed resync).
 */
void sys_time_clear(void);

/* ------------------------------------------------------------------------ */
/* Configuration (NVS)                                                       */
/* ------------------------------------------------------------------------ */

/**
 * @brief Open the NVS namespace and prepare it for sys_config_* calls. Idempotent.
 *
 * Also performs nvs_flash_init() internally so components never touch NVS
 * directly (docs/TODOHELPER_REQUIREMENTS.md 7.13 / 9.1).
 *
 * @param nvs_namespace NULL selects CONFIG_TODOHELPER_NVS_NAMESPACE ("todohelper").
 */
esp_err_t sys_config_init(const char *nvs_namespace);

/**
 * @brief Read a string. Returns ESP_ERR_NOT_FOUND if the key is absent and
 *        ESP_ERR_INVALID_SIZE if @p out is too small.
 */
esp_err_t sys_config_get_str(const char *key, char *out, size_t len);

/**
 * @brief Read a string into a freshly allocated buffer (caller frees).
 */
esp_err_t sys_config_get_str_alloc(const char *key, char **out);

esp_err_t sys_config_set_str(const char *key, const char *val);
esp_err_t sys_config_get_i32(const char *key, int32_t *out);
esp_err_t sys_config_set_i32(const char *key, int32_t val);
esp_err_t sys_config_get_bool(const char *key, bool *out);
esp_err_t sys_config_set_bool(const char *key, bool val);
esp_err_t sys_config_erase(const char *key);

/**
 * @brief Convenience: get_i32 with a default, never fails.
 */
int32_t sys_config_get_i32_or(const char *key, int32_t fallback);
bool sys_config_get_bool_or(const char *key, bool fallback);

/**
 * @brief Convenience: copy a string key into @p out, applying @p fallback when absent.
 */
esp_err_t sys_config_get_str_or(const char *key, char *out, size_t len, const char *fallback);

/* ------------------------------------------------------------------------ */
/* Diagnostics                                                               */
/* ------------------------------------------------------------------------ */

/**
 * @brief Log the heap situation with a stage label.
 *
 * This board has only ~310 KB of **internal** RAM against 12.4 MB of PSRAM, and
 * several subsystems (esp_wifi_init, mDNS, I2S DMA, task stacks) need large
 * contiguous *internal* blocks. When one of them fails with ESP_ERR_NO_MEM it is
 * almost always internal exhaustion, so every boot stage logs this trace:
 *
 *     I TH:heap: after bsp            internal    231 KB (largest    180 KB)  ...
 *
 * `largest` (the biggest free internal block) is the number that matters for
 * esp_wifi_init(); the total can look healthy while fragmentation still breaks a
 * large allocation.
 *
 * `int-low` is the *internal* low-water mark since boot. It is the column that
 * catches a transient burst (a bulk JSON parse used to be the one) emptying the
 * internal pool and taking an unrelated subsystem down with `ESP_ERR_NO_MEM`;
 * see AGENT.md 9.0.5.
 *
 * @param stage Short stage label, e.g. "bsp", "ui_core", "wifi".
 */
void sys_heap_log(const char *stage);

/** @brief Largest free block of internal DMA-capable RAM, in bytes. */
size_t sys_heap_largest_internal(void);

/**
 * @brief Make cJSON allocate from PSRAM instead of internal RAM.
 *
 * Call once, early in `app_main`, before anything parses JSON.
 *
 * This is **not** an optimisation, it is the fix for the SD-card boot abort:
 * cJSON allocates one ~56-byte node per JSON element, and every allocation of
 * <= `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (4096) goes to internal RAM first.
 * Parsing the 546 KB CET-6 word book therefore empties the ~107 KB internal
 * pool, after which the concurrently connecting WiFi driver can no longer get
 * the ~100 bytes it needs for `esp_timer_create()` and aborts behind
 * `ESP_ERROR_CHECK`. See AGENT.md 9.0.5 and `sys_json.c`.
 */
void sys_json_use_psram(void);

#ifdef __cplusplus
}
#endif
