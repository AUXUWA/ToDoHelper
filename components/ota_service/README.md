# ota_service —— 固件在线升级（OTA）

把新固件传上来，设备写进**另一个**槽位，校验通过后「上膛」，
下一次启动就跑它。写坏了不会变砖。

## 用法

**网页端**（推荐）：首页 → 「固件升级」→ 选 `build/todohelper.bin` → 上传并升级。
上传期间设备照常工作（待办、AI、天气都不受影响），上传成功之后设备会重启一次
（约 10 秒后刷新页面就能看到新版本）。

**命令行**：

```bash
curl -X POST http://192.168.137.138/api/v1/ota/upload \
     -H "Content-Type: application/octet-stream" \
     --data-binary @build/todohelper.bin
# → {"ok":true,"state":"ready","written":3284624,"total":3284624,"boot":"ota_0"}

curl -X POST http://192.168.137.138/api/v1/ota/reboot
# → {"ok":true,"state":"rebooting"}
```

| 路由 | 说明 |
|---|---|
| `POST /api/v1/ota/upload` | **请求体就是固件镜像**。写完校验通过就切到新槽位并「上膛」（下次启动就跑它） |
| `POST /api/v1/ota/reboot` | **v1.12.0 新增**。重启进刚写好的固件；没有待启动的镜像时回 503 |
| `GET /api/v1/ota/status` | `{state, written, total, error, running, pending, boot, busy}` |

`state`：`idle` / `writing` / `verifying` / `ready` / `rebooting` / `failed`。

槽位字段：

* `running` —— 现在跑的是哪个槽位；
* `boot` —— **下次启动**会跑哪个（等于 `running` 表示没有待启动的）；
* `pending` —— 下一个会被**写进去**的槽位（**永远等于「另一个」**，
  别拿它判断「有没有升级等着启动」，那是 `boot` 的事）；
* `busy` —— 现在正忙（擦除/写入/校验/上膛期间都是 true）。

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
* **重启是单独一步**：在上传的 handler 里直接重启（或定时重启）会把**响应**切掉 ——
  芯片复位把 WiFi 协议栈一起带走，那个响应就丢了，
  浏览器看到的是「上传失败」，而设备其实已经换了固件。
  所以上传只负责「写好并上膛」，重启由 `POST /api/v1/ota/reboot` 发起。
* **一次只允许一个升级**：从请求被接受到重启之前都算忙，
  这期间再来一次上传会回 503。没有这道护栏，第二次上传会写**同一个**槽位
  （还没重启，`esp_ota_get_next_update_partition()` 返回的就是另一个），
  把刚校验通过的镜像擦掉，甚至让设备从一个写了一半的镜像启动。

## 槽位与容量的坑

app 的空间是**一个槽位的大小**，不是两个：现在是 `0x3E0000` = 3.97 MB，
当前固件 3.28 MB —— **余量只有 0.7 MB**。固件涨过 3.9 MB 就装不下了
（构建会报分区放不下）。要再腾地方，只能缩 `assets` 分区
（它有 5.75 MB，只用了 16 KB）或挪 `storage`（**不建议**：
storage 里是设备本地的数据，动了偏移量那些数据就找不到了）。
