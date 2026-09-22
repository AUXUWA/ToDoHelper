/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "coffee_assets.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_mmap_assets.h"
#include "mmap_generate_coffee_pjpg.h"

#define COFFEE_ASSET_PARTITION_NAME  "coffee_pjpg"
#define COFFEE_ASSET_DRIVE_LETTER    'C'
#define COFFEE_ASSET_PATH_LEN        64

typedef struct {
    const char *symbol;
    const char *asset_name;
    char resolved_path[COFFEE_ASSET_PATH_LEN];
    bool resolved;
} coffee_asset_entry_t;

static const char *TAG = "coffee_assets";
static mmap_assets_handle_t s_assets;
static esp_lv_fs_handle_t s_fs_handle;
static bool s_initialized;

static coffee_asset_entry_t s_asset_table[COFFEE_ASSET_COUNT] = {
    [COFFEE_ASSET_UI_IMG_BG_PNG] = { .symbol = "ui_img_bg_png", .asset_name = "BG.png" },
    [COFFEE_ASSET_UI_IMG_SHAPE1_PNG] = { .symbol = "ui_img_shape1_png", .asset_name = "shape1.png" },
    [COFFEE_ASSET_UI_IMG_PLAY_BUTTON_PNG] = { .symbol = "ui_img_play_button_png", .asset_name = "Play_button.png" },
    [COFFEE_ASSET_UI_IMG_BIG_ESPRESSO_PNG] = { .symbol = "ui_img_big_espresso_png", .asset_name = "big_espresso.png" },
    [COFFEE_ASSET_UI_IMG_BIG_RISTRETTO_PNG] = { .symbol = "ui_img_big_ristretto_png", .asset_name = "big_ristretto.png" },
    [COFFEE_ASSET_UI_IMG_BIG_LATTE_PNG] = { .symbol = "ui_img_big_latte_png", .asset_name = "big_latte.png" },
    [COFFEE_ASSET_UI_IMG_ARROW_PNG] = { .symbol = "ui_img_arrow_png", .asset_name = "arrow.png" },
    [COFFEE_ASSET_UI_IMG_BLE_PNG] = { .symbol = "ui_img_ble_png", .asset_name = "ble.png" },
    [COFFEE_ASSET_UI_IMG_WIFI_PNG] = { .symbol = "ui_img_wifi_png", .asset_name = "wifi.png" },
    [COFFEE_ASSET_UI_IMG_SETTINGS_PNG] = { .symbol = "ui_img_settings_png", .asset_name = "settings.png" },
    [COFFEE_ASSET_UI_IMG_LATTE_ILLUSTRATION_PNG] = { .symbol = "ui_img_latte_illustration_png", .asset_name = "latte_illustration.png" },
    [COFFEE_ASSET_UI_IMG_CHEVRON_PNG] = { .symbol = "ui_img_chevron_png", .asset_name = "chevron.png" },
    [COFFEE_ASSET_UI_IMG_266850372] = { .symbol = "ui_img_266850372", .asset_name = "chevron_1.png" },
    [COFFEE_ASSET_UI_IMG_DISABLEDBEAN_PNG] = { .symbol = "ui_img_disabledbean_png", .asset_name = "disabledbean.png" },
    [COFFEE_ASSET_UI_IMG_BEANS_WHITE_PNG] = { .symbol = "ui_img_beans_white_png", .asset_name = "beans_white.png" },
    [COFFEE_ASSET_UI_IMG_CUP_PNG] = { .symbol = "ui_img_cup_png", .asset_name = "cup.png" },
    [COFFEE_ASSET_UI_IMG_CUPS_PNG] = { .symbol = "ui_img_cups_png", .asset_name = "cups.png" },
    [COFFEE_ASSET_UI_IMG_COFFEE_ILLUSTRATION_PNG] = { .symbol = "ui_img_coffee_illustration_png", .asset_name = "Coffee_illustration.png" },
    [COFFEE_ASSET_UI_IMG_ESPRESSO_PNG] = { .symbol = "ui_img_espresso_png", .asset_name = "espresso.png" },
    [COFFEE_ASSET_UI_IMG_SLIDER_BG_PNG] = { .symbol = "ui_img_slider_bg_png", .asset_name = "slider_bg.png" },
    [COFFEE_ASSET_UI_IMG_TOP_PNG] = { .symbol = "ui_img_top_png", .asset_name = "Top.png" },
    [COFFEE_ASSET_UI_IMG_PARAMETERS_PNG] = { .symbol = "ui_img_parameters_png", .asset_name = "parameters.png" },
    [COFFEE_ASSET_UI_IMG_1726788397] = { .symbol = "ui_img_1726788397", .asset_name = "parameters_1.png" },
    [COFFEE_ASSET_UI_IMG_TEMP_BODY_PNG] = { .symbol = "ui_img_temp_body_png", .asset_name = "temp_body.png" },
    [COFFEE_ASSET_UI_IMG_TEMPERATURE_PNG] = { .symbol = "ui_img_temperature_png", .asset_name = "temperature.png" },
    [COFFEE_ASSET_UI_IMG_FOAM_BODY_PNG] = { .symbol = "ui_img_foam_body_png", .asset_name = "foam_body.png" },
    [COFFEE_ASSET_UI_IMG_FOAM_PNG] = { .symbol = "ui_img_foam_png", .asset_name = "foam.png" },
    [COFFEE_ASSET_UI_IMG_DONE_PNG] = { .symbol = "ui_img_done_png", .asset_name = "done.png" },
    [COFFEE_ASSET_UI_IMG_STANDBY_PNG] = { .symbol = "ui_img_standby_png", .asset_name = "standby.png" },
    [COFFEE_ASSET_UI_IMG_LIGHT_PNG] = { .symbol = "ui_img_light_png", .asset_name = "light.png" },
    [COFFEE_ASSET_UI_IMG_WATER_PNG] = { .symbol = "ui_img_water_png", .asset_name = "water.png" },
    [COFFEE_ASSET_UI_IMG_CLEAN_PNG] = { .symbol = "ui_img_clean_png", .asset_name = "clean.png" },
    [COFFEE_ASSET_UI_IMG_TASTE1_PNG] = { .symbol = "ui_img_taste1_png", .asset_name = "taste1.png" },
    [COFFEE_ASSET_UI_IMG_CUP1_PNG] = { .symbol = "ui_img_cup1_png", .asset_name = "cup1.png" },
    [COFFEE_ASSET_UI_IMG_UNITS_PNG] = { .symbol = "ui_img_units_png", .asset_name = "units.png" },
    [COFFEE_ASSET_UI_IMG_LANGUAGES_PNG] = { .symbol = "ui_img_languages_png", .asset_name = "languages.png" },
    [COFFEE_ASSET_UI_IMG_PROFILE_PNG] = { .symbol = "ui_img_profile_png", .asset_name = "profile.png" },
    [COFFEE_ASSET_UI_IMG_ROLLER_PNG] = { .symbol = "ui_img_roller_png", .asset_name = "roller.png" },
    [COFFEE_ASSET_UI_IMG_ALERT_PNG] = { .symbol = "ui_img_alert_png", .asset_name = "alert.png" },
    [COFFEE_ASSET_UI_IMG_TANK_PNG] = { .symbol = "ui_img_tank_png", .asset_name = "tank.png" },
    [COFFEE_ASSET_UI_IMG_WAVE1_PNG] = { .symbol = "ui_img_wave1_png", .asset_name = "wave1.png" },
    [COFFEE_ASSET_UI_IMG_WAVE_BOTTOM_PNG] = { .symbol = "ui_img_wave_bottom_png", .asset_name = "wave_bottom.png" },
    [COFFEE_ASSET_UI_IMG_DONE_60_PNG] = { .symbol = "ui_img_done_60_png", .asset_name = "done_60.png" },
    [COFFEE_ASSET_UI_IMG_ARROWS_PNG] = { .symbol = "ui_img_arrows_png", .asset_name = "arrows.png" },
    [COFFEE_ASSET_UI_IMG_BIGBEAN_PNG] = { .symbol = "ui_img_bigbean_png", .asset_name = "bigbean.png" },
};

static bool coffee_asset_same_stem(const char *asset_name, const char *stored_name)
{
    const char *asset_dot = strrchr(asset_name, '.');
    const char *stored_dot = strrchr(stored_name, '.');
    if (!asset_dot || !stored_dot) {
        return strcmp(asset_name, stored_name) == 0;
    }

    size_t asset_stem_len = (size_t)(asset_dot - asset_name);
    size_t stored_stem_len = (size_t)(stored_dot - stored_name);
    return asset_stem_len == stored_stem_len && strncmp(asset_name, stored_name, asset_stem_len) == 0;
}

static void coffee_assets_resolve_paths(void)
{
    const int stored_files = mmap_assets_get_stored_files(s_assets);
    ESP_LOGI(TAG, "Coffee assets partition mounted with %d files", stored_files);

    for (int i = 0; i < stored_files; i++) {
        const char *stored_name = mmap_assets_get_name(s_assets, i);
        if (!stored_name) {
            continue;
        }

        for (int j = 0; j < COFFEE_ASSET_COUNT; j++) {
            coffee_asset_entry_t *entry = &s_asset_table[j];
            if (!entry->resolved && coffee_asset_same_stem(entry->asset_name, stored_name)) {
                snprintf(entry->resolved_path, sizeof(entry->resolved_path), "%c:%s", COFFEE_ASSET_DRIVE_LETTER, stored_name);
                entry->resolved = true;
                break;
            }
        }
    }

    for (int i = 0; i < COFFEE_ASSET_COUNT; i++) {
        coffee_asset_entry_t *entry = &s_asset_table[i];
        if (!entry->resolved) {
            snprintf(entry->resolved_path, sizeof(entry->resolved_path), "%c:%s", COFFEE_ASSET_DRIVE_LETTER, entry->asset_name);
            ESP_LOGW(TAG, "Coffee asset %s is not in partition, fallback to %s", entry->symbol, entry->resolved_path);
        }
    }
}

esp_err_t coffee_assets_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    mmap_assets_config_t mmap_cfg = {
        .partition_label = COFFEE_ASSET_PARTITION_NAME,
        .max_files = MMAP_COFFEE_PJPG_FILES,
        .checksum = MMAP_COFFEE_PJPG_CHECKSUM,
        .flags = {
            .mmap_enable = 1,
            .app_bin_check = 0,
        },
    };
    esp_err_t ret = mmap_assets_new(&mmap_cfg, &s_assets);
    ESP_RETURN_ON_ERROR(ret, TAG, "init coffee mmap assets failed");

    fs_cfg_t fs_cfg = {
        .fs_letter = COFFEE_ASSET_DRIVE_LETTER,
        .fs_nums = mmap_assets_get_stored_files(s_assets),
        .fs_assets = s_assets,
    };
    ret = esp_lv_adapter_fs_mount(&fs_cfg, &s_fs_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "mount coffee LVGL FS failed");

    coffee_assets_resolve_paths();
    s_initialized = true;
    ESP_LOGI(TAG, "Coffee assets are ready");
    return ESP_OK;
}

const char *coffee_assets_get_src(coffee_asset_id_t id)
{
    if (id < 0 || id >= COFFEE_ASSET_COUNT) {
        return NULL;
    }

    coffee_asset_entry_t *entry = &s_asset_table[id];
    if (entry->resolved_path[0] == '\0') {
        snprintf(entry->resolved_path, sizeof(entry->resolved_path), "%c:%s", COFFEE_ASSET_DRIVE_LETTER, entry->asset_name);
    }
    return entry->resolved_path;
}
