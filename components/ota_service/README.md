# ota_service —— 固件在线升级（OTA）

把新固件传上来，设备写进**另一个**槽位，校验通过后重启进新固件。
写坏了不会变砖。

## 用法

**网页端**（推荐）：首页 → 「固件升级」→ 选 `build/todohelper.bin` → 上传并升级。
上传期间设备照常工作（待办、AI、天气都不受影响），只有最后会重启一次（约 10 秒）。

**命令行**：

```bash
curl -X POST http://192.168.137.138/api/v1/ota/upload \\
     -H "Content-Type: application/octet-stream" \\
     --data-binary @build/todohelper.bin
```

| 路由 | 说明 |
|---|---|
| `POST /api/v1/ota/upload` | **请求体就是固件镜像**。写完校验通过就切到新槽位并重启 |
| `GET /api/v1/ota/status` | `{state, written, total, error, running, pending}` |

`state`：`idle` / `writing` / `verifying` / `rebooting` / `failed`。

## 三条安全线

1. **写进另一个槽位。** flash 上有两个 app 槽位（`ota_0` / `ota_1`，见
   `partitions.csv`），设备一次只从一个启动，升级不碰正在跑的那个。
2. **写完校验。** `esp_ota_end()` 会按 ESP-IDF 的镜像格式检查一遍 ——
   截断的文件、外来的文件当场被拒，设备**不重启**，继续跑旧固件。
3. **开机回滚。** 开了 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 之后，新固件起来
   要调用 `esp_ota_mark_app_valid_cancel_rollback()`「认下」自己（app_main 里
   `ota_service_mark_valid()` 干这件事）。认不了（崩了 / 卡死了），
   bootloader 下次启动就**退回上一个版本**。

这三条都在真机上验过（见 `docs/ROADMAP_v1.11.0.md`）。

## 为什么是这个形状

* **分块读、分块写**：固件 3.3 MB，内部堆只有 180 KB ——
  `http_read_body()` 会 malloc 3.3 MB，直接就没了。所以用
  `httpd_req_recv()` 每次读 32 KB，直接 `esp_ota_write()` 进槽位。
* **上传走自己的任务**：esp_http_server 只有一个 httpd 任务，URI handler 在里面
  **串行**执行：handler 不返回，连 `accept()` 都不跑，表现为「ping 得通、串口正常，
  但任何 TCP 连接都建不上」（`AGENT.md` §9.0.1 最后一条）。
  所以用 `httpd_req_async_handler_begin()` 把 socket 交给自有任务，
  handler 立刻返回（和 ai_service 同一套做法）。
* **重启用定时器**：在 handler 里直接重启会把响应切掉（浏览器看到的是失败，
  设备其实已经换了固件）。所以重启推迟 1.5 秒。

## 槽位与容量的坑

app 的空间是**一个槽位的大小**，不是两个：现在是 `0x3E0000` = 3.97 MB，
当前固件 3.28 MB —— **余量只有 0.7 MB**。固件涨过 3.9 MB 就装不下了
（构建会报分区放不下）。要再腾地方，只能缩 `assets` 分区
（它有 5.75 MB，只用了 16 KB）或挪 `storage`（**不建议**：
storage 里是设备本地的数据，动了偏移量那些数据就找不到了）。
