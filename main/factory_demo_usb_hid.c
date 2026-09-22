/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "factory_demo_usb_hid.h"

#include <string.h>
#include "esp_check.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"

#define FACTORY_USB_HID_EVENT_QUEUE_LEN       10
#define FACTORY_USB_HID_USB_TASK_STACK        4096
#define FACTORY_USB_HID_APP_TASK_STACK        4096
#define FACTORY_USB_HID_DRIVER_TASK_STACK     4096
/* hardware_test runs camera, LVGL, and audio continuously. Keep USB host
 * event processing above those workloads so HID enumeration is not starved.
 */
#define FACTORY_USB_HID_USB_TASK_PRIORITY     7
#define FACTORY_USB_HID_APP_TASK_PRIORITY     6
#define FACTORY_USB_HID_DRIVER_TASK_PRIORITY  7
#define FACTORY_USB_HID_BOOT_WAIT_TICKS       pdMS_TO_TICKS(1000)

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
    void *arg;
} factory_usb_hid_event_t;

static const char *TAG = "factory_usb_hid";

static const char *s_hid_proto_name[] = {
    "NONE",
    "KEYBOARD",
    "MOUSE",
};

static const char *s_hid_device_name[] = {
    "Generic",
    "Keyboard",
    "Mouse",
};

static void factory_usb_hid_set_status(factory_usb_hid_t *hid,
                                       factory_usb_hid_state_t state,
                                       const char *device,
                                       const char *status)
{
    xSemaphoreTake(hid->lock, portMAX_DELAY);
    hid->snapshot.state = state;
    if (device) {
        strlcpy(hid->snapshot.device, device, sizeof(hid->snapshot.device));
    }
    if (status) {
        strlcpy(hid->snapshot.status, status, sizeof(hid->snapshot.status));
    }
    xSemaphoreGive(hid->lock);
}

static const char *factory_usb_hid_proto_name(hid_protocol_t proto)
{
    int proto_idx = (int)proto;

    return (proto_idx >= 0 && proto_idx < (sizeof(s_hid_proto_name) / sizeof(s_hid_proto_name[0]))) ?
           s_hid_proto_name[proto_idx] : "Unknown";
}

static const char *factory_usb_hid_device_name(hid_protocol_t proto)
{
    int proto_idx = (int)proto;

    return (proto_idx >= 0 && proto_idx < (sizeof(s_hid_device_name) / sizeof(s_hid_device_name[0]))) ?
           s_hid_device_name[proto_idx] : "Unknown";
}

static void factory_usb_hid_note_connect(factory_usb_hid_t *hid, hid_protocol_t proto)
{
    const char *device = factory_usb_hid_device_name(proto);

    xSemaphoreTake(hid->lock, portMAX_DELAY);
    hid->snapshot.state = FACTORY_USB_HID_STATE_CONNECTED;
    hid->snapshot.connect_count++;
    strlcpy(hid->snapshot.device, device, sizeof(hid->snapshot.device));
    if (proto == HID_PROTOCOL_NONE) {
        strlcpy(hid->snapshot.status, "Generic HID connected", sizeof(hid->snapshot.status));
    } else {
        snprintf(hid->snapshot.status, sizeof(hid->snapshot.status), "%s connected", device);
    }
    xSemaphoreGive(hid->lock);

    ESP_LOGI(TAG, "HID Device, protocol '%s' CONNECTED", factory_usb_hid_proto_name(proto));
}

static void factory_usb_hid_note_disconnect(factory_usb_hid_t *hid, hid_protocol_t proto)
{
    const char *device = factory_usb_hid_device_name(proto);

    xSemaphoreTake(hid->lock, portMAX_DELAY);
    hid->snapshot.state = FACTORY_USB_HID_STATE_DISCONNECTED;
    hid->snapshot.disconnect_count++;
    strlcpy(hid->snapshot.device, device, sizeof(hid->snapshot.device));
    snprintf(hid->snapshot.status, sizeof(hid->snapshot.status), "%s disconnected", device);
    xSemaphoreGive(hid->lock);

    ESP_LOGI(TAG, "HID Device, protocol '%s' DISCONNECTED", factory_usb_hid_proto_name(proto));
}

static bool factory_usb_hid_key_found(const uint8_t *src, uint8_t key, unsigned int length)
{
    for (unsigned int i = 0; i < length; i++) {
        if (src[i] == key) {
            return true;
        }
    }
    return false;
}

static void factory_usb_hid_keyboard_report(factory_usb_hid_t *hid, const uint8_t *data, int length)
{
    hid_keyboard_input_report_boot_t *report = (hid_keyboard_input_report_boot_t *)data;
    static uint8_t prev_keys[HID_KEYBOARD_KEY_MAX];

    if (length < sizeof(hid_keyboard_input_report_boot_t)) {
        return;
    }

    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
        if (report->key[i] > HID_KEY_ERROR_UNDEFINED &&
                !factory_usb_hid_key_found(prev_keys, report->key[i], HID_KEYBOARD_KEY_MAX)) {
            xSemaphoreTake(hid->lock, portMAX_DELAY);
            hid->snapshot.state = FACTORY_USB_HID_STATE_CONNECTED;
            hid->snapshot.input_count++;
            strlcpy(hid->snapshot.device, "Keyboard", sizeof(hid->snapshot.device));
            snprintf(hid->snapshot.last_input, sizeof(hid->snapshot.last_input),
                     "Key code: 0x%02x", report->key[i]);
            strlcpy(hid->snapshot.status, "Keyboard input", sizeof(hid->snapshot.status));
            xSemaphoreGive(hid->lock);
        }
    }

    memcpy(prev_keys, report->key, HID_KEYBOARD_KEY_MAX);
}

static void factory_usb_hid_mouse_report(factory_usb_hid_t *hid, const uint8_t *data, int length)
{
    hid_mouse_input_report_boot_t *report = (hid_mouse_input_report_boot_t *)data;

    if (length < sizeof(hid_mouse_input_report_boot_t)) {
        return;
    }

    xSemaphoreTake(hid->lock, portMAX_DELAY);
    hid->snapshot.state = FACTORY_USB_HID_STATE_CONNECTED;
    hid->snapshot.input_count++;
    hid->snapshot.mouse_x += report->x_displacement;
    hid->snapshot.mouse_y += report->y_displacement;
    hid->snapshot.mouse_left = report->buttons.button1;
    hid->snapshot.mouse_right = report->buttons.button2;
    strlcpy(hid->snapshot.device, "Mouse", sizeof(hid->snapshot.device));
    snprintf(hid->snapshot.last_input, sizeof(hid->snapshot.last_input),
             "Mouse: x=%d y=%d L=%d R=%d",
             hid->snapshot.mouse_x,
             hid->snapshot.mouse_y,
             hid->snapshot.mouse_left,
             hid->snapshot.mouse_right);
    strlcpy(hid->snapshot.status, "Mouse input", sizeof(hid->snapshot.status));
    xSemaphoreGive(hid->lock);
}

static void factory_usb_hid_interface_callback(hid_host_device_handle_t handle,
                                               const hid_host_interface_event_t event,
                                               void *arg)
{
    factory_usb_hid_t *hid = (factory_usb_hid_t *)arg;
    uint8_t data[64] = {0};
    size_t data_length = 0;
    hid_host_dev_params_t dev_params;

    ESP_ERROR_CHECK(hid_host_device_get_params(handle, &dev_params));

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(handle, data, sizeof(data), &data_length));
        if (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
            if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
                factory_usb_hid_keyboard_report(hid, data, data_length);
            } else if (dev_params.proto == HID_PROTOCOL_MOUSE) {
                factory_usb_hid_mouse_report(hid, data, data_length);
            }
        } else {
            xSemaphoreTake(hid->lock, portMAX_DELAY);
            hid->snapshot.state = FACTORY_USB_HID_STATE_CONNECTED;
            hid->snapshot.input_count++;
            strlcpy(hid->snapshot.device, "Generic", sizeof(hid->snapshot.device));
            snprintf(hid->snapshot.last_input, sizeof(hid->snapshot.last_input), "Generic report: %u bytes",
                     (unsigned int)data_length);
            strlcpy(hid->snapshot.status, "Generic HID input", sizeof(hid->snapshot.status));
            xSemaphoreGive(hid->lock);
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        factory_usb_hid_note_disconnect(hid, dev_params.proto);
        ESP_ERROR_CHECK(hid_host_device_close(handle));
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        factory_usb_hid_set_status(hid, FACTORY_USB_HID_STATE_ERROR, NULL, "HID transfer error");
        break;
    default:
        ESP_LOGW(TAG, "Unhandled HID interface event: %d", event);
        break;
    }
}

static void factory_usb_hid_device_event(factory_usb_hid_t *hid,
                                         hid_host_device_handle_t handle,
                                         hid_host_driver_event_t event,
                                         void *arg)
{
    (void)arg;

    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(handle, &dev_params));

    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }

    factory_usb_hid_note_connect(hid, dev_params.proto);

    const hid_host_device_config_t dev_config = {
        .callback = factory_usb_hid_interface_callback,
        .callback_arg = hid,
    };

    if (dev_params.proto == HID_PROTOCOL_NONE) {
        /* Match the IDF HID host example: generic protocol is reported as connected,
         * but it is not opened as a boot keyboard/mouse interface here.
         */
        return;
    }

    ESP_ERROR_CHECK(hid_host_device_open(handle, &dev_config));
    if (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        ESP_ERROR_CHECK(hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT));
        if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
            ESP_ERROR_CHECK(hid_class_request_set_idle(handle, 0, 0));
        }
    }
    ESP_ERROR_CHECK(hid_host_device_start(handle));
}

static void factory_usb_hid_device_callback(hid_host_device_handle_t handle,
                                            const hid_host_driver_event_t event,
                                            void *arg)
{
    factory_usb_hid_t *hid = (factory_usb_hid_t *)arg;
    const factory_usb_hid_event_t queued_event = {
        .handle = handle,
        .event = event,
        .arg = arg,
    };

    if (hid && hid->event_queue && xQueueSend(hid->event_queue, &queued_event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Drop HID driver event: queue full, event=%d", event);
    }
}

static void factory_usb_hid_usb_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB host installed");
    xTaskNotifyGive((TaskHandle_t)arg);

    while (true) {
        uint32_t event_flags = 0;
        (void)usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

static void factory_usb_hid_app_task(void *arg)
{
    factory_usb_hid_t *hid = (factory_usb_hid_t *)arg;
    factory_usb_hid_event_t event;

    BaseType_t task_created = xTaskCreatePinnedToCore(factory_usb_hid_usb_task,
                                                      "usb_events",
                                                      FACTORY_USB_HID_USB_TASK_STACK,
                                                      xTaskGetCurrentTaskHandle(),
                                                      FACTORY_USB_HID_USB_TASK_PRIORITY,
                                                      &hid->usb_task,
                                                      0);
    if (task_created != pdTRUE) {
        factory_usb_hid_set_status(hid, FACTORY_USB_HID_STATE_ERROR, NULL, "USB task create failed");
        vTaskDelete(NULL);
        return;
    }

    if (ulTaskNotifyTake(pdFALSE, FACTORY_USB_HID_BOOT_WAIT_TICKS) == 0) {
        factory_usb_hid_set_status(hid, FACTORY_USB_HID_STATE_ERROR, NULL, "USB host install timeout");
        vTaskDelete(NULL);
        return;
    }

    const hid_host_driver_config_t driver_config = {
        .create_background_task = true,
        .task_priority = FACTORY_USB_HID_DRIVER_TASK_PRIORITY,
        .stack_size = FACTORY_USB_HID_DRIVER_TASK_STACK,
        .core_id = 0,
        .callback = factory_usb_hid_device_callback,
        .callback_arg = hid,
    };

    ESP_ERROR_CHECK(hid_host_install(&driver_config));
    ESP_LOGI(TAG, "USB HID host initialized");
    factory_usb_hid_set_status(hid, FACTORY_USB_HID_STATE_WAITING, "None", "Waiting for HID device");

    while (true) {
        if (xQueueReceive(hid->event_queue, &event, portMAX_DELAY) == pdTRUE) {
            factory_usb_hid_device_event(hid, event.handle, event.event, event.arg);
        }
    }
}

esp_err_t factory_usb_hid_init(factory_usb_hid_t *hid)
{
    ESP_RETURN_ON_FALSE(hid, ESP_ERR_INVALID_ARG, TAG, "hid handle is null");
    memset(hid, 0, sizeof(*hid));

    hid->lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(hid->lock, ESP_ERR_NO_MEM, TAG, "hid mutex alloc failed");

    hid->event_queue = xQueueCreate(FACTORY_USB_HID_EVENT_QUEUE_LEN, sizeof(factory_usb_hid_event_t));
    ESP_RETURN_ON_FALSE(hid->event_queue, ESP_ERR_NO_MEM, TAG, "hid event queue alloc failed");

    hid->snapshot.state = FACTORY_USB_HID_STATE_IDLE;
    strlcpy(hid->snapshot.device, "None", sizeof(hid->snapshot.device));
    strlcpy(hid->snapshot.status, "Starting USB HID host", sizeof(hid->snapshot.status));
    strlcpy(hid->snapshot.last_input, "No input", sizeof(hid->snapshot.last_input));

    BaseType_t ok = xTaskCreate(factory_usb_hid_app_task,
                                "factory_usb_hid",
                                FACTORY_USB_HID_APP_TASK_STACK,
                                hid,
                                FACTORY_USB_HID_APP_TASK_PRIORITY,
                                &hid->app_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "hid app task create failed");

    hid->initialized = true;
    return ESP_OK;
}

void factory_usb_hid_get_snapshot(factory_usb_hid_t *hid, factory_usb_hid_snapshot_t *snapshot)
{
    if (!hid || !snapshot || !hid->lock) {
        return;
    }

    xSemaphoreTake(hid->lock, portMAX_DELAY);
    *snapshot = hid->snapshot;
    xSemaphoreGive(hid->lock);
}
