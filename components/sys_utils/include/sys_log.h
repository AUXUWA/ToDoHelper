/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sys_log.h
 * @brief Unified, level-filtered logging for every ToDoHelper component.
 *
 * All components log through the SYS_LOGx() macros so that:
 *   - every line carries a single "TH:" prefix followed by the component tag;
 *   - the level can be changed at runtime with sys_log_set_level();
 *   - the format string is always checked at compile time.
 */

#pragma once

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYS_LOG_ERROR = 0,
    SYS_LOG_WARN,
    SYS_LOG_INFO,
    SYS_LOG_DEBUG,
} sys_log_level_t;

/**
 * @brief Apply the compile-time log level and the noisy-tag policy.
 *
 * Call once, early in app_main. Without it only sys_log_write()'s own threshold
 * is in effect and the noisy-tag demotion never happens, which is why the WiFi
 * PHY banner lines kept showing up in the boot log.
 */
void sys_log_init(void);

/**
 * @brief Set the global log level. Messages above this level are dropped.
 *
 * Also re-applies the noisy-tag policy, so raising the level for debugging does
 * not re-enable the WiFi PHY banner spam.
 */
void sys_log_set_level(sys_log_level_t level);

/**
 * @brief Get the current global log level.
 */
sys_log_level_t sys_log_get_level(void);

/**
 * @brief Emit one log line. Prefer the SYS_LOGx() macros.
 *
 * @param level One of sys_log_level_t (passed as int for macro friendliness).
 * @param tag   Component tag, without the "TH:" prefix.
 */
void sys_log_write(int level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define SYS_LOGE(tag, fmt, ...) sys_log_write(SYS_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define SYS_LOGW(tag, fmt, ...) sys_log_write(SYS_LOG_WARN, tag, fmt, ##__VA_ARGS__)
#define SYS_LOGI(tag, fmt, ...) sys_log_write(SYS_LOG_INFO, tag, fmt, ##__VA_ARGS__)
#define SYS_LOGD(tag, fmt, ...) sys_log_write(SYS_LOG_DEBUG, tag, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
