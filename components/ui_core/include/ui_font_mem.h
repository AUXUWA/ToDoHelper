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
 * @brief 设置 FreeType 的字形缓存上限
 *
 * LVGL 9 的 FreeType **所有字号共用一份缓存**：缓存节点的 key 是
 * （字体文件, 样式, 渲染模式），**没有字号**（lv_freetype.c 的 search_key）。
 * 所以这个上限是**整个界面共用**的预算，不是每个字号一份。
 * （缓存条目自己的 key 才带字号：{unicode, size}。）
 *
 * 上限越大越流畅（被挤掉的字形每一帧都要重新光栅化，一个字约 10 ms），
 * 但缓存的元数据也要跟着涨（每个字形约 200 B，现在住在 PSRAM）。
 *
 * 上限只在**第一次**创建这个字体文件的缓存节点时被读一次
 * （`cache_hitting == false` 那条分支），所以必须在创建**第一个**
 * FreeType 字体之前调用。
 */
void ui_font_mem_set_cache_cnt(uint32_t cnt);

/** @brief 读回当前使用的字形缓存上限（0 = 还没初始化） */
uint32_t ui_font_mem_cache_cnt(void);

/**
 * @brief 登记一个字体，好让 @ref ui_font_mem_stats 能报缓存情况
 *
 * @param font        要登记的字体
 * @param is_freetype 这个字体是不是 FreeType 字体
 *
 * **只有 FreeType 字体才有缓存**。判断这件事交给调用方（它知道自己是
 * 从哪条路拿到的字体）—— 靠读 `font->dsc` 的头几个字节去猜类型是
 * 跨类型读内置字体的数据结构，不该这么干。
 */
void ui_font_mem_track(const lv_font_t *font, bool is_freetype);

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
