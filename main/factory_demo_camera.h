/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/ppa.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FACTORY_CAMERA_STATE_IDLE = 0,
    FACTORY_CAMERA_STATE_STARTING,
    FACTORY_CAMERA_STATE_PREVIEW,
    FACTORY_CAMERA_STATE_ERROR,
} factory_camera_state_t;

typedef struct {
    factory_camera_state_t state;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t pixelformat;
    uint32_t frame_count;
    char status[96];
} factory_camera_snapshot_t;

typedef struct factory_camera_t {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    void *bsp_camera;
    lv_display_t *disp;
    lv_obj_t *canvas;
    void *canvas_buf;
    uint32_t canvas_width;
    uint32_t canvas_height;
    uint32_t canvas_stride;
    bool render_enabled;
    bool stop_requested;
    factory_camera_state_t state;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t pixelformat;
    uint32_t frame_count;
    ppa_client_handle_t ppa_srm;
    void *source_buf;
    void *preview_buf;
    char status[96];
} factory_camera_t;

esp_err_t factory_camera_init(factory_camera_t *camera, lv_display_t *disp);
esp_err_t factory_camera_attach_canvas(factory_camera_t *camera, lv_obj_t *canvas, void *canvas_buf,
                                       uint32_t canvas_width, uint32_t canvas_height, uint32_t canvas_stride);
esp_err_t factory_camera_start_preview(factory_camera_t *camera);
esp_err_t factory_camera_stop_preview(factory_camera_t *camera);
esp_err_t factory_camera_set_display(factory_camera_t *camera, lv_display_t *disp);
esp_err_t factory_camera_set_render_enabled(factory_camera_t *camera, bool enable);
void factory_camera_get_snapshot(factory_camera_t *camera, factory_camera_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
