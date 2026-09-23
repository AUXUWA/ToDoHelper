/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file voice_service.h
 * @brief FR7: microphone capture, playback, and the ASR/TTS integration points.
 *
 * Hardware status (from AGENT.md / requirement section 4): I2S + esp_codec_dev
 * are already implemented, so capture and playback are real; the **microphone
 * model** and **whether a speaker/PA is fitted** are still unconfirmed
 * (requirement 17.2), and the ASR/TTS provider choice is still open
 * (requirement 17.2 items 6 and 7).
 *
 * This component therefore does the honest thing:
 *   - capture and playback are implemented and usable today;
 *   - ASR/TTS talk to a **configurable HTTP endpoint**, so choosing a provider is
 *     a configuration step, not a rewrite;
 *   - every unavailable piece degrades to the paths the requirement mandates:
 *     no microphone -> on-screen keyboard / HTTP text input, no speaker -> text
 *     bubbles only.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#include "sys_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(VOICE_EVENT);

typedef enum {
    VOICE_EVENT_RECORD_DONE = 0, /**< payload: voice_recording_t */
    VOICE_EVENT_ASR_DONE,        /**< payload: const char *text ("" on failure) */
    VOICE_EVENT_PLAY_DONE,       /**< no payload */
    VOICE_EVENT_WAKE,            /**< no payload: user pressed to talk */
} voice_event_id_t;

typedef enum {
    VOICE_WAKE_NONE = 0,
    VOICE_WAKE_BUTTON,   /**< touch / button, the only mode that works offline */
    VOICE_WAKE_KEYWORD,  /**< offline keyword spotting: not available in this build */
} voice_wake_mode_t;

/** Capture sample rate requested from the codec. */
#define VOICE_SAMPLE_RATE 16000
/** Hard cap on one recording, in milliseconds (protects PSRAM from a stuck mic). */
#define VOICE_MAX_RECORD_MS 10000
/** Maximum length of a recognised or synthesised sentence. */
#define VOICE_TEXT_MAX 512

/** Metadata for a finished capture. */
typedef struct {
    const int16_t *samples;  /**< valid only inside the event handler */
    size_t count;            /**< number of samples (mono) */
    uint32_t duration_ms;
    bool saved;              /**< true when the PCM file was written to storage */
    char path[96];           /**< storage path when @c saved */
} voice_recording_t;

/**
 * @brief Bring up the codec handles lazily and load the wake mode.
 *
 * Succeeds even when audio is unavailable: query
 * voice_service_microphone_available() / voice_service_speaker_available() and
 * degrade (requirement FR7).
 */
esp_err_t voice_service_init(void);

/** @brief True when a microphone was created successfully. */
bool voice_service_microphone_available(void);

/** @brief True when a speaker/PA path was created successfully. */
bool voice_service_speaker_available(void);

/**
 * @brief Start capturing into the internal PSRAM buffer.
 *
 * Posts VOICE_EVENT_WAKE immediately (the button/touch path) and
 * VOICE_EVENT_RECORD_DONE when the capture ends, which happens on
 * voice_service_stop_record() or after VOICE_MAX_RECORD_MS.
 */
esp_err_t voice_service_start_record(void);

/** @brief Stop the capture early. Safe to call when not recording. */
esp_err_t voice_service_stop_record(void);

/** @brief True while a capture is running. */
bool voice_service_is_recording(void);

/**
 * @brief Play a PCM file from storage.
 *
 * Accepts the format this component writes: 16 kHz, 16-bit signed, mono, i.e.
 * the raw payload of a WAV without the header.
 */
esp_err_t voice_service_play(const char *pcm_path);

/** @brief Stop playback. */
esp_err_t voice_service_stop_play(void);

/** @brief Set and persist the wake mode. */
esp_err_t voice_service_set_wake_mode(voice_wake_mode_t mode);

/** @brief Current wake mode. */
voice_wake_mode_t voice_service_get_wake_mode(void);

/** @brief Name of a wake mode, for the API and logs. */
const char *voice_service_wake_mode_name(voice_wake_mode_t mode);

/* ---- Cloud ASR / TTS integration points -------------------------------- */

/**
 * @brief Set the ASR endpoint. Empty clears it.
 *
 * The endpoint receives `POST` with the raw 16 kHz mono PCM body and must answer
 * `{"text":"..."}`. Anything else is treated as a failure and the caller falls
 * back to text input.
 */
esp_err_t voice_service_set_asr_endpoint(const char *url);

/** @brief Set the TTS endpoint. Empty clears it.
 *
 * The endpoint receives `POST {"text":"..."}` and must answer `audio/pcm`
 * (16 kHz, 16-bit signed, mono). Failures degrade to text-only output.
 */
esp_err_t voice_service_set_tts_endpoint(const char *url);

/** @brief True when an ASR endpoint is configured. */
bool voice_service_asr_configured(void);

/** @brief True when a TTS endpoint is configured. */
bool voice_service_tts_configured(void);

/**
 * @brief Send the last capture to the ASR endpoint.
 *
 * Posts VOICE_EVENT_ASR_DONE with the text (or "" when it failed / is not
 * configured). Blocking: call from a task, never from the LVGL task.
 */
esp_err_t voice_service_transcribe_last(void);

/**
 * @brief Synthesise @p text and play it.
 *
 * Blocking; degrades to ESP_ERR_NOT_SUPPORTED when TTS is not configured, which
 * the UI turns into "text bubble only" (requirement FR7 degradation).
 */
esp_err_t voice_service_speak(const char *text);

/** @brief Publish the /api/v1/voice/... routes. */
esp_err_t voice_service_register_http(void);

#ifdef __cplusplus
}
#endif
