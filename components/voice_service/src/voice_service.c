/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voice_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "http_router.h"
#include "storage_manager.h"
#include "th_bsp.h"

static const char *TAG = "voice";

ESP_EVENT_DEFINE_BASE(VOICE_EVENT);

#ifndef CONFIG_TODOHELPER_VOICE_HTTP_TIMEOUT_MS
#define CONFIG_TODOHELPER_VOICE_HTTP_TIMEOUT_MS 30000
#endif
#ifndef CONFIG_TODOHELPER_VOICE_SAVE_PCM
#define CONFIG_TODOHELPER_VOICE_SAVE_PCM 1
#endif

/** The BSP brings I2S up at this format, so the codec is opened to match. */
#define VOICE_CODEC_RATE 48000
#define VOICE_CODEC_BITS 16
#define VOICE_CODEC_CHANNELS 2
/** Decimation factor from the codec rate to VOICE_SAMPLE_RATE. */
#define VOICE_DECIMATE (VOICE_CODEC_RATE / VOICE_SAMPLE_RATE)
#define VOICE_READ_CHUNK (VOICE_CODEC_CHANNELS * 256)   /* frames*channels */
#define VOICE_MAX_SAMPLES ((VOICE_SAMPLE_RATE * VOICE_MAX_RECORD_MS) / 1000)

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

static bool s_initialized;
static bool s_audio_ready;
static bool s_mic_ok;
static bool s_speaker_ok;
static volatile bool s_recording;
static volatile bool s_stop_requested;
static volatile bool s_playing;

static int16_t *s_pcm;          /* VOICE_MAX_SAMPLES mono samples, in PSRAM */
static size_t s_pcm_count;
static uint32_t s_pcm_ms;

static char s_asr_url[192];
static char s_tts_url[192];
static voice_wake_mode_t s_wake_mode = VOICE_WAKE_BUTTON;

/** Playback scratch: mono 16 kHz expanded to stereo 48 kHz. */
#define VOICE_PLAY_CHUNK (VOICE_DECIMATE * VOICE_CODEC_CHANNELS * 256)
static int16_t s_play_buf[VOICE_PLAY_CHUNK];

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

bool voice_service_microphone_available(void)
{
    return s_mic_ok;
}

bool voice_service_speaker_available(void)
{
    return s_speaker_ok;
}

bool voice_service_is_recording(void)
{
    return s_recording;
}

const char *voice_service_wake_mode_name(voice_wake_mode_t mode)
{
    switch (mode) {
    case VOICE_WAKE_BUTTON:
        return "button";
    case VOICE_WAKE_KEYWORD:
        return "keyword";
    case VOICE_WAKE_NONE:
    default:
        return "none";
    }
}

static esp_codec_dev_handle_t mic_handle(void)
{
    return (esp_codec_dev_handle_t)th_bsp_audio_mic();
}

static esp_codec_dev_handle_t speaker_handle(void)
{
    return (esp_codec_dev_handle_t)th_bsp_audio_speaker();
}

static void ensure_audio(void)
{
    if (s_audio_ready) {
        return;
    }
    s_audio_ready = true;

    if (th_bsp_audio_init() != ESP_OK) {
        SYS_LOGW(TAG, "audio hardware unavailable; voice features disabled");
        return;
    }

    s_mic_ok = (mic_handle() != NULL);
    s_speaker_ok = (speaker_handle() != NULL);

    if (!s_mic_ok) {
        SYS_LOGW(TAG, "no microphone; text input must be used instead (FR7 degradation)");
    }
    if (!s_speaker_ok) {
        SYS_LOGW(TAG, "no speaker; replies will be text only (FR7 degradation)");
    }
}

/* ------------------------------------------------------------------------ */
/* Recording                                                                 */
/* ------------------------------------------------------------------------ */

static void record_task(void *arg)
{
    (void)arg;

    esp_codec_dev_handle_t mic = mic_handle();
    /* esp_codec_dev_open() takes a non-const pointer. */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = VOICE_CODEC_RATE,
        .channel = VOICE_CODEC_CHANNELS,
        .bits_per_sample = VOICE_CODEC_BITS,
    };

    if (esp_codec_dev_open(mic, &fs) != ESP_OK) {
        SYS_LOGE(TAG, "codec open for capture failed");
        s_recording = false;
        vTaskDelete(NULL);
        return;
    }

    int16_t chunk[VOICE_READ_CHUNK];
    size_t produced = 0;
    uint32_t frames_since_decimate = 0;
    int32_t acc[VOICE_DECIMATE] = { 0 };
    size_t acc_index = 0;

    while (!s_stop_requested && produced < VOICE_MAX_SAMPLES) {
        const int got = esp_codec_dev_read(mic, chunk, sizeof(chunk));
        if (got <= 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        const size_t samples = (size_t)got / sizeof(int16_t);
        for (size_t i = 0; i + 1 < samples; i += VOICE_CODEC_CHANNELS) {
            /* Downmix to mono first, then decimate 48 kHz -> 16 kHz. */
            const int32_t mono = ((int32_t)chunk[i] + (int32_t)chunk[i + 1]) / 2;
            acc[acc_index++] = mono;
            frames_since_decimate++;

            if (acc_index >= VOICE_DECIMATE) {
                int32_t sum = 0;
                for (size_t k = 0; k < VOICE_DECIMATE; k++) {
                    sum += acc[k];
                }
                s_pcm[produced++] = (int16_t)(sum / (int32_t)VOICE_DECIMATE);
                acc_index = 0;
                if (produced >= VOICE_MAX_SAMPLES) {
                    break;
                }
            }
        }
        (void)frames_since_decimate;
    }

    esp_codec_dev_close(mic);
    s_pcm_count = produced;
    s_pcm_ms = (uint32_t)((produced * 1000) / VOICE_SAMPLE_RATE);
    s_recording = false;

    voice_recording_t info = {
        .samples = s_pcm,
        .count = produced,
        .duration_ms = s_pcm_ms,
        .saved = false,
    };

#if CONFIG_TODOHELPER_VOICE_SAVE_PCM
    if (storage_available() && produced > 0) {
        snprintf(info.path, sizeof(info.path), "ai/rec_%llu.pcm",
                 (unsigned long long)sys_time_ms());
        if (storage_write_file(info.path, s_pcm, produced * sizeof(int16_t)) == ESP_OK) {
            info.saved = true;
        } else {
            info.path[0] = '\0';
        }
    }
#endif

    SYS_LOGI(TAG, "recorded %u ms (%u samples)%s", (unsigned)s_pcm_ms, (unsigned)produced,
             info.saved ? ", saved" : "");
    (void)sys_event_post_to(VOICE_EVENT, VOICE_EVENT_RECORD_DONE, &info, sizeof(info));
    vTaskDelete(NULL);
}

esp_err_t voice_service_start_record(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_recording) {
        return ESP_OK;
    }

    ensure_audio();
    if (!s_mic_ok) {
        /* Required degradation path: no microphone is not an error, it just
         * means the caller has to fall back to text input. */
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_pcm == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_stop_requested = false;
    s_pcm_count = 0;
    s_recording = true;

    (void)sys_event_post_to(VOICE_EVENT, VOICE_EVENT_WAKE, NULL, 0);

    if (xTaskCreate(record_task, "th_record", 4096, NULL, 5, NULL) != pdPASS) {
        s_recording = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t voice_service_stop_record(void)
{
    if (!s_recording) {
        return ESP_OK;
    }
    s_stop_requested = true;
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Playback                                                                  */
/* ------------------------------------------------------------------------ */

/**
 * Expand mono 16 kHz samples to the stereo 48 kHz format the codec/I2S is
 * configured for. Nearest-neighbour is intentional: it is cheap, and voice
 * intelligibility does not need interpolation.
 */
static size_t expand_to_codec(const int16_t *mono, size_t count, int16_t *out, size_t out_cap)
{
    size_t o = 0;
    for (size_t i = 0; i < count; i++) {
        for (int r = 0; r < VOICE_DECIMATE; r++) {
            if (o + 2 > out_cap) {
                return o;
            }
            out[o++] = mono[i];
            out[o++] = mono[i];
        }
    }
    return o;
}

esp_err_t voice_service_play(const char *pcm_path)
{
    if (pcm_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ensure_audio();
    if (!s_speaker_ok) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!storage_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t size = 0;
    if (storage_exists(pcm_path, &size) != ESP_OK || size < sizeof(int16_t)) {
        return ESP_ERR_NOT_FOUND;
    }

    int16_t *raw = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (raw == NULL) {
        raw = malloc(size);
    }
    if (raw == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t len = size;
    if (storage_read_file(pcm_path, raw, &len) != ESP_OK) {
        free(raw);
        return ESP_FAIL;
    }

    esp_codec_dev_handle_t spk = speaker_handle();
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = VOICE_CODEC_RATE,
        .channel = VOICE_CODEC_CHANNELS,
        .bits_per_sample = VOICE_CODEC_BITS,
    };

    if (esp_codec_dev_open(spk, &fs) != ESP_OK) {
        free(raw);
        return ESP_FAIL;
    }
    (void)esp_codec_dev_set_out_vol(spk, 70);

    s_playing = true;
    const size_t total = len / sizeof(int16_t);
    size_t done = 0;
    while (done < total && s_playing) {
        const size_t chunk = (total - done > 256) ? 256 : (total - done);
        const size_t out = expand_to_codec(&raw[done], chunk, s_play_buf, VOICE_PLAY_CHUNK);
        if (out > 0 && esp_codec_dev_write(spk, s_play_buf, out * sizeof(int16_t)) != ESP_OK) {
            break;
        }
        done += chunk;
    }

    esp_codec_dev_close(spk);
    free(raw);
    s_playing = false;

    (void)sys_event_post_to(VOICE_EVENT, VOICE_EVENT_PLAY_DONE, NULL, 0);
    return ESP_OK;
}

esp_err_t voice_service_stop_play(void)
{
    s_playing = false;
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Cloud ASR / TTS                                                           */
/* ------------------------------------------------------------------------ */

/** Response accumulator used by the speech endpoints. */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool overflow;
} voice_body_t;

#define VOICE_MAX_RESPONSE (256 * 1024)

static esp_err_t voice_http_event(esp_http_client_event_t *evt)
{
    voice_body_t *ctx = (voice_body_t *)evt->user_data;
    if (ctx == NULL || evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) {
        return ESP_OK;
    }
    if (ctx->overflow || ctx->len + (size_t)evt->data_len >= VOICE_MAX_RESPONSE) {
        ctx->overflow = true;
        return ESP_OK;
    }

    if (ctx->buf == NULL) {
        ctx->cap = 4096;
        ctx->buf = malloc(ctx->cap);
        if (ctx->buf == NULL) {
            ctx->overflow = true;
            return ESP_OK;
        }
        ctx->buf[0] = '\0';
    }
    if (ctx->len + (size_t)evt->data_len + 1 > ctx->cap) {
        size_t grown = ctx->cap * 2;
        while (grown < ctx->len + (size_t)evt->data_len + 1) {
            grown *= 2;
        }
        char *bigger = realloc(ctx->buf, grown);
        if (bigger == NULL) {
            ctx->overflow = true;
            return ESP_OK;
        }
        ctx->buf = bigger;
        ctx->cap = grown;
    }

    memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
    ctx->len += (size_t)evt->data_len;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

/**
 * POST @p body and hand the response text back through @p response (heap, the
 * caller frees it).
 */
static esp_err_t http_post(const char *url, const char *content_type, const void *body,
                          size_t body_len, char **response)
{
    *response = NULL;

    voice_body_t ctx = { 0 };
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_TODOHELPER_VOICE_HTTP_TIMEOUT_MS,
        .event_handler = voice_http_event,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_post_field(client, (const char *)body, (int)body_len);

    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        free(ctx.buf);
        return err;
    }
    if (status != 200) {
        SYS_LOGW(TAG, "speech endpoint returned HTTP %d", status);
        free(ctx.buf);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (ctx.buf == NULL || ctx.overflow) {
        free(ctx.buf);
        return ESP_ERR_INVALID_SIZE;
    }

    *response = ctx.buf;
    return ESP_OK;
}

bool voice_service_asr_configured(void)
{
    return s_asr_url[0] != '\0';
}

bool voice_service_tts_configured(void)
{
    return s_tts_url[0] != '\0';
}

esp_err_t voice_service_set_asr_endpoint(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        s_asr_url[0] = '\0';
        return sys_config_erase(SYS_KEY_ASR_URL);
    }
    if (strlen(url) >= sizeof(s_asr_url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(s_asr_url, url, sizeof(s_asr_url));
    return sys_config_set_str(SYS_KEY_ASR_URL, s_asr_url);
}

esp_err_t voice_service_set_tts_endpoint(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        s_tts_url[0] = '\0';
        return sys_config_erase(SYS_KEY_TTS_URL);
    }
    if (strlen(url) >= sizeof(s_tts_url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(s_tts_url, url, sizeof(s_tts_url));
    return sys_config_set_str(SYS_KEY_TTS_URL, s_tts_url);
}

esp_err_t voice_service_transcribe_last(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!voice_service_asr_configured()) {
        /* FR7 degradation: tell the UI to fall back to the keyboard / HTTP text. */
        const char *empty = "";
        (void)sys_event_post_to(VOICE_EVENT, VOICE_EVENT_ASR_DONE, empty, 1);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_pcm_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    char *response = NULL;
    esp_err_t err = http_post(s_asr_url, "application/octet-stream", s_pcm,
                             s_pcm_count * sizeof(int16_t), &response);
    if (err != ESP_OK) {
        const char *empty = "";
        (void)sys_event_post_to(VOICE_EVENT, VOICE_EVENT_ASR_DONE, empty, 1);
        return err;
    }

    char text[VOICE_TEXT_MAX] = { 0 };
    cJSON *root = cJSON_Parse(response);
    if (root != NULL) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(item)) {
            strlcpy(text, item->valuestring, sizeof(text));
        }
        cJSON_Delete(root);
    }
    free(response);

    (void)sys_event_post_to(VOICE_EVENT, VOICE_EVENT_ASR_DONE, text, strlen(text) + 1);
    return (text[0] != '\0') ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t voice_service_speak(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!voice_service_tts_configured()) {
        /* Degradation: the UI shows a bubble instead of playing audio. */
        return ESP_ERR_NOT_SUPPORTED;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddNumberToObject(root, "sample_rate", VOICE_SAMPLE_RATE);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *response = NULL;
    esp_err_t err = http_post(s_tts_url, "application/json", json, strlen(json), &response);
    cJSON_free(json);
    if (err != ESP_OK) {
        return err;
    }
    free(response);
    return ESP_ERR_NOT_SUPPORTED;
}

/* ------------------------------------------------------------------------ */
/* Configuration / HTTP                                                      */
/* ------------------------------------------------------------------------ */

esp_err_t voice_service_set_wake_mode(voice_wake_mode_t mode)
{
    s_wake_mode = mode;
    esp_err_t err = sys_config_set_i32(SYS_KEY_VOICE_WAKE, (int32_t)mode);
    SYS_LOGI(TAG, "wake mode set to '%s'", voice_service_wake_mode_name(mode));
    return err;
}

voice_wake_mode_t voice_service_get_wake_mode(void)
{
    return s_wake_mode;
}

static esp_err_t voice_status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "voice status");
    }

    cJSON_AddBoolToObject(root, "microphone", voice_service_microphone_available());
    cJSON_AddBoolToObject(root, "speaker", voice_service_speaker_available());
    cJSON_AddBoolToObject(root, "recording", voice_service_is_recording());
    cJSON_AddBoolToObject(root, "asr_configured", voice_service_asr_configured());
    cJSON_AddBoolToObject(root, "tts_configured", voice_service_tts_configured());
    cJSON_AddStringToObject(root, "wake_mode",
                            voice_service_wake_mode_name(voice_service_get_wake_mode()));
    cJSON_AddNumberToObject(root, "last_ms", (double)s_pcm_ms);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    const esp_err_t err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t voice_config_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return http_reply_bad_request(req, "invalid json");
    }

    const cJSON *asr = cJSON_GetObjectItemCaseSensitive(root, "asr_url");
    const cJSON *tts = cJSON_GetObjectItemCaseSensitive(root, "tts_url");
    const cJSON *wake = cJSON_GetObjectItemCaseSensitive(root, "wake_mode");

    if (cJSON_IsString(asr)) {
        (void)voice_service_set_asr_endpoint(asr->valuestring);
    }
    if (cJSON_IsString(tts)) {
        (void)voice_service_set_tts_endpoint(tts->valuestring);
    }
    if (cJSON_IsString(wake)) {
        voice_wake_mode_t mode = VOICE_WAKE_BUTTON;
        if (strcmp(wake->valuestring, "none") == 0) {
            mode = VOICE_WAKE_NONE;
        } else if (strcmp(wake->valuestring, "keyword") == 0) {
            mode = VOICE_WAKE_KEYWORD;
        }
        (void)voice_service_set_wake_mode(mode);
    }
    cJSON_Delete(root);

    return voice_status_get_handler(req);
}

esp_err_t voice_service_register_http(void)
{
    static const struct {
        const char *path;
        httpd_method_t method;
        http_route_handler_t handler;
    } routes[] = {
        { "/api/v1/voice/status", HTTP_GET, voice_status_get_handler },
        { "/api/v1/voice/config", HTTP_POST, voice_config_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = http_router_register(routes[i].path, routes[i].method, routes[i].handler);
        if (err != ESP_OK) {
            SYS_LOGE(TAG, "register %s failed: %s", routes[i].path, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Init                                                                      */
/* ------------------------------------------------------------------------ */

esp_err_t voice_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    (void)sys_config_get_str_or(SYS_KEY_ASR_URL, s_asr_url, sizeof(s_asr_url), "");
    (void)sys_config_get_str_or(SYS_KEY_TTS_URL, s_tts_url, sizeof(s_tts_url), "");
    s_wake_mode = (voice_wake_mode_t)sys_config_get_i32_or(SYS_KEY_VOICE_WAKE, VOICE_WAKE_BUTTON);

    /* Capture buffer lives in PSRAM: 10 s of 16 kHz mono is 320 KB. */
    s_pcm = heap_caps_malloc(VOICE_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s_pcm == NULL) {
        s_pcm = malloc(VOICE_MAX_SAMPLES * sizeof(int16_t));
    }
    if (s_pcm == NULL) {
        SYS_LOGE(TAG, "capture buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;

    /* Audio is opened lazily by start_record()/play() so a broken codec does not
     * slow down boot. */
    ensure_audio();

    esp_err_t http_err = voice_service_register_http();
    if (http_err != ESP_OK) {
        SYS_LOGW(TAG, "HTTP routes unavailable: %s", esp_err_to_name(http_err));
    }

    SYS_LOGI(TAG, "ready (mic=%d speaker=%d asr=%d tts=%d wake '%s')",
             s_mic_ok, s_speaker_ok, voice_service_asr_configured(),
             voice_service_tts_configured(), voice_service_wake_mode_name(s_wake_mode));
    return ESP_OK;
}
