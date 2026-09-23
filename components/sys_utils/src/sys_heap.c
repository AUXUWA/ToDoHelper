/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sys_heap.c
 * @brief Heap diagnostics.
 *
 * Exists because this board separates the two pools very sharply: ~310 KB of
 * internal RAM versus 12.4 MB of PSRAM. A subsystem that reports
 * `ESP_ERR_NO_MEM` while the log shows "free heap: 10 MB" is not out of memory -
 * it is out of *internal* memory, and the number that predicts the failure is the
 * largest free internal block, not the total.
 */

#include "sys_utils.h"

#include "esp_heap_caps.h"
#include "esp_system.h"

size_t sys_heap_largest_internal(void)
{
    /* DMA-capable internal memory is what esp_wifi_init(), I2S DMA descriptors
     * and task stacks actually compete for. */
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}

void sys_heap_log(const char *stage)
{
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t internal_largest = sys_heap_largest_internal();
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t min_free = esp_get_minimum_free_heap_size();

    SYS_LOGI("heap", "after %-14s internal %6u KB (largest %6u KB)  psram %5u KB  min %6u KB",
             stage != NULL ? stage : "?",
             (unsigned)(internal_free / 1024), (unsigned)(internal_largest / 1024),
             (unsigned)(psram_free / 1024), (unsigned)(min_free / 1024));
}
