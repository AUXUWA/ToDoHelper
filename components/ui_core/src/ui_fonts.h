/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_fonts.h
 * @brief Internal font manager (used by ui_theme.c). Not a public header.
 */

#pragma once

#include "lvgl.h"

#include "ui_font_mem.h"
#include "ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Resolve every font role once. Idempotent.
 *
 * Must run after th_bsp_init() (the FreeType path needs the LVGL adapter) and
 * after storage_manager_init() (a font on storage overrides the embedded one).
 */
void ui_fonts_init(void);

/** @brief Font for @p role, or NULL when nothing is available. */
const lv_font_t *ui_fonts_get(ui_font_role_t role);

/**
 * @brief Re-resolve the fonts, dropping any FreeType handles.
 *
 * Used when a font file is added or replaced on storage while the device runs.
 */
void ui_fonts_reload(void);

#ifdef __cplusplus
}
#endif
