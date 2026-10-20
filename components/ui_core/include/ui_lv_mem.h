/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_lv_mem.h
 * @brief LVGL 的分配器：平时跟 malloc() 一样，只有画字的时候走 PSRAM
 */

#ifndef UI_LV_MEM_H
#define UI_LV_MEM_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 标记「接下来的分配来自画字这条路」
 *
 * FreeType 的字形缓存（字形描述 + 位图）在**内部堆**里的元数据每个字形约
 * 200 B，而整个界面用到的（字形, 字号）组合有 500 多个 —— 光这些元数据就
 * 要 100 KB，内部堆只有 290 KB，撑不住。
 *
 * 所以画字的时候（FreeType 的 get_glyph_dsc / get_glyph_bitmap 回调里），
 * 分配改走 PSRAM；平时 LVGL 的分配一个字都不动，还是内部堆，速度不变。
 */
void ui_lv_mem_font_scope_enter(void);

/** @brief 结束画字分配标记 */
void ui_lv_mem_font_scope_leave(void);

/** @brief 当前是否处于「画字」分配区间 */
bool ui_lv_mem_in_font_scope(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_LV_MEM_H */
