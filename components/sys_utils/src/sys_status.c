/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sys_status.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static sys_status_t s_status;

static esp_err_t status_lock_ensure(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t sys_status_get(sys_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = status_lock_ensure();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);

    /* Time is derived, not stored: refresh it on every read so the status bar
     * clock keeps moving without anyone having to publish every second. */
    out->time_synced = sys_time_synced();
    out->now_ts = sys_time_ts();
    return ESP_OK;
}

esp_err_t sys_status_update(void (*mutator)(sys_status_t *status, void *arg), void *arg)
{
    if (mutator == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = status_lock_ensure();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    mutator(&s_status, arg);
    xSemaphoreGive(s_lock);

    return sys_status_publish();
}

esp_err_t sys_status_publish(void)
{
    /* No payload: consumers call sys_status_get() for the details, which keeps
     * the shared event queue free of a 100+ byte struct copy per update. */
    return sys_events_post(SYS_EVENT_STATUS_CHANGED, NULL, 0);
}
