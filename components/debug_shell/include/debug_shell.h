/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file debug_shell.h
 * @brief A tiny shell on the device so a developer can inspect and drive the UI
 *        over HTTP, without a serial cable and without anyone at the board.
 *
 * ## Why this exists
 *
 * Several UI bugs (v1.3.1's "the token dialog will not close") could only be
 * confirmed by *looking at the screen* - and the previous verification step for
 * them was literally "please tap this button and tell me what you see". That
 * does not scale and it does not work when the board is somewhere else.
 *
 * There was also no way to switch pages or press a widget from a script, so
 * anything behind a tap was untestable from CI/curl.
 *
 * This component closes that gap: `POST /api/v1/debug/exec` runs one command and
 * returns its text output, the way a shell does. Commands are grouped as
 *
 *   - **inspect**: `help`, `heap`, `tasks`, `routes`, `pages`, `ui`, `widgets`,
 *     `log`, `nvs`, `cfg`, `time`, `version`, `status`
 *   - **drive**:   `page <id|name>`, `click <target>`, `tap <x> <y>`,
 *     `text <target> <value>`, `idle <enter|exit>`, `bright <0-100>`,
 *     `reboot`
 *
 * ## Safety
 *
 * This is a **development** aid, and it can reboot the device or move its UI:
 *
 *  - the whole component is behind `CONFIG_TODOHELPER_DEBUG_SHELL` (default
 *    **off**, see the Kconfig); with it off nothing here is compiled in and no
 *    route is registered;
 *  - every route it registers goes through the project's normal auth hook, so
 *    with an HTTP token configured the shell needs `X-Auth-Token` like the rest;
 *  - `debug_shell_register_http()` additionally marks its routes as
 *    auth-required even when auth is globally off, so enabling the shell in a
 *    build does not silently expose device control on an open network.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the `/api/v1/debug/...` routes.
 *
 * Called from the module table in app_main like any other service. When
 * `CONFIG_TODOHELPER_DEBUG_SHELL` is not set this is a no-op that returns
 * ESP_OK, so the module row can stay in the table unconditionally.
 */
esp_err_t debug_shell_init(void);

/**
 * @brief Run one command and copy its plain-text output into a buffer.
 *
 * This is the same engine the HTTP route uses (`POST /api/v1/debug/exec`),
 * exposed so that a command can also come from somewhere else - today: the
 * serial console (debug_shell_serial.c), which lets the web console drive the
 * device over a USB serial cable even when its HTTP server is not answering.
 *
 * @param line      The command line.
 * @param out       Buffer for the output text.
 * @param out_len   In: the size of @p out. Out: the number of bytes written.
 * @return ESP_OK on success (also when the command itself failed - the
 *         failure is in the text), ESP_ERR_NO_MEM when the output did not
 *         fit.
 */
esp_err_t debug_shell_run(const char *line, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
