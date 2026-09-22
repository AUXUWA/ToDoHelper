/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FACTORY_AUDIO_STATE_IDLE = 0,
    FACTORY_AUDIO_STATE_RECORDING,
    FACTORY_AUDIO_STATE_PLAYING,
    FACTORY_AUDIO_STATE_ERROR,
} factory_audio_state_t;

typedef struct {
    factory_audio_state_t state;
    size_t recorded_size;
    size_t capacity;
    char status[96];
} factory_audio_snapshot_t;

typedef struct factory_audio_t {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    esp_codec_dev_handle_t mic;
    esp_codec_dev_handle_t speaker;
    uint8_t *buffer;
    size_t buffer_size;
    size_t recorded_size;
    size_t play_offset;
    bool stop_requested;
    bool initialized;
    factory_audio_state_t state;
    char status[96];
} factory_audio_t;

esp_err_t factory_audio_init(factory_audio_t *audio);
esp_err_t factory_audio_start_record(factory_audio_t *audio);
esp_err_t factory_audio_start_playback(factory_audio_t *audio);
esp_err_t factory_audio_stop(factory_audio_t *audio);
void factory_audio_get_snapshot(factory_audio_t *audio, factory_audio_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
