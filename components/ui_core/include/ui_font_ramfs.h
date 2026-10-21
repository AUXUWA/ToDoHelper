/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_font_ramfs.h
 * @brief A read-only file served straight out of RAM through the IDF VFS.
 *
 * Why this exists
 * ---------------
 * FreeType reads the .ttf through plain stdio (fopen/fread), so whatever path
 * it is given decides how fast a glyph can be rasterised. The font used to sit
 * on the SD card, and every glyph cost ~9.8 ms of SD-card reads (FreeType's own
 * work is 100~200 us). That is what makes a cold screen take 300+ ms.
 *
 * LVGL's own answer to this is its memfs driver, but that only works with
 * `LV_FREETYPE_USE_LVGL_PORT`, and turning that on makes FreeType and LVGL
 * both define `FT_Stream_Open` - the ESP-IDF FreeType build always compiles
 * its own ftsystem.c, so the link fails.
 *
 * So this module does the same job one layer lower: it registers a **read-only
 * VFS** whose files are plain RAM buffers. FreeType keeps using fopen(), the
 * path stays a normal path ("/ramfont/ui_cjk.ttf"), and nothing outside this
 * project has to change.
 */

#ifndef UI_FONT_RAMFS_H
#define UI_FONT_RAMFS_H

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the RAM filesystem (idempotent).
 *
 * Must be called before ui_font_ramfs_add(). Safe to call more than once.
 *
 * @return ESP_OK when the filesystem is registered (or was already),
 *         an error code otherwise.
 */
esp_err_t ui_font_ramfs_init(void);

/**
 * @brief Publish a RAM buffer as a file.
 *
 * The buffer must stay valid for as long as the file may be opened - the
 * module does not copy it and does not free it.
 *
 * @param name  File name, e.g. "ui_cjk.ttf" (no directory part).
 * @param data  The bytes.
 * @param size  Number of bytes.
 * @return ESP_OK on success.
 */
esp_err_t ui_font_ramfs_add(const char *name, const void *data, size_t size);

/**
 * @brief Absolute path of a published file.
 *
 * @param name  The name passed to ui_font_ramfs_add().
 * @return The path (e.g. "/ramfont/ui_cjk.ttf"), or NULL when the file is not
 *         published. The returned string is owned by the module.
 */
const char *ui_font_ramfs_path(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* UI_FONT_RAMFS_H */
