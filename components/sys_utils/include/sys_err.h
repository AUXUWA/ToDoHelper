/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sys_err.h
 * @brief Fine-grained internal error causes plus their esp_err_t mapping.
 *
 * Public component APIs always return an IDF esp_err_t. sys_err_t only exists
 * so that a component can keep a finer reason internally and log it, then
 * convert on the way out with sys_err_to_esp(). Never cast the enum directly
 * to esp_err_t (see docs/TODOHELPER_REQUIREMENTS.md 7.1).
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYS_OK = 0,
    SYS_ERR_INVALID_ARG,
    SYS_ERR_NO_MEM,
    SYS_ERR_NOT_FOUND,
    SYS_ERR_STORAGE,
    SYS_ERR_NETWORK,
    SYS_ERR_TIMEOUT,
    SYS_ERR_BUSY,
    SYS_ERR_INVALID_STATE,
    SYS_ERR_NOT_SUPPORTED,
    SYS_ERR_UNKNOWN,
} sys_err_t;

/**
 * @brief Map an internal cause onto the IDF error code returned by public APIs.
 */
esp_err_t sys_err_to_esp(sys_err_t err);

/**
 * @brief Best-effort reverse mapping, used when wrapping IDF calls.
 */
sys_err_t sys_esp_to_err(esp_err_t err);

/**
 * @brief Human readable name, for logs and JSON error details.
 */
const char *sys_err_name(sys_err_t err);

#ifdef __cplusplus
}
#endif
