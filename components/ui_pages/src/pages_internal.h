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

/* Pages still under construction. They render a common placeholder so that
 * navigation is complete from day one; replace the descriptor with a real one
 * in ui_pages.c as each feature lands. */
extern const ui_page_t page_wifi;
extern const ui_page_t page_todo;
extern const ui_page_t page_todo_detail;
extern const ui_page_t page_weather;
extern const ui_page_t page_calendar;
extern const ui_page_t page_word;
extern const ui_page_t page_ai_chat;
extern const ui_page_t page_settings;
extern const ui_page_t page_idle_face;

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

#ifdef __cplusplus
}
#endif
