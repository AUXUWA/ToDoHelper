/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sys_notify.h
 * @brief Fire-and-forget change notifications between layers.
 *
 * A service (L3) must not call sync_server (L4) to push an SSE event, and
 * sync_server must not know every service. Both sides therefore meet on this
 * L0 channel:
 *
 *   todo_manager --sys_notify("todo_changed")--> SYS_NOTIFY_EVENT
 *                                                   |
 *                                        sync_server (SSE) / ui_core (redraw)
 *
 * The payload is carried by value so receivers never see a dangling pointer.
 */

#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(SYS_NOTIFY_EVENT);

/** Maximum length of a notification name (without NUL). */
#define SYS_NOTIFY_NAME_MAX 24
/** Maximum length of the attached JSON payload (without NUL). */
#define SYS_NOTIFY_PAYLOAD_MAX 192

/** Notification payload, copied by value into the event loop queue. */
typedef struct {
    char name[SYS_NOTIFY_NAME_MAX + 1];
    char payload[SYS_NOTIFY_PAYLOAD_MAX + 1];
} sys_notify_t;

/**
 * @brief Publish a notification on the shared event loop.
 *
 * Non-blocking: when the queue is full the notification is dropped and
 * ESP_ERR_TIMEOUT is returned. Notifications are hints for the UI and the web
 * clients, never the only copy of any data.
 *
 * @param name    Short snake_case name, e.g. "todo_changed".
 * @param payload Optional serialised JSON (pass NULL for "{}").
 */
esp_err_t sys_notify(const char *name, const char *payload);

/** @brief Register a receiver for every notification. */
esp_err_t sys_notify_subscribe(esp_event_handler_t handler, void *handler_arg,
                               esp_event_handler_instance_t *instance);

#ifdef __cplusplus
}
#endif
