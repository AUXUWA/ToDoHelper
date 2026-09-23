/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sys_err.h"

esp_err_t sys_err_to_esp(sys_err_t err)
{
    switch (err) {
    case SYS_OK:
        return ESP_OK;
    case SYS_ERR_INVALID_ARG:
        return ESP_ERR_INVALID_ARG;
    case SYS_ERR_NO_MEM:
        return ESP_ERR_NO_MEM;
    case SYS_ERR_NOT_FOUND:
        return ESP_ERR_NOT_FOUND;
    case SYS_ERR_STORAGE:
        return ESP_ERR_INVALID_STATE;
    case SYS_ERR_NETWORK:
        return ESP_ERR_INVALID_RESPONSE;
    case SYS_ERR_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case SYS_ERR_BUSY:
        return ESP_ERR_INVALID_STATE;
    case SYS_ERR_INVALID_STATE:
        return ESP_ERR_INVALID_STATE;
    case SYS_ERR_NOT_SUPPORTED:
        return ESP_ERR_NOT_SUPPORTED;
    case SYS_ERR_UNKNOWN:
    default:
        return ESP_FAIL;
    }
}

sys_err_t sys_esp_to_err(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return SYS_OK;
    case ESP_ERR_INVALID_ARG:
        return SYS_ERR_INVALID_ARG;
    case ESP_ERR_NO_MEM:
        return SYS_ERR_NO_MEM;
    case ESP_ERR_NOT_FOUND:
        return SYS_ERR_NOT_FOUND;
    case ESP_ERR_TIMEOUT:
        return SYS_ERR_TIMEOUT;
    case ESP_ERR_INVALID_STATE:
        return SYS_ERR_BUSY;
    case ESP_ERR_NOT_SUPPORTED:
        return SYS_ERR_NOT_SUPPORTED;
    case ESP_ERR_INVALID_SIZE:
    case ESP_ERR_INVALID_RESPONSE:
    case ESP_ERR_INVALID_CRC:
        return SYS_ERR_INVALID_ARG;
    default:
        return SYS_ERR_UNKNOWN;
    }
}

const char *sys_err_name(sys_err_t err)
{
    switch (err) {
    case SYS_OK:
        return "ok";
    case SYS_ERR_INVALID_ARG:
        return "invalid_arg";
    case SYS_ERR_NO_MEM:
        return "no_mem";
    case SYS_ERR_NOT_FOUND:
        return "not_found";
    case SYS_ERR_STORAGE:
        return "storage";
    case SYS_ERR_NETWORK:
        return "network";
    case SYS_ERR_TIMEOUT:
        return "timeout";
    case SYS_ERR_BUSY:
        return "busy";
    case SYS_ERR_INVALID_STATE:
        return "invalid_state";
    case SYS_ERR_NOT_SUPPORTED:
        return "not_supported";
    case SYS_ERR_UNKNOWN:
    default:
        return "unknown";
    }
}
