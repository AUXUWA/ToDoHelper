# sys_utils

**L0 基础组件** — 全项目共用的日志、事件循环、时间与 NVS 配置。所有其他组件都依赖它。

- 头文件：`include/sys_utils.h`（聚合）、`sys_log.h`、`sys_err.h`、`sys_keys.h`
- 依赖：IDF `esp_event` / `nvs_flash` / `esp_timer` / `log`
- 无上游 ToDoHelper 依赖

## API 摘要

| 分组 | 函数 |
|---|---|
| 日志 | `sys_log_set_level()`、`SYS_LOGE/W/I/D(tag, fmt, ...)` |
| 事件 | `sys_events_init()`、`sys_events_get_loop()`、`sys_event_post_to()`、`sys_events_post()`、`sys_event_subscribe()` |
| 时间 | `sys_time_ms()`、`sys_time_ts()`、`sys_time_synced()`、`sys_time_set_ts()`、`sys_time_clear()` |
| 配置 | `sys_config_init()`、`sys_config_get/set_str/i32/bool()`、`sys_config_erase()`、`*_or()` 便捷版 |

## 关键约定

1. **一个共享事件循环**。组件不自己建 loop，统一用 `sys_events_get_loop()` + 自己的事件基址。
2. **时间必须判同步**。本板**无 RTC**，`sys_time_ts()` 在 NTP 成功前返回 **0**；
   UI 显示"时间未同步"，**不得**显示 1970（需求 FR4）。
3. **NVS 唯一入口**。组件不得调用 `nvs_open()`；键名统一在 `sys_keys.h` 定义。
   敏感键（`wifi_pass`/`ai_api_key`/`auth_token`/`weather_key`）**不得写日志**。
4. 事件数据只在回调期间有效，接收方需自行拷贝。

## Kconfig

`CONFIG_TODOHELPER_LOG_LEVEL`（默认 2=INFO）、`CONFIG_TODOHELPER_NVS_NAMESPACE`（默认 `todohelper`）、
`CONFIG_TODOHELPER_EVENT_QUEUE_SIZE` / `_TASK_STACK` / `_TASK_PRIO`。

## 测试方法

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build
idf.py -p COM34 monitor
```

期望串口输出（上电后）：

```
I (xxx) TH:sys_config: config ready (namespace 'todohelper')
I (xxx) TH:sys_events: shared event loop ready
```

手工验证：`sys_config_set_i32("first_boot", 1)` 后重启，`sys_config_get_i32` 应返回 1；
`sys_time_synced()` 在未联网时应为 `false` 且 `sys_time_ts() == 0`。
