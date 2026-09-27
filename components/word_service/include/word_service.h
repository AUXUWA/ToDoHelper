/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file word_service.h
 * @brief FR5: flashcards, three-way grading and spaced review.
 *
 * The wordbook lives on storage (`<app_dir>/words/<book>.json`); review
 * progress lives in `<app_dir>/words/progress.json`. A small starter book is
 * compiled in so the feature works before the user supplies a real one
 * (requirement 17.2 still lists "词库来源" as open).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#include "sys_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(WORD_EVENT);

typedef enum {
    WORD_EVENT_LOADED = 0,     /**< payload: size_t word count */
    WORD_EVENT_REVIEWED,       /**< payload: uint32_t word id */
    WORD_EVENT_PROGRESS_SAVED, /**< no payload */
    WORD_EVENT_LOAD_FAILED,    /**< payload: esp_err_t */
} word_event_id_t;

typedef enum {
    WORD_REVIEW_UNKNOWN = 0,
    WORD_REVIEW_FUZZY,
    WORD_REVIEW_KNOWN,
} word_review_result_t;

/** Text limits, mirroring docs/TODOHELPER_REQUIREMENTS.md 7.6. */
#define WORD_SPELLING_MAX 64
#define WORD_PHONETIC_MAX 64
#define WORD_MEANING_MAX  256
#define WORD_EXAMPLE_MAX  256

/** Upper bound on the loaded book, so a bad file cannot exhaust memory. */
#define WORD_MAX_ITEMS 2000

/**
 * Sentinel id for a card that came from the MaiMemo study plan (plan A) but has
 * no matching entry in the local book.
 *
 * Such a word can still be shown and graded, but there is no local scheduling
 * record for it: its progress lives in the account, so grading routes to the
 * cloud push path instead of the local spaced-repetition state.
 */
#define WORD_ID_CLOUD 0xFFFFFFFFu

typedef struct {
    uint32_t id;
    char spelling[WORD_SPELLING_MAX];
    char phonetic[WORD_PHONETIC_MAX];
    char meaning[WORD_MEANING_MAX];
    char example[WORD_EXAMPLE_MAX];
    uint8_t level;            /**< 0 (new) .. 5 (well known) */
    int64_t next_review_ts;   /**< Unix seconds, or seconds-since-boot before NTP */
} word_item_t;

typedef struct {
    size_t total;
    size_t new_words;       /**< never reviewed */
    size_t due;             /**< scheduled for review now */
    size_t learned;         /**< level >= 3 */
    uint32_t reviewed_total;
    uint32_t known_total;
    uint32_t fuzzy_total;
    uint32_t unknown_total;
} word_stats_t;

/** Default book name (without extension).
 *
 * CET-6 per user request. The real book is a JSON file on the SD card at
 * <app_dir>/words/cet6.json; the compiled-in 32-word starter is only the fallback
 * used when that file cannot be read. */
#define WORD_DEFAULT_BOOK "cet6"
/** Default number of words offered per day. */
#define WORD_DEFAULT_DAILY_COUNT 10

/**
 * @brief Load the wordbook and progress.
 *
 * Order: `<app_dir>/words/<book>.json` -> written-out template of the compiled
 * in starter book. Progress is always read from `words/progress.json`.
 */
esp_err_t word_service_init(void);

/**
 * @brief Replace the active book from an explicit storage path.
 *
 * @param path Absolute path, or relative to the application directory.
 */
esp_err_t word_load_from_sd(const char *path);

/** @brief Re-load the book selected through NVS. */
esp_err_t word_reload(void);

/** @brief Name of the active book. */
const char *word_get_book(void);

/** @brief Select and persist a book name, then reload. */
esp_err_t word_set_book(const char *book);

/* ---- MaiMemo cloud queue (plan A) ---------------------------------------- */

/**
 * @brief Is today's queue sourced from the MaiMemo study plan?
 *
 * True only when cloud mode is selected **and** a token is configured, so a
 * missing token silently keeps the offline book working.
 */
bool word_cloud_mode(void);

/** @brief Switch between the cloud plan and the local book (persisted). */
esp_err_t word_set_cloud_mode(bool enabled);

/**
 * @brief Fetch the account's today-list into the queue. Blocking.
 *
 * On failure the cloud queue is dropped and the local book takes over, so the
 * user never ends up with an empty page because the network was down.
 */
esp_err_t word_cloud_refresh(void);

/** @brief Number of cloud words currently loaded. */
size_t word_cloud_count(void);

/**
 * @brief Today's queue: due words first, then new words, capped by the daily count.
 *
 * @param out Array of at least @p max entries (NULL to only query the count).
 */
esp_err_t word_get_today(word_item_t *out, size_t max, size_t *count);

/**
 * @brief The next card in today's queue, advancing an internal cursor.
 */
esp_err_t word_next(word_item_t *out);

/** @brief Look one word up by id (includes its current level). */
esp_err_t word_get(uint32_t id, word_item_t *out);

/** @brief Rewind the cursor, so word_next() restarts from the first card. */
esp_err_t word_restart_queue(void);

/** @brief Position of the cursor inside today's queue (1-based) and its size. */
esp_err_t word_queue_position(size_t *position, size_t *total);

/**
 * @brief Record a grade and reschedule the word.
 */
esp_err_t word_review(uint32_t id, word_review_result_t result);

/**
 * @brief Persist progress to `words/progress.json`.
 *
 * @warning **Never call this from the LVGL task.** It serialises every reviewed
 *          word and rewrites the whole file on the SD card, and LVGL callbacks
 *          run with the adapter lock held (AGENT.md 9.0.1), so calling it from a
 *          button handler stalls rendering for the duration of a FAT write.
 *          word_review(), word_set_daily_count() and word_reset_progress() all
 *          use word_save_progress_async() internally for this reason; call this
 *          only from a worker task or when the result must be known immediately.
 */
esp_err_t word_save_progress(void);

/**
 * @brief Mark progress dirty and write it from a worker task (never blocks).
 *
 * Requests are coalesced inside a short debounce window, so a burst of grades or
 * taps produces a single write. This is the variant every UI path must use.
 */
esp_err_t word_save_progress_async(void);

/** @brief Aggregate counters for the UI and the HTTP API. */
esp_err_t word_stats(word_stats_t *out);

/** @brief Words offered per day. */
uint32_t word_get_daily_count(void);

/** @brief Change the daily target (persisted), clamped to 1..100. */
esp_err_t word_set_daily_count(uint32_t count);

/** @brief Drop all progress for the active book. */
esp_err_t word_reset_progress(void);

/** @brief Machine readable name of a grade, used by the HTTP API. */
const char *word_review_name(word_review_result_t result);

/** @brief Parse a grade name; returns false when unknown. */
bool word_review_from_name(const char *name, word_review_result_t *out);

/** @brief Publish the /api/v1/words/... routes (requirement 8.4). */
esp_err_t word_service_register_http(void);

#ifdef __cplusplus
}
#endif
