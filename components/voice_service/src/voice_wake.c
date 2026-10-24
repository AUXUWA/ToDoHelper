/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file voice_wake.c
 * @brief VOICE_WAKE_KEYWORD 的真实现（v2.0.0）：本地关键词唤醒。
 *
 * 数据链（参照 xiaozhi-esp32 的 AFE 引擎，但简化为单任务）：
 *
 *   ES8311 (48 kHz / 2 ch / 16 bit, 与录音路径同一采集格式)
 *     → 降混 + 1/3 抽取 → 16 kHz mono（与 record_task 相同的算法）
 *     → AFE feed（AFE_TYPE_SR / LOW_COST，模型走 model 分区）
 *     → fetch 轮询 → wakeup_state == WAKENET_DETECTED
 *     → 亮屏 + VOICE_EVENT_WAKE（页面层接管：切 AI 页 + 自动录音）。
 *
 * 与录音/播放的互斥：codec 是**一个** ES8389 芯片（s_codec_lock 互斥），
 * 唤醒监听**不能常驻持锁**，否则 TTS 一句话都放不出来。所以监听按
 * 「批」工作：每批拿锁约 1 s，批内每个 chunk 都检查 s_recording /
 * s_playing —— 按下录音或开始播放的瞬间（这两个标志在入口即置位），
 * 监听立刻中断本批、交出 codec；录音/播放结束后下一轮批自然恢复。
 *
 * 内存（esp-sr 官方 s31 基准 + 本板余量）：
 *   WakeNet10 内部 RAM 16 KB / PSRAM 523 KB / 19.9% 单核；
 *   本板 boot 后内部空闲 ~150-200 KB、PSRAM 空闲 ~9.5 MB —— 充裕，
 *   但 AFE 的分配路径仍显式走 MORE_PSRAM，把内部 RAM 留给 WiFi PHY。
 */

#include "voice_wake.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_heap_caps.h"
#include "model_path.h"

#include "th_bsp.h"
#include "sys_utils.h"

static const char *TAG = "voice_wake";

/* 与 voice_service.c 的采集路径保持同一格式（record_task 也按这个降混）。 */
#define WAKE_CODEC_RATE 48000
#define WAKE_CODEC_BITS 16
#define WAKE_CODEC_CHANNELS 2
#define WAKE_DECIMATE (WAKE_CODEC_RATE / 16000)
/** One 48 kHz chunk, same shape as VOICE_READ_CHUNK in voice_service.c. */
#define WAKE_READ_CHUNK (WAKE_CODEC_CHANNELS * 256)
/** One listening batch: ~1 s of 16 kHz mono before re-checking the codec. */
#define WAKE_BATCH_SAMPLES 16000

/** Do not fire twice within this window (also covers the dialog that follows). */
#define WAKE_COOLDOWN_MS 5000
/** Give up on a codec that will not answer, like ensure_audio does. */
#define WAKE_LOCK_TIMEOUT_MS 2000

/**
 * Feed/fetch task stack. The AFE feed call runs the feature extraction and the
 * WakeNet inference inline in LOW_COST mode, so this stack carries both the
 * codec read path and the model's frame buffers.
 */
#define WAKE_TASK_STACK 8192
#define WAKE_TASK_PRIO 4

static bool s_running;          /* task lifecycle */
static volatile bool s_started; /* wake pipeline exists */
static TaskHandle_t s_task;

static srmodel_list_t *s_models;
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static int s_feed_chunk; /* 16 kHz samples per feed chunk */

/** Monotonic-ms timestamp of the last fire (sys_time_ms()); 0 = none. */
static int64_t s_last_fire_ms;

static void wake_hit(void)
{
    const int64_t now = sys_time_ms();
    if (s_last_fire_ms != 0 && now - s_last_fire_ms < WAKE_COOLDOWN_MS) {
        return;
    }
    s_last_fire_ms = now;

    /* Light the screen first: the wake word is the whole point of not pressing
     * a button, and the AI page switch below is visible only if the panel is
     * not in standby. */
    th_bsp_report_activity();
    SYS_LOGI(TAG, "wake word detected -> WAKE event");
    (void)sys_event_post_to(VOICE_EVENT, VOICE_EVENT_WAKE, NULL, 0);
}

/**
 * Downmix the 48 kHz stereo chunk to 16 kHz mono into @p dst (with the same
 * 3:1 accumulate-and-average as record_task, so the two paths hear alike).
 *
 * @return samples written to @p dst.
 */
static size_t wake_decimate(const int16_t *chunk, size_t frames,
                            int16_t *dst, size_t dst_cap, int16_t *carry,
                            int *carry_cnt)
{
    size_t produced = 0;
    int32_t acc = 0;
    int cnt = *carry_cnt;
    if (cnt > 0) {
        acc = *carry;
    }
    for (size_t i = 0; i + 1 < frames * WAKE_CODEC_CHANNELS; i += WAKE_CODEC_CHANNELS) {
        const int32_t mono = ((int32_t)chunk[i] + (int32_t)chunk[i + 1]) / 2;
        acc += mono;
        if (++cnt >= WAKE_DECIMATE) {
            if (produced < dst_cap) {
                dst[produced++] = (int16_t)(acc / WAKE_DECIMATE);
            }
            acc = 0;
            cnt = 0;
        }
    }
    *carry = (int16_t)acc;
    *carry_cnt = cnt;
    return produced;
}

static void wake_task(void *arg)
{
    (void)arg;
    const size_t feed_bytes = (size_t)s_feed_chunk * sizeof(int16_t);

    /* Scratch buffers: bulk audio data lives in PSRAM (AGENT.md 9.0.5), the
     * only exception is the small per-chunk read buffer the codec DMA fills. */
    int16_t *raw = heap_caps_malloc_prefer(WAKE_READ_CHUNK * sizeof(int16_t), 2,
                                           MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    int16_t *mono = heap_caps_malloc_prefer(WAKE_BATCH_SAMPLES * sizeof(int16_t), 2,
                                            MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (raw == NULL || mono == NULL) {
        SYS_LOGE(TAG, "wake buffers OOM");
        heap_caps_free(raw);
        heap_caps_free(mono);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    esp_codec_dev_handle_t mic = voice_wake_mic_handle();

    while (s_running) {
        if (mic == NULL) {
            /* ensure_audio_async() is still bringing the codec up (or failed) -
             * re-read the handle each round; it appears once the BSP init is
             * done. */
            mic = voice_wake_mic_handle();
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        /* The codec belongs to whoever is actually doing something with it:
         * recording, playback, or (idle) this listener. The two flags are set
         * at the *entry* of start_record / playback, so yielding here is
         * immediate - a TTS utterance never waits for the batch to end. */
        if (voice_wake_codec_busy()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (s_last_fire_ms != 0 &&
            sys_time_ms() - s_last_fire_ms < 1000) {
            /* Just fired: let the following dialog (page switch, auto record)
             * take the codec without us grabbing it right back. */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        SemaphoreHandle_t lock = voice_wake_codec_lock();
        if (lock == NULL ||
            xSemaphoreTake(lock, pdMS_TO_TICKS(WAKE_LOCK_TIMEOUT_MS)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        esp_codec_dev_sample_info_t fs = {
            .sample_rate = WAKE_CODEC_RATE,
            .channel = WAKE_CODEC_CHANNELS,
            .bits_per_sample = WAKE_CODEC_BITS,
        };
        bool opened = (esp_codec_dev_open(mic, &fs) == ESP_OK);
        if (!opened) {
            xSemaphoreGive(lock);
            SYS_LOGW(TAG, "codec open failed; retry in 2 s");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        int carry = 0;
        int16_t carry_acc = 0;
        size_t have = 0; /* mono samples in the batch buffer, pending feed */
        int read_error_logged = 0;

        while (s_running && !voice_wake_codec_busy() && have + s_feed_chunk <= WAKE_BATCH_SAMPLES) {
            const int ret = esp_codec_dev_read(mic, raw, WAKE_READ_CHUNK * sizeof(int16_t));
            if (ret != 0) {
                if (read_error_logged == 0) {
                    read_error_logged = 1;
                    SYS_LOGW(TAG, "wake read failed: %d (0x%x)", ret, (unsigned)ret);
                }
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            have += wake_decimate(raw, WAKE_READ_CHUNK / WAKE_CODEC_CHANNELS,
                                  mono + have, WAKE_BATCH_SAMPLES - have,
                                  &carry_acc, &carry);

            /* Feed whole chunks; keep the remainder for the next iteration. */
            while (have >= (size_t)s_feed_chunk) {
                s_afe->feed(s_afe_data, mono);
                have -= (size_t)s_feed_chunk;
                if (have > 0) {
                    memmove(mono, mono + s_feed_chunk, have * sizeof(int16_t));
                }
                /* Non-blocking poll: LOW_COST runs the detection inside feed,
                 * fetch returns the most recent result when one is ready. */
                afe_fetch_result_t *res = s_afe->fetch_with_delay(s_afe_data, 0);
                if (res != NULL && res->wakeup_state == WAKENET_DETECTED) {
                    wake_hit();
                    break;
                }
            }
            if (s_last_fire_ms != 0 && sys_time_ms() - s_last_fire_ms < 1000) {
                break; /* fired mid-batch: hand the codec over at once */
            }
        }

        esp_codec_dev_close(mic);
        xSemaphoreGive(lock);

        /* One batch per loop; the tail delay keeps this task from spinning the
         * CPU when the batch was cut short by a takeover. */
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    heap_caps_free(raw);
    heap_caps_free(mono);
    s_started = false;
    SYS_LOGI(TAG, "wake listener stopped");
    vTaskDelete(NULL);
}

esp_err_t voice_wake_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    /* The listener needs the codec; bring it up here (async - this may be
     * called from the httpd task through voice/config, and the codec I2C
     * bring-up must never run inline there, AGENT.md 9.0.1.4). */
    (void)voice_service_ensure_audio_async();

    /* Models come from the "model" partition (srmodels.bin, packed at build
     * time from the Kconfig selection). A missing/empty partition means the
     * image was built for a different wake configuration - degrade loudly. */
    s_models = esp_srmodel_init("model");
    const char *wn_name =
        (s_models != NULL) ? esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL) : NULL;
    if (wn_name == NULL) {
        SYS_LOGE(TAG, "no wakenet model in the 'model' partition; keyword wake unavailable");
        return ESP_ERR_NOT_FOUND;
    }

    /* Config first, handle second: esp_afe_handle_from_config() reads the
     * config (the first boot crashed at config+0x60 with NULL passed in -
     * the handle getter is not nullable, whatever the signature suggests). */
    afe_config_t *cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (cfg == NULL) {
        SYS_LOGE(TAG, "AFE config init failed");
        return ESP_ERR_NO_MEM;
    }
    /* "M" has no playback reference, so these are structurally off; the rest
     * of the LOW_COST defaults (vadnet on, nsnet off) are what we want. The
     * one thing to pin explicitly is the allocation path: this board's
     * internal RAM is the WiFi PHY's territory (AGENT.md 9.0). */
    cfg->aec_init = false;
    cfg->se_init = false;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_core = 1;
    cfg->afe_perferred_priority = WAKE_TASK_PRIO;

    s_afe = esp_afe_handle_from_config(cfg);
    if (s_afe == NULL) {
        SYS_LOGE(TAG, "AFE handle unavailable");
        return ESP_ERR_NO_MEM;
    }

    s_afe_data = s_afe->create_from_config(cfg);
    if (s_afe_data == NULL) {
        SYS_LOGE(TAG, "AFE create failed (memory?)");
        return ESP_ERR_NO_MEM;
    }
    s_feed_chunk = s_afe->get_feed_chunksize(s_afe_data);
    SYS_LOGI(TAG, "AFE ready: wakenet '%s', feed chunk %d samples", wn_name, s_feed_chunk);

    s_running = true;
    s_started = true;
    if (xTaskCreate(wake_task, "th_voice_wake", WAKE_TASK_STACK, NULL,
                    WAKE_TASK_PRIO, &s_task) != pdPASS) {
        s_running = false;
        s_started = false;
        SYS_LOGE(TAG, "wake task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void voice_wake_stop(void)
{
    s_running = false;
    /* The task exits at a safe point (never holding the codec lock), so there
     * is nothing to join here - worst case it dies a moment later. */
    s_task = NULL;
}
