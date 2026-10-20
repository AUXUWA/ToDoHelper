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

#include "esp_heap_caps.h"

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
/* Studied-word uplink (v1.4.0)                                              */
/* ------------------------------------------------------------------------ */

/*
 * ## What this does, and what it deliberately does not claim
 *
 * MaiMemo's Open API has **no endpoint that submits a review grade** - its study
 * surface is progress/today/records/add/advance only (verified against the
 * official CLI and its study.proto-derived types). So a device-side grade can
 * never move the account's "今日 N/M" counter, and this code does not pretend it
 * can.
 *
 * What it does is the honest, useful part: words the user studied on the board
 * are collected and **advanced back into the account's review plan** via
 * `advance_study`, so the cloud schedules them instead of forgetting them, and
 * they reappear in the phone app.
 *
 * Three properties make this safe on this board:
 *   - it is **batched** - a word is queued and sent with the next flush, not
 *     pushed one HTTPS round trip at a time (the old path did one resolve plus
 *     one add per word);
 *   - the network call runs on **one** worker task, never in the LVGL callback
 *     (AGENT.md 9.0.1.1);
 *   - the queue is a fixed small array, so a busy study session cannot allocate
 *     without bound.
 */
#define WORD_UPLINK_MAX 24

static char s_uplink[WORD_UPLINK_MAX][WORD_SPELLING_MAX];
static size_t s_uplink_count;
static bool s_uplink_running;

/**
 * Flush the collected spellings to MaiMemo on a worker task.
 *
 * The queue is copied and cleared on the LVGL task before this starts, so the
 * worker owns its copy outright and the UI can keep grading while the TLS round
 * trips happen.
 */
static void uplink_worker(void *arg)
{
    char (*batch)[WORD_SPELLING_MAX] = (char (*)[WORD_SPELLING_MAX])arg;

    const char *spellings[WORD_UPLINK_MAX];
    size_t count = 0;
    while (count < WORD_UPLINK_MAX && batch[count][0] != '\0') {
        spellings[count] = batch[count];
        count++;
    }

    if (count > 0) {
        int advanced = 0;
        const esp_err_t err = maimemo_advance_spellings(spellings, count, &advanced);
        if (err == ESP_OK) {
            SYS_LOGI(TAG, "uplinked %d of %u studied word(s) back to the plan",
                     advanced, (unsigned)count);
        } else {
            /* Not surfaced as a toast: the local record is already saved, and a
             * cloud-plan update failing is not something the user asked for. */
            SYS_LOGW(TAG, "uplink skipped: %s", esp_err_to_name(err));
        }
    }

    free(batch);
    s_uplink_running = false;
    vTaskDelete(NULL);
}

/**
 * Send whatever has been collected so far (LVGL task only).
 *
 * Called when the queue fills or when the page is left, which keeps the request
 * count proportional to the session rather than to the number of cards graded.
 */
static void uplink_flush(void)
{
    if (s_uplink_count == 0 || s_uplink_running) {
        return;
    }
    /* Cheap pre-checks: no allocation and no task when the feature is off, which
     * is the default because advancing mutates the account's plan. */
    if (!maimemo_advance_enabled() || !maimemo_has_token()) {
        s_uplink_count = 0;
        return;
    }

    /* PSRAM: this is a bulk buffer, and internal RAM is the scarce resource. */
    char (*batch)[WORD_SPELLING_MAX] = heap_caps_calloc_prefer(
        WORD_UPLINK_MAX, sizeof(*batch), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (batch == NULL) {
        s_uplink_count = 0;
        return;
    }
    memcpy(batch, s_uplink, s_uplink_count * sizeof(*batch));
    const size_t queued = s_uplink_count;
    s_uplink_count = 0;
    s_uplink_running = true;

    /* 5 KB: TLS handshake plus the JSON bodies, and the stack must be internal
     * RAM (AGENT.md 9.0.1). If it cannot start, the words stay in the local
     * history, which is the honest outcome - the plan update is best effort. */
    if (xTaskCreate(uplink_worker, "th_word_up", 5120, batch, 3, NULL) != pdPASS) {
        SYS_LOGW(TAG, "cannot start the uplink worker; %u word(s) kept locally only",
                 (unsigned)queued);
        free(batch);
        s_uplink_running = false;
    }
}

/** Queue one studied spelling; flushes when the batch is full (LVGL task only). */
static void uplink_add(const char *spelling)
{
    if (spelling == NULL || spelling[0] == '\0') {
        return;
    }
    if (!maimemo_advance_enabled() || !maimemo_has_token()) {
        return;
    }

    /* Skip a spelling already queued: re-grading a card should not duplicate it. */
    for (size_t i = 0; i < s_uplink_count; i++) {
        if (strcasecmp(s_uplink[i], spelling) == 0) {
            return;
        }
    }

    if (s_uplink_count >= WORD_UPLINK_MAX) {
        uplink_flush();
    }
    if (s_uplink_count >= WORD_UPLINK_MAX) {
        return;   /* flush could not start: drop rather than overwrite */
    }

    strlcpy(s_uplink[s_uplink_count], spelling, sizeof(s_uplink[0]));
    s_uplink_count++;

    if (s_uplink_count >= WORD_UPLINK_MAX) {
        uplink_flush();
    }
}

/* ------------------------------------------------------------------------ */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------ */

static void render_enrich(void)
{
    if (s_enrich_label == NULL) {
        return;
    }
    /*
     * The card body already carries the local book's example (or the cloud's),
     * so this block is now only for what the card cannot show: the 助记 (which
     * has no field on word_item_t) and, for a word whose meaning came from the
     * cloud cache rather than the card, the example read back from there.
     */
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

/** Repaint the card body; defined below, referenced by enrich_ready_async(). */
static void render_card(void);

/** Applied on the LVGL task after the worker finishes fetching. */
static void enrich_ready_async(void *arg)
{
    (void)arg;
    s_enrich_pending = false;
    /*
     * render_card(), not just render_enrich(): since v1.4.0 the worker may have
     * filled in the card's meaning/example (that is how a cloud-only word gets
     * its body text), so the card itself has to be repainted. render_card() ends
     * by calling render_enrich(), so the footnote is still handled.
     */
    render_card();
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

    /*
     * v1.4.0: this is where a cloud-only card gets its body text. The queue is
     * built without any network content (see cloud_fill_content in
     * word_service.c), so the meaning the user is about to read arrives here.
     *
     * Only the fields the card is missing are filled: a local-book card keeps
     * its curated text even if the cloud would say something different.
     */
    if (err == ESP_OK) {
        bool changed = false;
        if (s_word.meaning[0] == '\0' && enrich.meaning[0] != '\0') {
            strlcpy(s_word.meaning, enrich.meaning, sizeof(s_word.meaning));
            changed = true;
        }
        if (s_word.example[0] == '\0' && enrich.example[0] != '\0') {
            strlcpy(s_word.example, enrich.example, sizeof(s_word.example));
            changed = true;
        }
        if (changed) {
            SYS_LOGI(TAG, "'%s' got its content from MaiMemo (释义 %u B, 例句 %u B)",
                     spelling, (unsigned)strlen(enrich.meaning),
                     (unsigned)strlen(enrich.example));
        }
    }

    free(spelling);
    lv_async_call(enrich_ready_async, NULL);
    vTaskDelete(NULL);
}

/**
 * Kick off a plan B prefetch for the current card (never blocks the UI).
 *
 * The word page used to create one 5 KB worker per card shown, which is the
 * allocation that took the board's internal RAM from 74 KB to 49 KB and its
 * largest contiguous block from 18 KB to 5 KB (CHANGELOG 1.3.2 "Known issues").
 *
 * v1.4.0 role: this is now the **content path for cloud-only words**. The queue
 * build deliberately does no network I/O (fetching 64 words' content up front
 * fragmented internal RAM to 160 bytes and wedged the web server - see
 * cloud_fill_content in word_service.c), so a word that is not in the local book
 * gets its meaning/example here, when the user actually flips to it, and the
 * result is cached on the SD card so later views are offline.
 *
 * The prefetch is still skipped when the card already has everything, which is
 * the common case: most queue words are in the local book.
 */
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

    /*
     * Nothing to add: the card already has both a meaning and an example (either
     * from the local book or from the cache), so the only thing this worker could
     * contribute is the 助记. Skipping keeps the common card from spawning a task
     * at all - and this board has only ~13 KB of contiguous internal heap.
     */
    if (s_word.meaning[0] != '\0' && s_word.example[0] != '\0' &&
        !maimemo_push_enabled()) {
        SYS_LOGD(TAG, "'%s' already has content; skipping the enrich prefetch", spelling);
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
        /*
         * Back: meaning and example, plus the 助记 from MaiMemo when we have it.
         *
         * A word with no meaning now has a *different* message depending on why,
         * because the two cases need different user action:
         *   - a cloud word with no content means the token's scope does not allow
         *     reading 释义/助记/例句, so the hint points at the probe (the console
         *     button that reports exactly which endpoints the token may use);
         *   - anything else means the word genuinely has no entry yet.
         * Showing a bare "（无释义）" for both is what made the earlier behaviour
         * look like a bug rather than a scope limit.
         */
        const char *meaning = s_word.meaning[0] ? s_word.meaning : NULL;
        const char *example = s_word.example[0] ? s_word.example : NULL;

        if (meaning == NULL) {
            const bool cloud_card = (s_word.id == WORD_ID_CLOUD) || word_cloud_mode();
            lv_label_set_text_fmt(s_card_label, "%s\n\n%s", s_word.spelling,
                                  cloud_card ? "（墨墨未返回释义：可在网页上位机点「探测令牌能力」查看原因）"
                                             : "（无释义）");
            lv_label_set_text(s_hint_label, "选择熟悉度，进入下一张");
            if (s_grade_bar != NULL) {
                lv_obj_remove_flag(s_grade_bar, LV_OBJ_FLAG_HIDDEN);
            }
            render_enrich();
            return;
        }

        if (example != NULL) {
            lv_label_set_text_fmt(s_card_label, "%s\n\n%s\n\n%s", s_word.spelling, meaning,
                                  example);
        } else {
            lv_label_set_text_fmt(s_card_label, "%s\n\n%s", s_word.spelling, meaning);
        }
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
        /*
         * Say where the queue came from. The user's report was that the word
         * page "only has words from the local book" - so the source is now
         * visible rather than something to infer: 墨墨 means the daily plan is
         * driving the cards, 本地 means the offline book is.
         */
        const bool cloud_queue = word_cloud_mode() && word_cloud_count() > 0;
        lv_label_set_text_fmt(s_progress_label, "%s %u/%u · 等级 %u",
                              cloud_queue ? "墨墨今日" : "今日队列",
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

    /*
     * Uplink (v1.4.0): every graded word is queued so the words studied on the
     * board go back into the account's plan in one batched call, instead of the
     * "not recognised" words being the only feedback the account ever gets.
     */
    uplink_add(s_word.spelling);

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
 * Leaving the page: send whatever is still queued for MaiMemo.
 *
 * Flushing here (rather than per card) is what keeps the request count low: a
 * ten-card session produces one call, not ten. The worker copies the queue
 * first, so navigating away does not cancel the in-flight upload.
 */
static esp_err_t word_on_hide(void)
{
    uplink_flush();
    return ESP_OK;
}

/**
 * A MaiMemo sync finished.
 *
 * Runs on the LVGL task (ui_event_subscribe marshals there), so it must stay
 * cheap - which it now is: word_service owns the queue build (see below).
 */
/*
 * These use ui_event_subscribe() rather than ui_event_bind(): see the forward
 * declaration above for why.
 */
static esp_err_t maimemo_sync_done_event(int32_t event_id, void *data)
{
    (void)event_id;
    (void)data;
    /*
     * The queue itself is built by word_service, which subscribes to this same
     * event (v1.4.0). It used to be built *here*, which meant the queue only
     * existed if the user happened to have the word page open when the sync
     * finished - syncing from the web console left the device on the local book.
     * All this handler needs to do now is repaint the numbers that changed.
     */
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
    .on_hide = word_on_hide,
    .on_event = word_on_event,
};
