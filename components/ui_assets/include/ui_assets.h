/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_assets.h
 * @brief L2 access to the PNG icons packed into the `assets` partition.
 *
 * The packing itself happens in this component's CMakeLists.txt (see the note
 * about the flat-directory constraint in docs/TODOHELPER_FRAMEWORK.md 8), so
 * main/ only has to call ui_assets_init() once.
 *
 * Lookup is by base name (file name without extension), which keeps call sites
 * free of file-name details: `ui_assets_src("w_sunny")` -> "A:w_sunny.png".
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mount the assets partition and register it with the LVGL adapter.
 *
 * Failing is not fatal: every getter then returns NULL, and callers fall back
 * to text (requirement 12: a missing asset must not blank the page).
 */
esp_err_t ui_assets_init(void);

/** @brief True when the partition is mounted and usable. */
bool ui_assets_ready(void);

/** @brief Number of packed files (0 when not initialised). */
int ui_assets_count(void);

/**
 * @brief LVGL image source for @p base_name, or NULL when it is not packed.
 *
 * The returned pointer is owned by this component and stays valid until
 * ui_assets_deinit().
 */
const char *ui_assets_src(const char *base_name);

/**
 * @brief Create an image widget sized @p size x @p size.
 *
 * When the icon is missing the widget is created but hidden, so layout stays
 * stable and the caller can put a text fallback in the same slot.
 */
lv_obj_t *ui_assets_image_create(lv_obj_t *parent, const char *base_name, int32_t size);

/**
 * @brief Weather icon name for a condition, falling back to "w_overcast".
 */
const char *ui_assets_weather_src(const char *icon_base);

#ifdef __cplusplus
}
#endif
