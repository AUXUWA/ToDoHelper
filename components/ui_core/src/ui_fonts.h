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

#include "ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Resolve all roles once. Idempotent; must run after th_bsp_init(). */
void ui_fonts_init(void);

/** Font for @p role, or NULL when nothing is available (ui_theme falls back). */
const lv_font_t *ui_fonts_get(ui_font_role_t role);

#ifdef __cplusplus
}
#endif
