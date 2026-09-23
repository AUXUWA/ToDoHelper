/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_internal.h
 * @brief State shared between the ui_core translation units. Not public.
 */

#pragma once

#include "lvgl.h"

#include "ui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the status bar inside @p parent (a flex column). */
lv_obj_t *ui_statusbar_create(lv_obj_t *parent);

/** Content area below the status bar; page containers are its children. */
lv_obj_t *ui_content_area(void);

#ifdef __cplusplus
}
#endif
