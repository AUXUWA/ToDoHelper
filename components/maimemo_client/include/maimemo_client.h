/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file maimemo_client.h
 * @brief 墨墨背单词 (MaiMemo) OpenAPI client — plans A / B / C.
 *
 * Three capabilities, matching the three plans the user chose:
 *
 *   A. **Study sync** — today's words and the real review progress from
 *      `/study/get_today_items`, `/study/get_study_progress` and
 *      `/study/query_study_records`, so the flashcard queue mirrors the account
 *      instead of a local book.
 *   B. **Enrichment prefetch** — mnemonics (`/notes`) and example sentences
 *      (`/phrases`) cached to the SD card so cards still show them offline.
 *   C. **Reverse push** — words graded "unknown" are added to the MaiMemo study
 *      plan with `/study/add_words` so the desktop account learns from the
 *      device.
 *
 * ## Token and permissions
 *
 * The token comes from NVS (`SYS_KEY_MAIMEMO_TOKEN`) and is set from the web
 * console; it is never logged or echoed back (requirement 8.8). **The token's
 * scope decides which plans can work**, and a study-scoped token is the common
 * case: it authorises the `/study/` endpoints and `/vocabulary` but returns
 * `common_permission_denied` for `/notes` and `/phrases`. That is not treated as
 * an error — see maimemo_enrich() — because a device that syncs the queue but
 * shows no mnemonic is still useful, whereas one that reports failure is not.
 *
 * ## Threading
 *
 * Every function here performs blocking TLS I/O. **None of them may be called
 * from the LVGL task** (AGENT.md 9.0.1: LVGL callbacks run with the adapter lock
 * held). The intended use is the worker task started by maimemo_sync_start().
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(MAIMEMO_EVENT);

typedef enum {
    MAIMEMO_EVENT_SYNC_STARTED = 0,  /**< no payload */
    MAIMEMO_EVENT_SYNC_DONE,         /**< payload: maimemo_progress_t */
    MAIMEMO_EVENT_SYNC_FAILED,       /**< payload: esp_err_t */
    MAIMEMO_EVENT_PUSH_DONE,         /**< payload: int (words accepted) */
} maimemo_event_id_t;

/** Longest spelling accepted back from the cloud (`voc_spelling`). */
#define MAIMEMO_SPELLING_MAX 64
/** Longest voc_id accepted (they are URL-safe base64-ish, ~50 bytes today). */
#define MAIMEMO_VOC_ID_MAX   80
/** Today's queue is capped here; the panel shows one card at a time anyway. */
#define MAIMEMO_MAX_TODAY    64

/** One word from the MaiMemo study plan, as returned by the study API. */
typedef struct {
    char voc_id[MAIMEMO_VOC_ID_MAX];
    char spelling[MAIMEMO_SPELLING_MAX];
    bool is_new;        /**< never studied before */
    bool is_finished;   /**< already done today */
    /**
     * `first_response`: "FORGET" | "FAMILIAR" | "STUDY_RESPONSE_UNSPECIFIED".
     *
     * Sized for the longest value the API actually returns: the "unspecified"
     * enum name is 25 characters plus the terminator. A 24-byte buffer silently
     * truncated it to "STUDY_RESPONSE_UNSPECIF" on the device - verified against
     * the live API, which is the only way this showed up.
     */
    char first_response[32];
} maimemo_item_t;

/** Account-level study progress, i.e. the numbers a real user recognises. */
typedef struct {
    int finished;       /**< words finished today */
    int total;          /**< today's target */
    int study_time_ms;  /**< time studied today */
    int due_count;      /**< words in the plan (query_study_records count) */
} maimemo_progress_t;

/** Enrichment for one word (plan B), cached on the SD card. */
typedef struct {
    char mnemonic[256];  /**< 助记: the first note, concatenated if short */
    char example[256];   /**< 例句: the first phrase */
    bool from_cache;     /**< true when served without touching the network */
} maimemo_enrich_t;

/**
 * @brief Is a token configured?
 *
 * Cheap: reads NVS only. Safe to call from the UI task, unlike everything else
 * in this header.
 */
bool maimemo_has_token(void);

/** @brief Store the token (NULL or "" clears it). Never logged. */
esp_err_t maimemo_set_token(const char *token);

/**
 * @brief Verify the token against the live API.
 *
 * Performs one lightweight `/study/get_study_progress` call, which doubles as
 * the connectivity check for the web console's "测试" button.
 */
esp_err_t maimemo_verify_token(void);

/**
 * @brief Plan A: fetch today's words. Blocking; call from a worker task.
 *
 * @param out   Array of at least @p max entries.
 * @param max   Capacity of @p out (MAIMEMO_MAX_TODAY is a sensible value).
 * @param count Receives the number of entries written.
 */
esp_err_t maimemo_fetch_today(maimemo_item_t *out, size_t max, size_t *count);

/** @brief Plan A: fetch account progress. Blocking. */
esp_err_t maimemo_fetch_progress(maimemo_progress_t *out);

/**
 * @brief Plan B: get the mnemonic and example for a spelling.
 *
 * Serves from the SD-card cache when present, so the common path is offline and
 * fast. On a cache miss it resolves the spelling to a `voc_id` and asks
 * `/notes` and `/phrases`.
 *
 * A `common_permission_denied` response (study-scoped token) is **not** a hard
 * failure: the result is cached as "unavailable" and ESP_ERR_NOT_SUPPORTED is
 * returned, so callers can distinguish "no data, do not retry" from a transient
 * network error that is worth retrying later.
 *
 * @param spelling Word to look up.
 * @param out      Receives the enrichment; zeroed when unavailable.
 */
esp_err_t maimemo_enrich(const char *spelling, maimemo_enrich_t *out);

/**
 * @brief Plan C: add words to the MaiMemo study plan.
 *
 * @param spellings Array of spellings.
 * @param count     Number of entries; the API accepts up to 1000 per call.
 * @param advance   true also advances them for immediate review
 *                  (`advance: true`, which lowers the level gate).
 * @param accepted  Optional; receives how many the server accepted.
 */
esp_err_t maimemo_push_words(const char **spellings, size_t count, bool advance,
                             int *accepted);

/**
 * @brief Plan C, single word: send one word the user graded "unknown".
 *
 * Honours `SYS_KEY_MAIMEMO_PUSH`: when the user has not opted in this returns
 * ESP_ERR_NOT_SUPPORTED without touching the network, because pushing mutates
 * the user's cloud study plan.
 */
esp_err_t maimemo_push_unknown(const char *spelling);

/** @brief Whether plan C is enabled in configuration. */
bool maimemo_push_enabled(void);

/** @brief Enable or disable plan C (persisted). */
esp_err_t maimemo_set_push_enabled(bool enabled);

/**
 * @brief Plan A, asynchronous: refresh today's queue on a worker task.
 *
 * Posts MAIMEMO_EVENT_SYNC_DONE / _FAILED when finished. Returns immediately, so
 * this is the one entry point that *is* safe to call from a UI event.
 */
esp_err_t maimemo_sync_start(void);

/** @brief Last known progress, without touching the network. */
esp_err_t maimemo_get_cached_progress(maimemo_progress_t *out);

/** @brief Register the /api/v1/maimemo/... routes. */
esp_err_t maimemo_register_http(void);

/** @brief Initialise config, cache and the sync worker. */
esp_err_t maimemo_client_init(void);

#ifdef __cplusplus
}
#endif
