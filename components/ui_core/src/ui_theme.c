/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_theme.h"

#include "ui_core.h"
#include "ui_fonts.h"

const lv_font_t *ui_font(ui_font_role_t role)
{
    const lv_font_t *font = ui_fonts_get(role);
    if (font != NULL) {
        return font;
    }
    return lv_font_get_default();
}

void ui_theme_apply(void)
{
    lv_obj_t *scr = lv_screen_active();
    if (scr == NULL) {
        return;
    }

    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_text_color(scr, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(scr, ui_font(UI_FONT_BODY), 0);
}
