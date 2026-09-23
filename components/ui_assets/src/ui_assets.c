/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_assets.h"

#include <stdio.h>
#include <string.h>

#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "esp_mmap_assets.h"

#include "sys_utils.h"

#include "mmap_generate_assets.h"

static const char *TAG = "ui_assets";

#define ASSET_PARTITION "assets"
#define ASSET_DRIVE     'A'
#define ASSET_DRIVE_STR "A"
#define ASSET_NAME_MAX  32
#define ASSET_SRC_MAX   40
/** Upper bound on packed icons; keeps the lookup table a fixed size. */
#define ASSET_MAX_ENTRIES 64

typedef struct {
    char base[ASSET_NAME_MAX];   /**< file name without extension */
    char src[ASSET_SRC_MAX];     /**< LVGL source string, e.g. "A:w_sunny.png" */
    bool used;
} ui_asset_entry_t;

static mmap_assets_handle_t s_assets;
static esp_lv_fs_handle_t s_fs;
static ui_asset_entry_t s_entries[ASSET_MAX_ENTRIES];
static int s_count;
static bool s_ready;

/** Strip the extension: "w_sunny.png" -> "w_sunny". */
static void base_name(const char *file, char *out, size_t len)
{
    size_t n = 0;
    while (file[n] != '\0' && file[n] != '.' && n + 1 < len) {
        out[n] = file[n];
        n++;
    }
    out[n] = '\0';
}

esp_err_t ui_assets_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    const mmap_assets_config_t cfg = {
        .partition_label = ASSET_PARTITION,
        .max_files = MMAP_ASSETS_FILES,
        .checksum = MMAP_ASSETS_CHECKSUM,
        .flags = {
            .mmap_enable = 1,
        },
    };

    esp_err_t err = mmap_assets_new(&cfg, &s_assets);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "assets partition not mounted: %s", esp_err_to_name(err));
        return err;
    }

    const int stored = mmap_assets_get_stored_files(s_assets);
    const fs_cfg_t fs_cfg = {
        .fs_letter = ASSET_DRIVE,
        .fs_nums = stored,
        .fs_assets = s_assets,
    };

    /* The adapter owns the lv_fs driver, so images can be referenced by path. */
    err = esp_lv_adapter_fs_mount(&fs_cfg, &s_fs);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "LVGL fs mount failed: %s", esp_err_to_name(err));
        mmap_assets_del(s_assets);
        s_assets = NULL;
        return err;
    }

    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;

    for (int i = 0; i < stored && s_count < ASSET_MAX_ENTRIES; i++) {
        const char *name = mmap_assets_get_name(s_assets, i);
        if (name == NULL) {
            continue;
        }

        ui_asset_entry_t *entry = &s_entries[s_count];
        base_name(name, entry->base, sizeof(entry->base));
        snprintf(entry->src, sizeof(entry->src), ASSET_DRIVE_STR ":%s", name);
        entry->used = true;
        s_count++;

        SYS_LOGD(TAG, "asset '%s' -> '%s'", entry->base, entry->src);
    }

    s_ready = true;
    SYS_LOGI(TAG, "%d asset(s) available", s_count);
    return ESP_OK;
}

bool ui_assets_ready(void)
{
    return s_ready;
}

int ui_assets_count(void)
{
    return s_count;
}

const char *ui_assets_src(const char *base_name)
{
    if (!s_ready || base_name == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].base, base_name) == 0) {
            return s_entries[i].src;
        }
    }
    return NULL;
}

lv_obj_t *ui_assets_image_create(lv_obj_t *parent, const char *base_name, int32_t size)
{
    lv_obj_t *img = lv_image_create(parent);
    if (img == NULL) {
        return NULL;
    }

    const char *src = ui_assets_src(base_name);
    if (src == NULL) {
        /* Hidden rather than absent: keeps the surrounding layout stable. */
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
        return img;
    }

    lv_image_set_src(img, src);
    if (size > 0) {
        lv_obj_set_size(img, size, size);
    }
    /* The PNGs are already white-on-transparent; no recolouring needed, which
     * is also why this component does not depend on ui_theme. */
    return img;
}

const char *ui_assets_weather_src(const char *icon_base)
{
    static const char *const fallbacks[] = { "w_overcast", "w_cloudy", "w_sunny" };

    if (icon_base != NULL) {
        const char *src = ui_assets_src(icon_base);
        if (src != NULL) {
            return src;
        }
    }
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
        const char *src = ui_assets_src(fallbacks[i]);
        if (src != NULL) {
            return src;
        }
    }
    return NULL;
}
