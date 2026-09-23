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
     * Format first, then emit through the standard ESP_LOGx macros.
     *
     * Two wrong turns led here, both visible in the device log:
     *
     * 1. esp_log_writev() emits the message verbatim - no trailing newline - so
     *    the line-buffered console held every application message back and merged
     *    it into the next line.
     * 2. esp_log_write() adds the newline but still none of the
     *    "I (1234) TH:tag: " decoration, because that prefix is built into the
     *    format string by the ESP_LOGx macros themselves, not by the log backend.
     *    The result read like a continuation of the previous line.
     *
     * Using the macros gives exactly the same shape as IDF's own output, so the
     * log is uniform and greppable. The runtime level is still filtered by
     * sys_log_write's own check, and sys_log_set_level() mirrors it into
     * esp_log_level_set() so IDF's filter agrees.
     */
    char line[SYS_LOG_LINE_BUF];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    switch (level) {
    case SYS_LOG_ERROR:
        ESP_LOGE(tagged, "%s", line);
        break;
    case SYS_LOG_WARN:
        ESP_LOGW(tagged, "%s", line);
        break;
    case SYS_LOG_DEBUG:
        ESP_LOGD(tagged, "%s", line);
        break;
    case SYS_LOG_INFO:
    default:
        ESP_LOGI(tagged, "%s", line);
        break;
    }
}
