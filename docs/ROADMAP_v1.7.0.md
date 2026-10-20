# v1.7.0 实施规格：性能优化

> 本轮**不改任何用户可见的功能**，只优化三个指标：**开机时间、内部 RAM 余量与碎片、无谓的网络往返**。
> 基线：v1.6.0（提交 `a2a073d`）。构建命令：`bash tools/build.sh build`（0 错误 0 警告）。
>
> 规格里的每个数字都来自**真机启动日志**（`build/after_ui.log`，毫秒时间戳）与
> 逐模块的 `TH:heap` 读数，不是估算。

---

## 0. 硬约束（沿用前两轮）

1. LVGL 回调里禁止阻塞；内部 RAM 是最紧的资源；触摸目标 ≥ 44 px；先比较再写。
2. 颜色/字号/间距取 `ui_theme.h` 的宏；不新增组件、不引入新依赖。
3. **只改自己负责的文件**；不改 `version.txt` / `CHANGELOG.md` / `README.md` / `docs/API.md`
   （主调度统一收口）。
4. 诚实汇报：做不完的写进 `not_done`。

---

## 1. 开机时间（当前 6.4 s）

真机时间线（`build/after_ui.log`）：

| 时刻 | 事件 | 段耗时 |
|---|---|---|
| 1141 | 开机 | |
| 1631 | bsp（显示 + LVGL） | 490 ms |
| 1938 | storage | 307 ms |
| 2046 | ui_pages | 108 ms |
| **5133** | **`phy_init: phy_version …`（射频校准结束）** | **3087 ms** |
| 5292 | wifi_manager ready | 159 ms |
| 5424 | todo: imported 4 items | 132 ms |
| **5929** | **todo: ready（中间那次写盘）** | **505 ms** |
| 6018 | weather ready（**此时还没 IP**） | 89 ms |
| 6069 | calendar ready | 51 ms |
| 6412 | word ready（读 833 词） | 343 ms |
| 6552 | **boot complete** | **总计 6.4 s** |

### A1. 开机时待办「读进来又原样写回去」——去掉那次写盘（省 ~500 ms + 一次 flash 写）

- **位置**：`components/todo_manager/src/todo_manager.c` 的 `todo_import_json()` 结尾
  无条件 `todo_save()`；`todo_manager_init()` 用它来加载开机缓存。
- **症状**：开机时清单是从 `cache/todos.json` 读出来的，内容**一模一样**，
  导入完又把它写回去。而 LittleFS 的第一次写要擦块、要分配，**真机实测 505 ms**
  （`todo: imported` → `todo: ready` 之间），白等半秒，还白磨一次 flash。
- **要求**：
  - 把「导入」拆成「装载」与「落盘」两件事：内部加一个
    `todo_import_json_ex(const char *json, bool persist)`，
    `persist=false` 时只改内存与版本号、**不落盘**；
    公开的 `todo_import_json()` 保持原语义（`persist=true`，因为
    HTTP 导入是「整体替换」，必须立刻落盘，否则断电会丢）。
  - `todo_manager_init()` 的开机加载路径用 `persist=false`。
  - **认领遗留临时件**那条路径（`todo_adopt_leftover()`）**必须 persist=true**
    ——那份内容此刻还只躺在 SD 的 `.tmp` 里，不落盘就等于没认领。
  - 顺带：落盘失败要有日志（现在有）。

### A2. 词库加载挪到后台（省 ~340 ms 开机时间）

- **位置**：`components/word_service/src/word_service.c` 的 `word_service_init()`
  → `word_load_from_sd()` → `parse_book()`（读 120 KB JSON、解析 833 词）。
- **症状**：这一步在主任务上同步做，**343 ms**（6333 → 6412），而开机这一刻
  屏幕上只有启动日志，词库还没人用。
- **要求**：
  - 词库改成**后台任务**加载：init 里只起一个任务（栈 4 KB 起步，不要给大），
    解析完再发一个事件（`WORD_EVENT_BOOK_READY` 之类，名字自定）通知 UI。
  - 词库没好之前，`word_get_today()` / `word_next()` / `word_stats()` 等对外接口
    要**返回明确的「还没加载好」错误**（不是空列表、不是崩溃），
    `page_word.c` 要显示「词库加载中…」而不是空白页。
  - **不要**动 `word_review()` 的进度记账语义（v1.5.0 刚修过）。
  - 加载失败要打日志并让页面能显示出错（不要静默空列表）。

### A3. 启动页的日志刷新节流（10 Hz → 4 Hz）

- **位置**：`components/ui_pages/src/page_splash.c` 的 `SPLASH_TICK_MS`（100）。
- **症状**：启动这 6.4 秒里它跑 64 次；每次有新日志就 `lv_label_set_text_static()`
  让一个最多 5 KB 的标签失效 → 下一次布局要重新量整段文字。
  启动页是那几秒里**唯一**的可见页面，UI 任务的时间全花在这上面。
- **要求**：刷新周期放宽到 **250 ms**；日志仍然是增量的（现有实现已经是追加式，别退回全量重建）。
  「最新一条」的状态行也按同一节流走。

### A4. 射频校准的 3.09 s：不可省，但要说清楚

- **事实**：`esp_wifi_init()` 里驱动要做射频校准，**每次烧录都会把 `phy_init` 分区
  写回出厂数据**，于是每次开机都重新校准 ≈ 3.1 s。
  校准期间调用方在等、CPU 大部分是闲的。
- **本轮不并行化它**：并行的诱惑很大（把 WiFi init 放任务里，别的模块同时 init，
  开机能省 3 s），但 **PHY 重新使能需要约 30 KB 连续内部块**（AGENT.md §9.0.3），
  而并行会让别的模块在这 3 秒里把堆切碎 —— 历史上有一次就是这样 abort 重启的。
  **安全优先，这轮不冒险。**
- 要做的是**把这件事写进文档**（AGENT.md + 本文件），让下一个人知道
  「3 秒不是 bug，是每次烧录的代价」。

---

## 2. 内部 RAM（省 ~11 KB，把最大连续块从 12 KB 拉回 20 KB+）

真机逐模块读数（v1.6.0，联网状态）：`after ui_pages` 265 KB → `after wifi_manager` 127 KB
→ `after todo_manager` 97 KB → … → `after boot complete` **55 KB（最大连续块 12 KB）**。

对比 v1.5.0（**当时设备没连上网**）：`after boot complete` 78 KB（最大连续块 34 KB）。
差的那 11 KB 出现在 **weather_service 那个时间窗**里——那一段跑的是
**墨墨云端词队列**（`th_word_cq`）。

### B1. `th_word_cq` 的 6 KB 内部栈 → 4 KB

- **位置**：`components/word_service/src/word_service.c` 的
  `xTaskCreate(cloud_build_worker, "th_word_cq", 6144, ...)`。
- **事实**：FreeRTOS 的任务栈**只能来自内部 RAM**，所以这 6 KB 是开机后
  内部 RAM 的一笔**固定开销**，而且任务跑完后还留下一个 6 KB 的洞（碎片）。
- **要求**：栈改 **4096**。这个任务里要做 TLS 握手（`maimemo_fetch_today`），
  4 KB 够不够**必须实测**：改了之后烧板看串口日志，
  如果出现栈溢出（`Stack overflow` / 随机 abort），就退到 5120 或 6144，
  并在 README 里记下这个数字是怎么定的。
- 另外：任务跑完可以**自我删除**（`vTaskDelete(NULL)`），别让一个一次性的任务
  在系统里赖着。

### B2. 文件读取的缓冲改 PSRAM 优先

- **位置**：`components/storage_manager/src/storage_manager.c` 的 `file_read_text()`：
  `char *buf = malloc(size + 1);` —— **裸 malloc**。
- **事实**：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` 意味着 **≤4096 字节的分配优先落内部堆**。
  于是每次读一个几百字节到 4 KB 的文件（`todos.json` 550 B、`weather.json` 211 B、
  `words/maimemo.json` …）都在**内部 RAM**上占一块，正好把内部堆切碎。
- **要求**：`file_read_text()` 改用
  `heap_caps_malloc_prefer(size + 1, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)`
  （与 `sys_json.c` 里 `json_malloc()` 同一套写法）。
  **释放不用改**：IDF 的 `esp_heap_caps.h` 写明「In IDF, free(p) is equivalent to
  heap_caps_free(p)」，所以现有调用方的 `free(out)` 对 PSRAM 块同样正确，
  **不需要跨文件改**。

### B3. 云端词队列的中间缓冲

- **位置**：`components/word_service/src/word_service.c` 的 `cloud_build_worker()`
  与 `components/maimemo_client/src/maimemo_client.c` 的 `mm_fetch_today()` 一类。
- **要求**：这个链路上的大缓冲（文件内容、JSON 文本、词条数组）都显式
  `MALLOC_CAP_SPIRAM`；已经在用 `heap_caps_calloc_prefer` 的保持不变。

---

## 3. 网络：不要在拿到 IP 之前就发请求

### C1. 第一次请求要等 IP

- **事实**（真机日志）：
  ```
  I (6018) module 'weather_service' ready
  E (6018) esp-tls: couldn't get hostname for :api.open-meteo.com: getaddrinfo() returns 202
  W (6042) TH:weather: refresh failed: ESP_ERR_HTTP_CONNECT
  I (7032) TH:wifi: got ip 192.168.137.95      ← IP 在这之后才有
  ```
  天气在 init 里就刷，**必然失败**（DNS 都解析不出来），
  而这一次失败 = 一次 DNS 查询 + 一次连接尝试的电台与 CPU 开销。
  墨墨的云端队列也一样（6546 ms 发起，7032 ms 才有 IP）。
- **要求**：
  - `wifi_manager` 已经有 `WIFI_MGR_EVENT_GOT_IP` 事件（现在**没有人订阅**）。
    天气服务、墨墨客户端（或单词服务的云端队列）改成**订阅它**，
    拿到 IP 之后再发第一次请求。
  - 兜底：如果一直没拿到 IP（连不上），仍要按原周期重试，别把刷新功能变成「只有联网才有」。
  - 天气服务在 init 里**可以**先把缓存读出来显示（现在的 `cache loaded: 南昌…` 保留）。

### C2. 失败重试要有退避

- **要求**：
  - 「网络不通 / DNS 解析失败」→ 退避重试（**分钟级**，别每 15 秒重试一次 TLS 握手）。
  - 「401 / 令牌无效」→ **不重试**（重试也不会成功），等用户改配置再说。
  - 现在的天气刷新周期是 900 s，本身不算密；但**开机失败后的那一次**要退避，
    不要紧接着再连一次。

---

## 4. 图形（等审计结果补充）

启动页的节流见 A3。其余条目等性能审计的结论（`docs/ROADMAP_v1.7.0.md` 的审计章节）。

---

## 5. 验收标准（主调度负责）

1. `bash tools/build.sh build` → 0 error 0 warning。
2. **烧到板子上量**：
   - 开机时间：从串口日志的 `TH:app: ready` / `boot complete` 读，目标 **≤ 5.0 s**
     （射频校准那 3.1 s 无法省，所以下限就在 3.5 s 附近）；
   - 内部 RAM：`after boot complete` 的 free / largest / int-low，
     目标 **largest ≥ 20 KB**（联网状态）。
3. **不出现新的崩溃/看门狗**：连续重启 3 次，日志里没有 abort / Stack overflow。
4. 用户目视确认：首页、日历页、设置页、单词页的观感与功能都**没变**（这轮只动性能）。

---

## 6. 本轮明确不做

- 不并行化 WiFi 初始化（见 A4：PHY 的 30 KB 连续块要求，冒险不值）。
- 不改用户可见的功能与文案。
- 不动 `components/esp_cam_sensor`、`managed_components/`。
- 不做帧率的量化改造（没有 FPS 探针，等有工具再说）。

---

## 7. 实测结果（烧到板子上量的，2026-10-20）

固件：v1.7.0（`build/todohelper.bin`，16:29 构建），板子 COM34，连抓 7 次启动日志。

### 开机时间

| | 基线（v1.6.0，`build/after_ui.log`） | v1.7.0（7 次实测） |
|---|---|---|
| `boot complete` | 6552 ms | **6070 / 6297 / 6320 / 6364 / 6399 / 6149 / 6149 ms** |
| 中位数 | 6552 ms | **≈ 6300 ms（−250 ms）** |

**没达到规格里的 ≤ 5.0 s**，差的不是这几项改动，而是：
- **射频校准 ≈ 3.1 s**（`phy_init`，每次重新烧录都要重校准，不是代码能省的）；
- **开机末尾仍有 3.0 s 死等**（`app_main.c:226` 的 `vTaskDelay(3000)`，
  纯粹留给用户看启动日志），占开机时间的 46%。

这两条都写进了 `docs/ROADMAP_v1.8.0.md`（F1 去掉死等；A4 说明射频校准不可并行）。

### 内部 RAM

| | 基线 | v1.7.0 |
|---|---|---|
| `after boot complete` internal free | 55 KB | **58~72 KB** |
| `after boot complete` largest | 12 KB | **12~28 KB**（取决于 WiFi 连上的时机） |
| 稳态（WiFi 已连上，调试 shell 读数） | largest 12 KB | **15 KB（SD 卡在位）/ 82 KB（SD 卡不在位）** |

目标 `largest ≥ 20 KB` **未达成**（联网状态下只到 15 KB）。
原因查清了：**三个 HTTPS 在地址刚到时同时握手**，mbedTLS 分配失败
（`mbedtls_ssl_setup returned -0x008D` = `PSA_ERROR_INSUFFICIENT_MEMORY`），
内部堆的连续块被切碎。治法是把内部 RAM 腾出来 —— 见 `docs/ROADMAP_v1.8.0.md` §1
（WiFi 静态缓冲 16→10 释放 ~19 KB、项目静态数组搬 PSRAM 释放 ~17 KB、
开机不再起墨墨同步任务释放 6 KB 栈）。

### 稳定性

- **连续 7 次重启，0 次 abort / panic / 栈溢出**。
- 其中一次抓到了真问题：`th_word_cq` 的栈按「省 2 KB」降到 4096 之后
  `***ERROR*** A stack overflow in task th_word_cq has been detected.`
  （实测这个任务一次 TLS 握手用掉 **4184 字节**）→ 改回 6144。
  现在每次跑完都打印栈水位（`cloud queue worker: 1960 stack item(s) never used`）。

### 复核结果

六个方向各由一名独立复核员按规格逐条核对，**发现 6 个问题并全部修掉**：

| # | 位置 | 问题 | 严重度 |
|---|---|---|---|
| 1 | `todo_manager.c:978` | 开机装载一律不落盘，导致「卡上那份更���」不会被提升到片内，拔卡后清单「变回旧版」 | medium |
| 2 | `weather_service.c:806` | 「有网」判据只判非空，而 `wifi_ip` 初值是 `"0.0.0.0"` → 等 IP 的分支一次都没进过，是死代码 | **high** |
| 3 | `weather_service.c:848` | 等网 15 s + HTTP 12 s > 同步路径 20 s → 接口从 200 变 503 | medium |
| 4 | `maimemo_client.c:1121` | 退避睡眠占着「一次只允许一个同步」的名额，网页「立即同步」3 分钟内一直被拒 | medium |
| 5 | `maimemo_client.c:1121` | 墨墨自己的开机同步没有等 IP，第一次尝试注定失败 | medium |
| 6 | `page_splash.c:235` | 追加日志时遇到「已认领但未发布」的行会留下重复行 | medium |

「单词服务」「存储层」两个方向的复核员长时间无输出，改由**主控人工复核**：
分配/释放配对（IDF 里 `free(p)` ≡ `heap_caps_free(p)`）、后台任务与 UI 的并发读、
双槽落盘协议（`X.new` → `X.bak` → `X`）没有被改坏。

### 本轮之外补的两条

- 复核之后主控又发现并修了两条：**栈溢出**（见上）与
  **「地址到了没有补一次刷新」**（天气首次刷新在 IP 晚到时仍会失败，
  现在订阅 `WIFI_MGR_EVENT_GOT_IP`，地址一到就补一次，并在传输失败时重试一次）。
