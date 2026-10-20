/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "th_bsp.h"

#include <string.h>

#include "bsp/esp32_s31_korvo.h"
#include "esp_codec_dev.h"

static const char *TAG = "bsp_periph";

/* ------------------------------------------------------------------------- */
/* Status LED                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t th_bsp_led_set(th_bsp_led_state_t state)
{
    switch (state) {
    case TH_BSP_LED_OFF:
        return bsp_led_clear();
    case TH_BSP_LED_WIFI:
        return bsp_led_set_rgb(BSP_LED_STATUS, 0, 0, 48);
    case TH_BSP_LED_SYNC:
        return bsp_led_set_rgb(BSP_LED_STATUS, 0, 48, 48);
    case TH_BSP_LED_ERROR:
        return bsp_led_set_rgb(BSP_LED_STATUS, 64, 0, 0);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

/* ------------------------------------------------------------------------- */
/* SD card                                                                   */
/* ------------------------------------------------------------------------- */

static bool s_sd_mounted;
static char s_sd_mount_point[24];

/**
 * 挂载 SD 卡时重试几次。
 *
 * 卡偶尔会「没准备好」：上电之后 esp_vfs_fat_sdmmc_sdcard_init() 超时
 * （真机日志 312 次开机里有 2 次：ESP_ERR_TIMEOUT）。
 * 后果**是用户看得见的**：卡上放着中文字库（fonts/ui_cjk.ttf），
 * 挂不上 → FreeType 用不了 → 退回内嵌的 GB2312 一级子集 →
 * **生僻字变成方块**。
 *
 * 所以这里宁可多试几次：IDF 在挂载失败时会把 SDMMC host 反初始化
 * （vfs_fat_sdmmc.c 的 call_host_deinit），所以重试是安全的。
 */
#define TH_BSP_SD_MOUNT_ATTEMPTS 3
/** 两次尝试之间歇多久（毫秒）。 */
#define TH_BSP_SD_MOUNT_RETRY_MS 500

esp_err_t th_bsp_sdcard_mount(void)
{
    if (s_sd_mounted) {
        return ESP_OK;
    }

    const bsp_sdcard_config_t cfg = BSP_SDCARD_DEFAULT_CONFIG();
    sdmmc_card_t *card = NULL;

    /* format_if_mount_failed stays false: silently reformatting a user's card
     * would destroy data, and requirement 12 only asks for a graceful fallback. */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= TH_BSP_SD_MOUNT_ATTEMPTS; attempt++) {
        err = bsp_sdcard_mount(&cfg, &card);
        if (err == ESP_OK) {
            break;
        }
        SYS_LOGW(TAG, "SD mount attempt %d/%d failed: %s", attempt,
                 TH_BSP_SD_MOUNT_ATTEMPTS, esp_err_to_name(err));
        if (attempt < TH_BSP_SD_MOUNT_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(TH_BSP_SD_MOUNT_RETRY_MS));
        }
    }
    if (err != ESP_OK) {
        SYS_LOGI(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return err;
    }

    const char *mount = bsp_sdcard_get_mount_point();
    strlcpy(s_sd_mount_point, mount ? mount : BSP_SDCARD_MOUNT_POINT, sizeof(s_sd_mount_point));
    s_sd_mounted = true;

    SYS_LOGI(TAG, "SD mounted at %s", s_sd_mount_point);
    return ESP_OK;
}

esp_err_t th_bsp_sdcard_unmount(void)
{
    if (!s_sd_mounted) {
        return ESP_OK;
    }
    esp_err_t err = bsp_sdcard_unmount();
    if (err == ESP_OK) {
        s_sd_mounted = false;
        s_sd_mount_point[0] = '\0';
    }
    return err;
}

bool th_bsp_sdcard_mounted(void)
{
    return s_sd_mounted;
}

const char *th_bsp_sdcard_mount_point(void)
{
    return s_sd_mounted ? s_sd_mount_point : NULL;
}

/* ------------------------------------------------------------------------- */
/* Audio (lazy)                                                              */
/* ------------------------------------------------------------------------- */

static bool s_audio_initialized;
static esp_codec_dev_handle_t s_speaker;
static esp_codec_dev_handle_t s_mic;

esp_err_t th_bsp_audio_init(void)
{
    if (s_audio_initialized) {
        return (s_speaker != NULL || s_mic != NULL) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    s_audio_initialized = true;

    esp_err_t err = bsp_audio_init(NULL);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "I2S init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_speaker = bsp_audio_codec_speaker_init();
    if (s_speaker == NULL) {
        SYS_LOGW(TAG, "speaker codec unavailable; TTS will be skipped");
    }

    s_mic = bsp_audio_codec_microphone_init();
    if (s_mic == NULL) {
        SYS_LOGW(TAG, "microphone codec unavailable; voice input will be skipped");
    }

    if (s_speaker == NULL && s_mic == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    SYS_LOGI(TAG, "audio ready (speaker=%d mic=%d)", s_speaker != NULL, s_mic != NULL);
    return ESP_OK;
}

void *th_bsp_audio_speaker(void)
{
    return s_speaker;
}

void *th_bsp_audio_mic(void)
{
    return s_mic;
}

bool th_bsp_audio_available(void)
{
    return s_speaker != NULL;
}
