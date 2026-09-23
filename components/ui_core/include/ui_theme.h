/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_theme.h
 * @brief Colours, fonts and metrics shared by every page.
 *
 * Pages must use these instead of literal colours/sizes so the whole UI can be
 * restyled from one place and so touch targets stay >= 44x44 px
 * (docs/TODOHELPER_REQUIREMENTS.md 10.3).
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Font roles. Implementation and fallback chain: see ui_fonts.c. */
typedef enum {
    UI_FONT_SMALL = 0,   /**< status bar, captions */
    UI_FONT_BODY,        /**< list rows, paragraphs */
    UI_FONT_TITLE,       /**< page titles, card titles */
    UI_FONT_HUGE,        /**< big numbers: temperature, clock */
    UI_FONT_ROLE_MAX,
} ui_font_role_t;

/** Font for a role. Never returns NULL. */
const lv_font_t *ui_font(ui_font_role_t role);

/* ---- Palette ----------------------------------------------------------- */

#define UI_COLOR_BG            lv_color_hex(0x0B0F14)  /* app background */
#define UI_COLOR_SURFACE       lv_color_hex(0x161C24)  /* cards */
#define UI_COLOR_SURFACE_ALT   lv_color_hex(0x1F2733)  /* pressed / alt rows */
#define UI_COLOR_PRIMARY       lv_color_hex(0x3DA9FC)  /* accent / selection */
#define UI_COLOR_SUCCESS       lv_color_hex(0x2ECC71)
#define UI_COLOR_WARNING       lv_color_hex(0xF1C40F)
#define UI_COLOR_DANGER        lv_color_hex(0xE74C3C)
#define UI_COLOR_TEXT          lv_color_hex(0xF2F5F8)
#define UI_COLOR_TEXT_MUTED    lv_color_hex(0x9AA7B4)
#define UI_COLOR_ON_PRIMARY    lv_color_hex(0x06121C)
#define UI_COLOR_IDLE_FG       lv_color_hex(0xFFFFFF)  /* idle eyes are pure white */

/* ---- Metrics ----------------------------------------------------------- */

#define UI_STATUSBAR_HEIGHT    40
#define UI_PAGE_PAD            14
#define UI_PAGE_GAP            12
#define UI_CARD_RADIUS         14
#define UI_CARD_PAD            14
#define UI_TOUCH_MIN           44      /* minimum touch target, requirement 10.3 */
#define UI_BUTTON_HEIGHT       48
#define UI_LIST_ROW_HEIGHT     56

/**
 * @brief Apply the base theme to the active screen (background, default font).
 */
void ui_theme_apply(void);

#ifdef __cplusplus
}
#endif
