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
    OTA_STATE_READY,      /**< 镜像已就绪并「上膛」—— 下次重启就进它 */
    OTA_STATE_REBOOTING,  /**< 正在重启 */
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
 * @brief 重启进新固件（前提：有一个已经就绪、等着启动的镜像）。
 *
 * **为什么重启不自动做**：上传请求的响应还没送到浏览器的时候重启，
 * 那个响应就丢了（芯片复位会把 WiFi 协议栈一起带走）—— 网页上看到的
 * 是「上传失败」，而固件其实已经写好了。所以分成两步：
 * 先上传（一定有响应），再显式重启。
 *
 * @return ESP_OK when a reboot was scheduled.
 */
esp_err_t ota_service_reboot(void);

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

/**
 * @brief **下一个会被写进去**的槽位（也就是「另一个槽位」）。
 *
 * 注意：这个**不是**「下次启动会跑哪个」。它永远等于另一个槽位，
 * 哪怕根本没有待启动的固件 —— 用它判断「有没有升级等着启动」会看错。
 * 要判断那个，用 ota_service_boot_partition()。
 */
const char *ota_service_pending_partition(void);

/**
 * @brief **下次启动会跑**的槽位（bootloader 按 otadata 选的那个）。
 *
 * 和 ota_service_running_partition() 相同 = 没有待启动的固件。
 */
const char *ota_service_boot_partition(void);

/**
 * @brief 现在是不是正忙（一次升级正在进行中）。
 *
 * 从「请求被接受」到「重启」之间都是忙的 —— 包括擦除目标槽位的那几秒，
 * 和校验通过之后、重启之前的 1.5 秒。这两段里都**不能**再来一次上传：
 * 第二次会擦掉第一个刚写好的镜像，甚至让设备从一个写了一半的槽位启动。
 */
bool ota_service_busy(void);

#ifdef __cplusplus
}
#endif
