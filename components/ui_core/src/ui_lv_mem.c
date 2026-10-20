/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_lv_mem.c
 * @brief LVGL 的分配器（CONFIG_LV_USE_CUSTOM_MALLOC）
 *
 * ## 为什么自己写分配器
 *
 * LVGL 的字形缓存有两种开销：
 *
 *   1. **位图**（每个汉字的 A8 位图）：v1.8.5 已经搬去 PSRAM
 *      （见 ui_font_mem.c，用的是 LVGL 的 font_draw_buf_handlers）。
 *   2. **元数据**（红黑树节点 + 缓存条目 + draw_buf 结构）：每个字形约 200 B，
 *      而整个界面用到的（字形, 字号）组合有 **517** 个 —— 光元数据就要
 *      **100 KB 内部堆**。内部堆一共才 290 KB，搬走位图也腾不出这个空间。
 *
 * 这份元数据是 LVGL 的 `lv_cache` 分配的，走的是 `lv_malloc()`。
 * 要把它单独搬去 PSRAM，就得自己接管 LVGL 的分配器
 * （CONFIG_LV_USE_CUSTOM_MALLOC=y）。
 *
 * ## 怎么只搬该搬的
 *
 * 接管了分配器，也不能一股脑全放 PSRAM —— LVGL 的控件对象每帧都要读，
 * 放 PSRAM 会让整个界面变慢。所以只在一个很窄的区间里改道：
 * **FreeType 取字形的那两个回调**（get_glyph_dsc / get_glyph_bitmap）。
 * 在这两个回调里，LVGL 分配的正好就是字形缓存的元数据（外加 FreeType 自己
 * 的临时缓冲），全部搬去 PSRAM；回调之外，分配行为跟原来一模一样。
 *
 * 这个区间由 ui_font_mem.c 包装字体回调时打开/关闭（见 ui_lv_mem.h）。
 *
 * 参见 AGENT.md §9.0.1.8。
 */

#include "ui_lv_mem.h"

#include "lvgl.h"

#include <stdlib.h>

#include "esp_heap_caps.h"

/**
 * 是否正在「画字」的分配区间。
 *
 * 注意这是一个简单的全局标志，不是线程局部变量：LVGL 的绘制都在 LVGL 任务
 * 上跑，其它任务即使恰好在这个区间里分配，也只是慢一点（拿到 PSRAM 而已），
 * 不会出错。真正要紧的是「画字的那次分配一定要进 PSRAM」。
 */
static volatile bool s_font_scope;

void ui_lv_mem_font_scope_enter(void)
{
    s_font_scope = true;
}

void ui_lv_mem_font_scope_leave(void)
{
    s_font_scope = false;
}

bool ui_lv_mem_in_font_scope(void)
{
    return s_font_scope;
}

/* ------------------------------------------------------------------------- */
/* LVGL's allocator backend (LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM)       */
/* ------------------------------------------------------------------------- */

void lv_mem_init(void)
{
    /* Nothing to do: the C library heap needs no setup. */
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL; /* Not supported */
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    if (s_font_scope) {
        /* Glyph cache metadata: it is big, cold and long-lived - exactly what
         * PSRAM is for. */
        void *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf != NULL) {
            return buf;
        }
        /* PSRAM full: fall back to the normal heap rather than failing. */
    }

    /* Exactly what LVGL's C library backend did: malloc() keeps small
     * allocations in internal RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) and
     * lets big ones go to PSRAM. */
    return malloc(size);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    if (p == NULL) {
        return lv_malloc_core(new_size);
    }

    if (s_font_scope) {
        /*
         * A block that lives in PSRAM has to stay there.
         *
         * realloc() is heap_caps_realloc_default(), which asks for INTERNAL
         * memory for anything up to CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL bytes.
         * And heap_caps_realloc_base() only keeps a block in its own heap when
         * the requested caps are compatible with that heap's - so a PSRAM block
         * reallocated to a small size would be copied into internal RAM and
         * the PSRAM block freed. That is exactly the memory this change is
         * trying to keep free.
         *
         * MALLOC_CAP_8BIT is satisfied by both internal RAM and PSRAM, so
         * asking for it keeps the block in whichever heap it came from.
         */
        return heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
    }

    /* An internal RAM block: exactly what LVGL's C library backend did. */
    return realloc(p, new_size);
}

void lv_free_core(void *p)
{
    /* Works for every heap: the block itself knows where it came from. */
    free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    /* The C library heap has no pools, so LVGL's own monitor cannot tell
     * anything useful - the debug shell's `heap` command uses the ESP-IDF
     * heap_caps API instead. */
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
