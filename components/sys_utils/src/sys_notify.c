/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sys_notify.h"

#include <string.h>

#include "sys_utils.h"

static const char *TAG = "sys_notify";

ESP_EVENT_DEFINE_BASE(SYS_NOTIFY_EVENT);

esp_err_t sys_notify(const char *name, const char *payload)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(name) > SYS_NOTIFY_NAME_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    sys_notify_t note;
    memset(&note, 0, sizeof(note));
    strlcpy(note.name, name, sizeof(note.name));
    strlcpy(note.payload, (payload != NULL) ? payload : "{}", sizeof(note.payload));

    const esp_err_t err = sys_event_post_to(SYS_NOTIFY_EVENT, 0, &note, sizeof(note));
    if (err != ESP_OK) {
        SYS_LOGD(TAG, "dropped notification '%s' (%s)", name, esp_err_to_name(err));
    }
    return err;
}

esp_err_t sys_notify_subscribe(esp_event_handler_t handler, void *handler_arg,
                               esp_event_handler_instance_t *instance)
{
    return sys_event_subscribe(SYS_NOTIFY_EVENT, ESP_EVENT_ANY_ID, handler, handler_arg, instance);
}
