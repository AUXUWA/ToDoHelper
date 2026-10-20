/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_font_mem.h
 * @brief 字形位图住进 PSRAM（只改 FreeType 这一条路）+ 字形缓存上限
 */

#ifndef UI_FONT_MEM_H
#define UI_FONT_MEM_H

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 一个字号的字形缓存现状。 */
typedef struct {
    uint32_t size;           /**< 字号（px） */
    uint32_t glyph_entries;  /**< 字形描述缓存里的条目数 */
    uint32_t bitmap_entries; /**< 位图缓存里的条目数 */
} ui_font_cache_stat_t;

/** 最多跟踪多少个字体（UI 有 4 个字号）。 */
#define UI_FONT_MEM_MAX_FONTS 8

/**
 * @brief 把 LVGL 的「字体绘制缓冲」改成从 PSRAM 分配
 *
 * 必须在任何 FreeType 字体被创建之前调用（ui_fonts_init() 一进来就调）。
 * 重复调用是安全的。
 */
esp_err_t ui_font_mem_init(void);

/**
 * @brief 设置**下一个**创建的 FreeType 字体的字形缓存上限
 *
 * LVGL 9 的 FreeType 给每个字号各留一份位图缓存，条目数是同一个上限
 * （位图缓存 = 上限，字形描述缓存 = 2 倍上限）。上限越大越流畅，
 * 但缓存的元数据在内部堆里，每个字形约 200 B。
 *
 * 所以按字号分别给：正文给大的，标题/小字/巨字给小的。
 * 必须在该字体的 esp_lv_adapter_ft_font_init() 之前调用。
 */
void ui_font_mem_set_cache_cnt(uint32_t cnt);

/** @brief 读回当前使用的字形缓存上限（0 = 还没初始化） */
uint32_t ui_font_mem_cache_cnt(void);

/**
 * @brief 登记一个 FreeType 字体，好让 @ref ui_font_mem_stats 能报缓存情况
 *
 * 只有 FreeType 字体才有缓存；内置字体（flash 里的字表）会被自动忽略。
 */
void ui_font_mem_track(const lv_font_t *font);

/**
 * @brief 忘掉所有登记过的字体
 *
 * 字体被销毁重建（换字库、reload）之前必须调用，否则登记的指针会变成野指针。
 */
void ui_font_mem_forget_all(void);

/**
 * @brief 读回各字号的字形缓存现状
 *
 * @param out  array to fill
 * @param max  how many entries @p out can hold
 * @return     number of entries written
 */
uint32_t ui_font_mem_stats(ui_font_cache_stat_t *out, uint32_t max);

#ifdef __cplusplus
}
#endif

#endif /* UI_FONT_MEM_H */
