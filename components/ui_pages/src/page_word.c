/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_word.c
 * @brief FR5: flashcards with a flip and three-way grading.
 *
 * The card shows the spelling first and the meaning/example after a tap, which
 * is the "翻卡：先单词，后释义" behaviour the requirement asks for.
 */

#include "pages_internal.h"

#include "word_service.h"

static const char *TAG = "page_word";

static lv_obj_t *s_progress_label;
static lv_obj_t *s_card_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_stats_label;
static lv_obj_t *s_grade_bar;

static word_item_t s_word;
static bool s_have_word;
static bool s_revealed;

/* ------------------------------------------------------------------------ */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------ */

static void render_card(void)
{
    if (s_card_label == NULL) {
        return;
    }

    if (!s_have_word) {
        lv_label_set_text(s_card_label, "词库为空\n请把词库放到 words/ 后重开");
        lv_label_set_text(s_hint_label, "");
        if (s_grade_bar != NULL) {
            lv_obj_add_flag(s_grade_bar, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (!s_revealed) {
        /* Front: spelling and phonetic only. */
        if (s_word.phonetic[0] != '\0') {
            lv_label_set_text_fmt(s_card_label, "%s\n%s", s_word.spelling, s_word.phonetic);
        } else {
            lv_label_set_text(s_card_label, s_word.spelling);
        }
        lv_label_set_text(s_hint_label, "点击卡片查看释义");
        if (s_grade_bar != NULL) {
            lv_obj_add_flag(s_grade_bar, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        /* Back: meaning and example. */
        lv_label_set_text_fmt(s_card_label, "%s\n\n%s\n\n%s",
                              s_word.spelling,
                              s_word.meaning[0] ? s_word.meaning : "（无释义）",
                              s_word.example[0] ? s_word.example : "（无例句）");
        lv_label_set_text(s_hint_label, "选择熟悉度，进入下一张");
        if (s_grade_bar != NULL) {
            lv_obj_remove_flag(s_grade_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void render_progress(void)
{
    if (s_progress_label != NULL) {
        size_t position = 0;
        size_t total = 0;
        (void)word_queue_position(&position, &total);
        lv_label_set_text_fmt(s_progress_label, "今日队列 %u/%u · 等级 %u",
                              (unsigned)position, (unsigned)total,
                              s_have_word ? s_word.level : 0);
    }

    if (s_stats_label != NULL) {
        word_stats_t stats;
        if (word_stats(&stats) == ESP_OK) {
            lv_label_set_text_fmt(s_stats_label,
                                  "词库 %u 词 · 新词 %u · 待复习 %u · 已掌握 %u",
                                  (unsigned)stats.total, (unsigned)stats.new_words,
                                  (unsigned)stats.due, (unsigned)stats.learned);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Actions                                                                   */
/* ------------------------------------------------------------------------ */

static void next_word(void)
{
    s_revealed = false;
    s_have_word = (word_next(&s_word) == ESP_OK);
    render_card();
    render_progress();
}

static void card_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (!s_have_word) {
        next_word();
        return;
    }
    if (!s_revealed) {
        s_revealed = true;
    } else {
        next_word();
    }
    render_card();
}

static void grade_clicked_cb(lv_event_t *e)
{
    const word_review_result_t grade =
        (word_review_result_t)(intptr_t)lv_event_get_user_data(e);
    if (!s_have_word) {
        return;
    }

    esp_err_t err = word_review(s_word.id, grade);
    if (err != ESP_OK) {
        ui_toast("记录失败：%s", esp_err_to_name(err));
    }
    next_word();
}

static void restart_clicked_cb(lv_event_t *e)
{
    (void)e;
    (void)word_restart_queue();
    next_word();
    ui_toast("已回到队列开头");
}

/* ------------------------------------------------------------------------ */
/* Page                                                                      */
/* ------------------------------------------------------------------------ */

static esp_err_t word_create(lv_obj_t *parent)
{
    (void)page_header_create(parent, "单词背诵");

    s_progress_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_progress_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_progress_label, UI_COLOR_PRIMARY, 0);

    /* Card body: tapping anywhere on it flips or advances. */
    lv_obj_t *card = lv_button_create(parent);
    if (card == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_bg_color(card, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_radius(card, UI_CARD_RADIUS, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(card, card_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_card_label = lv_label_create(card);
    lv_obj_set_width(s_card_label, LV_PCT(100));
    lv_label_set_long_mode(s_card_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_card_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_card_label, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(s_card_label, UI_COLOR_TEXT, 0);

    s_hint_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_hint_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_hint_label, UI_COLOR_TEXT_MUTED, 0);

    s_grade_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(s_grade_bar);
    lv_obj_set_width(s_grade_bar, LV_PCT(100));
    lv_obj_set_height(s_grade_bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_grade_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_grade_bar, 8, 0);
    lv_obj_add_flag(s_grade_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_grade_bar, LV_OBJ_FLAG_SCROLLABLE);

    const struct {
        const char *text;
        word_review_result_t grade;
        lv_color_t color;
    } grades[] = {
        { "不认识", WORD_REVIEW_UNKNOWN, UI_COLOR_DANGER },
        { "模糊", WORD_REVIEW_FUZZY, UI_COLOR_WARNING },
        { "认识", WORD_REVIEW_KNOWN, UI_COLOR_SUCCESS },
    };

    for (size_t i = 0; i < sizeof(grades) / sizeof(grades[0]); i++) {
        lv_obj_t *btn = lv_button_create(s_grade_bar);
        lv_obj_set_height(btn, UI_TOUCH_MIN);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_bg_color(btn, grades[i].color, 0);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, grades[i].text);
        lv_obj_set_style_text_font(label, ui_font(UI_FONT_BODY), 0);
        lv_obj_set_style_text_color(label, UI_COLOR_ON_PRIMARY, 0);
        lv_obj_center(label);
        lv_obj_add_event_cb(btn, grade_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)grades[i].grade);
    }

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_stats_label = lv_label_create(bar);
    lv_obj_set_flex_grow(s_stats_label, 1);
    lv_obj_set_style_text_font(s_stats_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_stats_label, UI_COLOR_TEXT_MUTED, 0);

    lv_obj_t *restart = lv_button_create(bar);
    lv_obj_set_height(restart, UI_TOUCH_MIN);
    lv_obj_set_style_bg_color(restart, UI_COLOR_SURFACE_ALT, 0);
    lv_obj_t *restart_label = lv_label_create(restart);
    lv_label_set_text(restart_label, "重新开始");
    lv_obj_set_style_text_font(restart_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_center(restart_label);
    lv_obj_add_event_cb(restart, restart_clicked_cb, LV_EVENT_CLICKED, NULL);

    next_word();

    (void)ui_event_bind(WORD_EVENT, WORD_EVENT_LOADED, UI_PAGE_WORD, 0);
    SYS_LOGD(TAG, "word page built");
    return ESP_OK;
}

static esp_err_t word_on_show(void)
{
    if (!s_have_word) {
        next_word();
    }
    render_card();
    render_progress();
    return ESP_OK;
}

static esp_err_t word_on_event(int32_t event_id, void *data)
{
    (void)event_id;
    (void)data;
    render_progress();
    return ESP_OK;
}

const ui_page_t page_word = {
    .id = UI_PAGE_WORD,
    .name = "word",
    .create = word_create,
    .on_show = word_on_show,
    .on_event = word_on_event,
};
