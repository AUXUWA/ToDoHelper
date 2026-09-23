/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_common.c
 * @brief Widgets shared by several pages: header, back button, tiles, placeholder.
 */

#include <stdio.h>

#include "pages_internal.h"

/* ------------------------------------------------------------------------- */
/* Navigation helpers                                                        */
/* ------------------------------------------------------------------------- */

static void back_clicked_cb(lv_event_t *e)
{
    (void)e;
    (void)ui_nav_back();
}

static void tile_clicked_cb(lv_event_t *e)
{
    const ui_page_id_t target = (ui_page_id_t)(intptr_t)lv_event_get_user_data(e);
    if (target >= UI_PAGE_MAX) {
        return;
    }
    (void)ui_nav_push(target);
}

lv_obj_t *page_back_button_create(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    if (btn == NULL) {
        return NULL;
    }

    lv_obj_set_height(btn, UI_TOUCH_MIN);
    lv_obj_set_width(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE_ALT, 0);
    lv_obj_set_style_radius(btn, UI_CARD_RADIUS, 0);
    lv_obj_set_style_pad_hor(btn, 16, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, "返回");
    lv_obj_set_style_text_font(label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
    lv_obj_center(label);

    lv_obj_add_event_cb(btn, back_clicked_cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

lv_obj_t *page_header_create(lv_obj_t *parent, const char *title)
{
    lv_obj_t *row = lv_obj_create(parent);
    if (row == NULL) {
        return NULL;
    }

    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    (void)page_back_button_create(row);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, title ? title : "");
    lv_obj_set_style_text_font(label, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
    lv_obj_set_flex_grow(label, 1);

    return row;
}

/* ------------------------------------------------------------------------- */
/* Home tiles                                                                */
/* ------------------------------------------------------------------------- */

lv_obj_t *page_tile_create(lv_obj_t *parent, const char *title, const char *subtitle,
                          ui_page_id_t target)
{
    lv_obj_t *tile = lv_button_create(parent);
    if (tile == NULL) {
        return NULL;
    }

    lv_obj_set_height(tile, 118);
    lv_obj_set_style_bg_color(tile, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tile, UI_CARD_RADIUS, 0);
    lv_obj_set_style_pad_all(tile, 14, 0);
    lv_obj_set_style_pad_row(tile, 6, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *title_label = lv_label_create(tile);
    lv_label_set_text(title_label, title ? title : "");
    lv_obj_set_style_text_font(title_label, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(title_label, UI_COLOR_TEXT, 0);

    lv_obj_t *sub_label = lv_label_create(tile);
    lv_label_set_text(sub_label, subtitle ? subtitle : "");
    lv_obj_set_style_text_font(sub_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(sub_label, UI_COLOR_TEXT_MUTED, 0);

    lv_obj_add_event_cb(tile, tile_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)target);
    return tile;
}

/* ------------------------------------------------------------------------- */
/* Placeholder page                                                          */
/* ------------------------------------------------------------------------- */

esp_err_t page_placeholder_create(lv_obj_t *parent, const char *title,
                                  const char *note, const char *stage)
{
    (void)page_header_create(parent, title);

    lv_obj_t *body = NULL;
    if (ui_card_create(parent, "功能开发中", &body) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_t *note_label = lv_label_create(body);
    lv_label_set_text(note_label, note ? note : "");
    lv_obj_set_width(note_label, LV_PCT(100));
    lv_label_set_long_mode(note_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(note_label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(note_label, UI_COLOR_TEXT, 0);

    if (stage != NULL && stage[0] != '\0') {
        lv_obj_t *stage_label = lv_label_create(body);
        lv_label_set_text_fmt(stage_label, "计划阶段：%s", stage);
        lv_obj_set_style_text_font(stage_label, ui_font(UI_FONT_SMALL), 0);
        lv_obj_set_style_text_color(stage_label, UI_COLOR_PRIMARY, 0);
    }

    return ESP_OK;
}
