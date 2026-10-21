/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Publish the /api/v1/ota/... routes. */
esp_err_t ota_service_register_http(void);

#ifdef __cplusplus
}
#endif
