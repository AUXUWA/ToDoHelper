/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_widgets.c
 * @brief Small shared building blocks: cards, toasts and modal dialogs.
 *
 * Keeping these here means pages stay declarative and the look stays consistent
 * (see docs/TODOHELPER_REQUIREMENTS.md 3 and 10.3).
 */

#include <stdarg.h>
#include <stdio.h>

#include "ui_core.h"
#include "ui_theme.h"

/* ------------------------------------------------------------------------- */
/* Card                                                                      */
/* ------------------------------------------------------------------------- */

lv_obj_t *ui_card_create(lv_obj_t *parent, const char *title, lv_obj_t **out_body)
{
    lv_obj_t *card = lv_obj_create(parent);
    if (card == NULL) {
        return NULL;
    }

    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UI_CARD_RADIUS, 0);
    lv_obj_set_style_pad_all(card, UI_CARD_PAD, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    if (title != NULL && title[0] != '\0') {
        lv_obj_t *label = lv_label_create(card);
        lv_label_set_text(label, title);
        lv_obj_set_style_text_font(label, ui_font(UI_FONT_TITLE), 0);
        lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
    }

    lv_obj_t *body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    if (out_body != NULL) {
        *out_body = body;
    }
    return card;
}

/* ------------------------------------------------------------------------- */
/* Toast                                                                     */
/* ------------------------------------------------------------------------- */

#define UI_TOAST_MS 2000

static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;

static void toast_hide_cb(lv_timer_t *timer)
{
    if (s_toast != NULL) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
    lv_timer_pause(timer);
}

esp_err_t ui_toast(const char *fmt, ...)
{
    if (fmt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char text[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    if (!ui_lock(-1)) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_toast != NULL) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }

    s_toast = lv_label_create(lv_layer_top());
    if (s_toast != NULL) {
        lv_label_set_text(s_toast, text);
        lv_obj_set_style_text_font(s_toast, ui_font(UI_FONT_BODY), 0);
        lv_obj_set_style_text_color(s_toast, UI_COLOR_TEXT, 0);
        lv_obj_set_style_bg_color(s_toast, UI_COLOR_SURFACE_ALT, 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_toast, UI_CARD_RADIUS, 0);
        lv_obj_set_style_pad_all(s_toast, 12, 0);
        lv_obj_set_style_max_width(s_toast, LV_PCT(80), 0);
        lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -28);
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);

        if (s_toast_timer == NULL) {
            s_toast_timer = lv_timer_create(toast_hide_cb, UI_TOAST_MS, NULL);
        }
        if (s_toast_timer != NULL) {
            lv_timer_reset(s_toast_timer);
            lv_timer_resume(s_toast_timer);
        }
    }

    ui_unlock();
    return (s_toast != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ------------------------------------------------------------------------- */
/* Dialog                                                                    */
/* ------------------------------------------------------------------------- */

static void dialog_close_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    if (mbox != NULL) {
        lv_msgbox_close(mbox);
    }
}

/**
 * Show a modal dialog.
 *
 * ## Why the box is clamped and scrollable
 *
 * A msgbox sizes itself to its content, so a body longer than the 480 px panel
 * grew taller than the screen: the title was clipped off the top and the rest of
 * the text ran past the bottom edge with no way to read it. That is exactly what
 * the "HTTP 鉴权已开启" dialog did - it is the longest string in the UI (a
 * multi-line Chinese explanation), and it covered the whole page.
 *
 * Clamping to a fraction of the screen and letting the *text* scroll keeps the
 * title and the OK button reachable no matter how long the body is, which is the
 * property a dialog needs; growing without bound is not.
 */
lv_obj_t *ui_dialog(const char *title, const char *text,
                    const char *ok_text, lv_event_cb_t on_close, void *user_data)
{
    if (!ui_lock(-1)) {
        return NULL;
    }

    lv_obj_t *mbox = lv_msgbox_create(lv_layer_top());
    if (mbox == NULL) {
        ui_unlock();
        return NULL;
    }

    /* Leave room for the status bar and a margin at both ends. */
    lv_obj_set_style_max_height(mbox, LV_PCT(88), 0);
    lv_obj_set_style_max_width(mbox, LV_PCT(92), 0);

    if (title != NULL && title[0] != '\0') {
        lv_msgbox_add_title(mbox, title);
    }
    if (text != NULL && text[0] != '\0') {
        lv_obj_t *body = lv_msgbox_add_text(mbox, text);
        if (body != NULL) {
            /* The text is what actually overflows, so it gets the scrollbar. */
            lv_obj_set_width(body, LV_PCT(100));
            lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    lv_obj_t *btn = lv_msgbox_add_footer_button(mbox, ok_text ? ok_text : "确定");
    if (btn != NULL) {
        lv_obj_set_style_min_height(btn, UI_TOUCH_MIN, 0);
        if (on_close != NULL) {
            lv_obj_add_event_cb(btn, on_close, LV_EVENT_CLICKED, user_data);
        }
        lv_obj_add_event_cb(btn, dialog_close_cb, LV_EVENT_CLICKED, NULL);
    }

    /* Applying the project font to the whole box keeps Chinese glyphs intact. */
    lv_obj_set_style_text_font(mbox, ui_font(UI_FONT_BODY), 0);

    ui_unlock();
    return mbox;
}

/**
 * Dialog whose body is a long, copyable secret (the HTTP auth token).
 *
 * The token is set in the small monospace-ish font and given its own scrollable
 * container so all 32 characters stay readable on one screen, and the
 * explanatory text is kept short - the previous version spent the whole panel on
 * prose and never actually rendered the token it told the user to copy.
 */
lv_obj_t *ui_dialog_secret(const char *title, const char *lead_in, const char *secret,
                           const char *note, const char *ok_text)
{
    if (secret == NULL) {
        return NULL;
    }
    if (!ui_lock(-1)) {
        return NULL;
    }

    lv_obj_t *mbox = lv_msgbox_create(lv_layer_top());
    if (mbox == NULL) {
        ui_unlock();
        return NULL;
    }

    lv_obj_set_style_max_height(mbox, LV_PCT(88), 0);
    lv_obj_set_style_max_width(mbox, LV_PCT(92), 0);

    if (title != NULL && title[0] != '\0') {
        lv_msgbox_add_title(mbox, title);
    }

    if (lead_in != NULL && lead_in[0] != '\0') {
        lv_obj_t *lead = lv_msgbox_add_text(mbox, lead_in);
        if (lead != NULL) {
            lv_obj_set_width(lead, LV_PCT(100));
            lv_label_set_long_mode(lead, LV_LABEL_LONG_MODE_WRAP);
        }
    }

    /* The secret itself: its own box, so it is visually distinct and selectable
     * at a glance rather than buried in a paragraph. */
    lv_obj_t *box = lv_obj_create(mbox);
    if (box != NULL) {
        lv_obj_remove_style_all(box);
        lv_obj_set_width(box, LV_PCT(100));
        lv_obj_set_height(box, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(box, UI_COLOR_SURFACE_ALT, 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(box, UI_CARD_RADIUS, 0);
        lv_obj_set_style_pad_all(box, 10, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *label = lv_label_create(box);
        lv_label_set_text(label, secret);
        lv_obj_set_width(label, LV_PCT(100));
        /* Wrap, because 32 hex characters do not fit on one 480 px line at the
         * body font; without this the token was cut off mid-string. */
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_color(label, UI_COLOR_PRIMARY, 0);
        lv_obj_center(label);
    }

    if (note != NULL && note[0] != '\0') {
        lv_obj_t *tail = lv_msgbox_add_text(mbox, note);
        if (tail != NULL) {
            lv_obj_set_width(tail, LV_PCT(100));
            lv_label_set_long_mode(tail, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_style_text_color(tail, UI_COLOR_TEXT_MUTED, 0);
        }
    }

    lv_obj_t *btn = lv_msgbox_add_footer_button(mbox, ok_text ? ok_text : "知道了");
    if (btn != NULL) {
        lv_obj_set_style_min_height(btn, UI_TOUCH_MIN, 0);
        lv_obj_add_event_cb(btn, dialog_close_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_set_style_text_font(mbox, ui_font(UI_FONT_BODY), 0);

    ui_unlock();
    return mbox;
}
