/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pages_internal.h
 * @brief Declarations of every page descriptor plus shared page helpers.
 */

#pragma once

#include "lvgl.h"

#include "ui_core.h"
#include "ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Implemented pages. */
extern const ui_page_t page_splash;
extern const ui_page_t page_home;
extern const ui_page_t page_wifi;
extern const ui_page_t page_todo;
extern const ui_page_t page_todo_detail;
extern const ui_page_t page_weather;
extern const ui_page_t page_calendar;
extern const ui_page_t page_word;
extern const ui_page_t page_settings;
extern const ui_page_t page_ai_chat;

/**
 * @brief Select which todo the detail page should show.
 *
 * The page table has no user-data channel, so the id is handed over through
 * this setter right before ui_nav_push(UI_PAGE_TODO_DETAIL).
 */
void page_todo_detail_set_id(uint32_t id);

/* UI_PAGE_IDLE_FACE is registered by the idle_face component itself
 * (idle_face_init() -> ui_page_register), so it is deliberately absent here. */

/**
 * @brief Build a "coming soon" page body.
 *
 * @param parent Container handed to create().
 * @param title  Feature name.
 * @param note   One line describing what the page will do.
 * @param stage  Milestone tag, e.g. "P0" / "P1" / "P2".
 */
esp_err_t page_placeholder_create(lv_obj_t *parent, const char *title,
                                  const char *note, const char *stage);

/**
 * @brief Build a tappable navigation tile, used by the home page.
 */
lv_obj_t *page_tile_create(lv_obj_t *parent, const char *title, const char *subtitle,
                          ui_page_id_t target);

/**
 * @brief Build a "back" button that calls ui_nav_back().
 *
 * Hidden automatically when there is nothing to go back to.
 */
lv_obj_t *page_back_button_create(lv_obj_t *parent);

/** @brief Shared page header: title on the left, optional back button. */
lv_obj_t *page_header_create(lv_obj_t *parent, const char *title);

/**
 * @brief Create an on-screen keyboard with the project's shared styling.
 *
 * Every keyboard must be created through this so they all look identical. LVGL's
 * default keyboard is light grey and clashed badly with the dark theme, and the
 * panel is only 480 px tall so the row height matters.
 *
 * @param parent   Usually the input overlay.
 * @param textarea Target text area; may be NULL and assigned later.
 * @return The keyboard, or NULL on allocation failure.
 */
lv_obj_t *ui_keyboard_create(lv_obj_t *parent, lv_obj_t *textarea);

#ifdef __cplusplus
}
#endif
