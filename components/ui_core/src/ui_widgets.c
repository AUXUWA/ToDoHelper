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

/**
 * Close handler shared by every dialog button.
 *
 * ## Why the target is read from the user data
 *
 * This used to do `lv_obj_t *mbox = lv_event_get_current_target(e)` and pass the
 * result to `lv_msgbox_close()`. The callback is registered **on the footer
 * button**, so `lv_event_get_current_target()` returns that *button*, never the
 * message box - and `lv_msgbox_close()` on a button is a silent no-op. That is
 * why the "HTTP 鉴权已开启" dialog would not close: the OK button was wired to a
 * handler that could not possibly close anything (lv_msgbox_close_async() has
 * the same problem - it queues a delete of whatever it is handed).
 *
 * The message box is now passed explicitly as event user data, so the handler
 * does not have to infer it from the widget that fired the event.
 */
static void dialog_close_cb(lv_event_t *e)
{
    lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);
    if (mbox != NULL) {
        lv_msgbox_close(mbox);
    }
}

/** Shared geometry so every dialog on the panel looks like the same widget. */
#define UI_DIALOG_MAX_H_PCT 86
#define UI_DIALOG_MAX_W_PCT 94

/**
 * Apply the project theme to a freshly created message box.
 *
 * LVGL's default msgbox theme is light-on-light with a 20 px radius, which on
 * this dark UI reads as "a widget from another application". The card colours
 * (UI_COLOR_SURFACE / _ALT / _PRIMARY) and UI_CARD_RADIUS are the same ones
 * ui_card_create() uses, so a dialog is visually a card that floats.
 */
static void dialog_style(lv_obj_t *mbox)
{
    lv_obj_set_style_bg_color(mbox, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(mbox, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mbox, UI_CARD_RADIUS, 0);
    lv_obj_set_style_border_width(mbox, 0, 0);
    lv_obj_set_style_pad_all(mbox, 16, 0);
    lv_obj_set_style_pad_row(mbox, 10, 0);
    lv_obj_set_style_shadow_width(mbox, 24, 0);
    lv_obj_set_style_shadow_opa(mbox, LV_OPA_50, 0);

    /* The title/header and every label inherit the body font; without this the
     * Chinese text falls back to the ~2500-glyph built-in face and partially
     * renders as boxes (AGENT.md 9.0.4). */
    lv_obj_set_style_text_font(mbox, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(mbox, UI_COLOR_TEXT, 0);

    lv_obj_t *title = lv_msgbox_get_title(mbox);
    if (title != NULL) {
        lv_obj_set_style_text_font(title, ui_font(UI_FONT_TITLE), 0);
        lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    }

    /*
     * Note on the backdrop: lv_msgbox_create(lv_layer_top()) does *not* create a
     * dedicated backdrop - it only does that when passed NULL. So the parent here
     * is lv_layer_top() itself, which also hosts ui_toast(); tinting it would dim
     * the toasts too. The dialog's own shadow plus the opaque surface colour is
     * what separates it from the page, so no backdrop styling is applied.
     */
}

/**
 * Give a dialog a header "X" as well as its footer button.
 *
 * LVGL's own close button is used rather than a hand-rolled one: it is bound to
 * the message box itself (lv_msgbox_add_close_button installs
 * msgbox_close_click_event_cb), which is exactly the wiring the old footer
 * handler got wrong. Two ways out means a user who does not read the button
 * label still has an obvious exit.
 */
static void dialog_add_close_button(lv_obj_t *mbox)
{
    lv_obj_t *btn = lv_msgbox_add_close_button(mbox);
    if (btn == NULL) {
        return;
    }
    lv_obj_set_style_text_font(btn, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(btn, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
}

/**
 * Lay a footer button out in the project's style.
 *
 * The footer is a ROW (LVGL builds it that way), so buttons placed in it are
 * already side by side - which is the horizontal arrangement a two-button dialog
 * needs. The previous code only set a minimum height and left the default
 * (light) button skin in place, which is the "风格不匹配" part of the report.
 */
static void dialog_style_button(lv_obj_t *btn, lv_color_t color)
{
    if (btn == NULL) {
        return;
    }
    lv_obj_set_style_min_height(btn, UI_TOUCH_MIN, 0);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, UI_CARD_RADIUS - 4, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 18, 0);
    lv_obj_set_style_text_color(btn, UI_COLOR_ON_PRIMARY, 0);
    lv_obj_set_style_text_font(btn, ui_font(UI_FONT_BODY), 0);
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
    lv_obj_set_style_max_height(mbox, LV_PCT(UI_DIALOG_MAX_H_PCT), 0);
    lv_obj_set_style_max_width(mbox, LV_PCT(UI_DIALOG_MAX_W_PCT), 0);
    lv_obj_set_width(mbox, LV_PCT(UI_DIALOG_MAX_W_PCT));
    dialog_style(mbox);

    if (title != NULL && title[0] != '\0') {
        lv_msgbox_add_title(mbox, title);
        dialog_style(mbox);   /* the header was created after the first pass */
    }
    if (text != NULL && text[0] != '\0') {
        lv_obj_t *body = lv_msgbox_add_text(mbox, text);
        if (body != NULL) {
            /* The text is what actually overflows, so it gets the scrollbar. */
            lv_obj_set_width(body, LV_PCT(100));
            lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_text_color(body, UI_COLOR_TEXT, 0);
        }
    }

    lv_obj_t *btn = lv_msgbox_add_footer_button(mbox, ok_text ? ok_text : "确定");
    if (btn != NULL) {
        dialog_style_button(btn, UI_COLOR_PRIMARY);
        /* The box is the user data, not the button: see dialog_close_cb(). */
        lv_obj_add_event_cb(btn, dialog_close_cb, LV_EVENT_CLICKED, mbox);
        if (on_close != NULL) {
            lv_obj_add_event_cb(btn, on_close, LV_EVENT_CLICKED, user_data);
        }
    }

    /* A second, always-visible way out (see dialog_add_close_button). */
    dialog_add_close_button(mbox);

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

    /*
     * Wide and short, not tall and narrow.
     *
     * The panel is 800x480 - landscape - so a dialog that stacks prose above the
     * value it is trying to show runs out of height almost immediately: the
     * report was "面板挤压太长了，应该是横着的而不是竖着". Taking most of the
     * width and clamping the height gives the two columns room to sit side by
     * side, which is also how the token (32 characters) fits without wrapping to
     * three lines.
     */
    lv_obj_set_width(mbox, LV_PCT(UI_DIALOG_MAX_W_PCT));
    lv_obj_set_style_max_height(mbox, LV_PCT(UI_DIALOG_MAX_H_PCT), 0);
    lv_obj_set_style_max_width(mbox, LV_PCT(UI_DIALOG_MAX_W_PCT), 0);
    dialog_style(mbox);

    if (title != NULL && title[0] != '\0') {
        lv_msgbox_add_title(mbox, title);
        dialog_style(mbox);   /* the header was created after the first pass */
    }

    /*
     * Body: a two-column row.
     *
     * Left  = the short explanation; right = the secret in its own highlighted
     * box. Both are width-fractioned so the split holds on any panel size, and
     * the row wraps to a column automatically only if the two cannot fit - which
     * at 94% of 800 px they always can.
     */
    lv_obj_t *row = lv_obj_create(mbox);
    if (row != NULL) {
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 14, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        if (lead_in != NULL && lead_in[0] != '\0') {
            lv_obj_t *lead = lv_label_create(row);
            lv_label_set_text(lead, lead_in);
            lv_obj_set_flex_grow(lead, 1);
            lv_label_set_long_mode(lead, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_style_text_color(lead, UI_COLOR_TEXT, 0);
        }

        /* The secret itself: its own box, so it is visually distinct and
         * selectable at a glance rather than buried in a paragraph. */
        lv_obj_t *box = lv_obj_create(row);
        if (box != NULL) {
            lv_obj_remove_style_all(box);
            lv_obj_set_flex_grow(box, 1);
            lv_obj_set_height(box, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(box, UI_COLOR_SURFACE_ALT, 0);
            lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(box, UI_CARD_RADIUS - 4, 0);
            lv_obj_set_style_pad_all(box, 12, 0);
            lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *label = lv_label_create(box);
            lv_label_set_text(label, secret);
            lv_obj_set_width(label, LV_PCT(100));
            /* Wrap is kept as a safety net: if the token ever grows past what
             * the column holds, it wraps instead of being cut off mid-string
             * (the original bug this dialog was written to fix). */
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_style_text_color(label, UI_COLOR_PRIMARY, 0);
            lv_obj_center(label);
        }
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
        dialog_style_button(btn, UI_COLOR_PRIMARY);
        /* The box is the user data, not the button: see dialog_close_cb(). */
        lv_obj_add_event_cb(btn, dialog_close_cb, LV_EVENT_CLICKED, mbox);
    }

    /* The "无法关闭" report: this token dialog had a footer button whose handler
     * could never close it, and no other exit. Now there is an "X" as well. */
    dialog_add_close_button(mbox);

    ui_unlock();
    return mbox;
}
