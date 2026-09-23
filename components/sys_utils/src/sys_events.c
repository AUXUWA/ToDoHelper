/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sys_utils.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sys_events";

ESP_EVENT_DEFINE_BASE(SYS_EVENT);

#ifndef CONFIG_TODOHELPER_EVENT_QUEUE_SIZE
#define CONFIG_TODOHELPER_EVENT_QUEUE_SIZE 32
#endif
#ifndef CONFIG_TODOHELPER_EVENT_TASK_STACK
#define CONFIG_TODOHELPER_EVENT_TASK_STACK 4096
#endif
#ifndef CONFIG_TODOHELPER_EVENT_TASK_PRIO
#define CONFIG_TODOHELPER_EVENT_TASK_PRIO 5
#endif

/** Guards lazy creation so two early callers cannot create two loops. */
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static esp_event_loop_handle_t s_loop;

static esp_err_t sys_events_ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t sys_events_init(void)
{
    esp_err_t err = sys_events_ensure_lock();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_loop != NULL) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    const esp_event_loop_args_t args = {
        .queue_size = CONFIG_TODOHELPER_EVENT_QUEUE_SIZE,
        .task_name = "th_events",
        .task_priority = CONFIG_TODOHELPER_EVENT_TASK_PRIO,
        .task_stack_size = CONFIG_TODOHELPER_EVENT_TASK_STACK,
        .task_core_id = tskNO_AFFINITY,
    };

    err = esp_event_loop_create(&args, &s_loop);
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        s_loop = NULL;
        SYS_LOGE(TAG, "esp_event_loop_create failed: %s", esp_err_to_name(err));
        return err;
    }

    SYS_LOGI(TAG, "shared event loop ready");
    return ESP_OK;
}

esp_event_loop_handle_t sys_events_get_loop(void)
{
    return s_loop;
}

esp_err_t sys_event_post_to(esp_event_base_t base, int32_t event_id,
                            const void *data, size_t data_size)
{
    if (s_loop == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Never block the caller: our event consumers are all non-realtime UI/log paths
     * and a full queue means the consumer is wedged, which must not stall services. */
    return esp_event_post_to(s_loop, base, event_id, (void *)data, data_size, 0);
}

esp_err_t sys_events_post(int32_t event_id, const void *data, size_t data_size)
{
    return sys_event_post_to(SYS_EVENT, event_id, data, data_size);
}

esp_err_t sys_event_subscribe(esp_event_base_t base, int32_t event_id,
                              esp_event_handler_t handler, void *handler_arg,
                              esp_event_handler_instance_t *instance)
{
    if (s_loop == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (instance != NULL) {
        return esp_event_handler_instance_register_with(s_loop, base, event_id,
                                                        handler, handler_arg, instance);
    }
    return esp_event_handler_register_with(s_loop, base, event_id, handler, handler_arg);
}

esp_err_t sys_event_unsubscribe(esp_event_base_t base, int32_t event_id,
                                esp_event_handler_instance_t instance)
{
    if (s_loop == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_event_handler_instance_unregister_with(s_loop, base, event_id, instance);
}
