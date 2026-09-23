# ToDoHelper 交接说明（HANDOVER）

> 面向**接手的下一个 Agent / 开发者**。本文档描述「当前真实状态、下一步做什么、哪里最容易踩坑」。
> 配套：`AGENT.md`（环境与命令，**先读它**）、`docs/TODOHELPER_FRAMEWORK.md`（框架设计）、
> `docs/API.md`（组件与 HTTP 接口）、`docs/TODOHELPER_REQUIREMENTS.md`（需求）。
>
> 优先级：**代码实际行为 > `AGENT.md` > 框架设计 > 需求文档**。

---

## 1. 一句话现状

P0/P1/P2 的**代码**基本写完（16 个组件、约 15.5k 行 C，`idf.py build` 全绿，
app 4.18 MB / 8 MB 分区余 50%），**且已完成首轮真机验证**。

**已验证（真机 COM34，`idf.py build` 0 警告）**

| 项 | 结果 |
|---|---|
| 稳定性 | **0 panic、0 task watchdog**，`main_task: Returned from app_main()` 干净收尾 |
| 12 个功能模块 | WiFi / Todo / 天气 / 日历 / 单词 / sync_server / idle_face / ai_service / voice_service **全部 `ready`** |
| 中文字体 | FreeType **16/20/28/48 px 全部生效**，用户确认**无方框** |
| SD 卡 | 挂载 `/sdcard`（SD16G/SDHC/4-bit/40 MHz），应用目录树创建成功，词库写入 `words/cet4.json` 成功 |
| mDNS | `mDNS published as todohelper.local` |
| HTTP | `listening on port 80, 37 route(s), auth=off` |
| 音频 | `mic=1 speaker=1`（I2S DMA 成功） |
| 内部 RAM | boot 结束余 **118 KB（最大连续块 74 KB）**、PSRAM 余 9655 KB |
| app 体积 | `0x3fcbf0`，8 MB 分区余 **50%** |

**尚未验证**

- WiFi 实际关联（需要 SSID/密码凭据）
- 联网后的 HTTP 全链路：浏览器待办页双向同步、SSE 推送、`/ws/events` 501、天气 IP 定位、SNTP 校时、单词、AI 对话
- **触摸与按键交互**（GT1151 已初始化，但从未点过）
- **待机 30 s 超时 → 眼睛动画 → 触摸唤醒** 完整链路
- DeepSeek 真实调用、ASR/TTS 端点

> **接手者必读**：首轮上机踩的坑全部记在 `AGENT.md` §9.0（五条实测结论）与
> §9.0.1（两个自伤 bug）。那两节是最容易重复踩的地方，尤其是
> "内部 RAM 碎片化 ≠ 总空闲不足" 这一条。

---

## 2. 已完成（对照需求文档）

| 需求 | 实现位置 | 说明 |
|---|---|---|
| FR1 WiFi | `components/wifi_manager` + `page_wifi.c` | 扫描/连接/NVS 保存/开机自动重连/断线退避重试/mDNS hostname/7 条 HTTP 路由 |
| FR2 Todo | `components/todo_manager` + `page_todo.c`（含详情页） | 设备端列表+勾选、`cache/todos.json` **原子写**（tmp+rename）、完整 REST、PC 端可直接改、断网可操作 |
| FR3 天气 | `components/weather_service` + `page_weather.c` | 默认 **Open-Meteo（免 Key）**、城市地理编码、**IP 定位**、15 分钟定时刷新、`cache/weather.json` 降级 + `stale` 标记、WMO 码→中文/图标 |
| FR4 日历 | `components/calendar_service` + `page_calendar.c`（+ 状态栏） | SNTP、POSIX 时区、月历翻页；**无 RTC**，未同步显示"时间未同步"而不是 1970 |
| FR5 单词 | `components/word_service` + `page_word.c` | 内置 32 词示例（可导出替换）、翻卡、三档评分、间隔复习（known→2^level 天 / fuzzy→1 天 / unknown→10 分钟）、进度持久化 |
| FR6 待机眼睛 | `components/idle_face` | **纯程序化、零位图**；11 状态**一张参数表**驱动 + 33 ms 插值动画；随机眨眼(3~8 s)/左右看；呼吸相位；**通知触发表情** |
| FR7 AI 对话 | `components/ai_service` + `page_ai_chat.c` | DeepSeek 阻塞+**流式**、历史轮数、真中断、HTTP+SSE、小眼睛四态联动 |
| FR7 语音 | `components/voice_service` | **真实录音与回放**；ASR/TTS 走可配置 HTTP 端点；三条降级路径齐全 |
| 需求 §8 HTTP 契约 | `components/http_router` + `sync_server` | 统一 `{code,msg,data}`；**501/404/405/401 集中实现**；29+ 条路由；`/api/v1/events` SSE；内嵌浏览器待办页 |
| 需求 §8.8 鉴权 | `sync_server.c` | `X-Auth-Token` 头（**不接受 query**）；`device/reboot`、`device/auth`、`ai/config`、`ai/history` 强制鉴权 |
| 需求 §9 数据 | `sys_utils/sys_keys.h` + `docs/API.md` §3 | 全部 NVS 键集中登记；组件不得直接 `nvs_open` |
| 需求 §16 文档 | `docs/API.md`（1381 行）+ 每个组件 README | 函数签名/事件/NVS 键/HTTP 路由/测试步骤 |
| 工程卫生 | `-Werror` 0 警告；5+2 个 git 提交；`.gitignore` 覆盖生成物 | |

---

## 3. 未完成 / 需要决策

### 3.1 阻塞项（**必须你或用户确认，我无法自行判定**）

来自需求 §17.2，直接决定架构：

| # | 问题 | 影响 |
|---|---|---|
| 1 | 板上装的是 **SD 卡槽** 还是 **SPI NAND flash**？（GPIO20~25 复用，二者互斥） | 字体/词库/素材该放 SD 还是片内分区；`storage_manager` 的回退路径哪条才是实际路径 |
| 2 | **麦克风型号**（数字/模拟）、**是否已装喇叭/功放** | FR7 能否做离线 ASR；TTS 播报是否可用（不可用则只上屏） |
| 3 | **ASR / TTS 选型**（本地离线 / 云端 / 跳过音频） | `voice_service` 的两个端点填什么；本地镜像**没有** `esp-sr`/`esp_tts` |
| 4 | **天气 API 选型与 Key** | 我默认用了免 Key 的 Open-Meteo；若要和风/OpenWeather，在 `weather_service` 加一个 provider 即可（§4.3） |
| 5 | **词库来源**（CET4/CET6？格式？要音标/例句/发音？） | 当前是内置 32 词示例；把真实词库按 `docs/API.md` 的格式丢到 `<storage>/todohelper/words/<book>.json` 即可替换 |
| 6 | **屏幕驱动 IC 型号** | 影响时序微调与背光方案（当前背光为 NC，用软件遮罩调光） |
| 7 | **DeepSeek Key / 模型 / 是否流式** | 通过 `POST /api/v1/ai/config` 或设置页配置即可，无需改代码 |
| 8 | 待机眼睛风格（圆眼/椭圆/线条/是否要瞳孔） | 改 `s_effects[]` 参数或 `idle_face.c` 的绘制即可 |
| 9 | 屏幕方向与触摸校准 | `th_bsp_init()` 里 `bsp_display_config_t.rotation` |

### 3.2 代码侧待办（不需要硬件，可直接做）

| 项 | 位置 | 说明 |
|---|---|---|
| `voice_service/README.md` | — | 唯一缺 README 的组件（其余 16 个都有） |
| ASR/TTS 真机联调 | `voice_service.c` 的 `http_post()` | 端点可配，但从未对真实服务验证过 |
| `SYNC_EVENT_CLIENT_CONNECTED` | `sync_server.h` 声明了但**从未 post** | 要么补上（在 catch-all 分发器里发），要么删掉声明 |
| 云同步 / MQTT | — | 需求 §14 P2 的"Mqtt/云同步"未做；本地镜像有 `espressif/mqtt` |
| OTA | — | 需求 §14 P2 未做；分区表目前**没有** ota 分区，需要先改 `partitions.csv` |
| 提醒/日程联动 | — | 需求 §14 P2 未做 |
| CDN/字体换源 | `tools/gen_cjk_font.py` | 若要二级字库加 `--level2`（约 3.8 MB），注意 app 分区余量 |
| 备份策略 | 需求 §17.2 #13 | `backup/todos_*.json` 的滚动保留未实现（NVS 键 `TODOHELPER_TODO_BACKUP_KEEP` 已留但未用） |
| IP 定位端点 | `SYS_KEY_GEO_URL`，默认 `https://ipapi.co/json/` | 免费额度有限，可换成自建或其它服务 |

### 3.3 实现与需求文档的**偏离**（需回改进需求文档）

我做了以下偏离，需求文档尚未同步（本文档即为记录；请顺手改 `docs/TODOHELPER_REQUIREMENTS.md`）：

| 偏离 | 原因 |
|---|---|
| **新增 L1 组件 `http_router`** | 需求 §7.12 让业务组件"反向注册"路由，但把注册表放 `sync_server`(L4) 会迫使 L3 组件向上依赖。移到 L1 后所有依赖仍然向下 |
| **新增 L2 组件 `ui_assets`** | 需求 §12 说"复用 `spiffs_create_partition_assets` 流程"；把它放在拥有素材的组件里，才能保证生成的 `mmap_generate_assets.h` 在编译前就绪 |
| **`UI_PAGE_IDLE_FACE` 由 `idle_face` 自己注册**，不在 `ui_pages` 的 `s_pages[]` 里 | 需求 §10.1 说"由 idle_face 组件接管"；这样 `ui_pages` 不必依赖 `idle_face` |
| **天气默认 Open-Meteo（免 Key）** | 需求 §17.2 #4 未定；选免 Key 的让 FR3 开箱可用，provider 可插拔 |
| **WebSocket 用 SSE 代替**（`/api/v1/events`），`/ws/events` 保留为 **501 占位** | 本地组件镜像里**没有** `esp_websocket_client`；需求 §8.7 允许 SSE 且要求未实现返回 501 |
| **新增 `sys_notify()`（L0 通知通道）** | 需求没有它；没有它，L3 想通知 UI/Web 就得向上依赖 L4。idle_face 的表情触发也建在它上面 |
| **`sdkconfig.defaults` 强制 `LOG_MAXIMUM_LEVEL_VERBOSE`** | 见 `AGENT.md` §9.1：esp32s31 + XIP_FROM_PSRAM 下否则**链接失败** |
| **`bsp` 门面符号前缀用 `th_bsp_`** 而非 `bsp_` | 需求 §6 想暴露 `bsp_display_*`，但工程外 Korvo BSP 已占用这些符号 |
| **设备端不支持中文输入**（屏幕键盘只有拉丁） | 没有 IME；中文待办/AI 提问请用电脑网页或 HTTP API。这是硬件/系统限制，不是遗漏 |
| **`ESP_LOGD` 用运行时级别**（`sys_log_writev`） | 这样 `sys_log_set_level()` 才能运行时生效；代价是日志前缀固定为 `TH:<tag>` |

---

## 4. 代码地图：改哪里做什么

### 4.1 新增一个业务组件

1. 建 `components/<name>/`：`CMakeLists.txt`、`include/<name>.h`、`src/<name>.c`、`README.md`、
   （可选）`Kconfig`、`idf_component.yml`；
2. `CMakeLists.txt` 里 `REQUIRES` **只能向下**（`sys_utils` / `storage_manager` / `bsp` / `http_router`）；
3. 事件基址在头文件里 `ESP_EVENT_DECLARE_BASE(...)`，在 `.c` 里 `ESP_EVENT_DEFINE_BASE(...)`；
4. 要 HTTP：写 `xxx_register_http()` 并在 `xxx_init()` 里调用（**不要**改 `sync_server`）；
5. 要通知 UI/Web：`sys_notify("name", NULL)`；
6. 在 `main/app_main.c` 的 `s_modules[]` 加**一行**；
7. 在 `main/CMakeLists.txt` 的 `REQUIRES` 加组件名；
8. 在 `docs/API.md` 追加一节。

### 4.2 新增一个页面

`components/ui_pages/src/page_xxx.c` 导出 `const ui_page_t page_xxx`；
在 `pages_internal.h` 加 `extern`；在 `ui_pages.c` 的 `s_pages[]` 加一行；
在 `CMakeLists.txt` 的 `SRCS` 加文件；若需新 id，在 `ui_core.h` 的 `ui_page_id_t` 加枚举
（**顺序要与需求 §10.1 一致**）。

### 4.3 新增一个天气 provider

1. `components/weather_service/src/weather_provider_<name>.c`，实现 `weather_provider_t`
   （`geocode` + `fetch` 两个函数指针）；
2. 在 `weather_service.c` 的 `s_providers[]` 加一行；
3. 需要 Key 的把 `needs_api_key = true`，用户用 `weather_set_api_key()` 配。

### 4.4 新增一个待机表情

1. `idle_face.h` 的 `idle_face_state_t` 加值；
2. `idle_face.c` 的 `s_effects[]` 加一行参数（`open/dx/dy/pupil/arc/cross/eye_dy/width_scale/speed`）；
3. 需求文档 FR6 与 §11.2 同步补一行（**三处必须一致**）。

### 4.5 让某个业务事件驱动表情

在 `idle_face.c` 的 `s_triggers[]` 加一行 `{ "通知名", 状态, 持续毫秒 }`，
然后让生产者调 `sys_notify("通知名", NULL)`。**两处，无其它改动。**

### 4.6 新增一个 HTTP 接口

在拥有该资源的组件里写 handler，`http_router_register(path, method, handler)`。
模式里允许**一个 `*`**（`/api/v1/x/*`、`/api/v1/x/*/done`），**最具体者胜**。
出参用 `http_reply_ok()` / `http_reply_err()`；路径段用 `http_path_segment_u32(req, 3, &id)`。
预留未实现的用 `http_router_register_not_implemented()`（回 501）。

---

## 5. 上机验证清单

### 5.1 首轮已验证 ✅

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build
idf.py -p COM34 flash
# 抓串口（idf.py monitor 会独占终端、不能重定向）
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' tools\monitor_capture.py `
    --port COM34 --seconds 30 --out build\boot.log
```

| # | 检查项 | 结果 |
|---|---|---|
| 1 | 启动无 panic / 无看门狗 | ✅ 全绿 |
| 2 | 屏幕点亮、状态栏 + 首页 6 个卡片 | ✅（用户确认） |
| 3 | 中文不是方框 | ✅（用户确认，FreeType 生效） |
| 4 | 存储挂载 | ✅ `/sdcard`，状态栏显示 `SD` |
| 5 | 12 个模块 ready | ✅ 全部 ready |
| 6 | mDNS + HTTP | ✅ `todohelper.local` / 80 端口 / 37 条路由 |
| 7 | 内嵌字体落盘 | ✅ `fonts/ui_cjk.ttf`（1224120 字节） |
| 8 | SD 写入 | ✅ 词库 `words/cet4.json` |
| 9 | 音频硬件 | ✅ `mic=1 speaker=1` |
| 10 | 内部 RAM 余量 | ✅ 118 KB / 最大连续块 74 KB |

### 5.2 待验证 ⬜

| # | 检查项 | 期望 | 阻塞原因 |
|---|---|---|---|
| 11 | 触摸 | 点卡片进页面、"返回"能回退 | GT1151 已初始化，但从未点过 |
| 12 | 按键 | MODE 切首页/待办、SET 返回、能唤醒待机 | 4 个 ADC 按键已注册 |
| 13 | 待机 | 30 s 无操作 → 全黑 + 白线眼睛 + 眨眼 + 变暗 | `idle timeout 30 s` 已生效 |
| 14 | 页面切换动画 | 新页面 200 ms 淡入 + 向右滑入 | 本轮新增，未观察 |
| 15 | WiFi 关联 | 扫描列表 → 输入密码 → 状态栏显示 dBm | **需要凭据** |
| 16 | 浏览器待办页 | `http://todohelper.local/` 双向同步 | 需要 WiFi |
| 17 | 501/404 契约 | `/ws/events` → **501**；`/api/v1/nope` → **404** | 需要 WiFi |
| 18 | 天气 IP 定位 | 无城市时自动定位，`/api/v1/weather` 有温度 | 需要 WiFi |
| 19 | 日历 SNTP | 状态栏时钟开始走 | 需要 WiFi |
| 20 | 单词 | 翻卡 + 三档评分 + 进度写入 | 可离线验证 |
| 21 | 设置页 | 亮度滑杆真的改变暗度；token 后写操作需鉴权 | 可离线验证 |
| 22 | AI 对话 | 配 Key 后 `/api/v1/ai/chat` 有回复 | 需要凭据 |
| 23 | 内存稳定性 | 跑 10 分钟 `free_heap` 不持续下降 | `/api/v1/device/status` |
| 24 | 重启 | `POST /api/v1/device/reboot` 后 WiFi 免配、待办仍在 | 需要 WiFi |

### 5.3 最可能出问题的地方

首轮上机的头号风险（FreeType 字体、渲染栈、内部 RAM、SD mkdir）**已被证实并修复**，
详见 `AGENT.md` §9.0。**现在剩下的风险按大小排序**：

1. **AI 请求时的 TLS 缓冲** —— boot 结束内部 RAM 最大连续块 74 KB，而 mbedtls 的
   SSL in/out 缓冲默认各 16 KB。若首次 `POST /api/v1/ai/chat` 报 `ESP_ERR_NO_MEM` 或
   握手失败，就调小 `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN`/`OUT_CONTENT_LEN`，
   或开 `CONFIG_MBEDTLS_DYNAMIC_BUFFER`。**这是目前最值得优先实测的一项。**
2. **`ui_settings` 的滑杆读数** —— 用 `lv_obj_set_user_data` + 共享 250 ms 定时器刷新，
   逻辑上没问题但没跑过；若数值不刷新，改成每个滑杆自己的 `VALUE_CHANGED` 回调。
3. **触摸命中区域** —— 页面是 flex 布局，控件位置靠推演，第一次点可能发现偏差。
4. **`idle_face` 的 X 眼几何** —— 用文件作用域静态点数组，眼形切换时与椭圆是否对齐未验证。
5. **录音重采样边界** —— 48k 立体声下混 + 3 倍抽取到 16k，缓冲边界未实测
   （`mic=1` 只证明 I2S 通道开起来了）。

---

## 6. 已知风险与不确定点（诚实清单）

- **联网相关功能全部未验证**（WiFi 关联、HTTP 全链路、天气、SNTP、AI）——这是当前
  最大的未知面，因为它们的代码路径从未真正执行过。
- **触摸/按键/待机链路未验证** —— 屏幕能显示、能渲染，但输入侧一次都没试过。
- **AI 首次调用的内部 RAM 是否够** —— 见 §5.3 第 1 条。TLS 会话要 16~32 KB 内部 RAM，
  而最大连续块 74 KB，理论上够但没验证过；同理 `esp_http_client` 的缓冲。
- **天气 IP 定位端点**（ipapi.co）免费额度有限，返回字段依赖第三方；已做成可配置且
  解析器兼容 `latitude/lat`、`longitude/lon/lng` 两种拼写。
- **`voice_service` 的 ASR/TTS** 只定义了协议（ASR 收 raw PCM 回 `{"text":...}`；
  TTS 收 `{"text":...}` 回 `audio/pcm`），**没有对任何真实服务验证过**。
- **看门狗策略**：`CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`。已刻意避免在 LVGL 任务里做网络/
  阻塞 IO（天气/语音/AI 都在自己的任务里）。首轮上机出现过的唯一一次看门狗已定位为
  "`app_main` 未持 LVGL 锁"并修复（`AGENT.md` §9.0.1）。
- **`sys_log_set_level()` 会把 `wifi`/`phy`/`pp`/`net80211` 压到 ERROR** —— 这是刻意的
  （每条 PHY 速率都打一行 W 级日志，十几行淹没应用日志）。要抓 WiFi 详细日志时，
  临时把 `sys_log.c` 里 `quiet[]` 的对应 TAG 摘掉。
- **app 分区余量 50%**：OTA 需要额外分区，届时要重新规划 `partitions.csv`
  （当前 `factory` 8 MB + `storage` 2 MB + `assets` 5.9 MB ≈ 15.9 MB 已用满 16 MB flash）。
- **字体写入是首次启动的一次性成本**：约 1~2 秒写 1.2 MB 到 SD。若换更慢的卡可能更久；
  日志会打印耗时。想避免就预先手动把 TTF 放到 `fonts/ui_cjk.ttf`。

---

## 7. 推荐的下一步顺序

1. **上机验证**（§5 清单），修运行期问题。预计这是收益最大的一步。
2. **落实 §3.1 的 9 个决策**（尤其 SD/NAND、麦克风/喇叭、ASR/TTS 选型）。
3. **文档收尾**：把 §3.3 的偏离回改进需求文档；补 `voice_service/README.md`。
4. **P2 剩余**：MQTT/云同步、OTA、提醒与日程联动、词库替换为真实 CET4/CET6。
5. （可选）**把 `ui_pages` 的中文输入问题绕过**：例如接入蓝牙键盘或做一个拼音候选条——
   但这属于新增需求，需先与用户确认。

---

## 8. 命令与验证速查

```powershell
# 构建 / 烧录 / 监视
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build
idf.py -p COM34 flash monitor

# 改素材或组件后
idf.py reconfigure ; idf.py build

# 从零复现（会清 NVS 配置）
idf.py fullclean
patch -p1 < patches/esp_lvgl_adapter_idf61_support.patch
idf.py build

# 重新生成内嵌中文字体
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' tools\gen_cjk_font.py
```

```powershell
# HTTP 冒烟测试（把 <dev> 换成设备 IP 或 todohelper.local）
curl.exe http://<dev>/api/v1/device/info
curl.exe http://<dev>/api/v1/todos
curl.exe -X POST http://<dev>/api/v1/todos -H "Content-Type: application/json" -d "{\"title\":\"test\"}"
curl.exe -X POST http://<dev>/api/v1/sync
curl.exe -i http://<dev>/ws/events          # 期望 501
curl.exe -i http://<dev>/api/v1/nope        # 期望 404
curl.exe http://<dev>/api/v1/weather
curl.exe -X POST http://<dev>/api/v1/weather/locate
curl.exe http://<dev>/api/v1/voice/status
```

**验收判据**（需求 §15）：功能 10 条 + 健壮性 6 条 + `idf.py build` 出现
`Project build complete.`；烧录每个镜像后 `Hash of data verified.`，
结尾 `Hard resetting via RTS pin...` + `Done`。

---

## 9. 交接检查单（接手者勾一遍）

- [ ] 读完 `AGENT.md`（环境/命令/两条不可删配置/两个坑）
- [ ] 读完 `docs/TODOHELPER_FRAMEWORK.md` §2（分层）与 §7（扩展点）
- [ ] 能独立跑通 `idf.py build`，认得 `Project build complete.`
- [ ] 知道 `sdkconfig` 是生成物（改默认值要改 `sdkconfig.defaults` 并删 `sdkconfig`）
- [ ] 知道 `managed_components/` 重新下载后**必须重新打 patch**
- [ ] 知道 `assets/ui/` **必须扁平**、字体走 `EMBED_FILES`
- [ ] 知道 **无 RTC / 无 GPS / 无传感器** 三条硬件前提
- [ ] 知道 `sys_time_ts()` 未同步返回 0，UI 不得显示 1970
- [ ] 按 §5 清单完成了首次上机验证，并把结果回填到本文档
- [ ] 从 §3.1 挑了下一项决策，或已向用户提问
