/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file todo_internal.h
 * @brief Helpers shared between the todo_manager translation units. Not public.
 */

#pragma once

#include "cJSON.h"

#include "todo_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Serialise one item using the shape documented in docs/API.md. */
cJSON *todo_item_to_json(const todo_item_t *item);

/**
 * @brief Parse an item body produced by the HTTP API.
 *
 * @param obj        Parsed JSON object.
 * @param out        Destination.
 * @param require_title Reject the body when "title" is missing or empty.
 */
esp_err_t todo_item_from_json(const cJSON *obj, todo_item_t *out, bool require_title);

#ifdef __cplusplus
}
#endif
