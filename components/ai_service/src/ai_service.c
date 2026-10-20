/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ai_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "idle_face.h"
#include "storage_manager.h"
#include "esp_attr.h"

static const char *TAG = "ai";

ESP_EVENT_DEFINE_BASE(AI_EVENT);

#ifndef CONFIG_TODOHELPER_AI_TIMEOUT_MS
#define CONFIG_TODOHELPER_AI_TIMEOUT_MS 45000
#endif
#ifndef CONFIG_TODOHELPER_AI_SAVE_HISTORY
#define CONFIG_TODOHELPER_AI_SAVE_HISTORY 0
#endif

#define AI_ENDPOINT "https://api.deepseek.com/chat/completions"
#define AI_HISTORY_FILE "ai/chat_history.json"
/*
 * 任务栈尺寸的依据：esp_http_client **没有自己的任务**，mbedTLS 的握手直接
 * 消耗调用者的栈。8192 B 是本工程跑 DeepSeek 请求已经验证过的尺寸
 * （同一条 TLS 路径），不要再往下砍。
 */
#define AI_TASK_STACK 8192
#define AI_TASK_PRIO 4
#define AI_MAX_RESPONSE (12 * 1024)

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

static bool s_initialized;
static ai_state_t s_state = AI_STATE_IDLE;
static char s_model[32] = AI_DEFAULT_MODEL;
static char s_system_prompt[512] = AI_DEFAULT_SYSTEM_PROMPT;
static uint32_t s_history_rounds = 10;
static bool s_save_history;

/*
 * 对话历史：24 × 1040 B ≈ 24 KB。
 *
 * 以前是常驻 .bss（内部 DRAM），开机就吃掉 66 KB 内部 RAM 余量的 37%，
 * 而 AI 只是可选的 P2 功能。现在改成**按需分配**、并且优先放 PSRAM，
 * 内部 DRAM 一字节都不占（见 history_ensure_locked()）。
 *
 * 指针一旦分配就不再改变、进程生命周期内不释放，所以
 * ai_service_get_last_reply() / ai_service_history_get() 拿到的指针
 * 生命周期仍然是「到进程结束为止」。
 */
static ai_message_t *s_messages;
static size_t s_message_count;
static EXT_RAM_BSS_ATTR char s_last_reply[AI_MESSAGE_MAX];

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

static TaskHandle_t s_task;
static QueueHandle_t s_requests;
static volatile bool s_abort;

/** Payload handed to the worker task. */
typedef struct {
    char text[AI_MESSAGE_MAX];
} ai_request_t;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

const char *ai_service_state_name(ai_state_t state)
{
    switch (state) {
    case AI_STATE_LISTENING:
        return "listening";
    case AI_STATE_RECOGNIZING:
        return "recognizing";
    case AI_STATE_THINKING:
        return "thinking";
    case AI_STATE_SPEAKING:
        return "speaking";
    case AI_STATE_ERROR:
        return "error";
    case AI_STATE_IDLE:
    default:
        return "idle";
    }
}

/**
 * Mirror the conversation state on the idle face (FR7: 小眼睛联动).
 */
static void express(ai_state_t state)
{
    switch (state) {
    case AI_STATE_LISTENING:
        (void)idle_face_express(IDLE_FACE_LISTENING, 4000);
        break;
    case AI_STATE_THINKING:
    case AI_STATE_RECOGNIZING:
        (void)idle_face_express(IDLE_FACE_THINKING, 20000);
        break;
    case AI_STATE_SPEAKING:
        (void)idle_face_express(IDLE_FACE_SPEAKING, 20000);
        break;
    case AI_STATE_ERROR:
        (void)idle_face_express(IDLE_FACE_ERROR, 2500);
        break;
    case AI_STATE_IDLE:
    default:
        /* Let ui_core's own policy decide again. */
        (void)idle_face_set_state(IDLE_FACE_NORMAL);
        break;
    }
}

static void set_state(ai_state_t state)
{
    if (s_state == state) {
        return;
    }
    s_state = state;
    SYS_LOGD(TAG, "state -> %s", ai_service_state_name(state));

    express(state);
    (void)sys_event_post_to(AI_EVENT, AI_EVENT_STATE_CHANGED, &s_state, sizeof(s_state));
}

static bool api_key(char *out, size_t len)
{
    if (sys_config_get_str(SYS_KEY_AI_API_KEY, out, len) != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return out[0] != '\0';
}

bool ai_service_has_api_key(void)
{
    char key[128];
    return api_key(key, sizeof(key));
}

/* ------------------------------------------------------------------------ */
/* History                                                                   */
/* ------------------------------------------------------------------------ */

/**
 * 分配对话历史缓冲区。**必须在持有 s_lock 时调用。**
 *
 * 优先放 PSRAM：这 24 KB 放在内部 DRAM 里会挤掉 WiFi/LVGL 需要的连续块，
 * 而本机内部 RAM 只剩 66 KB（开机后）。分配失败时保持 s_messages == NULL，
 * 调用方按「没有上下文」降级，不崩。
 */
static esp_err_t history_ensure_locked(void)
{
    if (s_messages != NULL) {
        return ESP_OK;
    }

    /* heap_caps_malloc_prefer() 是可变参的：第二个参数是「后面还有几个能力集」。
     * 先试 PSRAM，失败再退回默认堆（与 sys_json.c / word_service.c 同一写法）。 */
    ai_message_t *buf = heap_caps_malloc_prefer(
        (size_t)AI_HISTORY_MAX_MESSAGES * sizeof(ai_message_t), 2,
        MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (buf == NULL) {
        SYS_LOGE(TAG, "history buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }
    memset(buf, 0, (size_t)AI_HISTORY_MAX_MESSAGES * sizeof(ai_message_t));
    s_messages = buf;
    s_message_count = 0;

    /* 系统提示固定占 messages[0]：缓冲区是「懒」出来的，但内容语义要和
     * 「一开始就分配好」时完全一致。 */
    if (s_system_prompt[0] != '\0') {
        strlcpy(s_messages[0].role, "system", sizeof(s_messages[0].role));
        strlcpy(s_messages[0].content, s_system_prompt, sizeof(s_messages[0].content));
        s_message_count = 1;
    }
    return ESP_OK;
}

esp_err_t ai_service_clear_history(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_last_reply[0] = '\0';

    if (s_messages != NULL) {
        s_message_count = 0;

        /* Keep the system prompt as message 0. */
        const bool has_prompt = (s_system_prompt[0] != '\0');
        if (has_prompt) {
            strlcpy(s_messages[0].role, "system", sizeof(s_messages[0].role));
            strlcpy(s_messages[0].content, s_system_prompt, sizeof(s_messages[0].content));
            s_message_count = 1;
        }
    }
    /* 缓冲区还没分配时什么都不用做：等它被分配出来时天然就是「只有系统提示」。 */

    xSemaphoreGive(s_lock);

    SYS_LOGI(TAG, "conversation cleared");
    return ESP_OK;
}

size_t ai_service_history_count(void)
{
    if (s_lock == NULL) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    (void)history_ensure_locked();
    const size_t count = s_message_count;
    xSemaphoreGive(s_lock);
    return count;
}

esp_err_t ai_service_history_get(size_t index, ai_message_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (history_ensure_locked() != ESP_OK) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    if (index >= s_message_count) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    *out = s_messages[index];
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/** Append a message, honouring the retained-round limit. */
static void history_append(const char *role, const char *content)
{
    if (s_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (history_ensure_locked() != ESP_OK) {
        xSemaphoreGive(s_lock);
        return;
    }

    const bool has_prompt = (s_message_count > 0) &&
                            (strcmp(s_messages[0].role, "system") == 0);

    if (s_message_count >= AI_HISTORY_MAX_MESSAGES) {
        /* Drop the oldest non-system exchange (a user+assistant pair when
         * possible) so the context keeps alternating roles. */
        const size_t drop_from = has_prompt ? 1 : 0;
        const size_t drop_count = (s_message_count - drop_from > 1) ? 2 : 1;
        memmove(&s_messages[drop_from], &s_messages[drop_from + drop_count],
                (s_message_count - drop_from - drop_count) * sizeof(ai_message_t));
        s_message_count -= drop_count;
    }

    /* The retained-round setting trims even earlier if it asks for less. */
    const size_t max_messages = 1 + (size_t)s_history_rounds * 2;
    if (s_message_count >= max_messages && max_messages > 1) {
        const size_t drop_from = has_prompt ? 1 : 0;
        const size_t drop_count = (s_message_count - drop_from > 1) ? 2 : 1;
        memmove(&s_messages[drop_from], &s_messages[drop_from + drop_count],
                (s_message_count - drop_from - drop_count) * sizeof(ai_message_t));
        s_message_count -= drop_count;
    }

    strlcpy(s_messages[s_message_count].role, role, sizeof(s_messages[0].role));
    strlcpy(s_messages[s_message_count].content, content, sizeof(s_messages[0].content));
    s_message_count++;

    xSemaphoreGive(s_lock);
}

/** Build the request body for a non-streaming call. */
static char *build_request_json(bool stream)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    if (root == NULL || messages == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(messages);
        return NULL;
    }

    cJSON_AddItemToObject(root, "messages", messages);
    cJSON_AddStringToObject(root, "model", s_model);
    cJSON_AddBoolToObject(root, "stream", stream);
    cJSON_AddNumberToObject(root, "temperature", 0.7);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t hist = history_ensure_locked();
    for (size_t i = 0; hist == ESP_OK && i < s_message_count; i++) {
        cJSON *msg = cJSON_CreateObject();
        if (msg == NULL) {
            continue;
        }
        cJSON_AddStringToObject(msg, "role", s_messages[i].role);
        cJSON_AddStringToObject(msg, "content", s_messages[i].content);
        cJSON_AddItemToArray(messages, msg);
    }
    xSemaphoreGive(s_lock);

    if (hist != ESP_OK) {
        cJSON_Delete(root);
        return NULL;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* ------------------------------------------------------------------------ */
/* HTTP transport                                                            */
/* ------------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool overflow;
} ai_body_t;

/** Callback used by the streaming path to forward every delta. */
typedef esp_err_t (*ai_chunk_cb_t)(const char *text, void *arg);

typedef struct {
    ai_body_t body;
    ai_chunk_cb_t on_chunk;
    void *chunk_arg;
    bool stream;
    /** Accumulates a partial SSE line between ON_DATA callbacks. */
    char line[2048];
    size_t line_len;
    char *full;
    size_t full_len;
} ai_stream_ctx_t;

static void stream_append(ai_stream_ctx_t *ctx, const char *text)
{
    const size_t add = strlen(text);
    if (ctx->full_len + add + 1 >= AI_MAX_RESPONSE) {
        return;
    }
    if (ctx->full == NULL) {
        ctx->full = malloc(AI_MAX_RESPONSE);
        if (ctx->full == NULL) {
            return;
        }
        ctx->full[0] = '\0';
    }
    memcpy(ctx->full + ctx->full_len, text, add);
    ctx->full_len += add;
    ctx->full[ctx->full_len] = '\0';
}

/** Handle one complete `data: {...}` SSE line. */
static void handle_sse_line(ai_stream_ctx_t *ctx, const char *line)
{
    const char *payload = line;
    if (strncmp(payload, "data:", 5) == 0) {
        payload += 5;
        while (*payload == ' ') {
            payload++;
        }
    } else {
        return;
    }
    if (strcmp(payload, "[DONE]") == 0) {
        return;
    }

    /* 外部输入（远端返回的 SSE 记录）：用带深度上限的解析，
     * 别让畸形报文把任务栈吃穿。 */
    cJSON *root = sys_json_parse(payload, 32);
    if (root == NULL) {
        return;
    }

    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    const cJSON *delta = first ? cJSON_GetObjectItemCaseSensitive(first, "delta") : NULL;
    const cJSON *content = delta ? cJSON_GetObjectItemCaseSensitive(delta, "content") : NULL;

    if (cJSON_IsString(content) && content->valuestring[0] != '\0') {
        stream_append(ctx, content->valuestring);
        if (ctx->on_chunk != NULL) {
            (void)ctx->on_chunk(content->valuestring, ctx->chunk_arg);
        }
    }
    cJSON_Delete(root);
}

static esp_err_t ai_http_event(esp_http_client_event_t *evt)
{
    /* Aborting from another task is honoured here: a non-OK return from the
     * event handler makes esp_http_client_perform() stop immediately. */
    if (s_abort) {
        return ESP_FAIL;
    }

    ai_stream_ctx_t *ctx = (ai_stream_ctx_t *)evt->user_data;
    if (ctx == NULL || evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) {
        return ESP_OK;
    }

    if (ctx->stream) {
        /* Reassemble lines: a chunk boundary can split an SSE record. */
        for (int i = 0; i < evt->data_len; i++) {
            const char c = ((const char *)evt->data)[i];
            if (c == '\n') {
                ctx->line[ctx->line_len] = '\0';
                if (ctx->line_len > 0) {
                    handle_sse_line(ctx, ctx->line);
                }
                ctx->line_len = 0;
            } else if (c != '\r' && ctx->line_len + 1 < sizeof(ctx->line)) {
                ctx->line[ctx->line_len++] = c;
            }
        }
        return ESP_OK;
    }

    /* Non-streaming: accumulate the whole body. */
    if (ctx->body.overflow || ctx->body.len + (size_t)evt->data_len >= AI_MAX_RESPONSE) {
        ctx->body.overflow = true;
        return ESP_OK;
    }
    if (ctx->body.buf == NULL) {
        ctx->body.buf = malloc(AI_MAX_RESPONSE);
        if (ctx->body.buf == NULL) {
            ctx->body.overflow = true;
            return ESP_OK;
        }
        ctx->body.buf[0] = '\0';
    }
    memcpy(ctx->body.buf + ctx->body.len, evt->data, evt->data_len);
    ctx->body.len += (size_t)evt->data_len;
    ctx->body.buf[ctx->body.len] = '\0';
    return ESP_OK;
}

/**
 * Perform the request. On success @p out receives the answer (heap, caller
 * frees) for the non-streaming case, and the accumulated text for streaming.
 */
static esp_err_t ai_perform(const char *body_json, bool stream, ai_chunk_cb_t on_chunk,
                            void *chunk_arg, char **out)
{
    char key[128];
    if (!api_key(key, sizeof(key))) {
        return ESP_ERR_NOT_FOUND;
    }

    ai_stream_ctx_t ctx = {
        .on_chunk = on_chunk,
        .chunk_arg = chunk_arg,
        .stream = stream,
    };

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", key);

    const esp_http_client_config_t cfg = {
        .url = AI_ENDPOINT,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_TODOHELPER_AI_TIMEOUT_MS,
        .event_handler = ai_http_event,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    if (stream) {
        esp_http_client_set_header(client, "Accept", "text/event-stream");
    }
    esp_http_client_set_post_field(client, body_json, (int)strlen(body_json));

    s_abort = false;
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (s_abort) {
        free(ctx.body.buf);
        free(ctx.full);
        return ESP_ERR_TIMEOUT;
    }
    if (err != ESP_OK) {
        free(ctx.body.buf);
        free(ctx.full);
        SYS_LOGW(TAG, "request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status == 401 || status == 403) {
        free(ctx.body.buf);
        free(ctx.full);
        SYS_LOGE(TAG, "DeepSeek rejected the API key (HTTP %d)", status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (status != 200) {
        free(ctx.body.buf);
        free(ctx.full);
        SYS_LOGW(TAG, "DeepSeek returned HTTP %d", status);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (stream) {
        *out = ctx.full;
        return ESP_OK;
    }

    if (ctx.body.buf == NULL || ctx.body.overflow) {
        free(ctx.body.buf);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Non-streaming response: choices[0].message.content */
    cJSON *root = sys_json_parse(ctx.body.buf, 32);
    free(ctx.body.buf);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    const cJSON *message = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : NULL;
    const cJSON *content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;

    if (!cJSON_IsString(content) || content->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out = strdup(content->valuestring);
    cJSON_Delete(root);
    return (*out != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ------------------------------------------------------------------------ */
/* Chat                                                                      */
/* ------------------------------------------------------------------------ */

esp_err_t ai_service_chat(const char *text, char *reply, size_t len)
{
    if (text == NULL || text[0] == '\0' || reply == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    reply[0] = '\0';

    sys_status_t st;
    if (sys_status_get(&st) == ESP_OK && !st.online && !st.wifi_connected) {
        /* Requirement FR7: offline must be an explicit, contained state. */
        set_state(AI_STATE_ERROR);
        return ESP_ERR_INVALID_STATE;
    }
    if (!ai_service_has_api_key()) {
        set_state(AI_STATE_ERROR);
        return ESP_ERR_NOT_FOUND;
    }

    set_state(AI_STATE_THINKING);
    history_append("user", text);

    char *body = build_request_json(false);
    if (body == NULL) {
        set_state(AI_STATE_ERROR);
        return ESP_ERR_NO_MEM;
    }

    char *answer = NULL;
    esp_err_t err = ai_perform(body, false, NULL, NULL, &answer);
    cJSON_free(body);

    if (err != ESP_OK) {
        set_state(AI_STATE_ERROR);
        (void)sys_event_post_to(AI_EVENT, AI_EVENT_ERROR, &err, sizeof(err));
        return err;
    }

    strlcpy(reply, answer, len);
    history_append("assistant", answer);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_last_reply, answer, sizeof(s_last_reply));
    xSemaphoreGive(s_lock);
    free(answer);

    set_state(AI_STATE_SPEAKING);
    (void)sys_event_post_to(AI_EVENT, AI_EVENT_REPLY_DONE, NULL, 0);
    if (s_save_history) {
        (void)ai_service_save_history();
    }

    set_state(AI_STATE_IDLE);
    return ESP_OK;
}

esp_err_t ai_service_chat_stream(const char *text,
                                 esp_err_t (*on_chunk)(const char *text, void *arg),
                                 void *arg)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    sys_status_t st;
    if (sys_status_get(&st) == ESP_OK && !st.online && !st.wifi_connected) {
        set_state(AI_STATE_ERROR);
        return ESP_ERR_INVALID_STATE;
    }
    if (!ai_service_has_api_key()) {
        set_state(AI_STATE_ERROR);
        return ESP_ERR_NOT_FOUND;
    }

    set_state(AI_STATE_THINKING);
    history_append("user", text);

    char *body = build_request_json(true);
    if (body == NULL) {
        set_state(AI_STATE_ERROR);
        return ESP_ERR_NO_MEM;
    }

    char *answer = NULL;
    set_state(AI_STATE_SPEAKING);
    esp_err_t err = ai_perform(body, true, on_chunk, arg, &answer);
    cJSON_free(body);

    if (err != ESP_OK) {
        set_state(AI_STATE_ERROR);
        (void)sys_event_post_to(AI_EVENT, AI_EVENT_ERROR, &err, sizeof(err));
        return err;
    }

    if (answer != NULL) {
        history_append("assistant", answer);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_last_reply, answer, sizeof(s_last_reply));
        xSemaphoreGive(s_lock);
        free(answer);
    }

    (void)sys_event_post_to(AI_EVENT, AI_EVENT_REPLY_DONE, NULL, 0);
    if (s_save_history) {
        (void)ai_service_save_history();
    }

    set_state(AI_STATE_IDLE);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Worker task (used by the UI path)                                          */
/* ------------------------------------------------------------------------ */

static void ai_task(void *arg)
{
    (void)arg;
    ai_request_t request;

    while (xQueueReceive(s_requests, &request, portMAX_DELAY) == pdTRUE) {
        char reply[AI_MESSAGE_MAX];
        const esp_err_t err = ai_service_chat(request.text, reply, sizeof(reply));
        if (err == ESP_OK) {
            /* Chunks let the UI render progressively; a single chunk is fine
             * for the non-streaming path. */
            (void)sys_event_post_to(AI_EVENT, AI_EVENT_REPLY_CHUNK, reply,
                                    strlen(reply) + 1);
        }
    }
    vTaskDelete(NULL);
}

/**
 * 按需创建 worker 任务。
 *
 * th_ai 的 8 KB 栈只能从内部 RAM 出，AI 又是可选的 P2 功能，开机就建一个
 * 常驻任务等于开机白占 8 KB 内部 RAM。所以第一次 ai_service_chat_async()
 * 时才建，建完保留（对话是一次一轮的，频繁重建反而更折腾）。
 */
static esp_err_t ai_worker_ensure(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    if (s_task == NULL) {
        if (s_requests == NULL) {
            s_requests = xQueueCreate(1, sizeof(ai_request_t));
            if (s_requests == NULL) {
                SYS_LOGE(TAG, "request queue creation failed");
                err = ESP_ERR_NO_MEM;
            }
        }
        if (err == ESP_OK) {
            if (xTaskCreate(ai_task, "th_ai", AI_TASK_STACK, NULL, AI_TASK_PRIO,
                            &s_task) != pdPASS) {
                SYS_LOGE(TAG, "worker task creation failed");
                s_task = NULL;
                err = ESP_ERR_NO_MEM;
            } else {
                SYS_LOGI(TAG, "worker task started");
            }
        }
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t ai_service_chat_async(const char *text)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(text) >= AI_MESSAGE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_err_t err = ai_worker_ensure();
    if (err != ESP_OK) {
        return err;
    }

    ai_request_t request;
    memset(&request, 0, sizeof(request));
    strlcpy(request.text, text, sizeof(request.text));

    if (xQueueSend(s_requests, &request, 0) != pdTRUE) {
        /* One turn at a time: the UI should wait for AI_EVENT_REPLY_DONE. */
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

const char *ai_service_get_last_reply(void)
{
    return (s_last_reply[0] != '\0') ? s_last_reply : NULL;
}

esp_err_t ai_service_abort(void)
{
    s_abort = true;
    SYS_LOGI(TAG, "abort requested");
    return ESP_OK;
}

ai_state_t ai_service_get_state(void)
{
    return s_state;
}

/* ------------------------------------------------------------------------ */
/* Persistence                                                               */
/* ------------------------------------------------------------------------ */

esp_err_t ai_service_save_history(void)
{
    if (!storage_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    if (root == NULL || array == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(array);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(root, "messages", array);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t hist = history_ensure_locked();
    for (size_t i = 0; hist == ESP_OK && i < s_message_count; i++) {
        cJSON *msg = cJSON_CreateObject();
        if (msg == NULL) {
            continue;
        }
        cJSON_AddStringToObject(msg, "role", s_messages[i].role);
        cJSON_AddStringToObject(msg, "content", s_messages[i].content);
        cJSON_AddItemToArray(array, msg);
    }
    xSemaphoreGive(s_lock);

    if (hist != ESP_OK) {
        /* array 已经挂到 root 上了，删 root 就够了（再删一次 array 是双重释放）。 */
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = storage_write_file(AI_HISTORY_FILE, json, strlen(json));
    cJSON_free(json);
    return err;
}

esp_err_t ai_service_load_history(void)
{
    if (!storage_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    char *json = NULL;
    size_t len = 0;
    esp_err_t err = storage_read_text(AI_HISTORY_FILE, &json, &len);
    if (err != ESP_OK) {
        return err;
    }

    /* 存档文件在 SD 卡上，用户可以自己改：用带深度上限的解析，
     * 坏文件只会让这次载入失败，不会把开机路径带崩。 */
    cJSON *root = sys_json_parse(json, 32);
    free(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
    if (!cJSON_IsArray(messages)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    (void)ai_service_clear_history();

    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, messages) {
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(entry, "role");
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(entry, "content");
        if (!cJSON_IsString(role) || !cJSON_IsString(content)) {
            continue;
        }
        if (strcmp(role->valuestring, "system") == 0) {
            continue;   /* the live prompt wins */
        }
        history_append(role->valuestring, content->valuestring);
    }

    cJSON_Delete(root);
    SYS_LOGI(TAG, "history restored (%u messages)", (unsigned)ai_service_history_count());
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Configuration                                                             */
/* ------------------------------------------------------------------------ */

esp_err_t ai_service_set_api_key(const char *key)
{
    if (key == NULL || key[0] == '\0') {
        (void)sys_config_erase(SYS_KEY_AI_API_KEY);
        SYS_LOGI(TAG, "API key cleared");
        return ESP_OK;
    }
    /* Stored only: never logged, never returned by any API (requirement 8.8). */
    esp_err_t err = sys_config_set_str(SYS_KEY_AI_API_KEY, key);
    if (err == ESP_OK) {
        SYS_LOGI(TAG, "API key stored");
    }
    return err;
}

esp_err_t ai_service_set_system_prompt(const char *prompt)
{
    if (prompt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(prompt) >= sizeof(s_system_prompt)) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* 提示词会被历史缓冲区读，写的时候要跟读互斥，否则读到半截。 */
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    strlcpy(s_system_prompt, prompt, sizeof(s_system_prompt));
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }

    esp_err_t err = sys_config_set_str(SYS_KEY_AI_PROMPT, s_system_prompt);
    (void)ai_service_clear_history();
    return err;
}

const char *ai_service_get_system_prompt(void)
{
    /* 返回的是内部缓冲，不是副本：网页端随时可能改写提示词，调用方需要留
     * 住内容就自己复制一份。这里不取锁 —— 读的是一个固定地址的字符串，
     * 最多看到改写中的一瞬间，而复制同样躲不掉这个窗口。 */
    return s_system_prompt;
}

const char *ai_service_get_model(void)
{
    return s_model;
}

esp_err_t ai_service_set_model(const char *model)
{
    if (model == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(model, "deepseek-chat") != 0 && strcmp(model, "deepseek-reasoner") != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    strlcpy(s_model, model, sizeof(s_model));
    return sys_config_set_str(SYS_KEY_AI_MODEL, s_model);
}

uint32_t ai_service_get_history_rounds(void)
{
    return s_history_rounds;
}

esp_err_t ai_service_set_history_rounds(uint32_t rounds)
{
    if (rounds < 1) {
        rounds = 1;
    }
    if (rounds > (AI_HISTORY_MAX_MESSAGES / 2)) {
        rounds = AI_HISTORY_MAX_MESSAGES / 2;
    }
    s_history_rounds = rounds;
    return sys_config_set_i32(SYS_KEY_AI_ROUNDS, (int32_t)rounds);
}

/* ------------------------------------------------------------------------ */
/* Init                                                                      */
/* ------------------------------------------------------------------------ */

esp_err_t ai_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    (void)sys_config_get_str_or(SYS_KEY_AI_MODEL, s_model, sizeof(s_model), AI_DEFAULT_MODEL);
    (void)sys_config_get_str_or(SYS_KEY_AI_PROMPT, s_system_prompt, sizeof(s_system_prompt),
                               AI_DEFAULT_SYSTEM_PROMPT);
    s_history_rounds = (uint32_t)sys_config_get_i32_or(SYS_KEY_AI_ROUNDS, 10);
    s_save_history = sys_config_get_bool_or(SYS_KEY_AI_SAVE_HIST, CONFIG_TODOHELPER_AI_SAVE_HISTORY);
    s_message_count = 0;
    s_messages = NULL;   /* 历史缓冲区按需分配，不在开机时占内存 */

    s_initialized = true;
    (void)ai_service_clear_history();

    if (s_save_history) {
        (void)ai_service_load_history();
    }

    /* worker 任务不在这儿建：第一次 ai_service_chat_async() 时才按需创建，
     * 开机不白占 8 KB 内部 RAM 栈。 */

    esp_err_t http_err = ai_service_register_http();
    if (http_err != ESP_OK) {
        SYS_LOGW(TAG, "HTTP routes unavailable: %s", esp_err_to_name(http_err));
    }

    SYS_LOGI(TAG, "ready (model '%s', key=%s, rounds %u, save_history=%d)",
             s_model, ai_service_has_api_key() ? "yes" : "no",
             (unsigned)s_history_rounds, s_save_history);
    return ESP_OK;
}
