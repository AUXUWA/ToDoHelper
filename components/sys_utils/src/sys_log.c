/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sys_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "sys_utils.h"

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

/* ------------------------------------------------------------------------- */
/* Recent-log history                                                        */
/* ------------------------------------------------------------------------- */

/*
 * A ring of the last SYS_LOG_RECENT_CAPACITY lines so the splash page can show
 * the boot log on the panel (components/ui_pages/src/page_splash.c).
 *
 * Constraints, in order of importance:
 *
 *  1. sys_log_write() is called by *every* task - including WiFi event handlers
 *     and HTTP worker tasks - so capturing must not allocate, block or take a
 *     lock. Only one atomic add and two plain stores are added per line.
 *  2. Internal RAM is the scarcest resource on this board (~310 KB against
 *     12.4 MB of PSRAM, see AGENT.md 9.0), so the ring is allocated from PSRAM
 *     when possible and only falls back to internal RAM.
 *  3. Readers run in the LVGL task and must never display a half-written line.
 *
 * That is why each line carries its own sequence number instead of being guarded
 * by a mutex: the writer claims a number with __atomic_add_fetch(), marks the
 * slot busy (seq = 0), fills it in, and only then publishes the number. A reader
 * that observes a different number before and after its copy knows it lost a race
 * and reports failure; the reader here simply retries on its next timer tick.
 * There is exactly one writer per slot, so no compare-exchange is needed.
 */

typedef struct {
    char text[SYS_LOG_RECENT_LINE_LEN]; /**< "tag: message", NUL terminated */
    uint16_t len;                       /**< strlen(text) */
    uint8_t level;                      /**< sys_log_level_t */
    uint32_t uptime_ms;                 /**< sys_time_ms() at capture */
    volatile uint32_t seq;              /**< 0 while busy, else the line number */
} sys_log_slot_t;

_Static_assert(SYS_LOG_RECENT_CAPACITY > 0, "the log history needs at least one slot");
_Static_assert(SYS_LOG_RECENT_LINE_LEN > 1, "a log line needs room for text and NUL");

/** NULL when neither PSRAM nor internal RAM had room: capture is then disabled. */
static sys_log_slot_t *s_ring;

/** Lines claimed so far; also the sequence number of the newest line. */
static volatile uint32_t s_ring_seq;

/**
 * Bounded append.
 *
 * Used instead of snprintf("%s: %s") so the cost is the same for every call site
 * and the compiler has nothing to complain about with -Wformat-truncation.
 */
static void text_append(char *dst, size_t cap, size_t *used, const char *src)
{
    while (*src != '\0' && (*used + 1u) < cap) {
        dst[(*used)++] = *src++;
    }
    dst[*used] = '\0';
}

/**
 * Keep one line of history. Called from sys_log_write() for every emitted line.
 *
 * Allocation-free, lock-free and wait-free: see the block comment above.
 */
static void recent_capture(int level, const char *tag, const char *line)
{
    if (s_ring == NULL) {
        return;
    }

    const uint32_t seq = __atomic_add_fetch(&s_ring_seq, 1u, __ATOMIC_RELAXED);
    sys_log_slot_t *slot = &s_ring[(seq - 1u) % (uint32_t)SYS_LOG_RECENT_CAPACITY];

    /* Busy marker first: a reader must never trust a slot mid-write. */
    __atomic_store_n(&slot->seq, 0u, __ATOMIC_RELAXED);

    size_t used = 0;
    text_append(slot->text, sizeof(slot->text), &used, (tag != NULL) ? tag : "?");
    text_append(slot->text, sizeof(slot->text), &used, ": ");
    text_append(slot->text, sizeof(slot->text), &used, (line != NULL) ? line : "");
    slot->len = (uint16_t)used;
    slot->level = (uint8_t)level;
    slot->uptime_ms = (uint32_t)sys_time_ms();

    /* Release: everything above is visible before the line becomes readable. */
    __atomic_store_n(&slot->seq, seq, __ATOMIC_RELEASE);
}

uint32_t sys_log_recent_seq(void)
{
    if (s_ring == NULL) {
        return 0;
    }
    return __atomic_load_n(&s_ring_seq, __ATOMIC_ACQUIRE);
}

size_t sys_log_recent_count(void)
{
    const uint32_t seq = sys_log_recent_seq();
    return (seq < (uint32_t)SYS_LOG_RECENT_CAPACITY) ? (size_t)seq
                                                     : (size_t)SYS_LOG_RECENT_CAPACITY;
}

bool sys_log_recent_get(size_t age, char *out, size_t out_len, sys_log_recent_info_t *info)
{
    if (s_ring == NULL || out == NULL || out_len == 0) {
        return false;
    }

    const uint32_t seq = __atomic_load_n(&s_ring_seq, __ATOMIC_ACQUIRE);
    const uint32_t count = (seq < (uint32_t)SYS_LOG_RECENT_CAPACITY)
                               ? seq
                               : (uint32_t)SYS_LOG_RECENT_CAPACITY;
    if (age >= (size_t)count) {
        return false;
    }

    /* Slot index is derived from the line number alone, so the reader never has
     * to look at the writer's head pointer. */
    const uint32_t target = seq - (uint32_t)age; /* 1-based line number */
    sys_log_slot_t *slot = &s_ring[(target - 1u) % (uint32_t)SYS_LOG_RECENT_CAPACITY];

    if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) != target) {
        /* Not published yet, or already overwritten by a newer line. */
        return false;
    }

    size_t len = slot->len;
    if (len >= sizeof(slot->text)) {
        len = sizeof(slot->text) - 1u;
    }
    if (len >= out_len) {
        len = out_len - 1u;
    }
    memcpy(out, slot->text, len);
    out[len] = '\0';

    /* Acquire again: if the writer reused the slot while we copied, the text is
     * torn and the caller must ignore both it and the metadata below. */
    if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) != target) {
        return false;
    }

    if (info != NULL) {
        info->level = (sys_log_level_t)slot->level;
        info->uptime_ms = slot->uptime_ms;
        info->seq = target;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Level policy                                                              */
/* ------------------------------------------------------------------------- */

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

    if (s_ring == NULL) {
        const size_t bytes = sizeof(sys_log_slot_t) * SYS_LOG_RECENT_CAPACITY;
        const char *where = "PSRAM";

        /*
         * PSRAM first: this board has 12.4 MB of it against ~310 KB of internal
         * RAM, and the internal pool is what esp_wifi_init()/mDNS/I2S keep failing
         * on (AGENT.md 9.0). Allocated once here, never freed, and the readers only
         * ever walk the pointer, so the accessors stay cheap.
         */
        s_ring = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        if (s_ring == NULL) {
            where = "internal RAM";
            s_ring = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }

        if (s_ring == NULL) {
            SYS_LOGW("sys_log", "no room for the %u-byte log history, on-screen boot log disabled",
                     (unsigned)bytes);
        } else {
            memset(s_ring, 0, bytes);
            SYS_LOGI("sys_log", "log history: %u lines x %u bytes in %s",
                     (unsigned)SYS_LOG_RECENT_CAPACITY, (unsigned)SYS_LOG_RECENT_LINE_LEN, where);
        }
    }
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

    /* Copy into the history for the on-screen boot log before emitting, so the
     * ring keeps the order in which the lines were produced. */
    recent_capture(level, tag, line);

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
