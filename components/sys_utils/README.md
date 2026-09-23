# sys_utils

**L0 基础组件** — 全项目共用的日志、事件循环、时间、NVS 配置、堆诊断与 cJSON 分配策略。
所有其他组件都依赖它。

- 头文件：`include/sys_utils.h`（聚合）、`sys_log.h`、`sys_err.h`、`sys_keys.h`
- 依赖：IDF `esp_event` / `nvs_flash` / `esp_timer` / `log` / `heap`；托管 `espressif/cjson`
  （仅 `sys_json.c` 用来装分配钩子）
- 无上游 ToDoHelper 依赖

## API 摘要

| 分组 | 函数 |
|---|---|
| 日志 | `sys_log_set_level()`、`SYS_LOGE/W/I/D(tag, fmt, ...)` |
| 日志历史 | `sys_log_recent_seq()`、`sys_log_recent_count()`、`sys_log_recent_get()`（供启动页在屏幕上显示启动日志） |
| 事件 | `sys_events_init()`、`sys_events_get_loop()`、`sys_event_post_to()`、`sys_events_post()`、`sys_event_subscribe()` |
| 时间 | `sys_time_ms()`、`sys_time_ts()`、`sys_time_synced()`、`sys_time_set_ts()`、`sys_time_clear()` |
| 配置 | `sys_config_init()`、`sys_config_get/set_str/i32/bool()`、`sys_config_erase()`、`*_or()` 便捷版 |
| 堆诊断 | `sys_heap_log(stage)`、`sys_heap_largest_internal()`（`internal / largest / psram / int-low` 四列） |
| JSON 分配 | `sys_json_use_psram()`（把 cJSON 的分配改成先 PSRAM、失败回退默认堆） |

## 为什么要管 cJSON 的分配（`sys_json.c`）

`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` 让**所有 ≤4 KB 的分配优先落在内部 RAM**，而 cJSON
每个 JSON 元素约 56 B —— 解析一份大 JSON（CET-6 词库 546 KB / 约 7000 个节点）会在解析期间
把 ~107 KB 的内部空闲抽干。此时任何**只能来自内部 RAM** 的并发分配（例如正在连网的 WiFi 驱动
`esp_timer_create()`）就会拿到 `ESP_ERR_NO_MEM`，而它外面包着 `ESP_ERROR_CHECK` → 直接 abort。
**这就是"插 SD 卡即崩溃重启循环"的真因**（`AGENT.md §9.0.5`）。

`sys_json_use_psram()` 用 `cJSON_InitHooks()` 把 cJSON 的 `malloc/free` 换成
"`MALLOC_CAP_SPIRAM` 优先、失败回退 `MALLOC_CAP_DEFAULT`"，在 `app_main()` 最早期调用一次
（钩子是全局的）。释放统一用 `heap_caps_free()`，因此**混用**普通 `malloc()` 与 cJSON 分配的内存
也安全（`heap_caps_free` 会找到所属堆）。

## 堆诊断的四列（`sys_heap.c`）

```
I (1862) TH:heap: after ui_core        internal    260 KB (largest    216 KB)  psram 11255 KB  int-low    247 KB
```

- `internal`：内部 RAM 当前空闲；`largest`：内部 **DMA-capable** 最大连续块（`esp_wifi_init()`
  / I2S / SDMMC 真正争的就是它）；
- `psram`：PSRAM 空闲；
- **`int-low`**：内部 RAM 的**历史最低水位**（`heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)`）。
  它替代了原来那个被 12 MB PSRAM 主导、永远不动的 `esp_get_minimum_free_heap_size()`。
  **掉到个位数 KB 就说明某个瞬时峰值把内部 RAM 抽干过** —— 这正是插卡崩溃的判据，
  而当时的当前空闲量看起来还是健康的 107 KB。

## 日志历史（屏幕启动日志，`sys_log.h`）

`sys_log_write()` 在照常输出串口的同时，把每条日志额外抄一份进环形缓冲，供
`ui_pages` 的启动页在 800×480 屏幕上显示（见 `components/ui_pages/README.md`）。
**串口输出完全不变，只是新增。**

- 容量 `SYS_LOG_RECENT_CAPACITY = 64` 行 × `SYS_LOG_RECENT_LINE_LEN = 80` 字节，
  加上每行元数据（level / uptime_ms / seq）约 5.9 KB；
- **优先放 PSRAM**（`heap_caps_malloc(MALLOC_CAP_SPIRAM)`，失败才退回内部 RAM）：
  本板内部 RAM 只有 ~310 KB 而 PSRAM 有 12.4 MB（AGENT.md §9.0），这条路径不进
  `.bss`，不占用内部 RAM；分配只在 `sys_log_init()` 里做一次，此后访问器只走指针；
- **无锁、无分配、不阻塞**：写入路径只多一次原子加和两次普通写。每行自带序号
  做 seqlock（写入中为 0，写完再发布），读到的行要么完整要么返回 `false`，
  调用方下一拍重试即可；
- 只记录 `sys_log_write()` 实际输出的行（受 `sys_log_set_level()` 过滤），
  **不含 IDF/第三方组件自己的 `ESP_LOGx`**（那些不经过本组件）。若要把它们也显示出来，
  得改用 `esp_log_set_vprintf()` 钩子，且要自己处理 ANSI 颜色前缀，当前未做。

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
I (xxx) TH:sys_log: log history: 64 lines x 80 bytes in PSRAM
```

`sys_log: log history: …` 说明屏幕启动日志的环形缓冲已就绪（落在 PSRAM 或内部 RAM）；
若出现 `no room for the …-byte log history`，则屏幕上不会有启动日志，但串口不受影响。

手工验证：`sys_config_set_i32("first_boot", 1)` 后重启，`sys_config_get_i32` 应返回 1；
`sys_time_synced()` 在未联网时应为 `false` 且 `sys_time_ts() == 0`。
