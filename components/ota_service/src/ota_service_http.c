/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ota_service_http.c
 * @brief OTA 的 HTTP 接口。
 *
 * ## 为什么上传要走自己的任务
 *
 * esp_http_server 只有唯一的 httpd 任务，URI handler 在里面**串行**执行：
 * handler 不返回，连 accept() 都不再跑，表现为「ping 得通、串口正常，
 * 但任何 TCP 连接都建不上」（AGENT.md §9.0.1 最后一条）。
 *
 * 一次固件上传是好几 MB、好几秒 —— 用普通 handler 的话，那几秒里设备的所有
 * HTTP 接口都会没反应（AI、待办、天气全堵住）。所以这里用
 * httpd_req_async_handler_begin() 把 socket 交给自有任务，handler 立刻返回。
 *
 * 任务栈的依据：这里不发 HTTPS 请求（固件是浏览器推过来的），
 * 但 flash 擦写很吃栈，8192 足够。
 */

#include "ota_service.h"

#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "http_router.h"
#include "sys_utils.h"

static const char *TAG = "ota_http";

#define OTA_HTTP_TASK_STACK 8192
#define OTA_HTTP_TASK_PRIO 5
#define OTA_HTTP_QUEUE_LEN 2

/** One upload in flight. */
typedef struct {
    httpd_req_t *req;   /**< the async handle */
    size_t length;      /**< the image's length (0 = unknown) */
} ota_job_t;

static QueueHandle_t s_jobs;
static TaskHandle_t s_worker;
static StaticSemaphore_t s_worker_lock_buf;
static SemaphoreHandle_t s_worker_lock;

/* ------------------------------------------------------------------------ */
/* Worker                                                                    */
/* ------------------------------------------------------------------------ */

static void ota_job_run(ota_job_t *job)
{
    const char *err = NULL;
    const esp_err_t rc = ota_service_receive(job->req, job->length, &err);

    char body[192];
    if (rc == ESP_OK) {
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"state\":\"rebooting\",\"written\":%u,\"total\":%u}",
                 (unsigned)ota_service_written(), (unsigned)ota_service_total());
    } else {
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"error\":\"%s\",\"state\":\"failed\"}",
                 err != NULL ? err : "升级失败");
    }

    httpd_resp_set_type(job->req, "application/json");
    httpd_resp_sendstr(job->req, body);

    /* No matter how it ended, the session slot has to go back - otherwise httpd
     * keeps holding it and the device stops accepting connections at all. */
    (void)httpd_req_async_handler_complete(job->req);
}

static void ota_http_worker(void *arg)
{
    (void)arg;
    ota_job_t *job = NULL;

    while (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE) {
        ota_job_run(job);
        free(job);
        job = NULL;
    }
    vTaskDelete(NULL);
}

static esp_err_t ota_http_ensure_worker(void)
{
    if (s_worker_lock == NULL) {
        s_worker_lock = xSemaphoreCreateMutexStatic(&s_worker_lock_buf);
        if (s_worker_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_worker_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_worker == NULL) {
        if (s_jobs == NULL) {
            s_jobs = xQueueCreate(OTA_HTTP_QUEUE_LEN, sizeof(ota_job_t *));
            if (s_jobs == NULL) {
                xSemaphoreGive(s_worker_lock);
                return ESP_ERR_NO_MEM;
            }
        }
        if (xTaskCreate(ota_http_worker, "th_ota_http", OTA_HTTP_TASK_STACK, NULL,
                        OTA_HTTP_TASK_PRIO, &s_worker) != pdPASS) {
            xSemaphoreGive(s_worker_lock);
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreGive(s_worker_lock);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Handlers                                                                 */
/* ------------------------------------------------------------------------ */

/**
 * POST /api/v1/ota/upload - the body IS the firmware image.
 */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    if (ota_service_state() == OTA_STATE_WRITING) {
        return http_reply_unavailable(req, "another upgrade is in progress");
    }

    const esp_err_t worker_err = ota_http_ensure_worker();
    if (worker_err != ESP_OK) {
        return http_reply_esp_error(req, worker_err, "ota upload");
    }

    ota_job_t *job = calloc(1, sizeof(ota_job_t));
    if (job == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "ota upload");
    }
    job->length = (size_t)req->content_len;

    /* Hand the socket over to our own task; the handler returns right away so
     * the device keeps serving everything else while the image goes in. */
    if (httpd_req_async_handler_begin(req, &job->req) != ESP_OK) {
        free(job);
        return http_reply_unavailable(req, "cannot start the upgrade");
    }
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        (void)httpd_req_async_handler_complete(job->req);
        free(job);
        return http_reply_unavailable(req, "the upgrade queue is full");
    }
    return ESP_OK;
}

/**
 * GET /api/v1/ota/status - what the upgrade is doing, and which slot is which.
 */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    char body[320];
    const char *state = "idle";
    switch (ota_service_state()) {
    case OTA_STATE_WRITING:   state = "writing"; break;
    case OTA_STATE_VERIFYING: state = "verifying"; break;
    case OTA_STATE_REBOOTING: state = "rebooting"; break;
    case OTA_STATE_FAILED:    state = "failed"; break;
    case OTA_STATE_IDLE:
    default:                 state = "idle"; break;
    }

    snprintf(body, sizeof(body),
             "{\"state\":\"%s\",\"written\":%u,\"total\":%u,\"error\":\"%s\","
             "\"running\":\"%s\",\"pending\":\"%s\"}",
             state,
             (unsigned)ota_service_written(),
             (unsigned)ota_service_total(),
             ota_service_error(),
             ota_service_running_partition(),
             ota_service_pending_partition());
    return http_reply_ok(req, body);
}

esp_err_t ota_service_register_http(void)
{
    static const struct {
        const char *path;
        httpd_method_t method;
        http_route_handler_t handler;
    } routes[] = {
        { "/api/v1/ota/upload", HTTP_POST, upload_post_handler },
        { "/api/v1/ota/status", HTTP_GET, status_get_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        const esp_err_t err = http_router_register(routes[i].path, routes[i].method,
                                                  routes[i].handler);
        if (err != ESP_OK) {
            SYS_LOGE(TAG, "register %s failed: %s", routes[i].path, esp_err_to_name(err));
            return err;
        }
    }
    SYS_LOGD(TAG, "%u ota routes registered", (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
