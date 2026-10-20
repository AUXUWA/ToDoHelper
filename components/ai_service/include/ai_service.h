/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_service.h
 * @brief FR7: DeepSeek chat with history, streaming and idle-face feedback.
 *
 * Design points that come straight from the requirement:
 *   - the API key lives in NVS, is never hardcoded and is never returned by any
 *     API (requirement 8.8);
 *   - offline is a normal state: AI reports "unavailable" and nothing else on
 *     the device is affected;
 *   - the idle face mirrors the conversation state (LISTENING / THINKING /
 *     SPEAKING / HAPPY / ERROR), which is why this component talks to
 *     idle_face even though both sit above the business layer.
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

ESP_EVENT_DECLARE_BASE(AI_EVENT);

typedef enum {
    AI_EVENT_STATE_CHANGED = 0, /**< payload: ai_state_t */
    AI_EVENT_REPLY_CHUNK,       /**< payload: const char *text (copy immediately) */
    AI_EVENT_REPLY_DONE,        /**< no payload: see ai_service_get_last_reply() */
    AI_EVENT_ERROR,             /**< payload: esp_err_t */
} ai_event_id_t;

typedef enum {
    AI_STATE_IDLE = 0,
    AI_STATE_LISTENING,    /**< recording / waiting for input */
    AI_STATE_RECOGNIZING,  /**< speech to text in progress */
    AI_STATE_THINKING,     /**< waiting for the model */
    AI_STATE_SPEAKING,     /**< emitting the answer */
    AI_STATE_ERROR,
} ai_state_t;

/** Maximum length of one message body accepted from a caller. */
#define AI_MESSAGE_MAX 1024
/** Maximum number of messages (system + user + assistant) kept in context. */
#define AI_HISTORY_MAX_MESSAGES 24
/** Default model. */
#define AI_DEFAULT_MODEL "deepseek-chat"
/** Default system prompt. */
#define AI_DEFAULT_SYSTEM_PROMPT "你是一个运行在桌面设备上的中文学习助手，回答要简短、口语化。"

typedef struct {
    char role[16];   /**< "system" / "user" / "assistant" */
    char content[AI_MESSAGE_MAX];
} ai_message_t;

/**
 * @brief Load configuration from NVS. Never fails because of a missing key.
 */
esp_err_t ai_service_init(void);

/** @brief Store the API key (NVS). An empty string clears it. */
esp_err_t ai_service_set_api_key(const char *key);

/** @brief True when an API key is configured. The key itself is never exposed. */
bool ai_service_has_api_key(void);

/** @brief True when the conversation history is saved to storage. */
bool ai_service_save_history_enabled(void);

/** @brief Set and persist the system prompt (role definition). */
esp_err_t ai_service_set_system_prompt(const char *prompt);

/**
 * @brief Current system prompt (the role definition).
 *
 * The settings page shows a one sentence summary of it, so it needs a way to
 * read the prompt back.
 *
 * @return The internal buffer. It is **not** a copy: the prompt can be
 *         changed from the web console at any time, so copy it if you need to
 *         keep it around. Never NULL (an empty prompt yields an empty string).
 */
const char *ai_service_get_system_prompt(void);

/** @brief Selected model, "deepseek-chat" or "deepseek-reasoner". */
const char *ai_service_get_model(void);

/** @brief Select the model; returns ESP_ERR_NOT_SUPPORTED for unknown names. */
esp_err_t ai_service_set_model(const char *model);

/** @brief How many past rounds are kept (default 10). */
uint32_t ai_service_get_history_rounds(void);

/** @brief Change the retained round count, clamped to 1..(AI_HISTORY_MAX/2). */
esp_err_t ai_service_set_history_rounds(uint32_t rounds);

/** @brief Current state. */
ai_state_t ai_service_get_state(void);

/** @brief Human readable state name, used by the HTTP API and logs. */
const char *ai_service_state_name(ai_state_t state);

/**
 * @brief Blocking single turn.
 *
 * Intended for the internal worker and for HTTP handlers; it performs a network
 * request, so it must never be called from the LVGL task.
 *
 * @param[out] reply Receives the answer, NUL terminated.
 */
esp_err_t ai_service_chat(const char *text, char *reply, size_t len);

/**
 * @brief Queue a turn and return immediately.
 *
 * The answer arrives through AI_EVENT_REPLY_DONE; the UI subscribes and redraws.
 */
esp_err_t ai_service_chat_async(const char *text);

/** @brief Last complete answer. Returns NULL when there is none. */
const char *ai_service_get_last_reply(void);

/** @brief Drop the conversation, keeping the system prompt. */
esp_err_t ai_service_clear_history(void);

/** @brief Number of messages currently in context (including the system one). */
size_t ai_service_history_count(void);

/**
 * @brief Copy one context message.
 *
 * @return ESP_ERR_NOT_FOUND when @p index is out of range.
 */
esp_err_t ai_service_history_get(size_t index, ai_message_t *out);

/** @brief Cancel the in-flight request. The blocked caller returns ESP_ERR_TIMEOUT. */
esp_err_t ai_service_abort(void);

/** @brief Persist the conversation to `ai/chat_history.json` (best effort). */
esp_err_t ai_service_save_history(void);

/** @brief Load the conversation from `ai/chat_history.json` (best effort). */
esp_err_t ai_service_load_history(void);

/** @brief Publish the /api/v1/ai/... routes (requirement 8.5). */
esp_err_t ai_service_register_http(void);

/**
 * @brief Streaming turn for the HTTP API.
 *
 * Calls @p on_chunk for every delta as it arrives. Runs a long blocking request,
 * so the httpd task is the only sensible caller.
 */
esp_err_t ai_service_chat_stream(const char *text,
                                 esp_err_t (*on_chunk)(const char *text, void *arg),
                                 void *arg);

#ifdef __cplusplus
}
#endif
