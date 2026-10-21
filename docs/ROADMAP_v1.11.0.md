# ROADMAP v1.11.0 —— OTA（固件在线升级）

> 需求：实现 OTA。
> 先问一句「flash 装得下吗」—— 装不下就得先改分区表，而改分区表可能动到用户的数据。

## 1. 资源评估：装得下，但 app 的空间小了一半

原来的分区表是**一个 8 MB 的 app 分区**（factory）。OTA 要两个槽位
（设备一次只从一个启动，升级不碰正在跑的那个），所以 app 的空间
从 8 MB 变成**一个槽位的大小**。

16 MB flash 的几何（`storage` 在中间，2 MB，里面是设备本地的数据）：

```
nvs      0x9000   0x6000     24 KB
otadata  0xF000   0x2000      8 KB    ← 新增（bootloader 记「下次启动哪个槽位」）
phy_init 0x11000  0x1000      4 KB
ota_0    0x20000  0x3E0000  3.97 MB   ← 原来 0x10000/0x800000 的 factory
ota_1    0x400000 0x410000  4.06 MB
storage  0x810000 0x200000    2 MB     ← 偏移量**没动**
assets   0xA10000 0x5F0000  5.75 MB   ← 偏移量**没动**
```

**storage 和 assets 的偏移量刻意没动** —— storage 里是待办、单词进度、AI 历史，
动了那些数据就找不到了。（assets 里只有 16 KB 的图标，但它是构建产物，缩它很安全；
只是缩了也帮不到 app —— app 槽位在 storage 之前，够不着。）

**代价**：app 从 8 MB 变成 3.97 MB，当前固件 3.28 MB → **余量 0.7 MB**。
固件涨过 3.9 MB 就装不下了。

## 2. 三条安全线（都在真机上验过）

| # | 措施 | 怎么验的 | 结果 |
|---|---|---|---|
| 1 | 写进**另一个**槽位 | 传一个 1.11.0 的镜像 | 设备写进 ota_1，重启后 `running: ota_1`，版本 1.11.0 ✓ |
| 2 | 写完**校验** | 传 1 MB 的**截断**镜像 | 当场回「固件校验不通过」，**设备没重启**，继续跑旧固件 ✓ |
| 3 | 开机**回滚** | 故意做一个不「认下自己」的镜像传上去，再重启一次 | 第一次重启跑的是那个镜像；**第二次重启 bootloader 把它退回去**了，设备回到之前的版本 ✓ |

第 3 条是 OTA 最重要的一条：没有它，一次坏升级就变砖。
做法是 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` +
新固件启动后调用 `esp_ota_mark_app_valid_cancel_rollback()`「认下」自己。

## 3. 这一轮做了什么

* **新组件 `components/ota_service/`**：
  * `POST /api/v1/ota/upload` —— **请求体就是固件镜像**；
  * `GET /api/v1/ota/status` —— 状态、已写字节、当前槽位、待启动槽位；
  * `ota_service_mark_valid()` —— 新固件启动后「认下」自己（app_main 里调）。
* **网页端「固件升级」卡片**：选文件 → 上传（带进度条）→ 结果。
* **分区表改成两个 app 槽位** + 新增 `otadata` 分区。

## 4. 两个必须记住的工程细节

* **固件不能一次性读进内存。** 3.3 MB，而内部堆只有 180 KB ——
  `http_read_body()` 会 malloc 3.3 MB，直接就没了。
  所以请求体是**分块读、分块写**（32 KB 一块）。
* **上传不能占着 httpd 任务。** esp_http_server 只有一个 httpd 任务，
  handler 不返回就连 `accept()` 都不跑（ping 通、串口正常，但任何 TCP 都建不上）。
  所以用 `httpd_req_async_handler_begin()` 把 socket 交给自有任务
  （和 ai_service 同一套做法）。

## 5. 真机验证记录

```
# 升级前
running: ota_0, version 1.10.0

# 传 1.11.0 的镜像（3.3 MB，约 70 秒）
→ 重启
running: ota_1, version 1.11.0
待办、API Key、WiFi 凭据、storage 里的 120 KB 全部还在 ✓

# 传一个截断的镜像
→ {"ok":false,"error":"固件校验不通过（不是有效的 ESP-IDF 镜像？）"}
→ 设备没重启，版本不变 ✓

# 传一个不认自己的镜像
→ 重启后 running: ota_0（那个镜像）
→ 再重启一次
→ running: ota_1（bootloader 退回去了）✓
```

## 6. 还没做的

* **设备屏幕上没有升级界面**：升级是网页端发起的，设备屏幕上只有一个
  「正在重启」的提示（现在连这个都没有）。要做的话：设置页加一个
  「系统更新」入口 + 一个进度条（升级进度用 SSE 推给设备）。
* **没有「从 URL 升级」**：现在只能从网页端传文件。设备自己下载
  （`esp_http_client` → `esp_ota_write`）也可以，那条路能做无人值守升级。
* **app 槽位余量只有 0.7 MB** —— 再加功能要注意，或者缩 assets 分区
  （但它帮不到前面的槽位，得挪 storage）。
