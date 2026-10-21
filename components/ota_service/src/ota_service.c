/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ota_service.c
 * @brief OTA 的核心：把新固件写进另一个槽位，校验，切过去启动。
 */

#include "ota_service.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ota_service_http.h"
#include "sys_utils.h"

static const char *TAG = "ota";

/** How much of the image is read at a time (32 KB fits in any task stack). */
#define OTA_CHUNK_SIZE (32 * 1024)

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static volatile ota_state_t s_state = OTA_STATE_IDLE;
static volatile uint32_t s_written;
static volatile uint32_t s_total;
static char s_error[96];

/**
 * True from the moment an upload is accepted until the device reboots.
 *
 * The state alone is not enough to keep a second upload out: `esp_ota_begin()`
 * erases the whole target slot *before* the state becomes WRITING, and that
 * erase takes seconds. A second upload arriving in that window would be
 * accepted, would target the same slot (the running partition has not changed
 * yet, so `esp_ota_get_next_update_partition()` returns the same one), and
 * would erase the image the first upload had just written and validated.
 * The same window exists between "image validated" and "reboot" (1.5 s).
 *
 * So the guard is a separate flag that is set before anything is touched.
 */
static volatile bool s_busy;

/** Record the reason of a failure and hand it back to the caller. */
static esp_err_t ota_fail(ota_state_t state, const char **err, const char *reason)
{
    s_state = state;
    if (reason != NULL) {
        strlcpy(s_error, reason, sizeof(s_error));
    }
    if (err != NULL) {
        *err = reason;
    }
    return ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static const esp_partition_t *running_partition(void)
{
    /* The running partition is the one the bootloader booted from; ask the OTA
     * subsystem rather than guessing by name. */
    const esp_partition_t *current = esp_ota_get_running_partition();
    if (current != NULL) {
        return current;
    }
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
}

const char *ota_service_running_partition(void)
{
    const esp_partition_t *p = running_partition();
    return p != NULL ? p->label : "?";
}

const char *ota_service_pending_partition(void)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return "";
    }
    const esp_partition_t *running = running_partition();
    if (running != NULL && strcmp(target->label, running->label) == 0) {
        return "";
    }
    return target->label;
}

const char *ota_service_boot_partition(void)
{
    /* The partition the bootloader will start **next** - this is the one that
     * esp_ota_set_boot_partition() points at. It is not the same thing as
     * "the slot that would be written next" (ota_service_pending_partition),
     * which is always simply the other slot. */
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (boot == NULL) {
        return "";
    }
    return boot->label;
}

bool ota_service_busy(void)
{
    return s_busy;
}

ota_state_t ota_service_state(void)
{
    return s_state;
}

uint32_t ota_service_written(void)
{
    return s_written;
}

uint32_t ota_service_total(void)
{
    return s_total;
}

const char *ota_service_error(void)
{
    return s_error;
}

esp_err_t ota_service_mark_valid(void)
{
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_ERR_NOT_SUPPORTED) {
        /* The rollback is not enabled in this build - nothing to confirm. */
        SYS_LOGD(TAG, "rollback not enabled, nothing to confirm");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "mark app valid failed: %s", esp_err_to_name(err));
        return err;
    }
    SYS_LOGI(TAG, "running image confirmed (rollback cancelled)");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Writing the image                                                    */
/* ------------------------------------------------------------------ */

/** A reboot from inside the HTTP handler would cut the response off. */
static void reboot_timer_cb(void *arg)
{
    (void)arg;
    SYS_LOGW(TAG, "rebooting into the new firmware");
    esp_restart();
}

/**
 * Read the rest of the request body and write it into the other slot, then
 * make that slot the next boot's partition.
 *
 * The firmware is 3+ MB, so it must **not** be read into one buffer
 * (http_read_body would malloc 3 MB - the internal heap is 180 KB).
 * The body is read in chunks and written straight into the slot.
 *
 * @param req             the request (its body is the image)
 * @param content_length  the image's length (0 = unknown)
 * @param[out] err        a human-readable reason on failure
 */
esp_err_t ota_service_receive(httpd_req_t *req, size_t content_length, const char **err)
{
    /* Take the slot before touching anything: see the comment on s_busy. */
    if (s_busy) {
        SYS_LOGW(TAG, "an upload is already in progress - refusing a second one");
        if (err != NULL) {
            *err = "已有一次升级正在进行，请等它结束";
        }
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        s_busy = false;
        return ota_fail(OTA_STATE_FAILED, err, "找不到 OTA 槽位（分区表是不是没刷对？）");
    }

    if (content_length > target->size) {
        s_busy = false;
        return ota_fail(OTA_STATE_FAILED, err, "固件比槽位还大");
    }

    esp_ota_handle_t handle = 0;
    const esp_err_t begin_err = esp_ota_begin(target,
                                              content_length > 0 ? content_length : OTA_SIZE_UNKNOWN,
                                              &handle);
    if (begin_err != ESP_OK) {
        SYS_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(begin_err));
        s_busy = false;
        (void)ota_fail(OTA_STATE_FAILED, err, "擦除目标槽位失败");
        return begin_err;
    }

    s_state = OTA_STATE_WRITING;
    s_written = 0;
    s_total = (uint32_t)content_length;
    s_error[0] = '\0';

    char *buf = heap_caps_malloc(OTA_CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        (void)esp_ota_abort(handle);
        s_busy = false;
        (void)ota_fail(OTA_STATE_FAILED, err, "内存不够（32 KB 的缓冲都没了）");
        return ESP_ERR_NO_MEM;
    }

    size_t remaining = content_length;
    bool truncated = false;
    for (;;) {
        const int want = (remaining > 0 && remaining < OTA_CHUNK_SIZE)
                             ? (int)remaining
                             : OTA_CHUNK_SIZE;
        const int got = (int)httpd_req_recv(req, buf, (size_t)want);
        if (got == 0) {
            /* The body ended. If the client announced more than it sent, the
             * image is cut short - that is a different problem from "this file
             * is not a firmware", and it must not be reported as one. */
            truncated = (remaining > 0);
            break;
        }
        if (got < 0) {
            SYS_LOGE(TAG, "recv failed: %d", got);
            (void)esp_ota_abort(handle);
            free(buf);
            s_busy = false;
            (void)ota_fail(OTA_STATE_FAILED, err, "读取上传内容中断");
            return ESP_FAIL;
        }
        if (remaining > 0) {
            remaining -= (size_t)got;
        }

        const esp_err_t write_err = esp_ota_write(handle, buf, (size_t)got);
        if (write_err != ESP_OK) {
            SYS_LOGE(TAG, "ota_write failed: %s", esp_err_to_name(write_err));
            (void)esp_ota_abort(handle);
            free(buf);
            s_busy = false;
            (void)ota_fail(OTA_STATE_FAILED, err, "写入固件失败（flash 写不进去）");
            return write_err;
        }
        s_written += (uint32_t)got;
    }
    free(buf);

    if (truncated) {
        SYS_LOGE(TAG, "the upload stopped early: %u of %u bytes",
                 (unsigned)s_written, (unsigned)s_total);
        (void)esp_ota_abort(handle);
        s_busy = false;
        (void)ota_fail(OTA_STATE_FAILED, err, "上传被中断（固件没传完）");
        return ESP_ERR_INVALID_SIZE;
    }

    s_state = OTA_STATE_VERIFYING;

    /* End the image: this is where the bootloader-format check happens, so a
     * truncated or foreign file is rejected here and not at the next boot. */
    if (esp_ota_end(handle) != ESP_OK) {
        SYS_LOGE(TAG, "ota_end failed: the image is not valid");
        s_busy = false;
        (void)ota_fail(OTA_STATE_FAILED, err, "固件校验不通过（不是有效的 ESP-IDF 镜像？）");
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t set_err = esp_ota_set_boot_partition(target);
    if (set_err != ESP_OK) {
        SYS_LOGE(TAG, "set boot partition failed: %s", esp_err_to_name(set_err));
        s_busy = false;
        (void)ota_fail(OTA_STATE_FAILED, err, "设置启动分区失败");
        return set_err;
    }

    SYS_LOGI(TAG, "new firmware written to %s; it will start at the next reboot",
             target->label);
    s_state = OTA_STATE_READY;
    /* No reboot here on purpose: the HTTP response of this request still has
     * to reach the browser, and resetting the chip takes the WiFi stack with
     * it - the answer would be lost and the console would call a successful
     * upgrade a failure. The caller reboots explicitly afterwards. */
    return ESP_OK;
}

esp_err_t ota_service_reboot(void)
{
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *running = running_partition();
    if (boot == NULL || running == NULL || strcmp(boot->label, running->label) == 0) {
        SYS_LOGW(TAG, "reboot refused: no new image is waiting");
        return ESP_ERR_INVALID_STATE;
    }

    SYS_LOGI(TAG, "rebooting into %s", boot->label);
    s_state = OTA_STATE_REBOOTING;

    /* A short delay so the answer to this request can still get out. */
    const esp_timer_create_args_t args = {
        .callback = reboot_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "th_ota",
    };
    esp_timer_handle_t timer = NULL;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        (void)esp_timer_start_once(timer, 800);
    } else {
        SYS_LOGW(TAG, "reboot timer unavailable");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ota_service_init(void)
{
    s_state = OTA_STATE_IDLE;
    s_error[0] = '\0';
    s_busy = false;
    s_written = 0;
    s_total = 0;

    const esp_err_t http_err = ota_service_register_http();
    if (http_err != ESP_OK) {
        SYS_LOGE(TAG, "register the HTTP routes failed: %s", esp_err_to_name(http_err));
        return http_err;
    }

    const esp_partition_t *running = running_partition();
    SYS_LOGI(TAG, "ready (running from %s, rollback %s)",
             running != NULL ? running->label : "?",
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
             "armed"
#else
             "disabled"
#endif
    );
    return ESP_OK;
}
