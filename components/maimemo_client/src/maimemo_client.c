/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file maimemo_client.c
 * @brief MaiMemo OpenAPI client (plans A/B/C). See maimemo_client.h.
 *
 * ## Why this is hand-rolled rather than a generic HTTP helper
 *
 * The API is small and uniform: every call is a POST with a JSON body and a
 * bearer token, and every response is `{"success":bool,"data":{...},
 * "errors":[...]}`. One `mm_post()` plus a per-endpoint parser is less code than
 * a generic layer, and keeps the response-size limits explicit - which matters,
 * because this board has ~310 KB of internal RAM and a word book that once
 * drained it (AGENT.md 9.0.5).
 *
 * ## Error model
 *
 * The API returns HTTP 200 even for permission errors, with `success:false` and
 * a `common_permission_denied` code. So the status code alone is not enough:
 * every call inspects the envelope. `ESP_ERR_NOT_SUPPORTED` is used deliberately
 * for permission errors so callers can tell "this token may not do that, stop
 * asking" apart from "the network hiccuped, try again later".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
/* 只为了 ESP_ERR_ESP_TLS_* 这一段错误码（DNS/连接/握手失败），
 * 用来把「传输层失败」和「服务端给了结论」分开。 */
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "maimemo_client.h"
#include "storage_manager.h"
#include "sys_keys.h"
#include "sys_net.h"
#include "sys_utils.h"

static const char *TAG = "maimemo";

#define MM_BASE           "https://open.maimemo.com/open/api/v1"
#define MM_TIMEOUT_MS     15000
/** Response cap. `/study/get_today_items` for 64 words is ~8 KB; 24 KB is ample
 *  and bounded, which is what matters on this board. */
#define MM_MAX_RESPONSE   (24 * 1024)
/**
 * Cap on records requested by the study-records fallback.
 *
 * A study record is ~350 bytes of JSON, so this keeps the response comfortably
 * inside MM_MAX_RESPONSE. Requesting more would overflow the body buffer and
 * turn "long plan" into a hard failure (see mm_fetch_today_from_records).
 */
#define MM_RECORDS_MAX    56
/** Cap on the enrichment cache read from disk (see enrich_cache_load()). */
#define MM_MAX_CACHE_FILE (256 * 1024)
#define MM_TOKEN_MAX      192

ESP_EVENT_DEFINE_BASE(MAIMEMO_EVENT);

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
/**
 * Sync workers that are **on the wire** (waiting for the request lock, or doing
 * the request). Only these make a sync "busy".
 *
 * The old rule was "a sync task exists -> busy". That made the console's
 * "sync now" button answer 400 "a sync is already running" for as long as the
 * worker sat in its backoff sleep (minutes). The slot now belongs to the
 * **request**, not to the task: waiting for an address or sleeping between
 * attempts does not block the console.
 */
static int s_sync_on_wire;
/** Serialises the sync requests: at most one TLS round trip at a time. */
static SemaphoreHandle_t s_sync_req_lock;
/** One-shot timer that re-launches the sync after a backoff. */
static esp_timer_handle_t s_sync_retry_timer;
/** How many requests this round has already made. */
static uint32_t s_sync_attempts;
static bool s_initialized;

/*
 * wifi_manager's event base.
 *
 * maimemo_client deliberately does **not** depend on the wifi_manager
 * component (the same reasoning as word_service, see
 * components/word_service/src/word_service.c): an L3 -> L3 edge is what the
 * project avoids. The single symbol that is needed is declared here -
 * `ESP_EVENT_DEFINE_BASE(WIFI_MGR_EVENT)` in wifi_manager.c defines it as a
 * global `const char *` whose value is "WIFI_MGR_EVENT".
 *
 * A wifi_manager that is not linked in is a **link error**, never a silent
 * misbehaviour - which is the failure mode we want.
 */
extern esp_event_base_t const WIFI_MGR_EVENT;

/**
 * Whether the station had an address at the last WiFi event.
 *
 * The trigger for the automatic sync is the **transition**: the address
 * appeared. A scan (which also posts a WIFI_MGR event) never starts a
 * request, and a disconnect clears the flag again.
 */
static bool s_had_ip;

/**
 * Cached "a token is configured" flag.
 *
 * The WiFi event callback runs on the shared event task and must not touch the
 * flash (AGENT.md 9.0.1.1 iron rule 1) - and maimemo_has_token() reads NVS.
 * The cache is refreshed when the token is stored or cleared.
 */
static bool s_token_cached;

/**
 * Cached "the user wants the cloud book" flag (same reason as above).
 *
 * The automatic sync exists for the cloud book (plan A): with the local book
 * there is nothing to pull, so the request would be pure waste. This is the
 * same setting word_service reads (SYS_KEY_MAIMEMO_MODE), read once at init.
 */
static bool s_cloud_mode_cached;

/** Last successful progress, kept so the UI can paint without the network. */
static maimemo_progress_t s_progress;
static bool s_have_progress;

/** Enrichment cache, held in PSRAM: (spelling -> mnemonic/meaning/example). */
typedef struct {
    char spelling[MAIMEMO_SPELLING_MAX];
    char mnemonic[256];
    char example[256];
    /** 释义 from `/interpretations` (v1.4.0). Separate from the note/phrase
     *  pair because it is a different endpoint with a different scope, and it is
     *  the field that lets a word absent from the local book still be studied. */
    char meaning[192];
    /** true when the word was asked for and the API declined (permission).
     *  Cached so a study-scoped token does not re-ask on every card. */
    bool unavailable;
    /** true when `/interpretations` specifically was refused, tracked apart from
     *  the note/phrase pair (whose refusal sets `unavailable`). */
    bool meaning_unavailable;
    /** true when the last enrichment attempt was conclusive: every endpoint
     *  answered (even with "no data" or "not in scope"), so asking again
     *  cannot produce anything new.
     *
     *  This is what makes the cache usable: without it, a word the API simply
     *  has nothing for would be fetched again on every card view. */
    bool fetched;
    /** true when the last `/interpretations` attempt died on the transport
     *  (timeout, connection reset) rather than being answered. Only then is a
     *  retry worth another round trip. */
    bool meaning_retryable;
} mm_cache_entry_t;

static mm_cache_entry_t *s_cache;
static size_t s_cache_count;
static bool s_cache_loaded;

/* ------------------------------------------------------------------------ */
/* 失败分类与退避（roadmap v1.7.0 C2）                                       */
/* ------------------------------------------------------------------------ */

/**
 * 一次网络失败属于哪一类 —— 决定「要不要再试、隔多久再试」。
 *
 * ## 为什么必须分类
 *
 * 墨墨的错误模型很特别：**权限错误也返回 HTTP 200**，错误写在信封里
 * （见 mm_classify）；而 **HTTP 401 在 IDF 里会让
 * `esp_http_client_perform()` 直接失败** —— 401 没有 WWW-Authenticate 头时，
 * `esp_http_client_add_auth()` 打一句
 * "This request requires authentication, but does not provide header
 * information for that" 就返回 ESP_ERR_NOT_SUPPORTED
 * （esp_http_client.c:2126），于是 perform() 以失败告终，
 * **状态码已经是 401，但函数返回的是「没拿到应答」**。
 *
 * 于是同一个「令牌无效」有两种完全不同的长相：
 *   - 401 + 没有 WWW-Authenticate → perform() 失败（看起来像网络问题）；
 *   - 200 + `errors[].code` 含 unauthorized/token → 走信封分类。
 * 只看其中一处就会把「重试也不会成功」当成「网络抖了一下」，
 * 然后一遍遍地重试 —— 这正是 C2 要治的毛病。
 */
typedef enum {
    MM_FAIL_NONE = 0,     /**< 成功 */
    MM_FAIL_AUTH,         /**< 401/403/令牌被拒：重试也不会成功 */
    MM_FAIL_TRANSPORT,    /**< DNS/连接/超时：退避之后值得再试 */
    MM_FAIL_FINAL,        /**< 服务端已经给出结论（错误信封等）：不用再试 */
} mm_fail_t;

/** 连续传输层失败轮数；成功一次就清零（指数退避的指数）。 */
static uint32_t s_fail_streak;
/** 退避窗口的到期时刻（tick）。窗口内不再发自动请求。 */
static TickType_t s_backoff_until;
/** 上一次传输层失败的时刻（tick）：用来判断「这是不是同一轮失败」。 */
static TickType_t s_last_fail_tick;
/** 上一次失败的错误码：窗口内原样返回，调用方看到的错误分类不变。 */
static esp_err_t s_last_err;
/** 上一次失败的性质（存成整数，原子读写更稳）。 */
static uint32_t s_last_fail;
/**
 * 令牌被服务端拒绝（401/403）后置位。
 *
 * 置位期间**自动路径不再发请求**：重试同一个被拒的令牌不会成功，
 * 要等用户改配置（换令牌）才有意义。网页控制台里显式点的按钮仍然是
 * 一次真实请求 —— 那是用户的诊断动作，不是自动重试。
 */
static bool s_token_rejected;

/** 退避窗口的起始间隔：第一次传输层失败后等 1 分钟。 */
#define MM_BACKOFF_BASE_MS   (60 * 1000)
/** 退避上限：15 分钟。 */
#define MM_BACKOFF_MAX_MS    (15 * 60 * 1000)
/** 开机同步最多试几次（含第一次）。 */
#define MM_SYNC_MAX_ATTEMPTS 3
/**
 * 同步 worker 在发请求前最多等多久才放弃等地址。
 *
 * 真机日志：开机 6546 ms 发起同步，7032 ms 才拿到 IP —— 没有地址时
 * DNS 根本解析不出来（「getaddrinfo() returns 202」），那一次尝试是**注定
 * 失败**的。所以先等地址（roadmap v1.7.0 C1）。
 * 30 秒足够覆盖慢一点的 DHCP；到点还没地址就照常发一次（失败也比不发强）。
 */
#define MM_SYNC_IP_WAIT_MS 30000
/** 等地址时的轮询周期。 */
#define MM_SYNC_IP_POLL_MS 500
/**
 * 拿到地址之后还要等多久，网络才算真的能用。
 *
 * DHCP 把 DNS 服务器地址、路由和 lwIP 的解析器跟地址一起送到，
 * 比 IP 事件晚一点到。这两秒是在真机日志上量出来的：地址到了就立刻发请求，
 * 十有八九是 "couldn't get hostname"（DNS 还没就绪），然后白等一个
 * 60 秒的退避窗口才有机会再试。
 */
#define MM_SYNC_NET_SETTLE_MS 2000
/** 一次同步最多等「一次只允许一个请求」这把锁多久。 */
#define MM_SYNC_REQ_LOCK_MS 30000
/**
 * 拿到地址却没能起动 worker 时的重试间隔。
 *
 * 起不来只有一个原因：内部堆这一刻没有 6 KB 的连续空间（FreeRTOS 的任务栈
 * 只能来自内部 RAM）。几秒后再试一次就够了 —— 那时候 WiFi 已经把地址拿到，
 * 驱动也不再申请内存了。
 */
#define MM_SYNC_RETRY_SHORT_MS 5000

/**
 * 第 @p streak 轮失败之后要等多久。
 *
 * 指数退避 + 上限：60 s → 120 s → 240 s → 480 s → 900 s（封顶）。
 * 「分钟级」是刻意的 —— 每 15 秒重试一次 TLS 握手只会把电台和 CPU
 * 白白烧掉，而 DNS 失败到网络真正可用之间往往就是几十秒到几分钟。
 */
static uint32_t mm_backoff_delay_ms(uint32_t streak)
{
    if (streak == 0) {
        streak = 1;
    }
    uint32_t delay = MM_BACKOFF_BASE_MS;
    for (uint32_t i = 1; i < streak; i++) {
        if (delay >= MM_BACKOFF_MAX_MS) {
            break;
        }
        delay *= 2;
    }
    return (delay > MM_BACKOFF_MAX_MS) ? MM_BACKOFF_MAX_MS : delay;
}

/** 原子读：这些字段会被同步 worker、HTTP worker、UI worker 同时碰。 */
static mm_fail_t mm_state_last_fail(void)
{
    return (mm_fail_t)__atomic_load_n(&s_last_fail, __ATOMIC_ACQUIRE);
}

static esp_err_t mm_state_last_err(void)
{
    return (esp_err_t)__atomic_load_n(&s_last_err, __ATOMIC_ACQUIRE);
}

static uint32_t mm_state_streak(void)
{
    return __atomic_load_n(&s_fail_streak, __ATOMIC_ACQUIRE);
}

static bool mm_state_token_rejected(void)
{
    return __atomic_load_n(&s_token_rejected, __ATOMIC_ACQUIRE);
}

/**
 * 记下一次请求的结果，并把退避状态推进一格。
 *
 * @param kind 失败分类（MM_FAIL_NONE 表示成功）。
 * @param err  本次返回给调用方的错误码（原样存下来，窗口内照抄）。
 */
static void mm_record_outcome(mm_fail_t kind, esp_err_t err)
{
    __atomic_store_n(&s_last_fail, (uint32_t)kind, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_err, (int)err, __ATOMIC_RELEASE);

    if (kind == MM_FAIL_NONE) {
        /* 成功：退避计数与「令牌被拒」标记一起清掉。
         * 令牌可能是刚被换好的（改配置后第一次调用就会走到这里）。 */
        __atomic_store_n(&s_fail_streak, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_backoff_until, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_token_rejected, false, __ATOMIC_RELEASE);
        return;
    }
    if (kind == MM_FAIL_AUTH) {
        /* 令牌被拒：不再自动重试，等用户改配置。 */
        __atomic_store_n(&s_token_rejected, true, __ATOMIC_RELEASE);
        return;
    }
    if (kind != MM_FAIL_TRANSPORT) {
        return;   /* 服务端已给出结论：与退避无关 */
    }

    const TickType_t now = xTaskGetTickCount();
    uint32_t streak = mm_state_streak();
    /* 只有「隔了至少一个退避间隔」的失败才算新的一轮。
     * 否则一次 enrich 里的三个端点会连着失败，把退避直接顶到上限。 */
    if (streak == 0 ||
        (int32_t)(now - __atomic_load_n(&s_last_fail_tick, __ATOMIC_ACQUIRE)) >=
            (int32_t)pdMS_TO_TICKS(mm_backoff_delay_ms(streak))) {
        streak++;
    }
    __atomic_store_n(&s_fail_streak, streak, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_fail_tick, now, __ATOMIC_RELEASE);
    __atomic_store_n(&s_backoff_until,
                     (TickType_t)(now + pdMS_TO_TICKS(mm_backoff_delay_ms(streak))),
                     __ATOMIC_RELEASE);
}

/**
 * 现在处于传输层退避窗口内吗？
 *
 * 窗口内自动路径**不发请求**，直接返回上一次失败的错误码 ——
 * 调用方看到的错误分类与真发一次完全一样，只是省掉了整个 TLS 往返。
 */
static bool mm_backoff_active(void)
{
    if (mm_state_last_fail() != MM_FAIL_TRANSPORT) {
        return false;
    }
    const TickType_t until = __atomic_load_n(&s_backoff_until, __ATOMIC_ACQUIRE);
    if (until == 0) {
        return false;
    }
    /* tick 会回绕，用有符号差值比较。 */
    return (int32_t)(xTaskGetTickCount() - until) < 0;
}

void maimemo_allow_request_now(void)
{
    /*
     * 关掉退避闸门（保留失败计数）。
     *
     * 闸门只管**自动**路径：用户在网页控制台上点按钮是一次显式动作，
     * 它必须真的发一次请求 —— 网络很可能已经在窗口里恢复了
     * （开机时还没有 IP 的那一次失败，窗口要开 60 秒，
     *  而 IP 通常几秒后就到了）。
     * 下一次失败会照常把窗口重新打开。
     */
    __atomic_store_n(&s_backoff_until, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_fail, (uint32_t)MM_FAIL_NONE, __ATOMIC_RELEASE);
}

/** 这个错误码是不是「网络/传输层」的问题（值得退避后重试）。 */
static bool mm_err_is_transport(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_HTTP_CONNECT:            /**< 连不上（含 DNS 解析失败） */
    case ESP_ERR_HTTP_CONNECTING:
    case ESP_ERR_HTTP_WRITE_DATA:
    case ESP_ERR_HTTP_FETCH_HEADER:
    case ESP_ERR_HTTP_READ_TIMEOUT:
    case ESP_ERR_HTTP_EAGAIN:
    case ESP_ERR_HTTP_CONNECTION_CLOSED:
    case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME:
    case ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST:
    case ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT:
    case ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT:
    case ESP_ERR_ESP_TLS_TCP_CLOSED_FIN:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------ */
/* Token / config                                                            */
/* ------------------------------------------------------------------------ */

bool maimemo_has_token(void)
{
    char token[MM_TOKEN_MAX];
    if (sys_config_get_str(SYS_KEY_MAIMEMO_TOKEN, token, sizeof(token)) != ESP_OK) {
        return false;
    }
    return token[0] != '\0';
}

static bool mm_token(char *out, size_t len)
{
    if (sys_config_get_str(SYS_KEY_MAIMEMO_TOKEN, out, len) != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return out[0] != '\0';
}

/**
 * Refresh the cached configuration flags.
 *
 * Reads the flash, so it may only run in a task context - never from an event
 * callback (AGENT.md 9.0.1.1 iron rule 1).
 *
 * The token flag is what the automatic sync asks before it touches the
 * network; the mode flag says whether the automatic sync has anything to do
 * at all (the local book needs no plan from MaiMemo).
 */
static void mm_cache_config(void)
{
    char mode[16];
    s_cloud_mode_cached =
        (sys_config_get_str(SYS_KEY_MAIMEMO_MODE, mode, sizeof(mode)) == ESP_OK) &&
        (strcmp(mode, "cloud") == 0);
    s_token_cached = maimemo_has_token();
}

esp_err_t maimemo_set_token(const char *token)
{
    if (token == NULL || token[0] == '\0') {
        (void)sys_config_erase(SYS_KEY_MAIMEMO_TOKEN);
        s_token_cached = false;
        /* 令牌被换掉（或者清空）= 「用户改了配置」，退避与锁存一起清掉，
         * 下一次请求重新从头来过。 */
        mm_record_outcome(MM_FAIL_NONE, ESP_OK);
        SYS_LOGI(TAG, "token cleared");
        return ESP_OK;
    }
    /* Stored only: never logged, never returned by any API (requirement 8.8).
     * The length is logged because that is the one thing that helps diagnose a
     * truncated paste without revealing the secret. */
    const esp_err_t err = sys_config_set_str(SYS_KEY_MAIMEMO_TOKEN, token);
    if (err == ESP_OK) {
        s_token_cached = true;
        /* 换了令牌就等于「用户改了配置」：之前那次 401 不再作数，
         * 自动路径可以重新试一次（新令牌对不对，让这一次请求去回答）。 */
        mm_record_outcome(MM_FAIL_NONE, ESP_OK);
        SYS_LOGI(TAG, "token stored (%u chars)", (unsigned)strlen(token));
    }
    return err;
}

bool maimemo_push_enabled(void)
{
    char value[8];
    if (sys_config_get_str(SYS_KEY_MAIMEMO_PUSH, value, sizeof(value)) != ESP_OK) {
        return false;   /* plan C mutates cloud data: opt-in only */
    }
    return value[0] == '1';
}

esp_err_t maimemo_set_push_enabled(bool enabled)
{
    return sys_config_set_str(SYS_KEY_MAIMEMO_PUSH, enabled ? "1" : "0");
}

esp_err_t maimemo_get_cached_progress(maimemo_progress_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    const bool have = s_have_progress;
    if (have) {
        *out = s_progress;
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    return have ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------------ */
/* HTTP plumbing                                                             */
/* ------------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t len;
    bool overflow;
} mm_body_t;

static esp_err_t mm_http_event(esp_http_client_event_t *evt)
{
    mm_body_t *body = (mm_body_t *)evt->user_data;
    if (body == NULL || evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) {
        return ESP_OK;
    }
    /* Bodies are large here (a 64-word today list), so allocate lazily and
     * prefer PSRAM: this is bulk data, not a hot buffer (AGENT.md 9.0.5). */
    if (body->buf == NULL) {
        body->buf = heap_caps_malloc_prefer(MM_MAX_RESPONSE, 2,
                                            MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
        if (body->buf == NULL) {
            body->overflow = true;
            return ESP_OK;
        }
        body->buf[0] = '\0';
    }
    if (body->overflow || body->len + (size_t)evt->data_len + 1 >= MM_MAX_RESPONSE) {
        body->overflow = true;
        return ESP_OK;
    }
    memcpy(body->buf + body->len, evt->data, (size_t)evt->data_len);
    body->len += (size_t)evt->data_len;
    body->buf[body->len] = '\0';
    return ESP_OK;
}

/** Envelope codes we care about, parsed from `errors[].code`. */
typedef enum {
    MM_ERR_NONE = 0,
    MM_ERR_PERMISSION,   /**< common_permission_denied: token scope too narrow */
    MM_ERR_AUTH,         /**< token missing/expired */
    MM_ERR_OTHER,
} mm_err_class_t;

/**
 * Inspect `{"success":..,"errors":[{"code":..}]}`.
 *
 * HTTP 200 with success:false is the normal shape of a MaiMemo permission or
 * validation error, so this is not an edge case - it is the main error path.
 *
 * @param out_err   Receives the esp_err_t to return to the caller.
 * @param out_class Optional; receives the raw class, which the failure
 *                  classification (C2) needs: MM_ERR_AUTH means "the token was
 *                  rejected", which must never be retried.
 */
static mm_err_class_t mm_classify(const cJSON *root, esp_err_t *out_err,
                                  mm_err_class_t *out_class)
{
    const cJSON *success = cJSON_GetObjectItemCaseSensitive(root, "success");
    if (cJSON_IsBool(success) && cJSON_IsTrue(success)) {
        *out_err = ESP_OK;
        if (out_class != NULL) {
            *out_class = MM_ERR_NONE;
        }
        return MM_ERR_NONE;
    }

    const cJSON *errors = cJSON_GetObjectItemCaseSensitive(root, "errors");
    const cJSON *first = cJSON_IsArray(errors) ? cJSON_GetArrayItem(errors, 0) : NULL;
    const cJSON *code = first ? cJSON_GetObjectItemCaseSensitive(first, "code") : NULL;
    const char *code_str = cJSON_IsString(code) ? code->valuestring : "";

    if (strstr(code_str, "permission_denied") != NULL) {
        /* The token authenticated but its scope does not include this endpoint.
         * Log at info, not error: with a study-scoped token this is expected and
         * the feature degrades by design. */
        SYS_LOGI(TAG, "endpoint not in token scope (%s)", code_str);
        *out_err = ESP_ERR_NOT_SUPPORTED;
        if (out_class != NULL) {
            *out_class = MM_ERR_PERMISSION;
        }
        return MM_ERR_PERMISSION;
    }
    if (strstr(code_str, "unauthorized") != NULL || strstr(code_str, "token") != NULL) {
        SYS_LOGW(TAG, "token rejected (%s)", code_str);
        *out_err = ESP_ERR_INVALID_STATE;
        if (out_class != NULL) {
            *out_class = MM_ERR_AUTH;
        }
        return MM_ERR_AUTH;
    }

    const cJSON *msg = first ? cJSON_GetObjectItemCaseSensitive(first, "msg") : NULL;
    SYS_LOGW(TAG, "api error: %s (%s)", cJSON_IsString(msg) ? msg->valuestring : "?",
             code_str);
    *out_err = ESP_ERR_INVALID_RESPONSE;
    if (out_class != NULL) {
        *out_class = MM_ERR_OTHER;
    }
    return MM_ERR_OTHER;
}

/**
 * POST @p path with @p body (may be NULL) and parse the reply into a cJSON tree.
 *
 * On success the caller owns *out_root. The returned esp_err_t already carries
 * the permission/auth distinction described in mm_classify().
 *
 * @param fail Optional; receives the failure classification (C2) so the caller
 *             can decide whether the call deserves another try.
 */
static esp_err_t mm_post_ex(const char *path, const char *body, cJSON **out_root,
                            mm_fail_t *fail)
{
    *out_root = NULL;
    if (fail != NULL) {
        *fail = MM_FAIL_NONE;
    }

    char token[MM_TOKEN_MAX];
    if (!mm_token(token, sizeof(token))) {
        return ESP_ERR_NOT_FOUND;   /* no token: caller degrades quietly */
    }

    char url[192];
    snprintf(url, sizeof(url), "%s/%s", MM_BASE, path);

    char auth[MM_TOKEN_MAX + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", token);

    mm_body_t resp = { 0 };
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = MM_TIMEOUT_MS,
        .event_handler = mm_http_event,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        mm_record_outcome(MM_FAIL_FINAL, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    const char *payload = (body != NULL) ? body : "{}";
    esp_http_client_set_post_field(client, payload, (int)strlen(payload));

    /*
     * One HTTPS at a time (see components/sys_utils/include/sys_net.h).
     *
     * A TLS handshake allocates a lot (mbedTLS sizes the session buffers from
     * MBEDTLS_SSL_IN_CONTENT_LEN / OUT_CONTENT_LEN, 16 KB each), and the
     * internal heap on this board is what the WiFi PHY needs a ~30 KB
     * contiguous block of. Two handshakes at once (the weather refresh and
     * this sync used to overlap right after the address arrived) made mbedTLS
     * fail with -0x008D = PSA_ERROR_INSUFFICIENT_MEMORY, and the request then
     * sat in a 60 s backoff window.
     *
     * The lock is taken around the request only - no SD writes, no LVGL calls.
     */
    const esp_err_t lock_err = sys_net_lock(SYS_NET_LOCK_DEFAULT_MS);
    if (lock_err != ESP_OK) {
        SYS_LOGW(TAG, "another request is holding the network lock");
        return lock_err;
    }

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    sys_net_unlock();

    if (err != ESP_OK) {
        SYS_LOGW(TAG, "POST %s failed: %s", path, esp_err_to_name(err));
        free(resp.buf);
        /*
         * 失败也可能是「服务端已经答复了 401」：IDF 的 add_auth() 在没有
         * WWW-Authenticate 头时让 perform() 以失败告终（状态码已经是 401）。
         * 所以先看状态码，再看传输层错误码 —— 顺序反了就会把
         * 「令牌无效」当成「网络不通」然后一遍遍重试。
         */
        mm_fail_t kind;
        if (status == 401 || status == 403) {
            kind = MM_FAIL_AUTH;
        } else if (mm_err_is_transport(err)) {
            kind = MM_FAIL_TRANSPORT;
        } else {
            kind = MM_FAIL_FINAL;
        }
        if (kind == MM_FAIL_AUTH) {
            SYS_LOGW(TAG, "POST %s -> HTTP %d: the token was refused; not retrying",
                     path, status);
        }
        mm_record_outcome(kind, err);
        if (fail != NULL) {
            *fail = kind;
        }
        return err;
    }
    if (status != 200) {
        /* Same reasoning as mm_get(): a 401/403 still carries the error envelope
         * that names the actual problem, so inspect it before giving up. */
        if (resp.buf == NULL || resp.overflow) {
            SYS_LOGW(TAG, "POST %s -> HTTP %d (no parsable body)", path, status);
            free(resp.buf);
            const esp_err_t mapped = (status == 401 || status == 403)
                                         ? ESP_ERR_INVALID_STATE
                                         : ESP_ERR_INVALID_RESPONSE;
            const mm_fail_t kind = (status == 401 || status == 403) ? MM_FAIL_AUTH
                                                                   : MM_FAIL_FINAL;
            mm_record_outcome(kind, mapped);
            if (fail != NULL) {
                *fail = kind;
            }
            return mapped;
        }
        SYS_LOGD(TAG, "POST %s -> HTTP %d, inspecting the error envelope", path, status);
    } else if (resp.buf == NULL || resp.overflow) {
        SYS_LOGW(TAG, "POST %s: response missing or over %d bytes", path, MM_MAX_RESPONSE);
        free(resp.buf);
        mm_record_outcome(MM_FAIL_FINAL, ESP_ERR_INVALID_SIZE);
        if (fail != NULL) {
            *fail = MM_FAIL_FINAL;
        }
        return ESP_ERR_INVALID_SIZE;
    }

    /* The reply is external input: parse it with a depth limit so a malformed
     * answer cannot blow the stack of the worker that asked for it (roadmap B3). */
    cJSON *root = sys_json_parse(resp.buf, 32);
    free(resp.buf);
    if (root == NULL) {
        SYS_LOGW(TAG, "POST %s: response is not JSON", path);
        mm_record_outcome(MM_FAIL_FINAL, ESP_ERR_INVALID_RESPONSE);
        if (fail != NULL) {
            *fail = MM_FAIL_FINAL;
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t api_err = ESP_OK;
    mm_err_class_t api_class = MM_ERR_NONE;
    (void)mm_classify(root, &api_err, &api_class);
    if (api_err != ESP_OK) {
        cJSON_Delete(root);
        /* 信封说「令牌被拒」→ 不重试；「作用域不够」也是终态（永久性的），
         * 但那不是令牌坏了，不许锁存。 */
        const mm_fail_t kind = (api_class == MM_ERR_AUTH) ? MM_FAIL_AUTH : MM_FAIL_FINAL;
        mm_record_outcome(kind, api_err);
        if (fail != NULL) {
            *fail = kind;
        }
        return api_err;
    }

    mm_record_outcome(MM_FAIL_NONE, ESP_OK);
    *out_root = root;
    return ESP_OK;
}

/** mm_post() 的对外语义原样保留（返回码与错误分类都不动）。 */
static esp_err_t mm_post(const char *path, const char *body, cJSON **out_root)
{
    return mm_post_ex(path, body, out_root, NULL);
}

/**
 * GET @p path with @p query and parse the reply.
 *
 * @p path must **not** start with '/': MM_BASE has no trailing slash and the
 * format string below supplies the separator, so a leading slash yields a double
 * slash (".../v1//vocabulary") that the API answers with HTTP 404. That is
 * exactly how the spelling lookup failed on the device.
 *
 * Needed because the note/phrase **read** endpoints are GETs: `POST /notes`
 * exists too, but it *creates* a note (body `{"note":{voc_id,note_type,note}}`),
 * so using POST to read returns the wrong thing entirely. Verified against the
 * official CLI (`memo-api-cli`), which lists notes as
 * `GET /api/v1/notes?voc_id=...`.
 *
 * @param fail Optional; receives the failure classification (C2).
 */
static esp_err_t mm_get_ex(const char *path, const char *query, cJSON **out_root,
                           mm_fail_t *fail)
{
    *out_root = NULL;
    if (fail != NULL) {
        *fail = MM_FAIL_NONE;
    }

    char token[MM_TOKEN_MAX];
    if (!mm_token(token, sizeof(token))) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Tolerate a caller that passes the leading slash anyway - the double slash
     * failure is silent (a 404 that reads like "word not found"), so making this
     * robust is cheaper than trusting every future call site to remember. */
    while (*path == '/') {
        path++;
    }

    char url[320];
    snprintf(url, sizeof(url), "%s/%s%s%s", MM_BASE, path,
             (query != NULL && query[0] != '\0') ? "?" : "",
             (query != NULL) ? query : "");

    char auth[MM_TOKEN_MAX + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", token);

    mm_body_t resp = { 0 };
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = MM_TIMEOUT_MS,
        .event_handler = mm_http_event,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        mm_record_outcome(MM_FAIL_FINAL, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Authorization", auth);

    /*
     * One HTTPS at a time (see components/sys_utils/include/sys_net.h).
     *
     * A TLS handshake allocates a lot (mbedTLS sizes the session buffers from
     * MBEDTLS_SSL_IN_CONTENT_LEN / OUT_CONTENT_LEN, 16 KB each), and the
     * internal heap on this board is what the WiFi PHY needs a ~30 KB
     * contiguous block of. Two handshakes at once (the weather refresh and
     * this sync used to overlap right after the address arrived) made mbedTLS
     * fail with -0x008D = PSA_ERROR_INSUFFICIENT_MEMORY, and the request then
     * sat in a 60 s backoff window.
     *
     * The lock is taken around the request only - no SD writes, no LVGL calls.
     */
    const esp_err_t lock_err = sys_net_lock(SYS_NET_LOCK_DEFAULT_MS);
    if (lock_err != ESP_OK) {
        SYS_LOGW(TAG, "another request is holding the network lock");
        return lock_err;
    }

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    sys_net_unlock();

    if (err != ESP_OK) {
        SYS_LOGW(TAG, "GET %s failed: %s", path, esp_err_to_name(err));
        free(resp.buf);
        /* 与 mm_post_ex() 同一套判断：401 也会让 perform() 失败。 */
        mm_fail_t kind;
        if (status == 401 || status == 403) {
            kind = MM_FAIL_AUTH;
            SYS_LOGW(TAG, "GET %s -> HTTP %d: the token was refused; not retrying",
                     path, status);
        } else if (mm_err_is_transport(err)) {
            kind = MM_FAIL_TRANSPORT;
        } else {
            kind = MM_FAIL_FINAL;
        }
        mm_record_outcome(kind, err);
        if (fail != NULL) {
            *fail = kind;
        }
        return err;
    }
    if (status != 200) {
        /*
         * **Do not decide from the status code alone.**
         *
         * The API answers 403 (and 401) with a fully-formed error envelope, e.g.
         * `{"errors":[{"code":"common_permission_denied"}]}` for a token whose
         * scope excludes the endpoint. Returning early on 401/403 discarded that
         * body and collapsed every auth/scope problem into one opaque
         * ESP_ERR_INVALID_STATE - which is how "the token may not read 助记"
         * first looked like a generic failure on the device.
         *
         * So: parse the envelope whenever there is one and let mm_classify() name
         * the real reason; fall back to the status code only when the body is
         * missing or unparseable.
         */
        if (resp.buf == NULL || resp.overflow) {
            SYS_LOGW(TAG, "GET %s -> HTTP %d (no parsable body)", path, status);
            free(resp.buf);
            const esp_err_t mapped = (status == 401 || status == 403)
                                         ? ESP_ERR_INVALID_STATE
                                         : ESP_ERR_INVALID_RESPONSE;
            const mm_fail_t kind = (status == 401 || status == 403) ? MM_FAIL_AUTH
                                                                   : MM_FAIL_FINAL;
            mm_record_outcome(kind, mapped);
            if (fail != NULL) {
                *fail = kind;
            }
            return mapped;
        }
        SYS_LOGD(TAG, "GET %s -> HTTP %d, inspecting the error envelope", path, status);
    } else if (resp.buf == NULL || resp.overflow) {
        free(resp.buf);
        mm_record_outcome(MM_FAIL_FINAL, ESP_ERR_INVALID_SIZE);
        if (fail != NULL) {
            *fail = MM_FAIL_FINAL;
        }
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *root = sys_json_parse(resp.buf, 32);
    free(resp.buf);
    if (root == NULL) {
        SYS_LOGW(TAG, "GET %s: response is not JSON", path);
        mm_record_outcome(MM_FAIL_FINAL, ESP_ERR_INVALID_RESPONSE);
        if (fail != NULL) {
            *fail = MM_FAIL_FINAL;
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t api_err = ESP_OK;
    mm_err_class_t api_class = MM_ERR_NONE;
    (void)mm_classify(root, &api_err, &api_class);
    if (api_err != ESP_OK) {
        cJSON_Delete(root);
        const mm_fail_t kind = (api_class == MM_ERR_AUTH) ? MM_FAIL_AUTH : MM_FAIL_FINAL;
        mm_record_outcome(kind, api_err);
        if (fail != NULL) {
            *fail = kind;
        }
        return api_err;
    }

    mm_record_outcome(MM_FAIL_NONE, ESP_OK);
    *out_root = root;
    return ESP_OK;
}

/** mm_get() 的对外语义原样保留（返回码与错误分类都不动）。 */
static esp_err_t mm_get(const char *path, const char *query, cJSON **out_root)
{
    return mm_get_ex(path, query, out_root, NULL);
}

/** `data` object of a parsed envelope (borrowed, do not delete). */
static const cJSON *mm_data(const cJSON *root)
{
    return cJSON_GetObjectItemCaseSensitive(root, "data");
}

static void mm_copy(char *dst, size_t len, const cJSON *obj, const char *field)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, field);
    if (cJSON_IsString(item)) {
        strlcpy(dst, item->valuestring, len);
    } else {
        dst[0] = '\0';
    }
}

/* ------------------------------------------------------------------------ */
/* Plan A: study sync                                                        */
/* ------------------------------------------------------------------------ */

/**
 * Fetch the account progress. The raw variant: always talks to the network.
 *
 * Split out from maimemo_fetch_progress() so the callers that must make a
 * *live* attempt (the console's "verify" button, the sync worker) are not
 * silently short-circuited by the backoff gate.
 */
static esp_err_t mm_fetch_progress_raw(maimemo_progress_t *out, mm_fail_t *fail)
{
    memset(out, 0, sizeof(*out));

    cJSON *root = NULL;
    esp_err_t err = mm_post_ex("study/get_study_progress", "{}", &root, fail);
    if (err != ESP_OK) {
        return err;
    }

    const cJSON *data = mm_data(root);
    const cJSON *progress = data ? cJSON_GetObjectItemCaseSensitive(data, "progress") : NULL;
    if (cJSON_IsObject(progress)) {
        const cJSON *finished = cJSON_GetObjectItemCaseSensitive(progress, "finished");
        const cJSON *total = cJSON_GetObjectItemCaseSensitive(progress, "total");
        const cJSON *study_time = cJSON_GetObjectItemCaseSensitive(progress, "study_time");
        out->finished = cJSON_IsNumber(finished) ? (int)finished->valuedouble : 0;
        out->total = cJSON_IsNumber(total) ? (int)total->valuedouble : 0;
        out->study_time_ms = cJSON_IsNumber(study_time) ? (int)study_time->valuedouble : 0;
    }
    cJSON_Delete(root);

    /* The number of words in the plan needs a second call; it is cheap and it is
     * the "词库" figure the UI shows. Failure here must not lose the progress we
     * already have, so the error is deliberately swallowed. */
    cJSON *rec_root = NULL;
    if (mm_post("study/query_study_records", "{\"as_count\":true}", &rec_root) == ESP_OK) {
        const cJSON *data = mm_data(rec_root);
        const cJSON *count = data ? cJSON_GetObjectItemCaseSensitive(data, "count") : NULL;
        if (cJSON_IsNumber(count)) {
            out->due_count = (int)count->valuedouble;
        }
        cJSON_Delete(rec_root);
    } else {
        /*
         * 这一步是「锦上添花」，它失败**不许**动退避状态。
         *
         * 上面的进度调用已经成功了 —— 令牌是好的、网络当时也是通的。
         * 如果让这次可选调用的失败把退避窗口打开，随后的云端队列构建
         * （紧跟着 SYNC_DONE 发生）就会被闸门挡掉：一次成功的同步，
         * 队列反而建不出来。
         */
        mm_record_outcome(MM_FAIL_NONE, ESP_OK);
    }

    return ESP_OK;
}

esp_err_t maimemo_fetch_progress(maimemo_progress_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    /*
     * 退避窗口内：不发请求，直接照抄上一次的错误码。
     * 调用方看到的错误分类与真发一次完全一样，省掉的只是整个 TLS 往返
     * （以及一次 DNS 查询）。
     */
    if (mm_backoff_active()) {
        return mm_state_last_err();
    }
    return mm_fetch_progress_raw(out, NULL);
}

/**
 * Copy a `today_items` array into @p out. Returns how many entries landed.
 *
 * Split out because the same parsing is needed for the primary call and for the
 * fallback in maimemo_fetch_today().
 */
static size_t mm_parse_today_items(const cJSON *items, maimemo_item_t *out, size_t max)
{
    size_t index = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, items) {
        if (index >= max) {
            break;
        }
        if (!cJSON_IsObject(entry)) {
            continue;
        }
        maimemo_item_t *dst = &out[index];
        memset(dst, 0, sizeof(*dst));
        mm_copy(dst->voc_id, sizeof(dst->voc_id), entry, "voc_id");
        mm_copy(dst->spelling, sizeof(dst->spelling), entry, "voc_spelling");
        mm_copy(dst->first_response, sizeof(dst->first_response), entry, "first_response");
        const cJSON *is_new = cJSON_GetObjectItemCaseSensitive(entry, "is_new");
        const cJSON *is_finished = cJSON_GetObjectItemCaseSensitive(entry, "is_finished");
        dst->is_new = cJSON_IsBool(is_new) && cJSON_IsTrue(is_new);
        dst->is_finished = cJSON_IsBool(is_finished) && cJSON_IsTrue(is_finished);
        if (dst->spelling[0] != '\0') {
            index++;
        }
    }
    return index;
}

/**
 * Fallback: reconstruct a queue from `/study/query_study_records`.
 *
 * ## Why this exists (v1.3.1)
 *
 * `/study/get_today_items` is a **beta** endpoint and it only returns rows once
 * the user has opened the phone app that day to initialise the plan ("如果当日未
 * 打开 App 进行初始化则无法准确计算"). Observed against a live account:
 *
 *   POST /study/get_today_items  {}                        -> {"today_items":[]}
 *   POST /study/get_today_items  {"is_finished":false,...} -> 51 real words
 *   POST /study/query_study_records {}                     -> 58 records
 *
 * So an empty `today_items` does **not** mean "the account has no words" - it
 * means the beta endpoint has nothing to say right now, and the device showed an
 * empty queue for an account that plainly had 58 words in its plan.
 *
 * `/study/query_study_records` is not beta and is the authoritative list of what
 * is in the plan, so it is used to fill the queue when the beta call comes back
 * empty. Records carry `last_response`/`next_study_date` rather than the
 * today-item flags; `first_response` is mapped from `last_response` so the card
 * can still show "this word was forgotten last time".
 *
 * @return number of entries written; 0 when the plan is genuinely empty.
 */
static size_t mm_fetch_today_from_records(maimemo_item_t *out, size_t max)
{
    /*
     * One record is ~350 bytes of JSON (a ~60-char voc_id dominates), so the
     * 24 KB response cap holds roughly 60 records. Ask for at most that: a larger
     * `limit` would make the *response* overflow and fail the whole call, which
     * is worse than a shorter queue. The panel shows one card at a time anyway.
     */
    const size_t request = (max > MM_RECORDS_MAX) ? MM_RECORDS_MAX : max;
    char body[160];
    snprintf(body, sizeof(body),
             "{\"limit\":%u,\"next_study_date\":{\"end\":\"%s\"}}",
             (unsigned)request, "2100-01-01T00:00:00+08:00");

    cJSON *root = NULL;
    if (mm_post("study/query_study_records", body, &root) != ESP_OK) {
        /* 与上面那个可选调用同理：主调用（today_items）已经成功了，
         * 这次兜底失败不该把退避窗口打开，把随后的云端队列构建挡在外面。 */
        mm_record_outcome(MM_FAIL_NONE, ESP_OK);
        return 0;
    }

    const cJSON *data = mm_data(root);
    const cJSON *records = data ? cJSON_GetObjectItemCaseSensitive(data, "records") : NULL;
    if (!cJSON_IsArray(records)) {
        cJSON_Delete(root);
        return 0;
    }

    size_t index = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, records) {
        if (index >= max) {
            break;
        }
        if (!cJSON_IsObject(entry)) {
            continue;
        }
        maimemo_item_t *dst = &out[index];
        memset(dst, 0, sizeof(*dst));
        mm_copy(dst->voc_id, sizeof(dst->voc_id), entry, "voc_id");
        mm_copy(dst->spelling, sizeof(dst->spelling), entry, "voc_spelling");
        /* A record with no study history is a word the user has never graded:
         * that is exactly what "new" means to the UI. */
        const cJSON *study_count = cJSON_GetObjectItemCaseSensitive(entry, "study_count");
        dst->is_new = !cJSON_IsNumber(study_count) || study_count->valuedouble <= 0;
        const cJSON *last_response = cJSON_GetObjectItemCaseSensitive(entry, "last_response");
        if (cJSON_IsString(last_response)) {
            strlcpy(dst->first_response, last_response->valuestring,
                    sizeof(dst->first_response));
        }
        /* study_count 0 means never studied -> not finished. Anything else has
         * been seen at least once, but "finished" in MaiMemo terms means done
         * *today*, which a record cannot tell us; false keeps the card actionable
         * instead of hiding it. */
        dst->is_finished = false;
        if (dst->spelling[0] != '\0') {
            index++;
        }
    }

    cJSON_Delete(root);
    return index;
}

/**
 * Fetch today's queue. The raw variant: always talks to the network.
 *
 * Split out from maimemo_fetch_today() for the same reason as
 * mm_fetch_progress_raw(): the backoff gate must not turn an explicit
 * "拉取今日词表" into a cached error.
 */
static esp_err_t mm_fetch_today_raw(maimemo_item_t *out, size_t max, size_t *count,
                                    mm_fail_t *fail)
{
    /*
     * Ask for the unfinished words explicitly.
     *
     * The previous request body was `{}`, which the beta endpoint answers with an
     * **empty array** (verified against the live API - see the note on
     * mm_fetch_today_from_records). The documented way to get the queue is to
     * pass the filter, and `limit` above the panel's 64-card cap so the cap is
     * ours, not the server's default of 50.
     */
    char body[64];
    snprintf(body, sizeof(body), "{\"is_finished\":false,\"limit\":%u}",
             (unsigned)((max > 1000) ? 1000 : max));

    cJSON *root = NULL;
    const esp_err_t err = mm_post_ex("study/get_today_items", body, &root, fail);
    if (err != ESP_OK) {
        return err;
    }

    const cJSON *data = mm_data(root);
    const cJSON *items = data ? cJSON_GetObjectItemCaseSensitive(data, "today_items") : NULL;
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        mm_record_outcome(MM_FAIL_FINAL, ESP_ERR_INVALID_RESPONSE);
        if (fail != NULL) {
            *fail = MM_FAIL_FINAL;
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t index = mm_parse_today_items(items, out, max);
    cJSON_Delete(root);

    if (index == 0) {
        /* Beta endpoint has nothing today: fall back to the study records so an
         * account with words in its plan never shows an empty page. */
        SYS_LOGI(TAG, "today_items was empty; falling back to study records");
        index = mm_fetch_today_from_records(out, max);
        if (index > 0) {
            SYS_LOGI(TAG, "today: %u words from study records (fallback)",
                     (unsigned)index);
            *count = index;
            return ESP_OK;
        }
    }

    *count = index;
    SYS_LOGI(TAG, "today: %u words from the study plan", (unsigned)index);
    return ESP_OK;
}

esp_err_t maimemo_fetch_today(maimemo_item_t *out, size_t max, size_t *count)
{
    if (out == NULL || count == NULL || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;

    /* 退避窗口内：照抄上一次的错误码，不发请求（见 maimemo_fetch_progress）。 */
    if (mm_backoff_active()) {
        return mm_state_last_err();
    }
    return mm_fetch_today_raw(out, max, count, NULL);
}

/**
 * Does the station have an IPv4 address?
 *
 * Same rule as word_service's have_ip(): wifi_manager publishes "0.0.0.0"
 * while there is no address yet, so "not empty" is not enough - the check has
 * to look at the value, not at its length.
 */
static bool mm_have_ip(void)
{
    sys_status_t status;
    if (sys_status_get(&status) != ESP_OK) {
        return false;
    }
    return status.wifi_ip[0] != '\0' && strcmp(status.wifi_ip, "0.0.0.0") != 0;
}

/**
 * Wait until the station has an address (at most @p timeout_ms).
 *
 * Waiting here does **not** hold the "one sync at a time" slot: nothing has
 * been sent yet, so the console's "sync now" must not be refused because of it.
 */
static bool mm_wait_for_ip(uint32_t timeout_ms)
{
    const int64_t deadline = sys_time_ms() + (int64_t)timeout_ms;
    for (;;) {
        if (mm_have_ip()) {
            /*
             * 地址到了不等于网络能用了：DHCP 带来的 DNS 服务器地址、路由和
             * lwIP 的解析器是随地址一起到的，比 IP 事件晚几十到几百毫秒。
             * 这一刻就发请求，DNS 解析不出来
             * （真机日志："couldn't get hostname for :open.maimemo.com"），
             * 白等一个 60 秒的退避窗口才有机会再试。
             *
             * 歇两秒再发，这一下就把「开机同步失败」变成「开机同步成功」。
             */
            vTaskDelay(pdMS_TO_TICKS(MM_SYNC_NET_SETTLE_MS));
            return mm_have_ip();
        }
        if (sys_time_ms() >= deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(MM_SYNC_IP_POLL_MS));
    }
}

/** The sync worker (defined below). */
static void mm_sync_worker(void *arg);

/** Launch the sync worker (one request per worker instance). */
static esp_err_t mm_sync_launch(void)
{
    /* 6 KB stack: TLS plus a 24 KB heap body (AGENT.md 9.0.2 on stack sizing). */
    if (xTaskCreate(mm_sync_worker, "th_mm_sync", 6144, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/** Re-launch the sync after the backoff delay (the timer callback). */
static void mm_sync_retry_cb(void *arg)
{
    (void)arg;

    if (__atomic_load_n(&s_sync_on_wire, __ATOMIC_ACQUIRE) > 0) {
        /* A request is on the wire right now: that round covers the retry. */
        return;
    }
    /* The cached flag, not maimemo_has_token(): this is a timer callback, and
     * the flash does not belong in a callback (AGENT.md 9.0.1.1 iron rule 1).
     * The cache is refreshed whenever the token is stored or cleared. */
    if (!s_token_cached) {
        return;
    }
    const esp_err_t err = mm_sync_launch();
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "sync retry not started: %s", esp_err_to_name(err));
    }
}

/**
 * Arm the backoff retry.
 *
 * The worker does **not** sleep here: the backoff is minutes long, and
 * sleeping inside the task would hold 6 KB of internal RAM (a FreeRTOS stack
 * can only come from internal RAM) plus the "one sync at a time" slot for
 * that whole time - the console's "sync now" button would keep answering
 * 400 "a sync is already running".
 */
static void mm_sync_arm_retry(uint32_t delay_ms)
{
    if (s_sync_retry_timer == NULL) {
        return;
    }
    (void)esp_timer_stop(s_sync_retry_timer);
    (void)esp_timer_start_once(s_sync_retry_timer, (uint64_t)delay_ms * 1000ULL);
}

/** Drop a pending retry (a new round supersedes it). */
static void mm_sync_cancel_retry(void)
{
    if (s_sync_retry_timer != NULL) {
        (void)esp_timer_stop(s_sync_retry_timer);
    }
}

/**
 * Start a sync round **without touching the flash**.
 *
 * This is maimemo_sync_start() minus the NVS read for the token: the caller
 * has already checked the cached flags. That is what makes it usable from an
 * event callback (AGENT.md 9.0.1.1 iron rule 1).
 *
 * The work itself is not here: mm_sync_launch() only creates the worker task,
 * and the request happens in that task.
 */
static esp_err_t mm_sync_start_now(void)
{
    if (__atomic_load_n(&s_sync_on_wire, __ATOMIC_ACQUIRE) > 0) {
        return ESP_ERR_INVALID_STATE;   /* a request is on the wire */
    }

    /* A new round supersedes a pending retry. */
    mm_sync_cancel_retry();
    s_sync_attempts = 0;

    return mm_sync_launch();
}

/**
 * The station got an address: start the automatic sync.
 *
 * This is the trigger that replaces the unconditional boot sync (roadmap
 * v1.8.0 D3).
 *
 * The old boot sync ran while there was **no address yet**: the host name
 * could not even be resolved, so the attempt was doomed - it still paid for a
 * DNS lookup and a full TLS handshake, got a 401, and took 11 KB out of the
 * largest internal block right at the end of the boot. Now the request starts
 * when there is something to send it to.
 *
 * Two gates, both from the RAM cache (no flash here):
 *   - a token has to be configured, otherwise there is nothing to ask with;
 *   - the cloud book has to be selected, because the local book needs no plan
 *     from MaiMemo. That is the same condition the old boot call used
 *     (`word_cloud_mode()` in app_main), so the request volume is unchanged.
 */
static void mm_sync_on_address(void)
{
    if (!s_token_cached || !s_cloud_mode_cached) {
        return;
    }

    const esp_err_t err = mm_sync_start_now();
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "sync not started: %s", esp_err_to_name(err));
        /* No memory for the worker right now: try again in a few seconds
         * instead of losing the round (the retry timer is the same one the
         * backoff uses). */
        mm_sync_arm_retry(MM_SYNC_RETRY_SHORT_MS);
        return;
    }
    SYS_LOGI(TAG, "sync started (the station got an address)");
}

/**
 * A WiFi event arrived.
 *
 * The subscription covers **every** event of that base rather than one
 * hard-coded id: the id would have to be copied out of wifi_manager.h into
 * this file (a silent breakage if the enum ever changes), while the thing we
 * actually care about - "is there an address now?" - is published by
 * wifi_manager in sys_status *before* it posts its events.
 *
 * So the trigger is the **transition**: the address appeared. A scan (which
 * also posts a WIFI_MGR event) therefore never starts a request, and a
 * disconnect clears the flag again.
 *
 * The handler runs on the shared event task, so it stays light: it reads the
 * RAM status snapshot and starts the worker task. No flash, no network here.
 */
static void mm_on_wifi_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)base;
    (void)event_id;
    (void)data;

    const bool now = mm_have_ip();
    const bool had = s_had_ip;
    s_had_ip = now;

    if (!now || had) {
        return;   /* no address, or nothing changed since the last event */
    }
    mm_sync_on_address();
}

/** Worker body of the asynchronous sync. */
static void mm_sync_worker(void *arg)
{
    (void)arg;

    (void)sys_event_post_to(MAIMEMO_EVENT, MAIMEMO_EVENT_SYNC_STARTED, NULL, 0);

    maimemo_progress_t progress = { 0 };
    esp_err_t err = ESP_FAIL;
    mm_fail_t fail = MM_FAIL_NONE;

    /*
     * 先等地址，再发请求（roadmap v1.7.0 C1）。
     *
     * 开机这一刻常常还没有 IP（DNS 解析不出来），第一次调用必然失败；
     * 真机日志里「esp-tls: couldn't get hostname ...」后面紧跟着的就是它。
     * 等地址这段时间**不算一次尝试**，也**不占「一次只允许一个同步」的名额**
     * —— 网页控制台的「立即同步」在开机这几秒里照样能发起。
     */
    if (!mm_wait_for_ip(MM_SYNC_IP_WAIT_MS)) {
        SYS_LOGW(TAG, "no IP address after %u s - sending anyway",
                 (unsigned)(MM_SYNC_IP_WAIT_MS / 1000));
    }

    /*
     * 一次只发一个请求：控制台的同步与开机同步可能同时在跑（两者都是
     * 只读的进度查询），但**不能同时在电线上**。
     */
    __atomic_add_fetch(&s_sync_on_wire, 1, __ATOMIC_ACQ_REL);
    if (s_sync_req_lock == NULL) {
        /* The lock could not be created (init failed): go ahead anyway. */
        s_sync_attempts++;
        err = mm_fetch_progress_raw(&progress, &fail);
    } else if (xSemaphoreTake(s_sync_req_lock, pdMS_TO_TICKS(MM_SYNC_REQ_LOCK_MS)) != pdTRUE) {
        SYS_LOGW(TAG, "another sync is still running - this one is skipped");
        err = ESP_ERR_TIMEOUT;
        fail = MM_FAIL_TRANSPORT;
    } else {
        s_sync_attempts++;
        err = mm_fetch_progress_raw(&progress, &fail);
        xSemaphoreGive(s_sync_req_lock);
    }
    __atomic_sub_fetch(&s_sync_on_wire, 1, __ATOMIC_ACQ_REL);

    /*
     * 失败退避（roadmap v1.7.0 C2）。
     *
     * 现在的规矩：
     *   - 令牌被拒（401/403）→ **不重试**，重试也不会成功，等用户改配置；
     *   - 传输层失败（DNS/连接/超时）→ 指数退避（60 s → 120 s）后再试；
     *   - 服务端已给出结论（错误信封等）→ 不重试。
     * 最多 MM_SYNC_MAX_ATTEMPTS 次，之后发 SYNC_FAILED。
     *
     * 这里用的是 raw 变体：退避由本轮自己控制，不能被闸门挡掉。
     * 重试由一次性定时器重新拉起 worker（见 mm_sync_arm_retry）。
     */
    if (err == ESP_OK) {
        s_sync_attempts = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_progress = progress;
        s_have_progress = true;
        xSemaphoreGive(s_lock);

        SYS_LOGI(TAG, "study progress: %d/%d, %d ms, plan %d",
                 progress.finished, progress.total, progress.study_time_ms,
                 progress.due_count);
        (void)sys_event_post_to(MAIMEMO_EVENT, MAIMEMO_EVENT_SYNC_DONE,
                                &progress, sizeof(progress));
    } else if (fail == MM_FAIL_TRANSPORT && s_sync_attempts < MM_SYNC_MAX_ATTEMPTS) {
        const uint32_t wait_ms = mm_backoff_delay_ms(mm_state_streak());
        SYS_LOGW(TAG, "sync failed (%s); retrying in %u s (attempt %u/%u)",
                 esp_err_to_name(err), (unsigned)(wait_ms / 1000),
                 (unsigned)(s_sync_attempts + 1), (unsigned)MM_SYNC_MAX_ATTEMPTS);
        mm_sync_arm_retry(wait_ms);
    } else {
        s_sync_attempts = 0;
        (void)sys_event_post_to(MAIMEMO_EVENT, MAIMEMO_EVENT_SYNC_FAILED,
                                &err, sizeof(err));
    }

    vTaskDelete(NULL);
}

esp_err_t maimemo_sync_start(void)
{
    if (!maimemo_has_token()) {
        return ESP_ERR_NOT_FOUND;
    }
    /* The token check above is the only flash access: it happens here, in a
     * task context (the console's "sync now" button). */
    return mm_sync_start_now();
}

esp_err_t maimemo_verify_token(void)
{
    maimemo_progress_t progress = { 0 };
    /*
     * 刻意走 raw 变体：网页控制台上的「测试」是一次**显式**的连通性检查，
     * 它必须真的发一次请求，不能被退避闸门换成一句缓存的错误。
     */
    const esp_err_t err = mm_fetch_progress_raw(&progress, NULL);
    if (err == ESP_OK) {
        SYS_LOGI(TAG, "token OK: %d/%d finished today", progress.finished, progress.total);
    }
    return err;
}

/* ------------------------------------------------------------------------ */
/* Plan B: enrichment cache on the SD card                                   */
/* ------------------------------------------------------------------------ */

/**
 * Cache file, relative to the application directory: `words/enrich.json`.
 *
 * One file, loaded once into PSRAM, rewritten whole. A per-word file layout
 * would be friendlier to partial writes but costs a directory entry per word and
 * makes "is it cached?" a stat() call per card - the whole file is < 256 KB and
 * there are at most 2000 words, so holding it is cheaper than probing for it.
 *
 * The path is **relative** and goes through storage_read_text()/write_file() so
 * the cache follows whichever backend is mounted (SD card, or the internal
 * fallback when no card is present) instead of assuming a root.
 */
#define MM_CACHE_REL "words/enrich.json"

static void mm_cache_load(void)
{
    if (s_cache_loaded) {
        return;
    }
    s_cache_loaded = true;

    if (!storage_available()) {
        return;   /* no storage at all: enrichment simply is not cached */
    }

    char *text = NULL;
    size_t len = 0;
    if (storage_read_text(MM_CACHE_REL, &text, &len) != ESP_OK || text == NULL) {
        /* First boot: no cache yet is the normal case, not an error. */
        return;
    }
    if (len == 0 || len > MM_MAX_CACHE_FILE) {
        SYS_LOGW(TAG, "enrich cache has implausible size %u; ignoring", (unsigned)len);
        free(text);
        return;
    }

    cJSON *root = sys_json_parse(text, 32);
    free(text);
    if (root == NULL) {
        SYS_LOGW(TAG, "enrich cache is not valid JSON; starting empty");
        return;
    }

    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    const size_t count = cJSON_IsArray(entries) ? (size_t)cJSON_GetArraySize(entries) : 0;
    if (count > 0) {
        s_cache = heap_caps_calloc_prefer(count, sizeof(mm_cache_entry_t), 2,
                                         MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
        if (s_cache != NULL) {
            size_t index = 0;
            const cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, entries) {
                if (index >= count || !cJSON_IsObject(entry)) {
                    break;
                }
                mm_cache_entry_t *dst = &s_cache[index];
                mm_copy(dst->spelling, sizeof(dst->spelling), entry, "spelling");
                mm_copy(dst->mnemonic, sizeof(dst->mnemonic), entry, "mnemonic");
                mm_copy(dst->example, sizeof(dst->example), entry, "example");
                mm_copy(dst->meaning, sizeof(dst->meaning), entry, "meaning");
                const cJSON *unavailable =
                    cJSON_GetObjectItemCaseSensitive(entry, "unavailable");
                dst->unavailable = cJSON_IsBool(unavailable) && cJSON_IsTrue(unavailable);
                const cJSON *meaning_unavailable =
                    cJSON_GetObjectItemCaseSensitive(entry, "meaning_unavailable");
                dst->meaning_unavailable =
                    cJSON_IsBool(meaning_unavailable) && cJSON_IsTrue(meaning_unavailable);
                /* "fetched" was added in v1.5.0. An entry written by an older
                 * firmware does not have it: such an entry is treated as "not
                 * conclusive", so it is fetched once more and then rewritten
                 * with the flag. Self-healing, and only for words the old
                 * cache knows nothing about. */
                const cJSON *fetched = cJSON_GetObjectItemCaseSensitive(entry, "fetched");
                dst->fetched = cJSON_IsBool(fetched) && cJSON_IsTrue(fetched);
                const cJSON *meaning_retryable =
                    cJSON_GetObjectItemCaseSensitive(entry, "meaning_retryable");
                dst->meaning_retryable =
                    cJSON_IsBool(meaning_retryable) && cJSON_IsTrue(meaning_retryable);
                if (dst->spelling[0] != '\0') {
                    index++;
                }
            }
            s_cache_count = index;
        }
    }
    cJSON_Delete(root);
    SYS_LOGI(TAG, "enrich cache: %u entries", (unsigned)s_cache_count);
}

/** Persist the whole cache. Called from the worker path only, never the UI. */
static void mm_cache_save(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    if (entries == NULL) {
        cJSON_Delete(root);
        return;
    }
    for (size_t i = 0; i < s_cache_count; i++) {
        const mm_cache_entry_t *src = &s_cache[i];
        cJSON *obj = cJSON_CreateObject();
        if (obj == NULL) {
            continue;
        }
        cJSON_AddStringToObject(obj, "spelling", src->spelling);
        if (src->mnemonic[0] != '\0') {
            cJSON_AddStringToObject(obj, "mnemonic", src->mnemonic);
        }
        if (src->example[0] != '\0') {
            cJSON_AddStringToObject(obj, "example", src->example);
        }
        if (src->meaning[0] != '\0') {
            cJSON_AddStringToObject(obj, "meaning", src->meaning);
        }
        if (src->unavailable) {
            cJSON_AddBoolToObject(obj, "unavailable", true);
        }
        if (src->meaning_unavailable) {
            cJSON_AddBoolToObject(obj, "meaning_unavailable", true);
        }
        if (src->fetched) {
            cJSON_AddBoolToObject(obj, "fetched", true);
        }
        if (src->meaning_retryable) {
            cJSON_AddBoolToObject(obj, "meaning_retryable", true);
        }
        cJSON_AddItemToArray(entries, obj);
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL || !storage_available()) {
        free(text);
        return;
    }

    const esp_err_t err = storage_write_file(MM_CACHE_REL, text, strlen(text));
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "cannot write %s: %s", MM_CACHE_REL, esp_err_to_name(err));
    }
    free(text);
}

static mm_cache_entry_t *mm_cache_find(const char *spelling)
{
    for (size_t i = 0; i < s_cache_count; i++) {
        if (strcasecmp(s_cache[i].spelling, spelling) == 0) {
            return &s_cache[i];
        }
    }
    return NULL;
}

static mm_cache_entry_t *mm_cache_add(const char *spelling)
{
    /* One realloc-style grow per miss: the array is PSRAM and small, and misses
     * are far rarer than hits once warmed. */
    mm_cache_entry_t *grown = heap_caps_realloc_prefer(
        s_cache, (s_cache_count + 1) * sizeof(mm_cache_entry_t), 2,
        MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (grown == NULL) {
        return NULL;
    }
    s_cache = grown;
    mm_cache_entry_t *slot = &s_cache[s_cache_count++];
    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->spelling, spelling, sizeof(slot->spelling));
    return slot;
}

/** Resolve a spelling to a voc_id via `GET /vocabulary?spelling=`.
 *
 * Returns false when the word is unknown to MaiMemo, the token is missing, or
 * the request fails - the caller cannot act differently on those cases, so they
 * are deliberately collapsed.
 *
 * @param ignore_token_state  true 时不理会「令牌被拒」的锁存，强制发一次请求。
 *        网页控制台的「探测令牌能力」必须真的去问 —— 它的用途就是回答
 *        「这个令牌到底能做什么」，拿缓存答案顶替就没有意义了。
 */
static bool mm_resolve_voc_id_ex(const char *spelling, char *out, size_t len,
                                 bool ignore_token_state)
{
    out[0] = '\0';

    /*
     * 令牌已经被服务端拒绝（401/403）：再问一次也是同一个答案。
     * 自动路径（翻卡片、回传词条）到这里直接放弃，不再消耗一次 HTTPS 往返；
     * 等用户换了令牌（maimemo_set_token 会清掉锁存）才重新开始。
     */
    if (!ignore_token_state && mm_state_token_rejected()) {
        SYS_LOGD(TAG, "the token was refused earlier; not asking the cloud again");
        return false;
    }

    /* Spellings are single ASCII words, so no percent-encoding is needed. */
    char query[128];
    snprintf(query, sizeof(query), "spelling=%s", spelling);

    cJSON *root = NULL;
    if (mm_get("vocabulary", query, &root) != ESP_OK) {
        return false;
    }

    bool found = false;
    const cJSON *data = mm_data(root);
    const cJSON *voc = data ? cJSON_GetObjectItemCaseSensitive(data, "voc") : NULL;
    if (cJSON_IsObject(voc)) {
        mm_copy(out, len, voc, "id");
        found = out[0] != '\0';
    }
    cJSON_Delete(root);
    return found;
}

/** Resolve a spelling to a voc_id. Returns false when unknown to MaiMemo. */
static bool mm_resolve_voc_id(const char *spelling, char *out, size_t len)
{
    return mm_resolve_voc_id_ex(spelling, out, len, false);
}

/**
 * Resolve many spellings with ONE `POST /vocabulary/query` (v1.15.0).
 *
 * The endpoint takes `{"spellings":[...]}` (up to 1000 on the server side; the
 * device never asks for more than MAIMEMO_MAX_TODAY) and answers
 * `{"data":{"voc":[{"id","spelling"},...]}}` - the batch sibling of the
 * single-word GET above, confirmed against the official CLI
 * (memo-api-cli src/commands/voc.ts).
 *
 * Why it matters here: the advance path used to resolve one GET per word, and
 * every GET is a full TLS round trip on a board where handshakes serialise on
 * the network lock and eat the internal heap (AGENT.md 9.0). One request for
 * the whole batch removes N-1 of them.
 *
 * @return How many of the inputs were resolved (ids written into @p out rows;
 *         unresolved rows are left empty). 0 also when the call itself failed,
 *         in which case the caller should fall back to the serial resolver.
 */
static size_t mm_resolve_voc_ids_batch(const char *const *spellings, size_t count,
                                       char (*out)[MAIMEMO_VOC_ID_MAX])
{
    for (size_t i = 0; i < count; i++) {
        out[i][0] = '\0';
    }
    if (mm_state_token_rejected()) {
        return 0;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(body, "spellings");
    if (body == NULL || arr == NULL) {
        cJSON_Delete(body);
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (spellings[i] == NULL || spellings[i][0] == '\0') {
            continue;
        }
        cJSON *item = cJSON_CreateString(spellings[i]);
        if (item == NULL || cJSON_AddItemToArray(arr, item) != true) {
            cJSON_Delete(item);
            break;
        }
    }
    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (payload == NULL) {
        return 0;
    }

    cJSON *root = NULL;
    const esp_err_t err = mm_post("vocabulary/query", payload, &root);
    cJSON_free(payload);
    if (err != ESP_OK) {
        SYS_LOGD(TAG, "batch resolve unavailable (%s) - caller should go serial",
                 esp_err_to_name(err));
        return 0;
    }

    size_t resolved = 0;
    const cJSON *data = mm_data(root);
    const cJSON *voc = data ? cJSON_GetObjectItemCaseSensitive(data, "voc") : NULL;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, voc) {
        if (!cJSON_IsObject(entry)) {
            continue;
        }
        const cJSON *spelling = cJSON_GetObjectItemCaseSensitive(entry, "spelling");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(entry, "id");
        if (!cJSON_IsString(spelling) || !cJSON_IsString(id) ||
            spelling->valuestring[0] == '\0' || id->valuestring[0] == '\0') {
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            if (out[i][0] == '\0' && spellings[i] != NULL &&
                strcasecmp(spelling->valuestring, spellings[i]) == 0) {
                strlcpy(out[i], id->valuestring, MAIMEMO_VOC_ID_MAX);
                resolved++;
                break;
            }
        }
    }
    cJSON_Delete(root);
    return resolved;
}

/**
 * Read one field of the enrichment (`/notes` or `/phrases`).
 *
 * Both are **GET with `?voc_id=`**, not POST - see mm_get() for why that
 * distinction is load-bearing. The reply shape is
 * `{"data":{"notes":[{"note":"..."}]}}` (resp. `phrases`/`phrase`), confirmed
 * against the official CLI's response handling (`res.notes.map(...)`).
 *
 * Returns:
 *   ESP_OK                -> @p out filled in
 *   ESP_ERR_NOT_FOUND     -> endpoint answered, nothing for this word
 *   ESP_ERR_NOT_SUPPORTED -> token scope excludes the endpoint (cached as such)
 *
 * @param fail Optional; receives the failure classification (C2).
 */
static esp_err_t mm_fetch_field_ex(const char *path, const char *voc_id,
                                   const char *array_key, const char *text_key,
                                   char *out, size_t len, mm_fail_t *fail)
{
    out[0] = '\0';
    if (fail != NULL) {
        *fail = MM_FAIL_NONE;
    }

    char query[128];
    snprintf(query, sizeof(query), "voc_id=%s", voc_id);

    cJSON *root = NULL;
    const esp_err_t err = mm_get_ex(path, query, &root, fail);
    if (err != ESP_OK) {
        return err;
    }

    const cJSON *data = mm_data(root);
    const cJSON *list = data ? cJSON_GetObjectItemCaseSensitive(data, array_key) : NULL;
    esp_err_t result = ESP_ERR_NOT_FOUND;

    if (cJSON_IsArray(list)) {
        const cJSON *first = cJSON_GetArrayItem(list, 0);
        if (cJSON_IsObject(first)) {
            mm_copy(out, len, first, text_key);
            if (out[0] != '\0') {
                result = ESP_OK;
            }
        }
    }
    cJSON_Delete(root);
    return result;
}

/** mm_fetch_field() 的对外语义原样保留。 */
static esp_err_t mm_fetch_field(const char *path, const char *voc_id,
                                const char *array_key, const char *text_key,
                                char *out, size_t len)
{
    return mm_fetch_field_ex(path, voc_id, array_key, text_key, out, len, NULL);
}

/**
 * Is this answer conclusive?
 *
 * "Conclusive" means the API answered: it either gave the data, said "there is
 * nothing for this word", said "your token may not read this", or answered with
 * an error envelope. Asking again with the same token cannot produce anything
 * new, so the cache may answer from then on.
 *
 * Only the transport failures (timeout, connection reset) are worth another
 * try - and only for the field that failed. Everything else being treated as
 * retryable would bring back the "four HTTPS round trips per card view" that
 * the cache exists to avoid.
 */
static bool mm_answer_final(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
    case ESP_ERR_NOT_FOUND:        /**< the API answered: there is no data */
    case ESP_ERR_NOT_SUPPORTED:    /**< the token may not read this endpoint */
    case ESP_ERR_INVALID_STATE:    /**< the token was rejected */
    case ESP_ERR_INVALID_RESPONSE: /**< the API answered with an error envelope */
    case ESP_ERR_INVALID_SIZE:     /**< the answer does not fit - deterministic */
        return true;
    default:
        return false;               /**< transport: timeout, reset, ... */
    }
}

esp_err_t maimemo_fetch_meaning(const char *spelling, char *out, size_t len)
{
    if (spelling == NULL || spelling[0] == '\0' || out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (!maimemo_has_token()) {
        return ESP_ERR_NOT_FOUND;
    }

    char voc_id[MAIMEMO_VOC_ID_MAX];
    if (!mm_resolve_voc_id(spelling, voc_id, sizeof(voc_id))) {
        SYS_LOGD(TAG, "'%s' is not in the MaiMemo vocabulary", spelling);
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * `/interpretations` mirrors the shape of `/notes`: a GET with `?voc_id=`
     * returning an array under `data`. The official CLI declares it as
     * `GET /api/v1/interpretations?voc_id=` with
     * `{interpretations:[{interpretation,tags,status,...}]}`, and it is the only
     * endpoint in the Open API that returns 释义 - `/vocabulary` returns just
     * `{id, spelling}`.
     */
    return mm_fetch_field("interpretations", voc_id, "interpretations",
                          "interpretation", out, len);
}

esp_err_t maimemo_enrich_cached(const char *spelling, maimemo_enrich_t *out)
{
    if (spelling == NULL || spelling[0] == '\0' || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    mm_cache_load();
    const mm_cache_entry_t *cached = mm_cache_find(spelling);
    if (cached == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    strlcpy(out->mnemonic, cached->mnemonic, sizeof(out->mnemonic));
    strlcpy(out->example, cached->example, sizeof(out->example));
    strlcpy(out->meaning, cached->meaning, sizeof(out->meaning));
    out->from_cache = true;
    const bool have_text = (out->mnemonic[0] != '\0' || out->example[0] != '\0' ||
                            out->meaning[0] != '\0');
    xSemaphoreGive(s_lock);

    /* Deliberately no network here - that is the whole point of this variant. */
    return have_text ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t maimemo_enrich(const char *spelling, maimemo_enrich_t *out)
{
    if (spelling == NULL || spelling[0] == '\0' || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    mm_cache_load();

    mm_cache_entry_t *cached = mm_cache_find(spelling);
    if (cached != NULL) {
        /*
         * Cache hit rules (v1.5.0 fix).
         *
         * The old condition was
         *     have_text && (meaning_unavailable || meaning != "" || unavailable)
         * i.e. "we have a mnemonic but no 释义, and 释义 was not refused" -> **miss**.
         * That is exactly the shape of a study-scoped token (助记 readable,
         * 释义 empty), so every card of that kind re-ran four HTTPS round trips
         * and rewrote the whole enrich.json - while the user was *flipping
         * through* the cards.
         *
         * The rule now: the cache answers whenever it holds anything at all,
         * or when the previous attempt was conclusive (the API answered "there
         * is nothing here"). Only a request that died on the transport leaves
         * the entry open for a retry.
         */
        const bool have_text = cached->mnemonic[0] != '\0' ||
                               cached->example[0] != '\0' ||
                               cached->meaning[0] != '\0';
        const bool all_unavailable = cached->unavailable && cached->meaning_unavailable;
        if (all_unavailable) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NOT_SUPPORTED;   /* already known unobtainable */
        }
        if (!cached->meaning_retryable && (have_text || cached->fetched)) {
            strlcpy(out->mnemonic, cached->mnemonic, sizeof(out->mnemonic));
            strlcpy(out->example, cached->example, sizeof(out->example));
            strlcpy(out->meaning, cached->meaning, sizeof(out->meaning));
            out->from_cache = true;
            xSemaphoreGive(s_lock);
            /* Nothing to serve and nothing more to ask for: same answer as a
             * fresh lookup that found nothing. */
            return have_text ? ESP_OK : ESP_ERR_NOT_FOUND;
        }
    }
    xSemaphoreGive(s_lock);

    if (!maimemo_has_token()) {
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * 退避窗口内：不再发请求。
     * 返回值与「真的发一次然后失败」一模一样（解析不出词也是 NOT_FOUND），
     * 所以 UI 与网页都看不出区别，省掉的却是整条 HTTPS 往返。
     */
    if (mm_backoff_active()) {
        SYS_LOGD(TAG, "'%s': skipped, the network is inside its backoff window",
                 spelling);
        return ESP_ERR_NOT_FOUND;
    }

    char voc_id[MAIMEMO_VOC_ID_MAX];
    if (!mm_resolve_voc_id(spelling, voc_id, sizeof(voc_id))) {
        SYS_LOGD(TAG, "'%s' is not in the MaiMemo vocabulary", spelling);
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * 三个端点依次问，失败分类决定还要不要问下一个：
     *   - 传输层失败（DNS/连接/超时）→ **不再往下问**。网络已经断了，
     *     再连两次只是把同一次失败重复三遍（每次都是一次 TLS 握手）；
     *     剩下的端点等退避窗口过去、下次翻这张卡时再问。
     *   - 其它失败 → 按原来的规矩继续（作用域不够是终态，释义仍要单独问）。
     */
    mm_fail_t fail = MM_FAIL_NONE;
    esp_err_t notes_err = mm_fetch_field_ex("notes", voc_id, "notes", "note",
                                            out->mnemonic, sizeof(out->mnemonic),
                                            &fail);
    esp_err_t phrases_err = ESP_ERR_NOT_SUPPORTED;
    esp_err_t meaning_err = ESP_ERR_NOT_SUPPORTED;
    if (fail == MM_FAIL_TRANSPORT) {
        phrases_err = notes_err;
        meaning_err = notes_err;
    } else {
        /* Both endpoints are permission-gated for a study-scoped token; probing one
         * is enough to decide, so the second call is skipped when the first says
         * "not in scope". That halves the wasted requests on the common path. */
        if (notes_err != ESP_ERR_NOT_SUPPORTED) {
            phrases_err = mm_fetch_field_ex("phrases", voc_id, "phrases", "phrase",
                                            out->example, sizeof(out->example),
                                            &fail);
        }
        if (fail == MM_FAIL_TRANSPORT) {
            meaning_err = phrases_err;
        } else {
            /*
             * 释义 is a separate endpoint with its own scope, so it is attempted even
             * when the note pair was refused - that combination (queue readable, notes
             * refused, interpretations readable) is the common study-scoped token, and
             * the meaning is the field that actually matters for a card.
             */
            meaning_err = mm_fetch_field_ex("interpretations", voc_id,
                                            "interpretations", "interpretation",
                                            out->meaning, sizeof(out->meaning),
                                            &fail);
        }
    }

    const bool out_of_scope = (notes_err == ESP_ERR_NOT_SUPPORTED &&
                               phrases_err == ESP_ERR_NOT_SUPPORTED);
    const bool meaning_out_of_scope = (meaning_err == ESP_ERR_NOT_SUPPORTED);
    /* Whether the attempt was conclusive decides if the cache may answer next
     * time (see the hit rules in maimemo_enrich). */
    const bool conclusive = mm_answer_final(notes_err) &&
                            mm_answer_final(phrases_err) &&
                            mm_answer_final(meaning_err);
    const bool meaning_retryable = !mm_answer_final(meaning_err);
    SYS_LOGD(TAG, "'%s': 助记=%s 例句=%s 释义=%s%s", spelling,
             esp_err_to_name(notes_err), esp_err_to_name(phrases_err),
             esp_err_to_name(meaning_err),
             (out_of_scope && meaning_out_of_scope) ? " (token scope limits these)" : "");

    /*
     * Re-look the entry up instead of reusing the `cached` pointer taken above.
     *
     * The lock was released for the whole network fetch, and another task may
     * have called mm_cache_add() in the meantime - that reallocs s_cache, so the
     * old pointer would dangle and writing through it would corrupt the heap.
     * A fresh lookup inside the lock is the only safe way to reach the slot.
     */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mm_cache_entry_t *slot = mm_cache_find(spelling);
    if (slot == NULL) {
        slot = mm_cache_add(spelling);
    }
    if (slot != NULL) {
        /*
         * Only a field whose request actually answered may overwrite the cache.
         *
         * A timeout must not wipe what an earlier, successful lookup had
         * stored - otherwise one flaky network moment loses the mnemonic for
         * good and the next card view has to fetch everything again. The same
         * applies to the scope flags: "not in scope" is permanent, so a later
         * transport error must not clear it.
         */
        if (mm_answer_final(notes_err)) {
            strlcpy(slot->mnemonic, out->mnemonic, sizeof(slot->mnemonic));
            slot->unavailable = out_of_scope;
        } else if (slot->mnemonic[0] != '\0') {
            /* Keep serving what we already know. */
            strlcpy(out->mnemonic, slot->mnemonic, sizeof(out->mnemonic));
        }
        if (mm_answer_final(phrases_err)) {
            strlcpy(slot->example, out->example, sizeof(slot->example));
        } else if (slot->example[0] != '\0') {
            strlcpy(out->example, slot->example, sizeof(out->example));
        }
        if (mm_answer_final(meaning_err)) {
            strlcpy(slot->meaning, out->meaning, sizeof(slot->meaning));
            slot->meaning_unavailable = meaning_out_of_scope;
        } else if (slot->meaning[0] != '\0') {
            strlcpy(out->meaning, slot->meaning, sizeof(out->meaning));
        }
        slot->fetched = conclusive;
        slot->meaning_retryable = meaning_retryable;
        mm_cache_save();
    }
    xSemaphoreGive(s_lock);

    if (out_of_scope && meaning_out_of_scope) {
        /* Cached as unavailable, so this is reported once per word rather than
         * once per card shown. */
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (out->mnemonic[0] == '\0' && out->example[0] == '\0' && out->meaning[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    SYS_LOGI(TAG, "enriched '%s' (助记 %u B, 例句 %u B, 释义 %u B)", spelling,
             (unsigned)strlen(out->mnemonic), (unsigned)strlen(out->example),
             (unsigned)strlen(out->meaning));
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Plan C: reverse push                                                      */
/* ------------------------------------------------------------------------ */

esp_err_t maimemo_push_words(const char **spellings, size_t count, bool advance,
                             int *accepted)
{
    if (accepted != NULL) {
        *accepted = 0;
    }
    if (spellings == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Resolve each spelling to a voc_id first: /study/add_words takes ids, not
     * spellings. Words MaiMemo does not know are skipped rather than failing the
     * whole batch. */
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *words = cJSON_AddArrayToObject(root, "words");
    if (words == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    /* `advance` is a **required** property of /study/add_words, not an optional
     * one: omitting it fails with
     *   common_invalid_param: {base} must have required property 'advance'
     * even when the words array is otherwise valid. Verified against the live
     * API. It must therefore always be sent, false included. */
    cJSON_AddBoolToObject(root, "advance", advance);

    size_t resolved = 0;
    for (size_t i = 0; i < count; i++) {
        if (spellings[i] == NULL || spellings[i][0] == '\0') {
            continue;
        }
        char voc_id[MAIMEMO_VOC_ID_MAX];
        if (!mm_resolve_voc_id(spellings[i], voc_id, sizeof(voc_id))) {
            SYS_LOGD(TAG, "push: '%s' unknown to MaiMemo, skipped", spellings[i]);
            continue;
        }
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            continue;
        }
        cJSON_AddStringToObject(entry, "id", voc_id);
        cJSON_AddItemToArray(words, entry);
        resolved++;
    }

    if (resolved == 0) {
        cJSON_Delete(root);
        SYS_LOGW(TAG, "push: none of the %u words resolved", (unsigned)count);
        return ESP_ERR_NOT_FOUND;
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *reply = NULL;
    const esp_err_t err = mm_post("study/add_words", body, &reply);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    /* Report the server's own count: it may reject individual ids (a word that is
     * already in the plan, for instance), so the number we sent is not the number
     * that landed. */
    int added = (int)resolved;
    const cJSON *data = mm_data(reply);
    const cJSON *added_count = data ? cJSON_GetObjectItemCaseSensitive(data, "added_count") : NULL;
    if (cJSON_IsNumber(added_count)) {
        added = (int)added_count->valuedouble;
    }
    cJSON_Delete(reply);

    if (accepted != NULL) {
        *accepted = added;
    }
    SYS_LOGI(TAG, "pushed to the study plan: %d accepted of %u resolved (advance=%d)",
             added, (unsigned)resolved, (int)advance);
    return ESP_OK;
}

esp_err_t maimemo_push_unknown(const char *spelling)
{
    if (spelling == NULL || spelling[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    /* Mutating the user's cloud plan is opt-in: without it this is a no-op, not
     * a failure the UI needs to report. */
    if (!maimemo_push_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!maimemo_has_token()) {
        return ESP_ERR_NOT_FOUND;
    }

    const char *list[1] = { spelling };
    int accepted = 0;
    const esp_err_t err = maimemo_push_words(list, 1, false, &accepted);
    if (err == ESP_OK) {
        (void)sys_event_post_to(MAIMEMO_EVENT, MAIMEMO_EVENT_PUSH_DONE,
                                &accepted, sizeof(accepted));
    }
    return err;
}

/** One-shot worker for maimemo_push_unknown_async(). */
static void mm_push_worker(void *arg)
{
    char *spelling = (char *)arg;
    (void)maimemo_push_unknown(spelling);
    free(spelling);
    vTaskDelete(NULL);
}

/**
 * UI-safe wrapper around maimemo_push_unknown().
 *
 * The card's grade button used to call the synchronous version directly, i.e.
 * a full TLS round trip *inside an LVGL callback* - which also tried to spawn
 * the progress-writer task while the adapter lock was held, and on this board
 * (largest contiguous internal block ~18 KB at boot) that is exactly where
 * ESP_ERR_NO_MEM came from.
 *
 * Everything cheap (the opt-in check) happens here so the common "push disabled"
 * case costs nothing; the network call is handed to a worker.
 */
esp_err_t maimemo_push_unknown_async(const char *spelling)
{
    if (spelling == NULL || spelling[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    /* Cheap pre-checks on the caller's task: no thread and no allocation if the
     * feature is off, which is the default. */
    if (!maimemo_push_enabled() || !maimemo_has_token()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char *copy = strdup(spelling);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* 5 KB: TLS handshake plus the JSON bodies this path builds. */
    if (xTaskCreate(mm_push_worker, "th_mm_push", 5120, copy, 3, NULL) != pdPASS) {
        free(copy);
        SYS_LOGW(TAG, "cannot start the push worker");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Plan D: read 释义 / advance studied words / probe scopes                   */
/* ------------------------------------------------------------------------ */

/*
 * ## What "uploading the device's study state" can and cannot mean
 *
 * The Open API has **no endpoint that submits a review grade**. Its whole study
 * surface is `get_study_progress`, `get_today_items`, `query_study_records`,
 * `add_words` and `advance_study` (verified against the official CLI's `study`
 * command and the `study.proto`-derived types in `memo-api-cli`). There is no
 * "this word was graded FAMILIAR" call, so the account's own "今日 N/M" counter
 * cannot be moved from this device.
 *
 * `advance_study` is the closest legitimate write, and it is a genuinely useful
 * one: it makes words due for review now, so a word the user studied on the
 * board comes back in the account's plan (and thus in the phone app) rather than
 * being ignored by the cloud. That is what this function does, and the UI says
 * exactly that instead of implying a grade was submitted.
 */

bool maimemo_advance_enabled(void)
{
    /* One switch for both write paths: adding words and advancing words both
     * mutate the account, so a user who declined one has declined both. */
    return maimemo_push_enabled();
}

esp_err_t maimemo_advance_words(const char **voc_ids, size_t count, int *advanced)
{
    if (advanced != NULL) {
        *advanced = 0;
    }
    if (voc_ids == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!maimemo_has_token()) {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *ids = cJSON_AddArrayToObject(root, "voc_ids");
    if (ids == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    size_t usable = 0;
    for (size_t i = 0; i < count; i++) {
        if (voc_ids[i] == NULL || voc_ids[i][0] == '\0') {
            continue;
        }
        cJSON *entry = cJSON_CreateString(voc_ids[i]);
        if (entry == NULL) {
            continue;
        }
        cJSON_AddItemToArray(ids, entry);
        usable++;
    }
    if (usable == 0) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *reply = NULL;
    const esp_err_t err = mm_post("study/advance_study", body, &reply);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    /* Report the server's own count: it may decline ids it does not track. */
    int advanced_count = (int)usable;
    const cJSON *data = mm_data(reply);
    const cJSON *server_count =
        data ? cJSON_GetObjectItemCaseSensitive(data, "advanced_count") : NULL;
    if (cJSON_IsNumber(server_count)) {
        advanced_count = (int)server_count->valuedouble;
    }
    cJSON_Delete(reply);

    if (advanced != NULL) {
        *advanced = advanced_count;
    }
    SYS_LOGI(TAG, "advanced %d of %u studied word(s) back into the plan",
             advanced_count, (unsigned)usable);
    return ESP_OK;
}

esp_err_t maimemo_advance_spellings(const char **spellings, size_t count, int *advanced)
{
    if (spellings == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!maimemo_advance_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /*
     * Resolve first: advance_study takes voc_ids, and a spelling MaiMemo does
     * not know cannot be advanced.
     *
     * v1.15.0: one POST /vocabulary/query resolves the whole batch. The serial
     * one-GET-per-word loop survives only as the fallback for a token whose
     * scope excludes the batch endpoint - and for the days when it is simply
     * unavailable.
     *
     * The id buffer is **on the heap, not the stack**. A `char[64][80]` local is
     * 5 KB, and this function is called from a 5 KB worker task (and from the
     * HTTP worker of the same size): together with the TLS handshake's own stack
     * use that overflows the task, which on this board shows up as a reboot
     * rather than a clean error. PSRAM is the right home for bulk data here
     * (AGENT.md 9.0.5).
     */
    const size_t limit = (count < 64) ? count : 64;
    char (*storage)[MAIMEMO_VOC_ID_MAX] = heap_caps_calloc_prefer(
        limit, sizeof(*storage), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    const char **ids = heap_caps_calloc_prefer(limit, sizeof(*ids), 2,
                                               MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (storage == NULL || ids == NULL) {
        heap_caps_free(storage);
        heap_caps_free(ids);
        return ESP_ERR_NO_MEM;
    }

    /* Batch first: one request for everything (rows indexed by input). */
    const size_t batch_ok = mm_resolve_voc_ids_batch(spellings, limit, storage);
    if (batch_ok == 0) {
        /* Fallback: the serial one-GET-per-word path, writing row i for input i. */
        for (size_t i = 0; i < limit; i++) {
            if (spellings[i] == NULL || spellings[i][0] == '\0') {
                continue;
            }
            if (!mm_resolve_voc_id(spellings[i], storage[i], sizeof(storage[0]))) {
                SYS_LOGD(TAG, "advance: '%s' unknown to MaiMemo, skipped", spellings[i]);
            }
        }
    }

    /* Compact the non-empty rows into the id list advance_study takes. */
    size_t resolved = 0;
    for (size_t i = 0; i < limit; i++) {
        if (storage[i][0] != '\0') {
            ids[resolved++] = storage[i];
        }
    }

    esp_err_t err;
    if (resolved == 0) {
        SYS_LOGW(TAG, "advance: none of the %u words resolved", (unsigned)count);
        err = ESP_ERR_NOT_FOUND;
    } else {
        err = maimemo_advance_words(ids, resolved, advanced);
    }

    heap_caps_free(storage);
    heap_caps_free(ids);
    return err;
}

/* ------------------------------------------------------------------------ */
/* Cloud notepad (云生词本, v1.15.0)                                          */
/* ------------------------------------------------------------------------ */

/*
 * `memo/notepads` CRUD, shape confirmed against memo-api-cli
 * (src/commands/notepad.ts + src/types/notepad.ts, types from
 * openapi/notepad/v1/notepad.proto):
 *
 *   GET  memo/notepads?limit&offset      -> data.notepads[{id,title,brief,...}]
 *   GET  memo/notepads/{id}              -> data.notepad{status,content,...}
 *   POST memo/notepads                   {"notepad":{status,content,title,brief,tags}}
 *   POST memo/notepads/{id}              same body (full replace of content)
 *
 * The device keeps to a deliberately small slice: list, and "add these words
 * to the notepad called X (create it when missing)". Content is one word per
 * line; the merge re-writes the whole content, so the existing words are read
 * back first to avoid clobbering what other devices put there.
 */

/** Hard cap on the merged content so one bad merge cannot blow MM_MAX_RESPONSE. */
#define MM_NOTEPAD_MAX_WORDS 300
#define MM_NOTEPAD_MAX_CONTENT (MM_NOTEPAD_MAX_WORDS * (MAIMEMO_SPELLING_MAX + 1))

/** Case-insensitive "is this word already a line of @p content?" */
static bool notepad_content_has(const char *content, const char *word)
{
    const size_t len = strlen(word);
    const char *line = content;
    while (line != NULL && line[0] != '\0') {
        const char *nl = strchr(line, '\n');
        const size_t line_len = (nl != NULL) ? (size_t)(nl - line) : strlen(line);
        if (line_len == len && strncasecmp(line, word, len) == 0) {
            return true;
        }
        line = (nl != NULL) ? nl + 1 : NULL;
    }
    return false;
}

esp_err_t maimemo_notepad_list(maimemo_notepad_t *out, size_t max, size_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;

    cJSON *root = NULL;
    /* limit=10 is the CLI's default and known-good; the server answers
     * `common_invalid_param` for limit=50 (verified on a live account). */
    const esp_err_t err = mm_get("memo/notepads", "limit=10&offset=0", &root);
    if (err != ESP_OK) {
        /* Propagate the classified cause (permission / auth / transport) - the
         * console shows it, and a blanket INVALID_STATE hid exactly that. */
        return err;
    }

    const cJSON *data = mm_data(root);
    const cJSON *notepads = data ? cJSON_GetObjectItemCaseSensitive(data, "notepads") : NULL;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, notepads) {
        if (!cJSON_IsObject(entry)) {
            continue;
        }
        if (out != NULL && *count < max) {
            maimemo_notepad_t *dst = &out[*count];
            memset(dst, 0, sizeof(*dst));
            mm_copy(dst->id, sizeof(dst->id), entry, "id");
            mm_copy(dst->title, sizeof(dst->title), entry, "title");
            mm_copy(dst->brief, sizeof(dst->brief), entry, "brief");
            mm_copy(dst->updated, sizeof(dst->updated), entry, "updated_time");
        }
        (*count)++;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * Append @p spellings to the notepad named @p title, creating it on first use.
 *
 * Blocking HTTPS (1-3 round trips); UI callers go through
 * maimemo_notepad_add_words_async().
 */
esp_err_t maimemo_notepad_add_words(const char *title, const char *const *spellings,
                                    size_t count, int *added)
{
    if (title == NULL || title[0] == '\0' || spellings == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!maimemo_has_token()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (added != NULL) {
        *added = 0;
    }

    char notepad_id[80] = { 0 };
    char status[24] = "UNPUBLISHED";
    char brief[64] = "ToDoHelper 设备生词";
    /* Existing + new content. PSRAM: this is bulk string data (AGENT.md 9.0.5). */
    char *content = heap_caps_calloc_prefer(1, MM_NOTEPAD_MAX_CONTENT + 1, 2,
                                            MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (content == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 1. Find the notepad by title. The list lives in PSRAM: it is ~2.8 KB and
     * this function runs on 6 KB workers that also pay for a TLS handshake. */
    maimemo_notepad_t *list = heap_caps_calloc_prefer(
        16, sizeof(maimemo_notepad_t), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (list == NULL) {
        heap_caps_free(content);
        return ESP_ERR_NO_MEM;
    }
    size_t list_count = 0;
    if (maimemo_notepad_list(list, 16, &list_count) == ESP_OK) {
        for (size_t i = 0; i < list_count; i++) {
            if (strcmp(list[i].title, title) == 0) {
                strlcpy(notepad_id, list[i].id, sizeof(notepad_id));
                break;
            }
        }
    }
    heap_caps_free(list);

    esp_err_t err = ESP_OK;

    /* 2. Existing notepad: read the content back so the merge appends instead
     *    of replacing (update is a full-content replace server-side). */
    if (notepad_id[0] != '\0') {
        char path[112];
        snprintf(path, sizeof(path), "memo/notepads/%s", notepad_id);
        cJSON *root = NULL;
        if (mm_get(path, NULL, &root) == ESP_OK) {
            const cJSON *data = mm_data(root);
            const cJSON *notepad =
                data ? cJSON_GetObjectItemCaseSensitive(data, "notepad") : NULL;
            const cJSON *cur = cJSON_GetObjectItemCaseSensitive(notepad, "content");
            if (cJSON_IsString(cur) && cur->valuestring != NULL) {
                strlcpy(content, cur->valuestring, MM_NOTEPAD_MAX_CONTENT + 1);
            }
            mm_copy(status, sizeof(status), notepad, "status");
            mm_copy(brief, sizeof(brief), notepad, "brief");
            cJSON_Delete(root);
        } else {
            SYS_LOGW(TAG, "notepad read-back failed; refusing a blind overwrite");
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }

    /* 3. Merge: append the words that are not already lines. */
    size_t used = strlen(content);
    if (err == ESP_OK) {
        int appended = 0;
        for (size_t i = 0; i < count && appended < 64; i++) {
            if (spellings[i] == NULL || spellings[i][0] == '\0') {
                continue;
            }
            if (notepad_content_has(content, spellings[i])) {
                continue;
            }
            const size_t len = strlen(spellings[i]);
            if (used + len + 2 > MM_NOTEPAD_MAX_CONTENT) {
                SYS_LOGW(TAG, "notepad content cap reached (%d words)", MM_NOTEPAD_MAX_WORDS);
                break;
            }
            if (used > 0 && content[used - 1] != '\n') {
                content[used++] = '\n';
            }
            memcpy(content + used, spellings[i], len);
            used += len;
            content[used] = '\0';
            appended++;
        }
        if (added != NULL) {
            *added = appended;
        }
        if (appended == 0) {
            heap_caps_free(content);
            return ESP_OK;   /* nothing new: no request, no flash of state */
        }
    }

    /* 4. Write it back (create or update). */
    if (err == ESP_OK) {
        cJSON *note = cJSON_CreateObject();
        cJSON *body = cJSON_CreateObject();
        char *payload = NULL;
        if (note != NULL && body != NULL) {
            cJSON_AddStringToObject(note, "status", status);
            cJSON_AddStringToObject(note, "content", content);
            cJSON_AddStringToObject(note, "title", title);
            cJSON_AddStringToObject(note, "brief", brief);
            (void)cJSON_AddArrayToObject(note, "tags");
            cJSON_AddItemToObject(body, "notepad", note);
            payload = cJSON_PrintUnformatted(body);
        }
        cJSON_Delete(body);
        if (payload == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            char path[112];
            if (notepad_id[0] != '\0') {
                snprintf(path, sizeof(path), "memo/notepads/%s", notepad_id);
            } else {
                strlcpy(path, "memo/notepads", sizeof(path));
            }
            cJSON *reply = NULL;
            err = mm_post(path, payload, &reply);
            cJSON_Delete(reply);
            cJSON_free(payload);
        }
    }

    heap_caps_free(content);
    if (err == ESP_OK) {
        SYS_LOGI(TAG, "notepad '%s': %d word(s) added", title,
                 (added != NULL) ? *added : -1);
    }
    return err;
}

/** Job for the one-shot notepad worker; owns strdup'd copies of the words. */
typedef struct {
    char title[64];
    size_t count;
    char *spellings[];
} mm_notepad_job_t;

/** One-shot worker for maimemo_notepad_add_words_async(). */
static void mm_notepad_worker(void *arg)
{
    mm_notepad_job_t *job = (mm_notepad_job_t *)arg;
    int added = 0;
    const esp_err_t err =
        maimemo_notepad_add_words(job->title, (const char *const *)job->spellings,
                                  job->count, &added);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "notepad add failed: %s", esp_err_to_name(err));
    }
    for (size_t i = 0; i < job->count; i++) {
        free(job->spellings[i]);
    }
    heap_caps_free(job);
    vTaskDelete(NULL);
}

/** UI-safe wrapper: the blocking HTTPS runs on its own 6 KB task (TLS needs it). */
esp_err_t maimemo_notepad_add_words_async(const char *title,
                                          const char *const *spellings, size_t count)
{
    if (title == NULL || title[0] == '\0' || spellings == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!maimemo_has_token()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (count > 64) {
        count = 64;
    }

    mm_notepad_job_t *job = heap_caps_calloc_prefer(
        1, sizeof(*job) + count * sizeof(char *), 2,
        MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (job == NULL) {
        return ESP_ERR_NO_MEM;
    }
    job->count = count;
    strlcpy(job->title, title, sizeof(job->title));
    for (size_t i = 0; i < count; i++) {
        job->spellings[i] = strdup(spellings[i] != NULL ? spellings[i] : "");
        if (job->spellings[i] == NULL) {
            job->count = i;
            break;
        }
    }

    if (xTaskCreate(mm_notepad_worker, "th_mm_note", 6144, job, 3, NULL) != pdPASS) {
        for (size_t i = 0; i < job->count; i++) {
            free(job->spellings[i]);
        }
        heap_caps_free(job);
        SYS_LOGW(TAG, "cannot start the notepad worker");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/*
 * Probe table. Ordered by how much each endpoint matters to the study flow:
 * the first two decide whether the queue works at all, `/interpretations` and
 * the note pair decide whether a card can show content for a word the local
 * book does not have.
 */
static const struct {
    const char *path;      /**< endpoint to exercise */
    const char *label;     /**< what it proves, for the console */
} MM_PROBE[] = {
    { "study/get_study_progress", "账号进度" },
    { "study/get_today_items",    "今日词表" },
    { "vocabulary",               "拼写→ID 解析" },
    { "interpretations",          "释义" },
    { "notes",                    "助记" },
    { "phrases",                  "例句" },
    { "memo/notepads",            "云生词本" },
};

/** Resolve one spelling to a voc_id, for the probe's per-word endpoints.
 *
 * Probing needs *some* word to ask about. Hard-coding a single spelling made the
 * whole word-scoped half of the probe depend on that one word existing in the
 * account's vocabulary - and since the probe exists precisely to explain why a
 * card has no content, a wrong "no data" verdict would be the worst outcome.
 *
 * So a few common spellings are tried in turn; they are all in the shipped CET-6
 * book and in MaiMemo's vocabulary, so at least one resolving means the token
 * and the endpoint are fine. The word-scoped endpoints are asked about the first
 * one that resolves, which keeps the four probes comparable.
 *
 * Returns false only when none resolve, in which case the caller reports
 * "no data" rather than a permission verdict (see maimemo_probe()).
 */
static bool mm_probe_voc_id(char *out, size_t len)
{
    static const char *const CANDIDATES[] = { "abrupt", "abolish", "absolute" };

    for (size_t i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
        /* ignore_token_state = true：探测必须真的发请求。
         * 就算之前有一次 401 把「令牌被拒」锁存住了，这次探测也要问出
         * 每个端点自己的结论 —— 那正是这个接口存在的理由。 */
        if (mm_resolve_voc_id_ex(CANDIDATES[i], out, len, true)) {
            return true;
        }
    }
    return false;
}

esp_err_t maimemo_probe(maimemo_probe_result_t *out, size_t max, size_t *count)
{
    if (out == NULL || count == NULL || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;

    if (!maimemo_has_token()) {
        return ESP_ERR_NOT_FOUND;
    }

    /* One resolution shared by the four word-scoped endpoints: it is the same
     * lookup they would each do, and on this board every HTTPS round trip is
     * worth avoiding. */
    char voc_id[MAIMEMO_VOC_ID_MAX] = { 0 };
    bool have_voc = false;

    const size_t total = sizeof(MM_PROBE) / sizeof(MM_PROBE[0]);
    size_t written = 0;

    for (size_t i = 0; i < total && written < max; i++) {
        const char *path = MM_PROBE[i].path;
        cJSON *root = NULL;
        esp_err_t err;
        int detail = -1;

        const bool word_scoped = (strcmp(path, "study/get_study_progress") != 0 &&
                                  strcmp(path, "study/get_today_items") != 0);
        if (word_scoped && !have_voc) {
            have_voc = mm_probe_voc_id(voc_id, sizeof(voc_id));
        }

        if (strcmp(path, "study/get_study_progress") == 0) {
            err = mm_post(path, "{}", &root);
            if (err == ESP_OK) {
                const cJSON *data = mm_data(root);
                const cJSON *progress =
                    data ? cJSON_GetObjectItemCaseSensitive(data, "progress") : NULL;
                const cJSON *finished =
                    progress ? cJSON_GetObjectItemCaseSensitive(progress, "finished") : NULL;
                detail = cJSON_IsNumber(finished) ? (int)finished->valuedouble : -1;
            }
        } else if (strcmp(path, "study/get_today_items") == 0) {
            err = mm_post(path, "{\"is_finished\":false,\"limit\":64}", &root);
            if (err == ESP_OK) {
                const cJSON *data = mm_data(root);
                const cJSON *items =
                    data ? cJSON_GetObjectItemCaseSensitive(data, "today_items") : NULL;
                detail = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : -1;
            }
        } else if (strcmp(path, "vocabulary") == 0) {
            /* Exercised through the shared resolution so the probe measures the
             * same call the study flow depends on. */
            err = have_voc ? ESP_OK : ESP_ERR_NOT_FOUND;
            detail = have_voc ? 1 : 0;
        } else if (!have_voc) {
            /* No id to ask about: report the resolution failure rather than a
             * misleading permission verdict. */
            err = ESP_ERR_NOT_FOUND;
        } else {
            char query[128];
            snprintf(query, sizeof(query), "voc_id=%s", voc_id);
            const char *array_key = (strcmp(path, "interpretations") == 0) ? "interpretations"
                                    : (strcmp(path, "notes") == 0)         ? "notes"
                                                                          : "phrases";
            err = mm_get(path, query, &root);
            if (err == ESP_OK) {
                const cJSON *data = mm_data(root);
                const cJSON *list =
                    data ? cJSON_GetObjectItemCaseSensitive(data, array_key) : NULL;
                detail = cJSON_IsArray(list) ? cJSON_GetArraySize(list) : 0;
            }
        }

        cJSON_Delete(root);
        out[written].path = path;
        out[written].err = err;
        out[written].detail = detail;
        written++;
    }

    *count = written;
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Init                                                                      */
/* ------------------------------------------------------------------------ */

esp_err_t maimemo_client_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Serialises the sync requests (see s_sync_on_wire). */
    s_sync_req_lock = xSemaphoreCreateMutex();
    if (s_sync_req_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * The backoff retry is driven by a one-shot timer, not by a sleeping
     * task: the worker exits after a failed attempt, so its 6 KB internal
     * stack is back in the heap during the (minutes long) wait and the
     * console's "sync now" is not refused meanwhile.
     */
    const esp_timer_create_args_t timer_args = {
        .callback = mm_sync_retry_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "th_mm_sync",
    };
    if (esp_timer_create(&timer_args, &s_sync_retry_timer) != ESP_OK) {
        SYS_LOGW(TAG, "sync retry timer unavailable: a failed sync will not retry");
    }

    s_initialized = true;

    /*
     * 让 cJSON 的分配钩子一定是「PSRAM 优先」的样子。
     *
     * 本组件用 cJSON 拼请求体（`/study/add_words`、`/study/advance_study`）
     * 与整份助记缓存（`words/enrich.json`），而 cJSON 的每一次分配都走
     * 全局钩子。app_main() 早就在开机时调过 sys_json_use_psram()，这里
     * 再调一次是**幂等**的，只是保证「无论初始化顺序如何」本组件的 JSON
     * 树与打印出来的文本都落在 PSRAM，而不是把内部堆切碎
     * （roadmap B3；AGENT.md §9.0.5）。
     */
    sys_json_use_psram();

    esp_err_t http_err = maimemo_register_http();
    if (http_err != ESP_OK) {
        SYS_LOGW(TAG, "HTTP routes unavailable: %s", esp_err_to_name(http_err));
    }

    /*
     * The automatic sync is triggered by the network, not by the boot
     * (roadmap v1.8.0 D3).
     *
     * At boot there is no address yet: the host name cannot even be resolved,
     * so a request sent then is doomed - and it still pays for a DNS lookup
     * and a full TLS handshake, while taking 6 KB of internal task stack plus
     * the TLS session out of the largest internal block exactly at the end of
     * the boot. So the trigger is "the station got an address".
     *
     * The cached flags are read here (a task context, so the NVS read is
     * allowed); the event callback itself only reads RAM.
     */
    mm_cache_config();

    /*
     * **sys_event_subscribe(), not esp_event_handler_register().** The project
     * posts through a *custom* event loop (sys_event_post_to() ->
     * esp_event_post_to(s_loop, ...)), so registering on the ESP-IDF default
     * loop silently never fires.
     *
     * The subscription covers every event of the base; the handler reacts to
     * the moment the address appears (see mm_on_wifi_event).
     */
    const esp_err_t wifi_sub = sys_event_subscribe(WIFI_MGR_EVENT, ESP_EVENT_ANY_ID,
                                                   mm_on_wifi_event, NULL, NULL);
    if (wifi_sub != ESP_OK) {
        SYS_LOGW(TAG, "cannot subscribe to the WiFi events: %s",
                 esp_err_to_name(wifi_sub));
    }

    /* The address may already be there (a warm boot, or a reconnect that
     * finished before this module came up): no event will come, so ask for
     * the sync now. */
    s_had_ip = mm_have_ip();
    if (s_had_ip) {
        mm_sync_on_address();
    }

    SYS_LOGI(TAG, "ready (token=%s, push=%s)", maimemo_has_token() ? "set" : "unset",
             maimemo_push_enabled() ? "on" : "off");
    return ESP_OK;
}
