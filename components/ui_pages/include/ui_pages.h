/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_pages.h
 * @brief Page table of the whole application.
 *
 * This is the single composition point for pages: adding a feature page means
 * adding one file under src/ and one row in ui_pages.c (see
 * docs/TODOHELPER_FRAMEWORK.md 7.2).
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register every page with ui_core. Call after ui_core_init().
 */
esp_err_t ui_pages_init(void);

#ifdef __cplusplus
}
#endif
