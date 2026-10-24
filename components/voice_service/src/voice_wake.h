/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file voice_wake.h
 * @brief Local keyword wake (VOICE_WAKE_KEYWORD), internal to voice_service.
 *
 * The listener runs a feed/fetch loop on its own task (voice_wake.c) and
 * reaches into voice_service.c's codec state through the three accessors
 * below - they are the whole coupling surface, so the wake listener and the
 * record/play paths share the codec lock and the busy flags without either
 * owning the other.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_codec_dev.h"
#include "voice_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Microphone handle from the (lazily started) codec; NULL before ensure_audio. */
esp_codec_dev_handle_t voice_wake_mic_handle(void);

/** The codec mutex that record and playback take turns on. */
SemaphoreHandle_t voice_wake_codec_lock(void);

/**
 * True while a record or a playback owns/needs the codec. The wake listener
 * yields for as long as this is set; both flags are set at their entry points,
 * so yielding is immediate.
 */
bool voice_wake_codec_busy(void);

/**
 * @brief Bring the AFE + WakeNet pipeline up and start the listener task.
 *
 * Idempotent. Requires the model partition to carry the wakenet model selected
 * by Kconfig; degrades with ESP_ERR_NOT_FOUND (and a loud log) when it does
 * not. Starts the audio bring-up first if the codec is not up yet.
 */
esp_err_t voice_wake_start(void);

/** @brief Stop the listener at the next safe point (never holds the codec). */
void voice_wake_stop(void);

#ifdef __cplusplus
}
#endif
