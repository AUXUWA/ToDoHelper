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
 *
 * ## Failure handling and retries (v1.7.0)
 *
 * Failures are classified before anything is retried:
 *
 *   - **Authentication** (HTTP 401/403, or an `unauthorized`/`token` code in the
 *     envelope) is **never retried** - the same token would fail again. The
 *     component latches "the token was refused" and the automatic paths stop
 *     asking the cloud until the token is replaced (maimemo_set_token() clears
 *     the latch).
 *   - **Transport failures** (DNS, connect, timeout) are retried with an
 *     exponential backoff: 60 s, 120 s, 240 s, 480 s, capped at 15 minutes.
 *     While the backoff window is open the automatic paths do not touch the
 *     network at all and return the remembered error code, so the caller sees
 *     the same error class it would have seen from a real attempt.
 *   - Everything else (the API answered, even with an error envelope) is final.
 *
 * The explicit, user-triggered actions (the console's "verify" and "probe"
 * buttons) always make a live request - they exist to answer "is the network /
 * the token working?", so a cached answer would defeat their purpose.
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
    /**
     * 释义 (v1.4.0): `/interpretations` is a **separate** endpoint from
     * `/notes`, and on a study-scoped token it is often readable when the note
     * endpoints are not. It is the only way a word that is absent from the local
     * book can still be studied with a meaning on screen, so it has its own
     * field and its own permission outcome.
     */
    char meaning[192];
    bool from_cache;     /**< true when served without touching the network */
} maimemo_enrich_t;

/**
 * Outcome of one endpoint in maimemo_probe().
 *
 * The point of the probe is to turn "does my token allow X?" from a guess into
 * an observed fact: the same account token that syncs the daily queue may or may
 * not be allowed to read 释义/助记/例句, and the answer decides whether cloud
 * cards can show content. Reported per endpoint rather than as one verdict
 * because the scopes really are independent.
 */
typedef struct {
    const char *path;    /**< Endpoint path, for display */
    esp_err_t err;       /**< ESP_OK, ESP_ERR_NOT_SUPPORTED (scope), ... */
    int detail;          /**< Endpoint-specific count (words/notes/...), or -1 */
} maimemo_probe_result_t;

/** How many endpoints maimemo_probe() reports on. */
#define MAIMEMO_PROBE_MAX 6

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
 *
 * This is an explicit, user-triggered check, so it always makes a live request:
 * it is never short-circuited by the retry backoff.
 */
esp_err_t maimemo_verify_token(void);

/**
 * @brief Plan A: fetch today's words. Blocking; call from a worker task.
 *
 * Inside a transport-failure backoff window this returns the remembered error
 * code without touching the network (see "Failure handling and retries").
 *
 * @param out   Array of at least @p max entries.
 * @param max   Capacity of @p out (MAIMEMO_MAX_TODAY is a sensible value).
 * @param count Receives the number of entries written.
 */
esp_err_t maimemo_fetch_today(maimemo_item_t *out, size_t max, size_t *count);

/**
 * @brief Plan A: fetch account progress. Blocking.
 *
 * Inside a transport-failure backoff window this returns the remembered error
 * code without touching the network (see "Failure handling and retries").
 */
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
 * @brief Cache-only enrichment lookup: **never touches the network** (v1.4.0).
 *
 * Serves whatever a previous maimemo_enrich() call already cached, so a card
 * that has been viewed once keeps its meaning offline and without another round
 * trip. Safe to call from any task, including the LVGL one - it only reads the
 * in-PSRAM cache (loading it from the card once).
 *
 * Needed because filling every cloud word's content during a sync turned out to
 * be far too expensive on this board: 64 words x several TLS calls, each ending
 * in a whole-file cache rewrite, fragmented internal RAM down to 160 bytes and
 * wedged the web server. Content is therefore filled per card, when shown.
 *
 * @return ESP_OK when the cache had something, ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t maimemo_enrich_cached(const char *spelling, maimemo_enrich_t *out);

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
 *
 * @warning **Blocking TLS.** Call maimemo_push_unknown_async() from a UI event;
 *          this synchronous form is for worker/HTTP contexts that must know the
 *          outcome.
 */
esp_err_t maimemo_push_unknown(const char *spelling);

/**
 * @brief Plan C from a UI callback: queue the push on a worker task.
 *
 * Returns immediately without touching the network, so it is safe to call from
 * an LVGL event handler (AGENT.md 9.0.1.1 - those run with the adapter lock
 * held). The synchronous maimemo_push_unknown() was called there before, which
 * blocked rendering for the whole TLS handshake and, worse, allocated a task
 * while the LVGL lock was held.
 *
 * Silently does nothing when plan C is disabled or no token is configured, so a
 * missing optional feature never surfaces as a UI error.
 *
 * @param spelling Word to push; copied, so the caller may free it.
 */
esp_err_t maimemo_push_unknown_async(const char *spelling);

/** @brief Whether plan C is enabled in configuration. */
bool maimemo_push_enabled(void);

/** @brief Enable or disable plan C (persisted). */
esp_err_t maimemo_set_push_enabled(bool enabled);

/**
 * @brief Plan A, asynchronous: refresh today's queue on a worker task.
 *
 * Posts MAIMEMO_EVENT_SYNC_DONE / _FAILED when finished. Returns immediately, so
 * this is the one entry point that *is* safe to call from a UI event.
 *
 * The worker waits for an address before it touches the network (without one the
 * DNS lookup cannot even start), and a transport failure is retried with a
 * backoff (60 s, then 120 s, at most three attempts in total); an authentication
 * failure is not retried at all. The backoff does **not** block the caller: the
 * worker exits during the wait and a timer re-launches it, so ESP_ERR_INVALID_STATE
 * ("a sync is already running") only means that a request is on the wire right
 * now.
 */
esp_err_t maimemo_sync_start(void);

/** @brief Last known progress, without touching the network. */
esp_err_t maimemo_get_cached_progress(maimemo_progress_t *out);

/**
 * @brief Let the next request go out immediately, ignoring the retry backoff.
 *
 * The backoff window only guards the **automatic** paths (the boot sync, the
 * cloud queue, the per-card enrichment). An explicit, user-triggered action -
 * a button in the web console - must make a live request: the network has very
 * likely recovered inside the window (the boot sync fails before the IP
 * arrives, and the IP shows up seconds later).
 *
 * Only the transport backoff is affected. The "the token was refused" latch is
 * cleared by replacing the token (maimemo_set_token()), because that is the
 * configuration change the latch waits for.
 */
void maimemo_allow_request_now(void);

/**
 * @brief Plan D: fetch the 释义 for a spelling (`GET /interpretations`).
 *
 * ## Why this exists (v1.4.0)
 *
 * `/study/get_today_items` returns only `voc_id` + `voc_spelling` - no meaning.
 * `GET /vocabulary?spelling=` returns only `{id, spelling}` too, so it can
 * resolve an id but cannot supply content. The local CET-6 book covers a share
 * of the account's words and nothing more, so a cloud word outside the book had
 * no meaning at all on the card.
 *
 * `GET /interpretations?voc_id=` is the endpoint that actually carries 释义
 * (`{"data":{"interpretations":[{"interpretation":"...","tags":[...]}]}}`),
 * verified against the official CLI (`memo-api interp list -v <voc_id>`).
 *
 * Like the note/phrase readers this is permission-gated, and a study-scoped
 * token may be refused; that maps to ESP_ERR_NOT_SUPPORTED and is cached by
 * maimemo_enrich() so the question is asked at most once per word.
 *
 * @param spelling Word to look up.
 * @param out      Receives the meaning; empty when unavailable.
 */
esp_err_t maimemo_fetch_meaning(const char *spelling, char *out, size_t len);

/**
 * @brief Plan D: mark words as due now (`POST /study/advance_study`).
 *
 * ## Why this exists (v1.4.0) - and what it cannot do
 *
 * **MaiMemo's Open API has no endpoint that submits a review grade.** The
 * `study` surface is exactly `get_study_progress`, `get_today_items`,
 * `query_study_records`, `add_words` and `advance_study` (verified against the
 * official CLI's `study` command and its `study.proto`-derived types). There is
 * therefore no way for the device to report "the user recognised this word" -
 * the account's own "今日 N/M" counter will not move because of anything graded
 * on this board. Claiming otherwise would be a lie in the UI.
 *
 * `advance_study` is the closest legitimate write: it makes the given words due
 * for review now, so words studied on the device reappear in the account's plan
 * (and in the phone app's queue) instead of being forgotten by the cloud. That
 * is what "uploading the device's study state" can honestly mean.
 *
 * @param voc_ids Array of MaiMemo word ids (not spellings).
 * @param count   Number of ids; the API accepts up to 1000.
 * @param advanced Optional; receives the server's own `advanced_count`.
 */
esp_err_t maimemo_advance_words(const char **voc_ids, size_t count, int *advanced);

/**
 * @brief Plan D, by spelling: advance words the device has already studied.
 *
 * Resolves spellings to ids first (words MaiMemo does not know are skipped) and
 * then calls maimemo_advance_words(). Honours the same opt-in switch as the
 * push path, because it also mutates the user's cloud plan.
 *
 * @warning **Blocking TLS.** Worker/HTTP context only.
 */
esp_err_t maimemo_advance_spellings(const char **spellings, size_t count, int *advanced);

/**
 * @brief Probe which endpoints the configured token may use. Blocking.
 *
 * Fills @p out with one entry per endpoint (up to @p max, MAIMEMO_PROBE_MAX is
 * the intended size) and returns how many were written via @p count.
 *
 * Every endpoint is attempted even after a failure, because the scopes are
 * independent: the observed real-world case is a token that reads the study
 * endpoints and is refused on `/notes` and `/phrases`. This is the one call
 * that answers "what can this token actually do?" without hand-written curl
 * commands.
 */
esp_err_t maimemo_probe(maimemo_probe_result_t *out, size_t max, size_t *count);

/**
 * @brief Whether advancing studied words back to the plan is enabled.
 *
 * Shares `SYS_KEY_MAIMEMO_PUSH` with the add-words path: both mutate account
 * data, so one opt-in covers both.
 */
bool maimemo_advance_enabled(void);

/** @brief Register the /api/v1/maimemo/... routes. */
esp_err_t maimemo_register_http(void);

/** @brief Initialise config, cache and the sync worker. */
esp_err_t maimemo_client_init(void);

#ifdef __cplusplus
}
#endif
