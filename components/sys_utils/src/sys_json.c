/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sys_json.c
 * @brief Keep cJSON's allocations out of internal RAM.
 *
 * **This is the root cause of the "SD card inserted -> boot abort loop" bug**
 * (AGENT.md 9.0.5). The chain, verified on hardware:
 *
 * 1. `sdkconfig.defaults` sets `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`, and
 *    `heap_caps_malloc_default()` therefore sends *every* request of <= 4096
 *    bytes to the **internal** heap first and only falls back to PSRAM once that
 *    pool is exhausted (`heap_caps.c`: `malloc_alwaysinternal_limit`).
 * 2. cJSON allocates one ~56-byte node per JSON element, plus a copy of every
 *    key and every string value. The CET-6 word book is 546 KB / 833 entries /
 *    roughly 7000 nodes, so parsing it drives internal RAM to ~0 while it runs.
 * 3. With an SD card inserted that parse happens at boot (the book only exists
 *    on the card). Concurrently the WiFi driver is connecting, and its
 *    `ets_timer_setfn()` runs from the tick hook and calls
 *    `esp_timer_create()` -> `heap_caps_calloc(..., MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)`.
 *    That allocation cannot fall back to PSRAM, so it fails with
 *    `ESP_ERR_NO_MEM`, and because it sits behind `ESP_ERROR_CHECK` the device
 *    aborts (`ets_timer_legacy.c:58`) and reboots in a loop.
 *    Without a card the compiled-in 32-word starter book is parsed instead and
 *    nothing starves, which is why the crash looked card-dependent.
 *
 * Installing these hooks makes cJSON prefer PSRAM (11 MB free here) and fall
 * back to the normal heap, so JSON parsing can no longer starve internal RAM.
 * The hook is process-global, which suits this project: every JSON user
 * (todo/weather/word/wifi/ai/sync/HTTP handlers) handles bulk device data
 * rather than tiny protocol frames, and nothing parses JSON from an ISR or with
 * the flash cache disabled - the two places PSRAM may not be touched.
 */

#include "sys_utils.h"

#include "cJSON.h"
#include "esp_heap_caps.h"

/** PSRAM first, then whatever the default heap offers, so a build without PSRAM still works. */
static void *json_malloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
}

/**
 * `heap_caps_free()` finds the owning heap, so it is also correct for pointers
 * that a component allocated with plain `malloc()` and handed to cJSON (or the
 * other way round: cJSON memory released with plain `free()`).
 */
static void json_free(void *ptr)
{
    heap_caps_free(ptr);
}

void sys_json_use_psram(void)
{
    static bool s_installed;

    if (s_installed) {
        return;
    }
    s_installed = true;

    cJSON_Hooks hooks = {
        .malloc_fn = json_malloc,
        .free_fn = json_free,
    };
    cJSON_InitHooks(&hooks);
}
