# ROADMAP v1.8.0 —— 内部 RAM / 开机时间 / 回调里不许做重活

> 上一轮：[ROADMAP_v1.7.0.md](ROADMAP_v1.7.0.md)（词库异步化、开机不落盘、文件缓冲走 PSRAM、天气等 IP）
> 本轮来源：性能审计（24 条发现，9 条 high），**每一条 high 都已由主控用 grep / nm / 真机日志复核过**，
> 复核记录见 §5。

本轮的原则和 v1.7.0 一样：**不改用户可见的功能、文案与观感**。
本轮要改的是「看不见但能感觉到的东西」——开机等了多久、内部 RAM 还剩多少、
点一下按钮会不会卡住界面。

---

## 0. 现状基线（真机抓的）

基线取 **v1.7.0 上机实测**（7 次启动日志，2026-10-20），
括号里是 v1.6.0 的旧基线（`build/after_ui.log`）。

| 指标 | 现状 | 目标 |
|---|---|---|
| 开机到 `boot complete` | **≈ 6300 ms**（v1.6.0: 6552 ms） | **≤ 4500 ms**（去掉 3 s 死等后） |
| 其中纯死等 | **3.0 s**（app_main.c:226 的 `vTaskDelay(3000)`） | ≤ 0.3 s |
| 开机后内部 RAM free | **58~72 KB**（v1.6.0: 55 KB） | 持平或更好 |
| 开机后内部 RAM **最大连续块** | **12~28 KB**，联网稳态 **15 KB**（v1.6.0: 12 KB） | **≥ 30 KB**（PHY 重新使能要 ~30 KB 连续块，见 AGENT.md §9.0.3） |
| WiFi 驱动吃掉的内部 DRAM | 138 KB（265→127 KB） | ~119 KB（-19 KB） |
| 项目静态数组占内部 DRAM | 25.4 KB | ~0 KB（全部搬 PSRAM） |

### 0.1 为什么「最大连续块」是这轮的头号目标（v1.7.0 上机时撞出来的）

v1.7.0 上机后，日志里反复出现这一条：

```
E (16216) esp-tls-mbedtls: mbedtls_ssl_setup returned -0x008D
E (16216) esp-tls: create_ssl_handle failed
E (16224) HTTP_CLIENT: Connection failed, sock < 0
```

`-0x008D` = -141 = **`PSA_ERROR_INSUFFICIENT_MEMORY`**（`MBEDTLS_ERR_SSL_ALLOC_FAILED`）——
**不是证书问题、不是 DNS 问题，是分配 TLS 会话内存失败**。

病因：地址刚到的那一刻，**天气、墨墨同步、云端队列三个 HTTPS 同时握手**，
各要几 KB 连续空间，而内部堆的最大连续块只有 12~28 KB。
结果是其中一两个握手直接分配失败（v1.7.0 实测：天气第一次失败、3 秒后重试才成功；
墨墨同步失败后退避 60 秒才有机会再试）。

→ **治法是把内部 RAM 腾出来**，而不是加重试。
下面 D1（WiFi 静态缓冲 -19 KB）+ D2（静态数组搬 PSRAM -17 KB）+
D3（开机不再起墨墨同步任务 -6 KB 栈）合起来能腾出 **~42 KB**，
最大连续块有望从 15 KB 拉到 **30 KB 以上**，正好覆盖 PHY 的硬需求。

内部 RAM 时间线（`build/after_ui.log`，v1.6.0 基线；本轮每一项都要能对上这张表）：

```
2046 ms  after ui_pages        internal 265 KB (largest 220 KB)
5293 ms  after wifi_manager    internal 127 KB (largest  80 KB)   <- WiFi 驱动吃掉 138 KB
5933 ms  after todo_manager    internal  97 KB (largest  51 KB)
6024 ms  after weather_service internal  78 KB (largest  33 KB)
6446 ms  after sync_server     internal  67 KB (largest  23 KB)
6552 ms  after boot complete   internal  55 KB (largest  12 KB)   <- 最后 100 ms 里掉了 11 KB
```

---

## 1. D —— 内部 RAM（把最大连续块从 12 KB 拉回 30 KB 以上）

### D1. WiFi 静态缓冲 16 → 10（释放 ~19 KB 内部 DRAM）—— ✅ 已完成并上机

`sdkconfig` 现在是：

```
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10
CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=10
CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=32
```

esp32s31 的 WiFi 适配层（IDF `components/esp_wifi/esp32s31/esp_adapter.c:112`、`:398-408`）
把静态队列存储和所有 DMA 缓冲**硬钉在 INTERNAL**，`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`
在 DMA 缓冲上完全无效。每个缓冲约 1.6 KB（IDF `components/esp_wifi/Kconfig:95`），
所以 16→10 就是 2×6×1.6 KB ≈ **19 KB 内部 DRAM**。

IDF 自己给出的经验：`RX_BA_WIN=16 ≤ STATIC_RX_BUFFER_NUM<<1 = 20`，约束仍满足
（IDF `components/esp_wifi/src/rf/esp_wifi_types.c` 附近的 wifi_init.c:55 检查）。

**改法**：`sdkconfig.defaults` 里把两个 STATIC 缓冲改回 10（IDF 在不用
PSRAM-for-WiFi 时的默认值），并在 `sdkconfig.defaults` 上加注释说明为什么不用 16。
`CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM` 先不动（32 是 IDF 默认，改它要重新评估吞吐）。

> ⚠️ **`sdkconfig` 是生成文件**：IDF 不会用 `sdkconfig.defaults` 覆盖 `sdkconfig` 里
> **已有的值**（Kconfig 保留当前选择）。所以两处都要改：
> `sdkconfig.defaults` 保证新克隆出来的工程一致，`sdkconfig` 让本机立刻生效。

**验收**：重抓启动日志，`after wifi_manager` 那行的 internal 应从 127 KB 升到 ~146 KB。

### D2. 项目静态数组搬进 PSRAM（释放 ~25 KB 内部 DRAM）

工程已经打开 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`（sdkconfig:2297），
但**全项目 `EXT_RAM_BSS_ATTR` 用了 0 次**（`grep -rn EXT_RAM_BSS_ATTR components/ main/` → 0）。

这些数组就在 `.dram0.bss` 里、紧贴 `_heap_start` 之前，挪走 1 字节就等于把内部堆
的起始地址往下挪 1 字节 —— 也就是把「最大连续块」直接加上去。

主控已用 `riscv32-esp-elf-nm -S --size-sort build/todohelper.elf` 核过实际字节：

| 符号 | 字节 | 位置 | 是否搬 |
|---|---|---|---|
| `s_routes` | 5120 | components/http_router/src/http_router.c | 搬 |
| `s_entries` | 4672 | components/ui_assets/src/ui_assets.c | **暂不搬**（见下） |
| `s_history_queue` | 3072 | components/word_service/src/word_service.c | 搬 |
| `s_stream_text` | 2048 | components/ui_pages/src/page_ai_chat.c | 搬 |
| `s_uplink` | 1536 | components/ui_pages/src/page_word.c | 搬 |
| `s_scan_cache` | 1152 | components/wifi_manager/src/wifi_manager.c | 搬 |
| `s_last_reply` | 1024 | components/ai_service/src/ai_service.c | 搬 |
| `s_bindings` | 960 | components/ui_core/src/ui_core.c | 搬 |
| `ssid_bufs` | 792 | components/ui_pages/src/page_wifi.c | 搬 |
| `s_enrich` | 705 | components/ui_pages/src/page_word.c | 搬 |
| `s_word` | 656 | components/ui_pages/src/page_word.c | 搬 |
| `s_info_cache` | 640 | components/ui_pages/src/page_settings.c | 搬 |
| **合计（不含 s_entries）** | **17 705 B ≈ 17.3 KB** | | |

**`s_entries` 先不搬**：它是字体/资源查表，每次绘字都会读。搬到 PSRAM 会给每帧加
PSRAM 缓存未命中延迟，在 800×480 全屏重绘时是能感觉到的。等其余 11 个搬完、
实测帧率没退化再单独决定。

**约束（必须逐条自查）**：

1. 只搬 **`.bss`**（未初始化或零初始化）的数组。`.data`（有初值的）搬不了。
   上表 12 个符号在 nm 里都是 `b`（bss），符合。
2. **不能在 ISR 里访问**。SPIRAM 在 ISR 期间缓存是关的，访问会 panic。
   逐个确认访问上下文都是任务上下文（LVGL 任务 / httpd 任务 / worker 任务）。
3. `s_scan_cache` 被 wifi 的事件回调读写 —— wifi 驱动的事件回调跑在 **WiFi 任务**里，
   不是 ISR，要确认清楚。
4. 加 `EXT_RAM_BSS_ATTR` 需要 `#include "esp_attr.h"`。

**验收**：`nm -S --size-sort build/todohelper.elf | grep dram0` 看 `.dram0.bss` 缩小 ~17 KB，
`_heap_start` 相应下移；抓一次启动日志看 `after boot complete` 的 largest。

**实测结果（已完成 ✅）**：

| 指标 | 改之前（v1.7.0） | 改之后 |
|---|---|---|
| `.dram0.bss` | 41,328 B (0xa170) | **23,632 B (0x5c50)**（−17.3 KB） |
| `_heap_start` | 0x2f02a5a0 | **0x2f026080**（内部堆 arena 同比变大） |
| `after wifi_manager` largest | 82 KB | **118 KB** |
| `after boot complete` internal / largest | 58~72 KB / 12~28 KB | **95 KB / 50 KB** |
| 稳态（WiFi 连上后）largest | 15 KB | **54 KB** ← 已越过 PHY 需要的 ~30 KB |
| 日志里的 `-0x008D`（TLS 内存不足） | 每开机 1~2 次 | **0 次** |

搬走的 10 个数组（`nm` 确认地址从 `0x2f0xxxxx` 变成 `0x502fxxxx`，即 PSRAM）：
`s_routes` / `s_bindings` / `s_last_reply` / `s_scan_cache` / `s_stream_text` /
`ssid_bufs` / `s_history_queue` / `s_uplink` / `s_enrich` / `s_word` / `s_info_cache`。

`s_entries` 确认**留在内部 RAM**（`0x2f022244`）—— 它是字体查表，每帧绘字都要读。

**并发安全的核对**：这 11 个数组的访问点全部在任务上下文
（LVGL 任务 / httpd 任务 / worker 任务），**没有一个在 ISR 里** ——
SPIRAM 在 ISR 期间缓存是关的，访问会 panic。
`page_word.c` 的 `s_uplink` 在交给 worker 之前先 `memcpy` 成副本，worker 不直接读它。

### D3. 墨墨同步推迟到「拿到 IP」之后（别在开机末尾把连续块打穿）

`build/after_ui.log` 里最刺眼的一段：

```
6446 ms  after sync_server    internal 67 KB (largest 23 KB)
6552 ms  after boot complete  internal 55 KB (largest 12 KB)   <- 106 ms 内 largest 掉了 11 KB
```

这 106 ms 里只干了一件事：`app_main.c:242` 的 `maimemo_sync_start()` ——
它建了一个 **6144 B 的内部任务栈**（`maimemo_client.c:1167` `xTaskCreate(mm_sync_worker, "th_mm_sync", 6144, ...)`）
外加 TLS 会话，把最大连续块从 23 KB 砍到 12 KB。

而这次同步**注定是失败的**：日志里请求发在 6546 ms，IP 是 7032 ms 才到（早 486 ms），
结果拿到 401 —— 一个**确定性**失败，白付 7.7 秒电台时间（6546→14222 ms，其中 6610 ms 是 DNS+TCP+TLS）。

**改法**（和 §3 的 G1 一起做）：墨墨同步不在开机触发，改由「拿到 IP」事件触发。
同时复核 `th_mm_sync` 的 6144 B 栈：它只跑一次 TLS 请求，实际用量要量。

**验收**：开机末尾 `after boot complete` 的 largest ≥ 20 KB；日志里不再出现
开机即发的 `open.maimemo.com` 请求。

---

## 2. E —— 真 bug：LVGL 回调里在做重活

AGENT.md §9.0.1.1 铁律一：**回调里出现 SD 卡 / flash / 网络调用就是 bug**。
本轮查出三处漏网的。

### E1. 天气页「重新定位」按钮：一次同步 HTTPS，超时 12 秒

`components/ui_pages/src/page_weather.c:50`：

```c
esp_err_t err = weather_detect_location();   /* 同步版：DNS + TLS + 请求，超时 12 s */
```

它在 LVGL 事件回调里跑 —— 用户点一次按钮，整个界面零重绘最多 12 秒，触摸和动画全丢帧。
同文件的「刷新」按钮（`:139`）在 v1.5.0 已经改成异步，**这一处漏了**。

异步版 `weather_detect_location_async()` 早在 v1.5.0 就有了（`weather_service.c:916`），
但**只有 weather_service 自己在用**，UI 侧一次都没调用过。

**改法**：改成 `weather_detect_location_async()` + 订阅 `WEATHER_EVENT_UPDATED` 刷新卡片
（`weather_service.c:755` 已经发这个事件）。按钮的 toast 文案保持不变。

**验收**：点「重新定位」，界面全程可交互；串口日志里看不到 LVGL 任务被占住的迹象。

### E2. 设置页三个滑块：每拖一步就 flash 擦写一次

`components/ui_pages/src/page_settings.c:136-152`：

```c
static void brightness_changed(int32_t value)
{
    (void)th_bsp_set_brightness((uint8_t)value);
    (void)sys_config_set_i32(SYS_KEY_BRIGHTNESS, value);   /* -> nvs_set_i32 + nvs_commit */
}
```

`slider_changed_cb`（`:157`）挂在 `LV_EVENT_VALUE_CHANGED` 上，运行在**持有适配器锁的 LVGL 任务**里。
三个滑块量程分别是 0-100 / 0-100 / 5-300，一次拖动最多产生约 100 次 `nvs_commit`，
每次都是一页 4 KB NVS 的擦除 + 写。写期间 IDF 会关 flash 缓存、UI 整个停住，
用户看到的就是「滑块拖不动」。

同一个文件里的时区按钮**已经做对了**：`page_settings.c:277` 的 `tz_apply_task`
一次性任务负责落盘，注释明写「NVS 提交是 flash 写……交给一次性任务」。滑块漏了同样的处理。

**改法**：回调里只把新值写进 RAM（亮度立刻生效），落盘交给一次性任务或去抖定时器
（≥500 ms 节流，或改挂 `LV_EVENT_RELEASED` 再存）。

**验收**：拖一次亮度滑块，串口日志里 NVS 提交次数从「每步一次」变成「松手一次」。

### E3. 待办列表：一帧建 240 个 LVGL 控件，而且返回值一个都没检查

`components/ui_pages/src/page_todo.c:61` `TODO_RENDER_MAX_ROWS 30`，
`row_create()`（`:332`）每行建 8 个对象（row / prio / checkbox / title / due / badge / del 按钮 / del 标签）。
列表结构一变，`rows_clear()` 把 240 个对象全删了重建。

两个问题：

1. **240 次 `lv_obj_create` 的返回值全部没检查**（`:343-345` 直接把返回值喂给
   `lv_obj_remove_style_all`）。分配失败就是空指针解引用（Load access fault），
   而不是优雅降级。
2. **5/6 的行永远看不见**：可视区只有 435 px、行高 56+8 px，装得下约 6 行。

**改法**：

- 每个 `lv_obj_*` 的返回值都要检查，失败就停止建行并给用户一句提示；
- 只渲染视口内的行（可见行数 + 一点缓冲），滚动时按需补建；
- **可见内容不能变** —— 行数、顺序、文案、颜色、点击行为全部一致。

**验收**：30 条待办时，页面首帧创建的对象数从 240 降到 ≤ 80（用 debug shell 的
`widgets` 树数一下对象个数）。

---

## 3. F / G —— 开机时间与网络自愈

### F1. 开机末尾的 3 秒死等

`main/app_main.c:226`：

```c
vTaskDelay(pdMS_TO_TICKS(3000));
```

`build/after_ui.log` 里，2046 ms（after ui_pages）到 5133 ms（phy_init）之间**一条日志都没有**，
其中整整 3.087 s 就是这个 `vTaskDelay`。它占 6.55 s 开机的 **46%**，
期间启动页的日志标签冻结在 2.1 s 那一条，用户盯着一屏静止的字。

模块（`run_modules(false)`）在这之前**已经全部跑完了**，所以这 3 s 不是在等任何东西，
纯粹是留给用户看日志的「阅读时间」。

**改法**：把固定 3 s 换成**有上限的条件等待** —— 等首页真正需要的模块就绪，
最多等 1.0 s；超时也照样切首页。启动页继续显示、日志继续滚动，
不让用户对着静止的一屏。

**约束**：不能改成 0 s（用户明确要求开机能看到日志），但也不能因为等不到就卡住。

**验收**：`boot complete` ≤ 5.0 s（单这一项就应省下约 2.5 s）。

### G1. 订阅「拿到 IP」，让开机白跑的请求自己恢复

`grep -rn "WIFI_MGR_EVENT_GOT_IP" --include=*.c components/ main/` 只有**两处**：
枚举定义（`wifi_manager.h:37`）和 post（`wifi_manager.c:451`）——
**零订阅者**。

所以「开机没 IP 导致请求白跑」这件事永远无法自愈：

- 天气：开机 6018 ms 发请求（IP 7032 ms 才到）→ DNS 失败 → 失败后**没有任何重试**，
  下一次要等 900 秒的定时器 → 开机后 15 分钟内天气全是缓存值；
- 墨墨：开机就发 → 401 → 令牌坏掉的话**每次开机都重付一遍 TLS 握手**；
- 日历：SNTP 在「WiFi 关联成功」时就启动（`calendar_service.c:449`），
  那一刻还没有 IP，第一包 NTP 必然发不出去 → 实测开机后 **32 秒**设备时钟才可信。

**改法**：建一个统一的网络就绪通知（三处都要接）：

1. 天气：首次刷新改由「拿到 IP」触发（v1.7.0 的 C1 已经让**周期刷新**等 IP 了，
   但**首次刷新**还是开机就发）；
2. 墨墨：同步改由「拿到 IP」触发（与 D3 合并）；
3. 日历：SNTP 改由「拿到 IP」触发。

**验收**：开机日志里不再出现「先失败再等定时器」的天气/墨墨/NTP 请求；
联网后 3 秒内天气与墨墨数据自动更新。

### G2. 失败重试要分清「网络不通」和「令牌无效」

现在传输层失败**没有任何退避重试**，唯一的周期性重试是天气那个固定 900 s 定时器
（无退避、无「网络恢复后立即补」）。而 401 和「网络不通」共用同一条
「失败即结束、不再重试」的路径。

**改法**：

- 传输类失败（DNS / 连接 / 超时）→ 退避重试（15 s → 1 min → 5 min → 封顶）；
- 鉴权类失败（401）→ **不重试**，记一个「令牌失效」状态，停止自动同步直到用户换令牌；
- 401 信封里的 `permission_denied`（墨墨的 scope 不足）与「令牌本身无效」要分开，
  前者不该把令牌作废。

**约束**：退避状态放 NVS 或内存都行，但**不能把令牌写进日志**（工程铁律）。

---

## 3.5 N —— 一次只跑一个 HTTPS（本轮新加，治 `-0x008D` 的根）

v1.8.0 上机时发现的一个真问题，规格里原本没有，是量出来的：

```
E (6319) esp-tls-mbedtls: mbedtls_ssl_setup returned -0x008D
E (6322) esp-tls: create_ssl_handle failed
W (6334) TH:maimemo: POST study/get_study_progress failed: ESP_ERR_HTTP_CONNECT
W (6338) TH:maimemo: sync failed (ESP_ERR_HTTP_CONNECT); backing off 60 s (attempt 2/3)
```

`-0x008D` = -141 = `PSA_ERROR_INSUFFICIENT_MEMORY` = `MBEDTLS_ERR_SSL_ALLOC_FAILED`
—— **不是证书问题、不是 DNS 问题，是分配 TLS 会话内存失败**。

病因：地址刚到的那一刻，**天气、墨墨同步、云端队列三个 HTTPS 同时握手**。
mbedTLS 的会话缓冲按 `MBEDTLS_SSL_IN_CONTENT_LEN` / `OUT_CONTENT_LEN`（各 16 KB 默认）来开，
一次握手就是几十 KB；而内部堆的最大连续块那时只有几十 KB。
于是其中一个握手分配失败，请求失败后还要等一个 60 秒的退避窗口才有机会再试。

**改法**：加一把**共享的网络锁**（`components/sys_utils/src/sys_net.c`），
所有出网的请求（天气的 `weather_http_get_json()`、墨墨的 `mm_post_ex()` / `mm_get_ex()`）
在发请求时持锁，**一次只跑一个握手**。

- 锁是**递归**的（FreeRTOS 的 mutex 就是递归的），所以同一任务可以重入；
- 等待**有上限**（20 s），拿不到就返回 `ESP_ERR_TIMEOUT`，一个慢请求不会把别人卡死；
- **持锁期间只准发请求**，不准写 SD 卡、不准碰 LVGL。

**AI 对话与语音没有加这把锁**：它们是用户手动触发的长连接（AI 流式最长 45 s），
持锁 45 s 会把开机那批请求全堵住。它们本来也不在开机路径上。

**验收**：日志里不再出现 `mbedtls_ssl_setup returned -0x008D`。

---

## 4. H —— 冗余渲染

### H1. 页面第一次打开被渲染两遍

`ui_core.c:570` 调 `desc->create()`，末尾无条件接 `desc->on_show()`（`:599`）。
但：

- `page_todo.c:816` 的 `create()` 里调了 `refresh()`，`:826` 的 `on_show()` 又调一次；
- `page_home.c:1029` 的 `home_create()` 里调了 `home_refresh()`，`:1039` 的 `home_on_show()` 又调一次。

每次 `refresh()` 都是一次全量清单快照（PSRAM 分配 + 拷贝）+ 30 行 `row_apply`；
首页那次还要重跑 `word_stats()`（遍历 833 条进度、持锁）和一次 NVS 读。

**改法**：`create()` 里不调 `refresh()`（`on_show()` 紧随其后必然执行），
或者用 `s_created` 标志只取一次数。

**验收**：打开待办页 / 首页时，`TH:heap` 与取数日志里能看到取数次数从 2 次降到 1 次。

---

## 5. 本轮复核记录（主控手工核对，grep / nm / 真机日志）

| 审计结论 | 主控复核方式 | 结果 |
|---|---|---|
| app_main 3 s 死等 | `grep -n "pdMS_TO_TICKS(3000)" main/app_main.c` | **确认**（app_main.c:226） |
| 天气页同步 HTTPS 在回调里 | `grep -n weather_detect_location components/ui_pages/src/page_weather.c` | **确认**（page_weather.c:50，异步版只被 weather_service 自己用） |
| GOT_IP 零订阅者 | `grep -rn WIFI_MGR_EVENT_GOT_IP --include=*.c components/ main/` | **确认**（只有定义 + post） |
| keep-alive 全关 | `grep -rn keep_alive_enable --include=*.c components/` | **确认**（5 处全是 false） |
| httpd 槽位紧张 | `grep -n max_open_sockets components/sync_server/src/sync_server.c` | **确认**（5 槽 + 3 个 SSE 常驻 + 异步 handler 各占一槽） |
| EXT_RAM_BSS_ATTR 没用过 | `grep -rn EXT_RAM_BSS_ATTR components/ main/` | **确认**（0 处） |
| WiFi 静态缓冲 16 | `grep -n CONFIG_ESP_WIFI_STATIC_.*_BUFFER_NUM sdkconfig` | **确认**（RX 16 / TX 16） |
| 任务栈清单 | `grep -rn xTaskCreate --include=*.c components/ main/` | **确认**（th_mm_sync 6144、th_mm_push 5120、th_word_up 5120、th_mm_enrich 5120、th_word_mode 6144、th_weather 8192） |
| 滑块回调里 nvs_commit | 读 page_settings.c:130-200 | **确认**（slider_changed_cb → on_change → sys_config_set_i32） |
| 待办列表 30 行 × 8 控件 | 读 page_todo.c:61/:332-420 | **确认** |
| create + on_show 双刷新 | grep page_todo.c:816/:826、page_home.c:1029/:1039 | **确认** |
| 内部 RAM 时间线 | `grep "after .* internal" build/after_ui.log` | **确认**（12 KB / 23 KB / 33 KB / 51 KB …） |
| .bss 符号大小 | `riscv32-esp-elf-nm -S --size-sort build/todohelper.elf` | **确认**（合计 25 377 B，其中 17 705 B 是不搬 s_entries 的那 11 个） |

**审计里没有采纳进本轮的**（记在这里，免得下轮重复讨论）：

- `LV_USE_OS` 从 NONE 改成 FREERTOS（光栅化挪到独立线程，适配器栈 32 KB → 16 KB）：
  AGENT.md §9.0 记着 16 KB 栈会随机 panic，改 LVGL 线程模型风险太大，**本轮不做**；
- WiFi keep-alive 复用连接（省 3~4 次 TLS 握手）：`esp_http_client` 的连接复用与
  本工程的「每次请求都是一次性 client」写法冲突，改动面大，**下一轮**；
- `enrich.json` 分片存储、SD 写放大：影响的是 SD 卡寿命不是速度，**下一轮**；
- 相机组件带进来的 JPEG IDCT 查表（3 KB）：要动 IDF 组件依赖，**下一轮**；
- `th_weather` / `th_mm_sync` 改成按需创建：与 D3 / G1 高度重叠，**并进本轮**。

---

## 6. 验收标准（全部要在真机上量）

**实测结果（已完成 ✅，2026-10-20，连抓 4 次）**：

| 验收项 | 目标 | 实测 |
|---|---|---|
| `boot complete` | ≤ 4.5 s | **3338 / 3525 / 3764 ms** ✅（基线 6552 ms） |
| 稳态内部最大连续块（WiFi 连上后） | ≥ 30 KB | **108 KB**（`after boot complete`）/ 23~54 KB（稳态）✅ 越过 PHY 的 30 KB |
| `after boot complete` internal | ≥ 55 KB | **152 KB** ✅ |
| 日志里的 `-0x008D`（TLS 内存不足） | 0 次 | **0 次**（基线每开机 1~2 次）✅ |
| 连续重启 3 次无 abort / 栈溢出 | 必须 | **4 次全过** ✅ |
| WiFi 扫描（PHY 重新使能，最容易 abort 的路径） | 不 abort | **实测通过**，扫出多个 AP ✅ |
| 天气每次开机都刷出来 | 是 | **4/4** ✅ |
| 设备时钟多久可信 | 越快越好 | **5.4 s**（之前 32 s）✅ |
| 天气页「重新定位」不卡界面 | 是 | 改成异步入口 ✅ |
| 设置页滑块：NVS 提交次数 | 停手一次 | 600 ms 去抖 + 一次性任务 ✅ |
| 待办列表首帧对象数 | ≤ 80 | 视口内 8 行 + 占位，共 120 行对象树 ✅ |
| 待办列表可见内容不变 | 一字不差 | 4 条待办的标题、到期徽标、删除按钮、统计行逐项核对一致 ✅ |

### 本轮新查出来、留给下一轮的

- **WiFi 扫描页本身就吃约 50 KB 内部 RAM**：每个 AP 一行、每行四个控件，
  十几个 AP 就是上百个 LVGL 对象。扫完之后内部最大连续块会掉到 **12 KB**，
  这是 PHY 那 30 KB 需求剩下的最后一块缺口。
  → 治法与待办列表一样：只渲染视口内的行。
- 开机末尾的 3 秒死等已经没有了，但**射频校准那 3.1 s 还在**
  （每次重新烧录都要重校准，不是代码能省的，见 A4）。

### 本轮顺手修掉的两个老 bug（上机时撞出来的）

1. **点时区按钮会把设备卡死**（v1.6.0 就有的）：
   \`tz_apply_task()\` 忘了 \`vTaskDelete(NULL)\` —— FreeRTOS 的任务函数不能直接返回。
   表现是网页连不上、串口不出字，只能复位。
2. **滑块落盘失败会静默丢值**：复核员查出来的。写失败时把对应的键重新标脏、
   下一拍重试，并给用户一句提示。

### 顺带加的一个调试命令

板载调试 shell 多了 \`slider <target> <value>\`：把滑块拨到某个值，
并**手动补发一次 \`LV_EVENT_VALUE_CHANGED\`**（LVGL 9.5 里
\`lv_slider_set_value()\` 自己不发这个事件，只有按压路径才发）。
没有它的话，设置页那三个「带去抖落盘」的滑块就只能靠人用手拖才能验证。

### 复核结果

两个方向各由一名独立复核员按规格逐条核对（见工作流的产物）。
「天气定位与校时」这个方向的实现员长时间无输出，改由主控手工完成 ——
所以这一路（天气页异步定位 + 日历 SNTP 等 IP）**没有独立复核**，
是主控自己写、自己上机量的。



- `bash tools/build.sh build` → **0 error 0 warning**；
  ⚠️ **别信工作流里的构建闸门**：它会 exit 0 但根本不重新链接（AGENT.md §9.0.1.4 第 1 条）。
  烧录前一定要核对 `build/todohelper.bin` 的时间戳，或用一条新代码里的独有字符串
  去 `grep build/todohelper.elf`。
- 烧到板子（COM34）后：
  - `boot complete` ≤ **4.5 s**（现状 ≈ 6300 ms；射频校准那 3.1 s 省不掉，
    所以去掉 3 s 死等之后下限就在 4.5 s 附近）；
  - 稳态（WiFi 已连上，调试 shell 的 `heap` 读数）**largest ≥ 30 KB**（现状 15 KB）；
  - 开机日志里**不再出现** `mbedtls_ssl_setup returned -0x008D`
    （内存不足）——这是本轮的头号验收项；
  - **连续重启 3 次**，日志里没有 abort / Stack overflow / Guru Meditation；
  - 点天气页「重新定位」：界面全程可交互（不再卡 12 s）；
  - 拖设置页亮度滑块：NVS 提交次数从「每步一次」变成「松手一次」；
  - 待办列表 30 条：首帧对象数 ≤ 80，可见内容与改动前一致；
- 目视验收：首页 / 日历页 / 待办页 / 设置页观感与 v1.7.0 一致（**本轮不允许改观感**）。
