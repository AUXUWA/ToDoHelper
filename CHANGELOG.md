# 变更历史（CHANGELOG）

版本号规则见 `AGENT.md §8.1`：**MAJOR** = 一轮需求全部完成或有破坏性变更（存储格式 / HTTP API /
NVS 键语义）；**MINOR** = 新增功能或页面；**PATCH** = 修 bug、调参、文档。
固件版本来自工程根目录 `version.txt`，并由 git tag `vX.Y.Z` 标记。

---

## [1.1.0] — 2026-09-23

**待机动画与开机交接动画的增强。** 需求条目没有变化，改的是"看起来像活的"这件事。

### Added
- **待机时的慢动作三层**（`idle_face`，全部只改已有对象属性：零新对象、零分配）：
  1. **自发表情**：每 13–34 s 随机演一个 320–1100 ms 的表情（HAPPY / SLEEPY / LISTENING /
     THINKING / SPEAKING），优先级 `眨眼 > 通知/AI 表情 > 自发表情 > 基础状态`，
     绝不超出自己的时长，也不会在注视/扫视飞行途中启动；
  2. **困倦度** 0→1：无人交互约 2.5 分钟攒满 —— 眨眼更密（间隔 ×0.65）更慢（时长 ×1.6）、
     眼睛矮 10%、视线下垂 5 px；任何真实交互（通知/AI 表情、唤醒）立刻清零，
     **自发表情不清零**（那只是发呆；第一版每次发呆都清零，实测 `drowsy` 永远在 0–3%）；
  3. **瞳孔缩放**（5–11 s 重抽 0.91–1.14 的缓慢随机游走，LISTENING 再放大 6%）与
     **整脸 ±3/±2 px 缓慢漂移**（两条不相干频率正弦合成；表情占用时缓动归零，
     保证 ERROR 的 X 线与眼睛始终对齐）。
- 眨眼/注视的多样性：28% 的眨眼只眯到 40–62%、9% 是 240–380 ms 的慢眨、
  22% 的注视结束后立刻向另一侧再看一眼（幅度 55–80%）。
- **切页改成交叉淡入**（`ui_core`）：新页面 `translate_x 28→0` + 淡入（220 ms ease-out），
  旧页面 `translate_y 0→-16` + 淡出（240 ms ease-in），**动画结束才加 `HIDDEN`**。
  开机日志 → 主界面因此不再是一瞬间的硬切；页面也第一次有了"离场动画窗口"。
- 开机页状态行加**动画省略号**（每 420 ms 多一个点），启动过程不再像卡死。

### Fixed
- **`idle_face_standby_exit()` 是死代码**：它要求"当前处于 SLEEPY 姿态"，而
  `standby_enter()` 早已改成不闭眼，于是恒返回 `ESP_ERR_INVALID_STATE`，那段睁眼过场
  从未执行过。现在只要求没有**仍然有效**的表情占用。
- **首次进待机被静默拒绝**：动画器定时器是待机页**创建时**才启动的，所以页面创建那一刻
  可能还挂着一个几分钟前就该过期的临时表情（实测：t=7.7 s 的 2.5 s `wifi_connected`
  表情在 t=82 s 仍被当成"有效"）。现在 `standby_enter/exit` 都先调 `override_expire_due()`
  把已过期的清掉。
- 开机页显示的固件版本一直是 **`0.1.0`**：它读 `CONFIG_APP_PROJECT_VER`，而该符号只在
  `CONFIG_APP_PROJECT_VER_FROM_CONFIG` 打开时才存在。现在与设置页一致，用
  `esp_app_get_description()->version`（即 `version.txt`）。

### Verified（真机 COM34，插卡 `root=/sdcard`）
- 冷启动：0 panic、9 个模块全部 ready、`device/info` 的 `version` = `1.1.0`、
  内部 RAM `int-low 55 KB`（新动画没有增加内部 RAM 压力）。
- 200 s 静置：`idle timeout reached` → `standby enter (face stays awake)`（**不再被拒绝**）→
  自发表情实测 4 次，类型与时长各不相同（SPEAKING 353 ms、HAPPY 416 ms、LISTENING 828 ms、
  SLEEPY 633 ms），间隔 19.8–29.3 s（设计区间 13–34 s）；困倦度 15% → 29% → 42% → 61% 单调上升。
- **主观观感待用户目视确认**：三种慢动作是否自然、退出待机时的睁眼、开机日志 → 主界面的淡入。

---

## [1.0.0] — 2026-09-23

**首个编号版本。** 第二轮 8 项需求全部完成，两个阻塞性缺陷定位到真实根因并修复，
真机（ESP32-S31-Korvo，COM34，插卡 `root=/sdcard`）验证通过；四项只能靠人眼确认的
体验项也已由用户确认正常。

### Added
- `version.txt`（= `1.0.0`）：固件版本从此可追踪，并出现在设备「设置」页顶部
  （`固件 1.0.0 · IDF v6.1`）、`GET /api/v1/device/info` 的 `version` 与 app descriptor 里。
- `components/sys_utils/src/sys_json.c`：`sys_json_use_psram()` 把 cJSON 的分配改成
  **先 PSRAM、失败回退默认堆**（`cJSON_InitHooks`）。这是插卡启动崩溃的修复本体。
- `sys_heap_log()` 新增 **`int-low`** 列（内部 RAM 历史最低水位），替代原来被 12 MB PSRAM
  主导、永远不动的 `min` 列 —— 这一类"内部 RAM 被瞬时抽干"的 bug 从此一眼可见。
- 单词书解析诊断：`book tree alive: internal N KB free`、`loaded 833 words from '...' (120311 B)`。
- SSE 每个订阅者一个流式任务（`sync_server.c` 的 `sse_client_task`），
  以及"挂着流时普通请求必须仍然可用"的回归测试步骤。

### Fixed
- **插 SD 卡即崩溃重启循环**（阻塞级）。真因不是此前记录的"crypto 锁惰性初始化竞态"，
  而是：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` 让 ≤4 KB 的分配优先落在内部 RAM，
  而解析 546 KB / 833 词 / 约 7000 个 cJSON 节点的词库把 ~107 KB 内部空闲抽到接近 0；
  同时刻正在连网的 WiFi 驱动调用 `esp_timer_create()`（`MALLOC_CAP_INTERNAL`，**不许回退
  PSRAM**）拿到 `ESP_ERR_NO_MEM`，外面包着 `ESP_ERROR_CHECK` → abort。拔卡时解析的是内置
  32 词小词库，故"只有插卡才崩"。
  验证：插卡冷启动 ×3，`abort=0/3`、`TH:app: ready 3/3`、词库 3/3 加载成功。
- **SSE 长连接冻住整个 HTTP 服务器**（用户实测：页面显示"推送：实时推送中"却"设备不可达/
  读取失败"）。`esp_http_server` 的 URI handler 在唯一 httpd 任务里串行执行，而
  `/api/v1/events` 是"不返回"的循环，于是挂上 1 个订阅者后 `accept()` 都不再执行
  （ping 通、TCP 全超时）。改为 `httpd_req_async_handler_begin()` + 每客户端任务。
  验证：挂 3 条流时 `GET /` 仍 200/42 ms、第 4 条流 503、`POST /api/v1/todos` 后流内收到
  `event: todo_changed`、客户端退出后 `sse_clients` 归 0。

### Changed
- SSE 并发上限 **4 → 3**（`SYNC_SSE_MAX_CLIENTS`）：每个订阅者占 1 个 httpd 会话槽（共 5）
  加 1 个 3 KB 任务，必须给页面与 REST 留槽；网页相应文案同步更新，超限仍优雅降级到 15 s 轮询。
- 删除 `main/app_main.c` 里"启动期预初始化 crypto 锁"的代码 —— 它出自被推翻的归因、实测无效，
  留着只会误导后来人。
- `word_service` 的 `items[]` / `progress[]` 改为显式
  `heap_caps_*_prefer(..., MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)`。
- 文档全量回改：根 `README.md` 由上游 factory_demo 说明改写为 ToDoHelper 说明；
  `AGENT.md §9.0.1` 从"三个自伤 bug"扩为四个（新增 HTTP 长连接 handler）；
  `AGENT.md §9.0.5` 重写为实测根因并保留两处误读作为教训；`AGENT.md §8` 基线表更新为本机实测值；
  `docs/HANDOVER.md`、`docs/API.md`、`docs/TODOHELPER_FRAMEWORK.md`、
  `docs/TODOHELPER_REQUIREMENTS.md` 与相关组件 README 同步。

### Verified（真机）
- 插卡冷启动 ×3：0 panic、9 模块 ready、`TH:app: ready (storage=/sdcard)`、词库 833 词。
- 启动结束内部 RAM：`internal 67 KB / largest 24 KB / int-low 55 KB`。
- 端到端：`GET /api/v1/words/stats` → `book=cet6 total=833`；`GET /api/v1/wifi/status` → connected；
  `POST+GET /api/v1/wifi/scan` → 22 个 AP 且设备存活；网页 `GET /` 200/65 ms。
- 人眼确认（用户）：待机小眼睛保持睁眼并按时眨眼/扫视；开机 3 秒滚动日志顺畅；
  待办页勾选/删除按钮可用且不误触；网页刷新后状态与列表正常、推送与页面共存。

### Known gaps
- `/api/v1/ai/chat/stream` 与阻塞版 `/api/v1/ai/chat` 仍是同步 handler：一次 AI 回复期间
  整台设备的 HTTP 会不可用（网页不用这两个接口，所以不影响上位机）。**用户决定本轮先不动**，
  修法记录在 `AGENT.md §9.0.1` 与 `docs/HANDOVER.md §7`。
- 网页 WiFi 配置面板未做（设备端 7 条 `/api/v1/wifi/*` 路由已就绪）。
- 语音 ASR/TTS 端点选型未定（录音回放本身可用）。
- CET-6 词库是 833 条自编精选子集，不是官方约 2000 词表。

---

## 无版本号阶段（0.1.0 … 0.9.0，按提交顺序）

编号版本之前的工作，按时间顺序（提交见 `docs/HANDOVER.md §6`）：

1. **需求书全量实现 + 文档回改**：`docs/TODOHELPER_REQUIREMENTS.md` v0.5，含 15 处实现偏差记录。
2. **硬件 bring-up 与内存优化**：大量数据改放 SD 卡；UI 参考官方工程；待机动画参考 esp-vocat；
   字体放 SD 解决"中文方框"；烧录提速。
3. **一批现场 bug**：WiFi 扫描崩溃、连接崩溃、日志可读性、键盘样式统一、日历排版、
   天气页显示 "Text"、待机/唤醒行为、待机后设备失联。
4. **天气纯 IP 定位**：去掉手输城市、修 `lat/lon=0` 解析、默认城市设为南昌。
5. **性能与体验**：烧录 147.9 s → 31.6 s（4.7×）；丰富待机动画；单词书 CET-4 → CET-6。
6. **第二轮收尾 8 项**（`561a591`）：待办同步排查、待机眼睛动画、cet6 词库、SD 卡资源、
   设备端待办保留勾选/删除、网页上位机重写、开机屏幕滚动日志、子 agent 分工与文档同步。
7. **字库移出 app**（`162ba2d`）：镜像 4.16 MB → 2.94 MB，烧录 43.6 s → 31.6 s；
   字库改为只从 `<storage>/todohelper/fonts/ui_cjk.ttf` 加载。
8. **交接与两次根因更正**（`0936a27` → `b4d7683` → `86ba312`）：
   交接文档重写；把插卡崩溃从"crypto 锁竞态"更正为"JSON 解析抽干内部 RAM"并修复；
   把 SSE 冻住服务器的问题修复（详见 1.0.0 的 Fixed）。
