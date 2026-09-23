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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* ------------------------------------------------------------------------ */
/* Recent-log history (on-screen boot log)                                   */
/* ------------------------------------------------------------------------ */

/**
 * @brief How many log lines the in-RAM history keeps.
 *
 * sys_log_write() also copies every line it emits into a ring of this size so a
 * page can show the boot log on the panel instead of requiring a serial monitor
 * (see components/ui_pages/src/page_splash.c). 64 lines is roughly four screens
 * of text on the 800x480 panel. The serial output is unaffected.
 */
#define SYS_LOG_RECENT_CAPACITY 64

/** @brief Room for one retained line, including the terminating NUL. */
#define SYS_LOG_RECENT_LINE_LEN 80

/** @brief Metadata of one retained line. */
typedef struct {
    sys_log_level_t level;  /**< level the line was emitted with */
    uint32_t uptime_ms;     /**< sys_time_ms() at the moment of capture */
    uint32_t seq;           /**< 1-based line number; 0 while the slot is being written */
} sys_log_recent_info_t;

/**
 * @brief Lines captured since boot. Monotonic, never resets.
 *
 * A reader polls this to find out whether anything new arrived: when it changes,
 * the history has more lines. 0 means the history is unavailable (no memory for
 * the ring) or nothing has been logged yet.
 */
uint32_t sys_log_recent_seq(void);

/** @brief Number of lines currently retained: min(seq, SYS_LOG_RECENT_CAPACITY). */
size_t sys_log_recent_count(void);

/**
 * @brief Copy one retained line, stored as "tag: message".
 *
 * Lock-free and safe from any task while other tasks log: the writer claims a
 * sequence number, marks the slot busy and publishes it afterwards, so a reader
 * that races with a write gets @c false instead of a torn line and can retry.
 *
 * @param age     0 selects the newest line, 1 the one before it, and so on.
 * @param out     Destination for the NUL-terminated text; must not be NULL.
 * @param out_len Size of @p out; longer lines are truncated.
 * @param info    Optional metadata, only filled in when the copy succeeded.
 * @return true when the line was copied.
 */
bool sys_log_recent_get(size_t age, char *out, size_t out_len, sys_log_recent_info_t *info);

#ifdef __cplusplus
}
#endif
