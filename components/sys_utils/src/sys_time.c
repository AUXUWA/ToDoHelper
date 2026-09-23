/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sys_utils.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * The wall clock is derived from a monotonic anchor instead of being read back
 * from the RTC: this board has no RTC, so the only trustworthy reference is the
 * monotonic timer plus an anchor installed by calendar_service after NTP sync.
 */
static int64_t s_anchor_ts;       /* Unix seconds at the anchor moment */
static int64_t s_anchor_ms;       /* monotonic ms when the anchor was taken */
static bool s_synced;

int64_t sys_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

bool sys_time_synced(void)
{
    return s_synced;
}

int64_t sys_time_ts(void)
{
    if (!s_synced) {
        return 0;
    }
    const int64_t elapsed_ms = sys_time_ms() - s_anchor_ms;
    return s_anchor_ts + (elapsed_ms / 1000);
}

void sys_time_set_ts(int64_t ts)
{
    if (ts <= 0) {
        sys_time_clear();
        return;
    }
    /* esp_timer_get_time() is read twice on purpose: the anchor must pair the
     * timestamp with the monotonic value taken as close to it as possible. */
    s_anchor_ms = sys_time_ms();
    s_anchor_ts = ts;
    s_synced = true;
}

void sys_time_clear(void)
{
    s_synced = false;
    s_anchor_ts = 0;
    s_anchor_ms = 0;
}
