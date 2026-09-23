# calendar_service

**L3 业务组件（FR4）** — 墙钟、时区与月历。板子**没有 RTC**，时间只能靠 SNTP 现拿。

- 头文件：`include/calendar_service.h`
- 依赖：`sys_utils`、IDF `esp_netif`（SNTP）、`lwip`（`setenv`/`tzset`/`localtime_r`）、`http_router`、`cjson`
- 被谁依赖：`ui_pages`（`page_calendar.c`、`page_settings.c`）、`main`

## API 摘要

| 分组 | 函数 |
|---|---|
| 生命周期 | `calendar_service_init()`、`calendar_sync_ntp()` |
| 查询 | `calendar_time_synced()`、`calendar_get_now()`、`calendar_format_now()`、`calendar_get_month()`、`calendar_first_weekday()` |
| 时区 | `calendar_set_timezone()`、`calendar_get_timezone()` |
| 手动校时 | `calendar_set_time()`、`calendar_set_ntp_server()`、`calendar_get_ntp_server()` |
| HTTP | `calendar_service_register_http()`（由 `init()` 自动调用） |

## 关键约定

1. **没有 RTC，断电即失时**。上电后 `sys_time_ts()` 在 SNTP 成功之前返回 **0**
   （`sys_utils` 的墙钟由单调时钟 + 一个已同步锚点构成）；
   `calendar_time_synced()` 就是 `sys_time_synced()` 的转发，**不要**绕过它直接信任 `time(NULL)`。
2. **未同步时拒绝造时间**。`calendar_get_now()` 在未同步时返回 **`ESP_ERR_INVALID_STATE`**，
   绝不返回 1970（需求 FR4）。`calendar_format_now()` 此时把输出写成字面量 **`"时间未同步"`**；
   HTTP `GET /api/v1/calendar/now` 的 `datetime` 字段同样是 `"时间未同步"`，并带 `"synced":false,"ts":0`。
   **UI 必须显示"时间未同步"**（`page_calendar.c` / 状态栏已按此实现，`ui_core` 提供 `UI_STATUS_TIME_UNSYNCED`）。
3. **SNTP 懒启动**：`calendar_service_init()` **不会**立刻联网校时，而是
   `sys_event_subscribe(SYS_EVENT, SYS_EVENT_CONNECTIVITY, ...)`，链路起来时才 `sntp_start()`
   （所以"很晚才联网"也能自动补上）；若 `sys_status_get()` 显示已经在线，则立即启动一次。
   同步完成由 `sntp_sync_cb` 回调报告：它调用 `sys_time_set_ts(tv->tv_sec)` 把 `sys_utils` 的墙钟锚到真实值，
   再发 `CALENDAR_EVENT_TIME_SYNCED`（payload `int64_t`）并 `sys_status_publish()`。
   `esp_sntp_config_t.wait_for_sync = false`，所以初始化不阻塞调用方。
4. **时区是 POSIX TZ 字符串**（如 `CST-8` = UTC+8、`UTC0`）。`calendar_set_timezone()` 做
   `setenv("TZ", tz, 1)` + `tzset()`、写 NVS 键 `tz`、发 `CALENDAR_EVENT_TIMEZONE_CHANGED`（payload 为 TZ 字符串）。
   缓冲区 48 字节，超长返回 `ESP_ERR_INVALID_SIZE`。默认值来自 `CONFIG_TODOHELPER_TZ_DEFAULT`。
5. **手动校时/换服务器**：`calendar_set_time(ts)` 同时调 `settimeofday()` 与 `sys_time_set_ts()`，
   但**不持久化**——重启后仍需重新同步（`POST /api/v1/calendar/time`）。
   `calendar_set_ntp_server()` 写 NVS 键 `ntp_server`；若 SNTP 已在跑，会 `esp_netif_sntp_deinit()` 后用新地址重启
   （运行中的 SNTP 不会自动换服务器）。
6. **月历是纯算术，不需要时钟**。`calendar_get_month()` / `calendar_first_weekday()` 只用格里高利历规则
   （含闰年判断），`year` 允许 1970–2200，`month` 1–12；`calendar_first_weekday()` 用**正午**做 `mktime()`
   以避开午夜附近的 DST 边界。`days[0].tm_wday` 即该月 1 号的星期（0 = 周日）。
7. **事件**：`CALENDAR_EVENT` 的 `TIME_SYNCED`（`int64_t` 秒）、`SYNC_FAILED`（无 payload）、
   `TIMEZONE_CHANGED`（`const char *tz`）。`calendar_sync_ntp(0)` 用默认超时 10 s，
   超时返回 `ESP_ERR_TIMEOUT` 并发 `SYNC_FAILED`。

## HTTP 路由表（需求 §8.4）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/calendar/now` | `synced`、`ts`、`tz`、`ntp_server`；已同步时另有 `datetime`/`year`/`month`/`day`/`weekday` |
| GET | `/api/v1/calendar/month?year=&month=` | `days`、`first_weekday`、`weekdays[]`；本月时带 `today`，否则 `today:null` |
| POST | `/api/v1/calendar/sync` | 主动 `calendar_sync_ntp(0)`；失败返回 **503** `"NTP sync failed (is the network up?)"` |
| POST | `/api/v1/calendar/time` | body `{"ts":<unix秒>}`，手动设钟 |
| POST | `/api/v1/calendar/tz` | body `{"tz":"CST-8"}`，改时区并持久化 |

## Kconfig

- `CONFIG_TODOHELPER_NTP_SERVER`（默认 `ntp.aliyun.com`）— NVS 键 `ntp_server` 缺失时使用。
- `CONFIG_TODOHELPER_TZ_DEFAULT`（默认 `CST-8`）— NVS 键 `tz` 缺失时使用。

## 测试方法

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build
idf.py -p COM34 flash monitor
```

期望串口输出（WiFi 连上之前）：

```
I (xxx) TH:calendar: ready (tz 'CST-8', ntp 'ntp.aliyun.com', synced=0)
```

联网后：

```
I (xxx) TH:calendar: SNTP started (ntp.aliyun.com)
I (xxx) TH:calendar: time synchronised: 1730000000
```

断网验证（未同步态）：

```powershell
curl.exe http://todohelper.local/api/v1/calendar/now
# {"code":0,...,"data":{"synced":false,"ts":0,"tz":"CST-8","ntp_server":"ntp.aliyun.com","datetime":"时间未同步"}}
curl.exe "http://todohelper.local/api/v1/calendar/month?year=2026&month=1"
# 未同步也应正常返回：月历是纯算术
```

UI 上打开"日历"页：未同步时日期区域显示**"时间未同步"**而不是 `1970-01-01`；
等 SNTP 完成后页面（订阅了 `CALENDAR_EVENT_TIME_SYNCED`）应自动刷新。
