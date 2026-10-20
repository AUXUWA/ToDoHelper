/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_service_http.c
 * @brief Requirement 8.5: the /api/v1/ai/... endpoints.
 *
 * `/api/v1/ai/config` and `DELETE /api/v1/ai/history` are in the always-auth
 * list of sync_server because they can change credentials or destroy data.
 *
 * 会阻塞的 AI 请求**不在 httpd 任务里跑**：见下方「AI HTTP 工作任务」。
 */

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ai_service.h"
#include "http_router.h"
#include "storage_manager.h"

static const char *TAG = "ai_http";

/* ------------------------------------------------------------------------ */
/* AI HTTP 工作任务                                                           */
/* ------------------------------------------------------------------------ */

/*
 * 一轮 AI 请求最长 45 s（CONFIG_TODOHELPER_AI_TIMEOUT_MS），而 esp_http_server
 * 只有唯一的 httpd 任务、URI handler 在里面**串行**执行：handler 不返回，
 * 连 accept() 都不再跑，表现为「ping 得通、串口正常，但任何 TCP 连接都建不上」
 * （AGENT.md §9.0.1 最后一条）。
 *
 * 所以 AI 的 HTTP 接口一律用 httpd_req_async_handler_begin() 把 socket 交给
 * 自有任务，handler 立刻返回，设备其它接口在 AI 跑的这几十秒里照常服务。
 *
 * 任务栈的依据：esp_http_client **没有自己的任务**，mbedTLS 的握手直接吃
 * 调用者的栈。这段代码搬过来之前跑在 httpd 的 6144 B 栈上，现在给 8192 B
 * （与组件内 th_ai 同一条 TLS 路径的已验证尺寸），不能再小。
 *
 * 只用一个任务 + 一个队列：AI 请求本来就不该并发（s_abort 是全局标志，
 * 并发请求会互相踩），串行化既省内部 RAM，语义也更干净。
 */
#define AI_HTTP_TASK_STACK 8192
/* 与搬走之前所在的 httpd 任务同优先级（5），保持原有的调度体验。 */
#define AI_HTTP_TASK_PRIO 5
#define AI_HTTP_QUEUE_LEN 2

/** 与 ai_service.c 里的 AI_HISTORY_FILE 必须是同一个存档文件。 */
#define AI_HISTORY_FILE "ai/chat_history.json"

typedef enum {
    AI_JOB_CHAT = 0,   /**< 阻塞问答：一次请求一次响应 */
    AI_JOB_STREAM,     /**< SSE 流式 */
    AI_JOB_ERASE,      /**< 清空对话：删内存 + 删磁盘存档 */
} ai_job_kind_t;

/** 交给 AI HTTP 工作任务的一份工作。 */
typedef struct {
    ai_job_kind_t kind;
    char *prompt;      /**< prompt 的独立副本（堆内存，任务生命周期内有效） */
    httpd_req_t *req;  /**< httpd_req_async_handler_begin() 给的异步句柄 */
} ai_job_t;

static QueueHandle_t s_jobs;
static TaskHandle_t s_worker;
static StaticSemaphore_t s_worker_lock_buf;
static SemaphoreHandle_t s_worker_lock;

/* ------------------------------------------------------------------------ */
/* Streaming helper                                                          */
/* ------------------------------------------------------------------------ */

typedef struct {
    httpd_req_t *req;
    bool failed;
} sse_sink_t;

/**
 * Forward one delta as an SSE record.
 *
 * The record is JSON-encoded so a delta containing newlines cannot break the
 * framing.
 */
static esp_err_t stream_chunk_cb(const char *text, void *arg)
{
    sse_sink_t *sink = (sse_sink_t *)arg;
    if (sink == NULL || sink->failed) {
        return ESP_FAIL;
    }

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(obj, "delta", text);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const size_t needed = strlen(json) + 32;
    char *record = malloc(needed);
    if (record == NULL) {
        cJSON_free(json);
        return ESP_ERR_NO_MEM;
    }
    snprintf(record, needed, "event: chunk\ndata: %s\n\n", json);
    cJSON_free(json);

    const esp_err_t err = httpd_resp_send_chunk(sink->req, record, HTTPD_RESP_USE_STRLEN);
    free(record);
    if (err != ESP_OK) {
        sink->failed = true;
        return err;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Job execution (runs in the AI HTTP worker task)                            */
/* ------------------------------------------------------------------------ */

/** 跑一个 SSE 流式任务：把 delta 一段段写进 socket。 */
static void ai_job_run_stream(ai_job_t *job)
{
    httpd_req_t *req = job->req;

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    sse_sink_t sink = { .req = req };
    const esp_err_t err = ai_service_chat_stream(job->prompt, stream_chunk_cb, &sink);

    if (err != ESP_OK) {
        /* The stream is already open, so report inside the event stream rather
         * than switching to a JSON error the client is not parsing. */
        char data[96];
        snprintf(data, sizeof(data), "{\"code\":%d,\"msg\":\"%s\",\"data\":null}",
                 (err == ESP_ERR_NOT_FOUND) ? 400 : 503, esp_err_to_name(err));
        char record[128];
        snprintf(record, sizeof(record), "event: error\ndata: %s\n\n", data);
        (void)httpd_resp_send_chunk(req, record, HTTPD_RESP_USE_STRLEN);
    } else if (!sink.failed) {
        /* 客户端半路断开时不要再往死 socket 里写。 */
        const char *reply = ai_service_get_last_reply();
        if (reply != NULL) {
            char copy[AI_MESSAGE_MAX];
            strlcpy(copy, reply, sizeof(copy));

            cJSON *obj = cJSON_CreateObject();
            if (obj != NULL) {
                cJSON_AddStringToObject(obj, "reply", copy);
                char *json = cJSON_PrintUnformatted(obj);
                cJSON_Delete(obj);
                if (json != NULL) {
                    const size_t needed = strlen(json) + 32;
                    char *record = malloc(needed);
                    if (record != NULL) {
                        snprintf(record, needed, "event: done\ndata: %s\n\n", json);
                        (void)httpd_resp_send_chunk(req, record, HTTPD_RESP_USE_STRLEN);
                        free(record);
                    }
                    cJSON_free(json);
                }
            }
        }
    }

    /* 结束 chunked body。 */
    (void)httpd_resp_send_chunk(req, NULL, 0);
}

/** 跑一个阻塞问答任务：算完再一次性回 JSON。 */
static void ai_job_run_chat(ai_job_t *job)
{
    httpd_req_t *req = job->req;

    char reply[AI_MESSAGE_MAX];
    const esp_err_t err = ai_service_chat(job->prompt, reply, sizeof(reply));
    switch (err) {
    case ESP_OK:
        break;
    case ESP_ERR_NOT_FOUND:
        (void)http_reply_bad_request(req, "no API key configured (POST /api/v1/ai/config)");
        return;
    case ESP_ERR_INVALID_STATE:
        (void)http_reply_unavailable(req, "offline: AI is unavailable without a network");
        return;
    case ESP_ERR_TIMEOUT:
        (void)http_reply_unavailable(req, "request aborted or timed out");
        return;
    default:
        (void)http_reply_unavailable(req, esp_err_to_name(err));
        return;
    }

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "ai chat");
        return;
    }
    cJSON_AddStringToObject(obj, "reply", reply);
    cJSON_AddStringToObject(obj, "state", ai_service_state_name(ai_service_get_state()));
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (json == NULL) {
        (void)http_reply_esp_error(req, ESP_ERR_NO_MEM, "ai chat");
        return;
    }

    (void)http_reply_ok(req, json);
    cJSON_free(json);
}

/** 跑一个清空对话的任务：内存 + 磁盘存档一起清。 */
static void ai_job_run_erase(ai_job_t *job)
{
    httpd_req_t *req = job->req;

    (void)ai_service_clear_history();

    /* 只清内存的话，磁盘上的存档还在，重启后对话又回来 —— 用户以为「清空」
     * 其实没清。删文件要碰 SD 卡，所以放在任务里做，不占着 httpd 任务。 */
    if (storage_available()) {
        const esp_err_t del = storage_delete_file(AI_HISTORY_FILE);
        if (del != ESP_OK && del != ESP_ERR_NOT_FOUND) {
            SYS_LOGW(TAG, "history file delete failed: %s", esp_err_to_name(del));
        }
    }

    (void)http_reply_ok(req, "{\"cleared\":true}");
}

/**
 * AI HTTP 工作任务：一次只干一件事，干完把会话槽还回去。
 */
static void ai_http_worker(void *arg)
{
    (void)arg;
    ai_job_t *job = NULL;

    while (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE) {
        switch (job->kind) {
        case AI_JOB_STREAM:
            ai_job_run_stream(job);
            break;
        case AI_JOB_CHAT:
            ai_job_run_chat(job);
            break;
        case AI_JOB_ERASE:
        default:
            ai_job_run_erase(job);
            break;
        }

        /* 无论成功、失败、还是客户端半路断开，都必须把会话槽还回去：
         * 不 complete 的话 httpd 会一直占着这个会话，最后连 accept() 都不做，
         * 整机 HTTP 就再也连不上了（AGENT.md §9.0.1 最后一条）。 */
        (void)httpd_req_async_handler_complete(job->req);

        free(job->prompt);
        free(job);
        job = NULL;
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------ */
/* Dispatch                                                                   */
/* ------------------------------------------------------------------------ */

/**
 * 按需创建 AI HTTP 工作任务。
 *
 * 只有 httpd 任务会调它，但仍然上锁：万一以后有第二个调用者，
 * 重复建任务会让有的请求进了没人读的队列，socket 永远不 complete。
 */
static esp_err_t ai_http_ensure_worker(void)
{
    if (s_worker_lock == NULL) {
        s_worker_lock = xSemaphoreCreateMutexStatic(&s_worker_lock_buf);
        if (s_worker_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_worker_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    if (s_worker == NULL) {
        if (s_jobs == NULL) {
            s_jobs = xQueueCreate(AI_HTTP_QUEUE_LEN, sizeof(ai_job_t *));
            if (s_jobs == NULL) {
                SYS_LOGE(TAG, "job queue creation failed");
                err = ESP_ERR_NO_MEM;
            }
        }
        if (err == ESP_OK) {
            if (xTaskCreate(ai_http_worker, "th_ai_http", AI_HTTP_TASK_STACK, NULL,
                            AI_HTTP_TASK_PRIO, &s_worker) != pdPASS) {
                SYS_LOGE(TAG, "worker task creation failed");
                s_worker = NULL;
                err = ESP_ERR_NO_MEM;
            } else {
                SYS_LOGI(TAG, "AI HTTP worker started");
            }
        }
    }

    xSemaphoreGive(s_worker_lock);
    return err;
}

/** 任务没排上：就地回一个错误，并且把会话槽还回去。 */
static void ai_job_reject(ai_job_t *job)
{
    httpd_req_t *req = job->req;

    if (job->kind == AI_JOB_STREAM) {
        /* 流式客户端只认事件流：保持格式，code 503。 */
        httpd_resp_set_type(req, "text/event-stream");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        const char *record =
            "event: error\ndata: {\"code\":503,\"msg\":\"AI worker busy\",\"data\":null}\n\n";
        (void)httpd_resp_send_chunk(req, record, HTTPD_RESP_USE_STRLEN);
        (void)httpd_resp_send_chunk(req, NULL, 0);
    } else {
        (void)http_reply_unavailable(req, "AI worker busy");
    }

    (void)httpd_req_async_handler_complete(req);
    free(job->prompt);
    free(job);
}

/**
 * 把一次 AI 请求交给工作任务，handler 立刻返回。
 *
 * prompt 必须在进任务前**复制一份**：httpd_req_async_handler_begin() 之后
 * 原来的 req 就失效了（body 也读不到了）。
 */
static esp_err_t ai_http_dispatch(httpd_req_t *req, const char *prompt, ai_job_kind_t kind)
{
    ai_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return http_reply_unavailable(req, "no memory for AI request");
    }
    job->kind = kind;
    if (kind != AI_JOB_ERASE) {
        job->prompt = strdup(prompt);
        if (job->prompt == NULL) {
            free(job);
            return http_reply_unavailable(req, "no memory for AI request");
        }
    }

    const esp_err_t err = ai_http_ensure_worker();
    if (err != ESP_OK) {
        free(job->prompt);
        free(job);
        return http_reply_unavailable(req, "AI worker unavailable");
    }

    /* 交接之后只有 job->req 能用，req 本身已经失效。 */
    if (httpd_req_async_handler_begin(req, &job->req) != ESP_OK) {
        free(job->prompt);
        free(job);
        return http_reply_unavailable(req, "streaming unavailable");
    }

    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        ai_job_reject(job);
        return ESP_OK;
    }

    /* 立刻返回：httpd 任务不阻塞，设备其它接口照常服务。 */
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Handlers                                                                   */
/* ------------------------------------------------------------------------ */

static esp_err_t chat_stream_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing body");
    }

    /* 外部输入：走带深度上限的解析，别让畸形 JSON 在 httpd 任务上爆栈。 */
    cJSON *root = sys_json_parse(body, 32);
    free(body);
    if (root == NULL) {
        return http_reply_bad_request(req, "invalid json");
    }

    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(text) || text->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "field 'text' is required");
    }
    char prompt[AI_MESSAGE_MAX];
    strlcpy(prompt, text->valuestring, sizeof(prompt));
    cJSON_Delete(root);

    return ai_http_dispatch(req, prompt, AI_JOB_STREAM);
}

static esp_err_t chat_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing body");
    }

    cJSON *root = sys_json_parse(body, 32);
    free(body);
    if (root == NULL) {
        return http_reply_bad_request(req, "invalid json");
    }

    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(text) || text->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return http_reply_bad_request(req, "field 'text' is required");
    }
    char prompt[AI_MESSAGE_MAX];
    strlcpy(prompt, text->valuestring, sizeof(prompt));
    cJSON_Delete(root);

    return ai_http_dispatch(req, prompt, AI_JOB_CHAT);
}

/* ------------------------------------------------------------------------ */
/* Plain endpoints                                                           */
/* ------------------------------------------------------------------------ */

static esp_err_t history_delete_handler(httpd_req_t *req)
{
    /* 删磁盘存档要碰 SD 卡，放到工作任务里做：httpd 任务不因一次 SD 写而卡住。 */
    return ai_http_dispatch(req, NULL, AI_JOB_ERASE);
}

static esp_err_t history_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    if (root == NULL || array == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(array);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "ai history");
    }
    cJSON_AddItemToObject(root, "messages", array);

    const size_t count = ai_service_history_count();
    for (size_t i = 0; i < count; i++) {
        ai_message_t msg;
        if (ai_service_history_get(i, &msg) != ESP_OK) {
            continue;
        }
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            continue;
        }
        cJSON_AddStringToObject(entry, "role", msg.role);
        cJSON_AddStringToObject(entry, "content", msg.content);
        cJSON_AddItemToArray(array, entry);
    }
    cJSON_AddNumberToObject(root, "count", (double)count);
    cJSON_AddStringToObject(root, "model", ai_service_get_model());
    cJSON_AddStringToObject(root, "state", ai_service_state_name(ai_service_get_state()));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    const esp_err_t err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing body");
    }

    cJSON *root = sys_json_parse(body, 32);
    free(body);
    if (root == NULL) {
        return http_reply_bad_request(req, "invalid json");
    }

    const cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    const cJSON *prompt = cJSON_GetObjectItemCaseSensitive(root, "system_prompt");
    const cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
    const cJSON *rounds = cJSON_GetObjectItemCaseSensitive(root, "history_rounds");
    const cJSON *save = cJSON_GetObjectItemCaseSensitive(root, "save_history");

    if (cJSON_IsString(key)) {
        /* Accepting an empty string is how a key is removed. */
        (void)ai_service_set_api_key(key->valuestring);
    }
    if (cJSON_IsString(prompt)) {
        (void)ai_service_set_system_prompt(prompt->valuestring);
    }
    if (cJSON_IsString(model)) {
        esp_err_t err = ai_service_set_model(model->valuestring);
        if (err != ESP_OK) {
            cJSON_Delete(root);
            return http_reply_bad_request(req, "model must be deepseek-chat or deepseek-reasoner");
        }
    }
    if (cJSON_IsNumber(rounds)) {
        (void)ai_service_set_history_rounds((uint32_t)rounds->valuedouble);
    }
    if (cJSON_IsBool(save)) {
        (void)sys_config_set_bool(SYS_KEY_AI_SAVE_HIST, cJSON_IsTrue(save));
    }
    cJSON_Delete(root);

    /* The API key is deliberately not echoed back. */
    char data[200];
    snprintf(data, sizeof(data),
             "{\"model\":\"%s\",\"has_api_key\":%s,\"history_rounds\":%u,"
             "\"save_history\":%s,\"state\":\"%s\"}",
             ai_service_get_model(), ai_service_has_api_key() ? "true" : "false",
             (unsigned)ai_service_get_history_rounds(),
             ai_service_save_history_enabled() ? "true" : "false",
             ai_service_state_name(ai_service_get_state()));
    return http_reply_ok(req, data);
}

static esp_err_t abort_post_handler(httpd_req_t *req)
{
    (void)ai_service_abort();
    return http_reply_ok(req, "{\"aborted\":true}");
}

/**
 * The current configuration, for the web console.
 *
 * A GET (not a POST with an empty body): reading is reading. The API key is
 * never echoed - only whether one is configured.
 */
static esp_err_t config_get_handler(httpd_req_t *req)
{
    char data[192];
    snprintf(data, sizeof(data),
             "{\"model\":\"%s\",\"has_api_key\":%s,\"history_rounds\":%u,"
             "\"save_history\":%s,\"state\":\"%s\"}",
             ai_service_get_model(), ai_service_has_api_key() ? "true" : "false",
             (unsigned)ai_service_get_history_rounds(),
             ai_service_save_history_enabled() ? "true" : "false",
             ai_service_state_name(ai_service_get_state()));
    return http_reply_ok(req, data);
}

esp_err_t ai_service_register_http(void)
{
    static const struct {
        const char *path;
        httpd_method_t method;
        http_route_handler_t handler;
    } routes[] = {
        { "/api/v1/ai/chat", HTTP_POST, chat_post_handler },
        { "/api/v1/ai/chat/stream", HTTP_POST, chat_stream_post_handler },
        { "/api/v1/ai/history", HTTP_GET, history_get_handler },
        { "/api/v1/ai/history", HTTP_DELETE, history_delete_handler },
        { "/api/v1/ai/config", HTTP_GET, config_get_handler },
        { "/api/v1/ai/config", HTTP_POST, config_post_handler },
        { "/api/v1/ai/abort", HTTP_POST, abort_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = http_router_register(routes[i].path, routes[i].method, routes[i].handler);
        if (err != ESP_OK) {
            SYS_LOGE(TAG, "register %s failed: %s", routes[i].path, esp_err_to_name(err));
            return err;
        }
    }
    SYS_LOGD(TAG, "%u AI routes registered", (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
