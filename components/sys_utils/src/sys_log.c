/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sys_log.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#ifndef CONFIG_TODOHELPER_LOG_LEVEL
#define CONFIG_TODOHELPER_LOG_LEVEL 2
#endif

/** Max length of the "TH:<tag>" prefix built by sys_log_write(). */
#define SYS_LOG_TAG_BUF 48

/**
 * Max length of one formatted log line.
 *
 * Long lines are truncated rather than dropped; on the stack this costs 256 bytes
 * per caller, which is why it is not larger. It is on the stack deliberately: a
 * shared buffer would need a lock, and logging must never block or allocate.
 */
#define SYS_LOG_LINE_BUF 256

static sys_log_level_t s_level = (sys_log_level_t)CONFIG_TODOHELPER_LOG_LEVEL;

void sys_log_set_level(sys_log_level_t level)
{
    if (level < SYS_LOG_ERROR) {
        level = SYS_LOG_ERROR;
    }
    if (level > SYS_LOG_DEBUG) {
        level = SYS_LOG_DEBUG;
    }
    s_level = level;

    /* Keep the IDF logger in sync so third-party components honour the same level. */
    esp_log_level_set("*", (esp_log_level_t)(level + 1));

    /*
     * Tags that are noisy by design and drown out everything useful.
     *
     * `wifi` prints a W-level line for every PHY rate it settles on (a dozen lines
     * on association) and `pp`/`net80211`/`phy` print firmware banners. None of
     * them indicate a problem, and on a 115200 console they hide the app log.
     */
    static const char *const quiet[] = { "wifi", "phy", "pp", "net80211", "wifi_init" };
    for (size_t i = 0; i < sizeof(quiet) / sizeof(quiet[0]); i++) {
        esp_log_level_set(quiet[i], ESP_LOG_ERROR);
    }
}

void sys_log_init(void)
{
    sys_log_set_level(s_level);
}

sys_log_level_t sys_log_get_level(void)
{
    return s_level;
}

void sys_log_write(int level, const char *tag, const char *fmt, ...)
{
    if (level > (int)s_level) {
        return;
    }

    char tagged[SYS_LOG_TAG_BUF];
    snprintf(tagged, sizeof(tagged), "TH:%s", tag ? tag : "?");

    /*
     * Format first, then hand the finished text to esp_log_write().
     *
     * esp_log_writev() was used originally for its va_list convenience, but it
     * emits the message *verbatim*: no "I (1234) TH:tag:" prefix and, crucially,
     * no trailing newline. The console is line buffered, so every application
     * message stayed in the buffer and then merged into the next line - the boot
     * log came out as one enormous unreadable line, and anything logged after the
     * last IDF message (the boot-time Wi-Fi scan, the idle timeout) looked like it
     * never happened. esp_log_write() is the same entry point the ESP_LOGx macros
     * use, so this produces the standard, parseable format.
     */
    char line[SYS_LOG_LINE_BUF];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    esp_log_write((esp_log_level_t)(level + 1), tagged, "%s\n", line);
}
