/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_splash.c
 * @brief Boot banner. Shown while the optional service modules come up.
 */

#include "pages_internal.h"

static const char *TAG = "page_splash";

#ifndef CONFIG_APP_PROJECT_VER
#define CONFIG_APP_PROJECT_VER "0.1.0"
#endif

static lv_obj_t *s_status_label;

static esp_err_t splash_create(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    if (box == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 10, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "ToDoHelper");
    lv_obj_set_style_text_font(title, ui_font(UI_FONT_HUGE), 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);

    lv_obj_t *subtitle = lv_label_create(box);
    lv_label_set_text(subtitle, "桌面待办助手");
    lv_obj_set_style_text_font(subtitle, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(subtitle, UI_COLOR_PRIMARY, 0);

    s_status_label = lv_label_create(box);
    lv_label_set_text(s_status_label, "正在初始化…");
    lv_obj_set_style_text_font(s_status_label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_status_label, UI_COLOR_TEXT_MUTED, 0);

    lv_obj_t *version = lv_label_create(box);
    lv_label_set_text_fmt(version, "固件 %s", CONFIG_APP_PROJECT_VER);
    lv_obj_set_style_text_font(version, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(version, UI_COLOR_TEXT_MUTED, 0);

    SYS_LOGD(TAG, "splash built");
    return ESP_OK;
}

static esp_err_t splash_on_show(void)
{
    if (s_status_label != NULL) {
        lv_label_set_text(s_status_label, "正在初始化…");
    }
    return ESP_OK;
}

const ui_page_t page_splash = {
    .id = UI_PAGE_SPLASH,
    .name = "splash",
    .create = splash_create,
    .on_show = splash_on_show,
};
