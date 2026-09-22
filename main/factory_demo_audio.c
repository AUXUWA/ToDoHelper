/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "factory_demo_audio.h"

#include <string.h>
#include "bsp/esp32_s31_korvo.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define FACTORY_AUDIO_BUFFER_SECONDS      6
#define FACTORY_AUDIO_CHUNK_BYTES         (4 * 1024)
#define FACTORY_AUDIO_SAMPLE_RATE         BSP_AUDIO_DEFAULT_SAMPLE_RATE
#define FACTORY_AUDIO_BITS_PER_SAMPLE     BSP_AUDIO_DEFAULT_BITS_PER_SAMPLE
#define FACTORY_AUDIO_CHANNELS            BSP_AUDIO_DEFAULT_CHANNELS
#define FACTORY_AUDIO_BYTES_PER_SECOND    (FACTORY_AUDIO_SAMPLE_RATE * FACTORY_AUDIO_CHANNELS * (FACTORY_AUDIO_BITS_PER_SAMPLE / 8))
#define FACTORY_AUDIO_BUFFER_BYTES        (FACTORY_AUDIO_BYTES_PER_SECOND * FACTORY_AUDIO_BUFFER_SECONDS)
#define FACTORY_AUDIO_TASK_STACK          6144
#define FACTORY_AUDIO_TASK_PRIORITY       5

static const char *TAG = "factory_audio";

static esp_err_t factory_audio_codec_to_err(int ret, const char *operation)
{
    if (ret == ESP_CODEC_DEV_OK) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s failed (%d)", operation, ret);
    return ESP_FAIL;
}

static void factory_audio_set_status_locked(factory_audio_t *audio, factory_audio_state_t state, const char *status)
{
    audio->state = state;
    strlcpy(audio->status, status, sizeof(audio->status));
}

static void factory_audio_task(void *arg)
{
    factory_audio_t *audio = (factory_audio_t *)arg;
    uint8_t *chunk = heap_caps_malloc(FACTORY_AUDIO_CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!chunk) {
        xSemaphoreTake(audio->lock, portMAX_DELAY);
        factory_audio_set_status_locked(audio, FACTORY_AUDIO_STATE_ERROR, "Audio chunk alloc failed");
        xSemaphoreGive(audio->lock);
        ESP_LOGE(TAG, "Failed to allocate audio chunk buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        factory_audio_state_t state;
        size_t recorded_size;
        size_t play_offset;

        xSemaphoreTake(audio->lock, portMAX_DELAY);
        state = audio->state;
        recorded_size = audio->recorded_size;
        play_offset = audio->play_offset;
        xSemaphoreGive(audio->lock);

        if (state == FACTORY_AUDIO_STATE_RECORDING) {
            size_t remain = audio->buffer_size - recorded_size;
            if (remain == 0) {
                xSemaphoreTake(audio->lock, portMAX_DELAY);
                audio->stop_requested = false;
                factory_audio_set_status_locked(audio, FACTORY_AUDIO_STATE_IDLE, "Recording full");
                xSemaphoreGive(audio->lock);
                ESP_LOGI(TAG, "Recording buffer is full");
                continue;
            }

            size_t chunk_bytes = remain > FACTORY_AUDIO_CHUNK_BYTES ? FACTORY_AUDIO_CHUNK_BYTES : remain;
            esp_err_t ret = factory_audio_codec_to_err(esp_codec_dev_read(audio->mic, chunk, chunk_bytes), "read microphone");
            if (ret != ESP_OK) {
                xSemaphoreTake(audio->lock, portMAX_DELAY);
                audio->stop_requested = false;
                factory_audio_set_status_locked(audio, FACTORY_AUDIO_STATE_ERROR, "Microphone read failed");
                xSemaphoreGive(audio->lock);
                continue;
            }

            memcpy(audio->buffer + recorded_size, chunk, chunk_bytes);

            xSemaphoreTake(audio->lock, portMAX_DELAY);
            audio->recorded_size += chunk_bytes;
            if (audio->stop_requested) {
                audio->stop_requested = false;
                factory_audio_set_status_locked(audio, FACTORY_AUDIO_STATE_IDLE, "Recording stopped");
            } else {
                factory_audio_set_status_locked(audio, FACTORY_AUDIO_STATE_RECORDING, "Recording...");
            }
            xSemaphoreGive(audio->lock);
            continue;
        }

        if (state == FACTORY_AUDIO_STATE_PLAYING) {
            if (play_offset >= recorded_size) {
                xSemaphoreTake(audio->lock, portMAX_DELAY);
                audio->play_offset = 0;
                audio->stop_requested = false;
                factory_audio_set_status_locked(audio, FACTORY_AUDIO_STATE_IDLE, "Playback finished");
                xSemaphoreGive(audio->lock);
                ESP_LOGI(TAG, "Playback finished");
                continue;
            }

            size_t remain = recorded_size - play_offset;
            size_t chunk_bytes = remain > FACTORY_AUDIO_CHUNK_BYTES ? FACTORY_AUDIO_CHUNK_BYTES : remain;
            esp_err_t ret = factory_audio_codec_to_err(esp_codec_dev_write(audio->speaker, audio->buffer + play_offset, chunk_bytes),
                                                       "write speaker");
            if (ret != ESP_OK) {
                xSemaphoreTake(audio->lock, portMAX_DELAY);
                audio->play_offset = 0;
                audio->stop_requested = false;
                factory_audio_set_status_locked(audio, FACTORY_AUDIO_STATE_ERROR, "Speaker write failed");
                xSemaphoreGive(audio->lock);
                continue;
            }

            xSemaphoreTake(audio->lock, portMAX_DELAY);
            audio->play_offset += chunk_bytes;
            if (audio->stop_requested) {
                audio->stop_requested = false;
                audio->play_offset = 0;
                factory_audio_set_status_locked(audio, FACTORY_AUDIO_STATE_IDLE, "Playback stopped");
            } else {
                factory_audio_set_status_locked(audio, FACTORY_AUDIO_STATE_PLAYING, "Playing...");
            }
            xSemaphoreGive(audio->lock);
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t factory_audio_init(factory_audio_t *audio)
{
    ESP_RETURN_ON_FALSE(audio, ESP_ERR_INVALID_ARG, TAG, "audio handle is null");
    memset(audio, 0, sizeof(*audio));

    audio->lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(audio->lock, ESP_ERR_NO_MEM, TAG, "audio mutex alloc failed");

    audio->buffer = heap_caps_malloc(FACTORY_AUDIO_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(audio->buffer, ESP_ERR_NO_MEM, TAG, "audio buffer alloc failed");
    memset(audio->buffer, 0, FACTORY_AUDIO_BUFFER_BYTES);
    audio->buffer_size = FACTORY_AUDIO_BUFFER_BYTES;

    audio->mic = bsp_audio_codec_microphone_init();
    audio->speaker = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(audio->mic && audio->speaker, ESP_FAIL, TAG, "audio codec init failed");

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = FACTORY_AUDIO_SAMPLE_RATE,
        .bits_per_sample = FACTORY_AUDIO_BITS_PER_SAMPLE,
        .channel = FACTORY_AUDIO_CHANNELS,
        .channel_mask = 0,
    };

    ESP_RETURN_ON_ERROR(factory_audio_codec_to_err(esp_codec_dev_open(audio->mic, &sample_info), "open microphone"),
                        TAG, "open microphone failed");
    ESP_RETURN_ON_ERROR(factory_audio_codec_to_err(esp_codec_dev_open(audio->speaker, &sample_info), "open speaker"),
                        TAG, "open speaker failed");
    ESP_RETURN_ON_ERROR(factory_audio_codec_to_err(esp_codec_dev_set_in_gain(audio->mic, 30.0f), "set microphone gain"),
                        TAG, "set microphone gain failed");
    ESP_RETURN_ON_ERROR(factory_audio_codec_to_err(esp_codec_dev_set_out_vol(audio->speaker, 55), "set speaker volume"),
                        TAG, "set speaker volume failed");

    factory_audio_set_status_locked(audio, FACTORY_AUDIO_STATE_IDLE, "Ready to record");
    audio->initialized = true;

    BaseType_t ok = xTaskCreate(factory_audio_task, "factory_audio", FACTORY_AUDIO_TASK_STACK, audio,
                                FACTORY_AUDIO_TASK_PRIORITY, &audio->task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "audio task create failed");

    ESP_LOGI(TAG, "Audio service initialized: %zu bytes buffer in PSRAM", audio->buffer_size);
    return ESP_OK;
}

esp_err_t factory_audio_start_record(factory_audio_t *audio)
{
    ESP_RETURN_ON_FALSE(audio && audio->initialized, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");

    xSemaphoreTake(audio->lock, portMAX_DELAY);
    if (audio->state != FACTORY_AUDIO_STATE_IDLE) {
        xSemaphoreGive(audio->lock);
        return ESP_ERR_INVALID_STATE;
    }

    audio->recorded_size = 0;
    audio->play_offset = 0;
    audio->stop_requested = false;
    memset(audio->buffer, 0, audio->buffer_size);
    factory_audio_set_status_locked(audio, FACTORY_AUDIO_STATE_RECORDING, "Recording...");
    xSemaphoreGive(audio->lock);

    ESP_LOGI(TAG, "Audio recording started");
    return ESP_OK;
}

esp_err_t factory_audio_start_playback(factory_audio_t *audio)
{
    ESP_RETURN_ON_FALSE(audio && audio->initialized, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");

    xSemaphoreTake(audio->lock, portMAX_DELAY);
    if (audio->state != FACTORY_AUDIO_STATE_IDLE) {
        xSemaphoreGive(audio->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (audio->recorded_size == 0) {
        factory_audio_set_status_locked(audio, FACTORY_AUDIO_STATE_IDLE, "No recording available");
        xSemaphoreGive(audio->lock);
        return ESP_ERR_INVALID_STATE;
    }

    audio->play_offset = 0;
    audio->stop_requested = false;
    factory_audio_set_status_locked(audio, FACTORY_AUDIO_STATE_PLAYING, "Playing...");
    xSemaphoreGive(audio->lock);

    ESP_LOGI(TAG, "Audio playback started: %zu bytes", audio->recorded_size);
    return ESP_OK;
}

esp_err_t factory_audio_stop(factory_audio_t *audio)
{
    ESP_RETURN_ON_FALSE(audio && audio->initialized, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");

    xSemaphoreTake(audio->lock, portMAX_DELAY);
    if (audio->state == FACTORY_AUDIO_STATE_IDLE || audio->state == FACTORY_AUDIO_STATE_ERROR) {
        xSemaphoreGive(audio->lock);
        return ESP_OK;
    }
    audio->stop_requested = true;
    xSemaphoreGive(audio->lock);

    ESP_LOGI(TAG, "Audio stop requested");
    return ESP_OK;
}

void factory_audio_get_snapshot(factory_audio_t *audio, factory_audio_snapshot_t *snapshot)
{
    if (!audio || !snapshot) {
        return;
    }
    if (!audio->initialized || !audio->lock) {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->state = FACTORY_AUDIO_STATE_IDLE;
        strlcpy(snapshot->status, "Audio disabled", sizeof(snapshot->status));
        return;
    }

    xSemaphoreTake(audio->lock, portMAX_DELAY);
    snapshot->state = audio->state;
    snapshot->recorded_size = audio->recorded_size;
    snapshot->capacity = audio->buffer_size;
    strlcpy(snapshot->status, audio->status, sizeof(snapshot->status));
    xSemaphoreGive(audio->lock);
}
