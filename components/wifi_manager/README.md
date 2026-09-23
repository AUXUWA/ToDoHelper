# wifi_manager

**L3 业务组件（FR1）** — WiFi 扫描、连接、凭据持久化、断线自动重连、状态发布。

- 头文件：`include/wifi_manager.h`
- 依赖：`sys_utils`、`bsp`（LED 状态灯）、IDF `esp_wifi` / `esp_netif` / `nvs_flash`
- 发布状态到 `sys_status_*`，**被依赖方向为零**（UI 只订阅事件或读 `sys_status_get()`）

## API 摘要

| 分组 | 函数 |
|---|---|
| 生命周期 | `wifi_manager_init()`（内部会调 `auto_connect`，可用 Kconfig 关闭） |
| 扫描 | `wifi_manager_start_scan()`（异步）、`wifi_manager_scan()`（阻塞）、`wifi_manager_get_scan_result()`（读缓存） |
| 连接 | `wifi_manager_connect()`、`wifi_manager_disconnect()`、`wifi_manager_auto_connect()` |
| 查询 | `wifi_manager_get_state()`、`_get_ip()`、`_get_mac()`、`_get_ssid()`、`_get_rssi()`、`_is_connected()`、`_state_name()` |
| 配置 | `wifi_manager_save_config()`、`wifi_manager_clear_config()` |

## 事件（基址 `WIFI_MGR_EVENT`）

`SCAN_DONE` · `CONNECTING` · `CONNECTED` · `GOT_IP` · `DISCONNECTED`(payload `uint8_t reason`) · `FAILED`(payload `uint8_t reason`)

同时向 `SYS_EVENT` 发 `SYS_EVENT_CONNECTIVITY`（payload `bool online`），
并更新 `sys_status_t` 的 `wifi_connected / wifi_connecting / wifi_ssid / wifi_ip / wifi_rssi / online`。

## NVS 键（经 `sys_config_*`）

| 键 | 类型 | 说明 |
|---|---|---|
| `wifi_ssid` | str | 上次成功配置的 SSID |
| `wifi_pass` | str | 密码（**永不写日志**，需求 §8.8） |
| `wifi_auto` | i32 | 是否开机自动重连（默认 1） |

## 行为约定

1. **开机自动重连**：`wifi_manager_init()` 末尾按 Kconfig `TODOHELPER_WIFI_AUTO_CONNECT`
   自动调用 `auto_connect()`；无凭据时返回 `ESP_ERR_NOT_FOUND` 并只打 INFO 日志，
   **不算启动失败**。
2. **断线重连**：非主动 `disconnect()` 引起的掉线会按
   `TODOHELPER_WIFI_RECONNECT_ATTEMPTS`（默认 5）× `..._DELAY_MS`（默认 3 s）重试；
   成功拿到 IP 后重置重试预算，用尽则进入 `WIFI_MGR_FAILED` 并点亮红色状态灯。
3. **阻塞扫描**：`wifi_manager_scan()` 内部用 `esp_wifi_scan_start(block=true)`，
   **禁止**在 WiFi 事件回调或共享事件循环里调用（会自锁）；UI/HTTP 用异步版本。
4. **网络存储**：`esp_wifi_set_storage(WIFI_STORAGE_RAM)` —— 凭据只由本组件经
   `sys_config_*` 管理，不让 WiFi 驱动另存一份。
5. **hostname**：固定 `todohelper`，供 DHCP 与后续 mDNS（`todohelper.local`）使用。

## Kconfig

`TODOHELPER_WIFI_AUTO_CONNECT`（默认 y）、`TODOHELPER_WIFI_RECONNECT_ATTEMPTS`（默认 5）、
`TODOHELPER_WIFI_RECONNECT_DELAY_MS`（默认 3000）、`TODOHELPER_WIFI_SCAN_MAX_APS`（默认 24）。

## 测试方法

```powershell
idf.py build && idf.py -p COM34 flash monitor
```

期望日志：

```
I (xxx) TH:wifi: ready (hostname 'todohelper', mac 1C:29:04:D0:71:B6)
I (xxx) TH:wifi: no saved network yet            # 首次上电
I (xxx) TH:wifi: auto connecting to stored network 'MyWiFi'
I (xxx) TH:wifi: state -> connecting
I (xxx) TH:wifi: state -> connected
I (xxx) TH:wifi: got ip 192.168.1.23 (ssid 'MyWiFi', -52 dBm)
```

手工验证：
1. 首页状态栏 WiFi 由"未连接"→"连接中"→"-52 dBm"（绿）；
2. 断开路由器：状态栏回到"未连接"（灰），串口出现 `disconnected (reason 200)` 与
   `reconnecting (N attempts left)`；路由器恢复后自动重连；
3. `idf.py -p COM34 monitor` 下断电重启，应免配置自动连上（凭据已存 NVS）。
