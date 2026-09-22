/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FACTORY_USB_HID_STATE_IDLE = 0,
    FACTORY_USB_HID_STATE_WAITING,
    FACTORY_USB_HID_STATE_CONNECTED,
    FACTORY_USB_HID_STATE_DISCONNECTED,
    FACTORY_USB_HID_STATE_ERROR,
} factory_usb_hid_state_t;

typedef struct {
    factory_usb_hid_state_t state;
    uint32_t connect_count;
    uint32_t disconnect_count;
    uint32_t input_count;
    int mouse_x;
    int mouse_y;
    bool mouse_left;
    bool mouse_right;
    char device[24];
    char last_input[64];
    char status[96];
} factory_usb_hid_snapshot_t;

typedef struct factory_usb_hid_t {
    SemaphoreHandle_t lock;
    QueueHandle_t event_queue;
    TaskHandle_t usb_task;
    TaskHandle_t app_task;
    factory_usb_hid_snapshot_t snapshot;
    bool initialized;
} factory_usb_hid_t;

esp_err_t factory_usb_hid_init(factory_usb_hid_t *hid);
void factory_usb_hid_get_snapshot(factory_usb_hid_t *hid, factory_usb_hid_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
