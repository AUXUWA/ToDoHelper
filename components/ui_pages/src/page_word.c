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

#include <stdlib.h>
#include <string.h>

#include "pages_internal.h"

#include "maimemo_client.h"
#include "word_service.h"

static const char *TAG = "page_word";

static lv_obj_t *s_progress_label;
static lv_obj_t *s_card_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_stats_label;
static lv_obj_t *s_grade_bar;
/** 助记/例句 fetched from MaiMemo (plan B); hidden when unavailable. */
static lv_obj_t *s_enrich_label;

static word_item_t s_word;
static bool s_have_word;
static bool s_revealed;

/**
 * Enrichment for the card on screen.
 *
 * Fetched on a worker task because maimemo_enrich() may do TLS; the label is
 * updated from that task via lv_async_call, never from inside the fetch (AGENT.md
 * 9.0.1: LVGL work must happen on the LVGL task).
 */
static maimemo_enrich_t s_enrich;
static bool s_enrich_valid;
static bool s_enrich_pending;

/* ------------------------------------------------------------------------ */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------ */

static void render_enrich(void)
{
    if (s_enrich_label == NULL) {
        return;
    }
    if (!s_revealed || !s_enrich_valid ||
        (s_enrich.mnemonic[0] == '\0' && s_enrich.example[0] == '\0')) {
        lv_obj_add_flag(s_enrich_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* Both fields are optional; only label the one(s) we actually have. */
    char text[560];
    text[0] = '\0';
    if (s_enrich.mnemonic[0] != '\0') {
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "助记 %s",
                 s_enrich.mnemonic);
    }
    if (s_enrich.example[0] != '\0') {
        if (text[0] != '\0') {
            strlcat(text, "\n", sizeof(text));
        }
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "例句 %s",
                 s_enrich.example);
    }

    lv_label_set_text(s_enrich_label, text);
    lv_obj_remove_flag(s_enrich_label, LV_OBJ_FLAG_HIDDEN);
}

/** Applied on the LVGL task after the worker finishes fetching. */
static void enrich_ready_async(void *arg)
{
    (void)arg;
    s_enrich_pending = false;
    render_enrich();
}

/*
 * Subscription handlers for the MaiMemo sync events, defined below with the page
 * table but referenced by word_create().
 *
 * They use ui_event_subscribe() rather than ui_event_bind(): a page has one
 * on_event() handler and it is not told which base fired, so with both
 * WORD_EVENT_LOADED (0) and MAIMEMO_EVENT_* (0,1,2) bound to the same page the
 * ids would be ambiguous. Separate handlers keep them distinguishable.
 */
static esp_err_t maimemo_sync_done_event(int32_t event_id, void *data);
static esp_err_t maimemo_sync_failed_event(int32_t event_id, void *data);

static void enrich_worker(void *arg)
{
    char *spelling = (char *)arg;
    maimemo_enrich_t enrich;
    const esp_err_t err = maimemo_enrich(spelling, &enrich);
    free(spelling);

    if (err == ESP_OK) {
        s_enrich = enrich;
        s_enrich_valid = true;
    } else {
        /* Not an error to report: a study-scoped token simply cannot read 助记/
         * 例句, and a word may have neither. The card just does not grow a
         * footnote. */
        memset(&s_enrich, 0, sizeof(s_enrich));
        s_enrich_valid = false;
        SYS_LOGD(TAG, "no enrichment (%s)", esp_err_to_name(err));
    }

    lv_async_call(enrich_ready_async, NULL);
    vTaskDelete(NULL);
}

/** Kick off a plan B prefetch for the current card (never blocks the UI). */
static void enrich_start(const char *spelling)
{
    memset(&s_enrich, 0, sizeof(s_enrich));
    s_enrich_valid = false;
    if (s_enrich_label != NULL) {
        lv_obj_add_flag(s_enrich_label, LV_OBJ_FLAG_HIDDEN);
    }

    if (spelling == NULL || spelling[0] == '\0' || !maimemo_has_token() ||
        s_enrich_pending) {
        return;
    }

    char *copy = strdup(spelling);
    if (copy == NULL) {
        return;
    }
    s_enrich_pending = true;
    /* 5 KB: TLS handshake plus JSON parsing, and paging after a 24 KB body.
     * The stack must be internal RAM and one is created per card shown, so this
     * is also the allocation that competes with the flash writer's task for the
     * board's ~13 KB of contiguous internal heap (see word_save_progress_async).
     * Failing to start it is harmless - the card simply shows no 助记 - so the
     * flag is cleared and the page carries on. */
    if (xTaskCreate(enrich_worker, "th_mm_enrich", 5120, copy, 3, NULL) != pdPASS) {
        s_enrich_pending = false;
        free(copy);
        SYS_LOGD(TAG, "no memory for the enrichment worker; skipping 助记");
    }
}

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
        /* Back: meaning and example, plus the 助记 from MaiMemo when we have it. */
        lv_label_set_text_fmt(s_card_label, "%s\n\n%s\n\n%s",
                              s_word.spelling,
                              s_word.meaning[0] ? s_word.meaning : "（无释义）",
                              s_word.example[0] ? s_word.example : "（无例句）");
        lv_label_set_text(s_hint_label, "选择熟悉度，进入下一张");
        if (s_grade_bar != NULL) {
            lv_obj_remove_flag(s_grade_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    render_enrich();
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
        /* In cloud mode the account's own progress is the meaningful number, so
         * show that instead of the local book's counters (which would read
         * "0/833" no matter how much the user studied on their phone). */
        maimemo_progress_t cloud;
        if (word_cloud_mode() && maimemo_get_cached_progress(&cloud) == ESP_OK) {
            /* Show seconds below a minute: rounding 59 s down to "0 分" reads as
             * "you have not studied", which is both wrong and discouraging. */
            if (cloud.study_time_ms < 60000) {
                lv_label_set_text_fmt(s_stats_label,
                                      "墨墨今日 %d/%d · 已用 %d 秒 · 计划 %d 词%s",
                                      cloud.finished, cloud.total, cloud.study_time_ms / 1000,
                                      cloud.due_count,
                                      (word_cloud_count() > 0) ? "" : " · 待同步");
            } else {
                lv_label_set_text_fmt(s_stats_label,
                                      "墨墨今日 %d/%d · 已用 %d 分 · 计划 %d 词%s",
                                      cloud.finished, cloud.total, cloud.study_time_ms / 60000,
                                      cloud.due_count,
                                      (word_cloud_count() > 0) ? "" : " · 待同步");
            }
            return;
        }

        word_stats_t stats;
        if (word_stats(&stats) == ESP_OK) {
            lv_label_set_text_fmt(s_stats_label,
                                  "词库 %u 词 · 新词 %u · 待复习 %u · 已掌握 %u%s",
                                  (unsigned)stats.total, (unsigned)stats.new_words,
                                  (unsigned)stats.due, (unsigned)stats.learned,
                                  word_cloud_mode() ? " · 云端待同步" : "");
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

    /* Plan B: warm the 助记/例句 for the card we just moved to. The fetch runs on
     * its own task; the front of the card does not need it, so it has the whole
     * time the user spends recalling the word to finish. */
    if (s_have_word) {
        enrich_start(s_word.spelling);
    } else {
        enrich_start(NULL);
    }
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

    /* Plan C: a word the user does not recognise is exactly the word worth
     * learning, so send it back to the MaiMemo study plan. The *_async variant
     * only queues the work on a worker task, which is what an LVGL callback
     * needs: the synchronous form did the whole TLS round trip (and a task
     * creation) with the adapter lock held, and on this board that is where the
     * "记录失败：ESP_ERR_NO_MEM" on grading came from. */
    if (grade == WORD_REVIEW_UNKNOWN && maimemo_push_enabled()) {
        (void)maimemo_push_unknown_async(s_word.spelling);
    }

    esp_err_t err = word_review(s_word.id, grade);
    if (err == ESP_ERR_NOT_FOUND && s_word.id == WORD_ID_CLOUD) {
        /* A cloud-only card: nothing local to record. The push above (or the
         * account's own sync) is the record. Be explicit rather than silent. */
        SYS_LOGD(TAG, "'%s' is not in the local book; graded in the account only",
                 s_word.spelling);
    } else if (err != ESP_OK) {
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

    /* Plan B: 助记/例句 from MaiMemo, shown under the hint once revealed. Hidden
     * by default so the card keeps its shape when there is nothing to add. */
    s_enrich_label = lv_label_create(parent);
    lv_obj_set_width(s_enrich_label, LV_PCT(100));
    lv_label_set_long_mode(s_enrich_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_enrich_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_enrich_label, UI_COLOR_PRIMARY, 0);
    lv_obj_add_flag(s_enrich_label, LV_OBJ_FLAG_HIDDEN);

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
    /* Refresh the progress line when a background MaiMemo sync lands, and bring
     * the cloud queue in with it (plan A). */
    (void)ui_event_subscribe(MAIMEMO_EVENT, MAIMEMO_EVENT_SYNC_DONE, 0,
                             maimemo_sync_done_event);
    (void)ui_event_subscribe(MAIMEMO_EVENT, MAIMEMO_EVENT_SYNC_FAILED, sizeof(esp_err_t),
                             maimemo_sync_failed_event);
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

/**
 * A MaiMemo sync finished: pull today's plan into the queue and restart.
 *
 * Runs on the LVGL task (ui_event_subscribe marshals there), and
 * word_cloud_refresh() does blocking TLS - so the fetch is handed to a worker
 * task; this handler only schedules it.
 */
static void cloud_fetch_worker(void *arg)
{
    (void)arg;
    const esp_err_t err = word_cloud_refresh();
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "cloud queue refresh failed: %s", esp_err_to_name(err));
    }
    lv_async_call(enrich_ready_async, NULL);   /* re-render, on the LVGL task */
    vTaskDelete(NULL);
}

/*
 * These use ui_event_subscribe() rather than ui_event_bind(): see the forward
 * declaration above for why.
 */
static esp_err_t maimemo_sync_done_event(int32_t event_id, void *data)
{
    (void)event_id;
    (void)data;
    if (word_cloud_mode()) {
        if (xTaskCreate(cloud_fetch_worker, "th_mm_fetch", 5120, NULL, 3, NULL) != pdPASS) {
            SYS_LOGW(TAG, "cannot start cloud fetch task");
        }
    }
    render_progress();
    return ESP_OK;
}

static esp_err_t maimemo_sync_failed_event(int32_t event_id, void *data)
{
    (void)event_id;
    /* Not surfaced as a toast: the page still works offline, and a failed
     * background sync is not something the user asked for. */
    const esp_err_t err = (data != NULL) ? *(const esp_err_t *)data : ESP_FAIL;
    SYS_LOGW(TAG, "maimemo sync failed: %s", esp_err_to_name(err));
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
