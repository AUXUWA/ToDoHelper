/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ota_service.h
 * @brief 固件在线升级（OTA）：把新固件写进另一个槽位，然后切过去启动。
 *
 * ## 为什么要两个槽位
 *
 * 升级不能覆盖正在运行的固件 —— 写坏了设备就起不来了。所以 flash 上有
 * 两个 app 槽位（ota_0 / ota_1，见 partitions.csv），设备一次只从一个
 * 槽位启动：把新固件写进**另一个**槽位，写完校验通过再把它设为下次启动
 * 的分区。
 *
 * ## 写坏了怎么办：回滚
 *
 * 开了 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 之后，bootloader 在启动新固件后
 * 会给它一个时间窗（默认 10 s）去调用 esp_ota_mark_app_valid_cancel_rollback()
 * 「认下」这个版本。新固件没来得及认（崩了 / 卡死了），bootloader 就自动
 * 退回上一个版本 —— 设备不会因为一次坏升级变砖。
 *
 * ota_service_mark_valid() 就是新固件启动成功后要调的那一句。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 升级过程的状态。 */
typedef enum {
    OTA_STATE_IDLE = 0,   /**< 没事发生 */
    OTA_STATE_WRITING,    /**< 正在把固件写进另一个槽位 */
    OTA_STATE_VERIFYING,  /**< 写完了，正在校验镜像 */
    OTA_STATE_REBOOTING,  /**< 校验通过，等着重启进新固件 */
    OTA_STATE_FAILED,     /**< 出错了（s_error 里有原因） */
} ota_state_t;

/**
 * @brief 把请求体（固件镜像）写进另一个槽位并切过去启动。
 *
 * 由 OTA 的 HTTP 工作任务调用（不在 httpd 任务里做，见 ota_service_http.c）。
 *
 * @param req             the request whose body is the image
 * @param content_length  the image's length (0 = unknown)
 * @param[out] err        a human-readable reason on failure
 */
esp_err_t ota_service_receive(httpd_req_t *req, size_t content_length, const char **err);

/**
 * @brief 初始化 OTA 服务并注册 HTTP 路由。
 *
 * 开机就会调用（app_main 的模块表）。
 */
esp_err_t ota_service_init(void);

/**
 * @brief 认下当前运行的固件（取消回滚）。
 *
 * **新固件启动成功后必须调用**，而且要尽早 —— 超过 bootloader 给的时间窗
 * bootloader 就会以为这个版本起不来，自动退回上一个版本。
 * 没开回滚时它什么也不做，安全。
 */
esp_err_t ota_service_mark_valid(void);

/** @brief 当前状态（给 HTTP 接口和 UI 用）。 */
ota_state_t ota_service_state(void);

/** @brief 已写入的字节数。 */
uint32_t ota_service_written(void);

/** @brief 这次升级的固件总长度（0 = 还没开始）。 */
uint32_t ota_service_total(void);

/** @brief 上一次失败的原因（空 = 没有失败）。 */
const char *ota_service_error(void);

/** @brief 设备现在跑的是哪个槽位（ota_0 / ota_1）。 */
const char *ota_service_running_partition(void);

/** @brief 待启动的槽位（没有待启动的 = 空串）。 */
const char *ota_service_pending_partition(void);

#ifdef __cplusplus
}
#endif
