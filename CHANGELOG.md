# 变更历史（CHANGELOG）

版本号规则见 `AGENT.md §8.1`：**MAJOR** = 一轮需求全部完成或有破坏性变更（存储格式 / HTTP API /
NVS 键语义）；**MINOR** = 新增功能或页面；**PATCH** = 修 bug、调参、文档。
固件版本来自工程根目录 `version.txt`，并由 git tag `vX.Y.Z` 标记。

---

## [1.2.0] — 2026-09-24

**系统流畅度优化（第一轮：消除锁内阻塞 I/O + 高频无效重绘）。** 需求条目没有变化，
改的是"界面跟不跟手"这件事。**没有破坏性变更**：HTTP API、NVS 键语义、存储格式全部不变。

根因一句话：**适配器在持有递归锁的情况下调用 `lv_timer_handler()`，所以所有 `lv_timer`
回调与控件事件回调都在锁内运行**，回调里任何同步 I/O 都会冻结整个 UI。详见新增的
`AGENT.md §9.0.1.1`（流畅度五条铁律）。

### Fixed
- **点"校时"会冻屏最多 10 秒**（`page_calendar` / `page_settings`）：点击回调直接调
  `calendar_sync_ntp(0)` → `esp_netif_sntp_sync_wait()`，持锁等待网络往返；而且因为
  `ui_post_event()` 当时要取同一把锁，**整个事件系统一起冻住**（SSE、待机表情、
  各服务的事件消费者全部停摆）。
- **每次单词评分都会卡一下**（`page_word` → `word_review` → `word_save_progress`）：
  持锁做 cJSON 序列化 + SD 卡整文件重写（open→write→remove→rename）。设置页的
  "每日单词数 ±5"、时区切换（NVS flash 写）同理。
- **设置页的 250 ms 滑杆定时器永不释放**：句柄没保存、页面隐藏后仍以 4 Hz 重写 4 个标签。
  现在保存句柄、`on_hide` 暂停、`on_show` 恢复，且值没变就不写。
- **上机实测的一次 `interrupt wdt timeout`**（`MCAUSE=0x18`，两核栈都在
  `vListInsert`/`xQueueSemaphoreTake`；触发条件是"WiFi 切换期 DNS 失败 + 通知突发"）：
  事件排空对每个节点调一次 `lv_async_call()`，打满其小固定池后退化为反复 malloc/free。
  改为在排空回调内直接派发（见 Changed）。

### Changed
- **事件投递改为无锁队列**（`ui_core.c`）：`ui_post_event()` 不再取 LVGL 锁，只把节点挂到
  单链表（短临界区），由 ui_core 自己的 20 ms `lv_timer` 在 LVGL 任务内排空并派发。
  队列上限 64，满了丢事件并告警（丢通知可接受，阻塞发布者不可接受）。发布者
  （事件循环 / 各 worker）与渲染任务彻底解耦，不再有双向阻塞。
- **新增异步 API**：
  - `calendar_sync_ntp_async()` + `CALENDAR_EVENT_SYNC_RESULT` 事件；校时跑在按需创建的
    `th_ntp` 任务（3072 B / prio 4）上，UI 用 toast 呈现结果。
  - `word_save_progress_async()`：600 ms 合并窗口，连续评分 / 连点只写一次卡；
    `word_review()` / `word_set_daily_count()` / `word_reset_progress()` 全部改走它。
  - 时区 NVS 提交改由一次性任务 `th_tz` 执行（flash 写会短暂关闭 cache）。
- **所有周期性 label 刷新加脏检查**（文本 / 字段不变则不写；`lv_label_set_text*` 即使文本
  相同也会 invalidate 对象并触发重排）：状态栏 4 个标签、首页 summary 与时钟、日历时钟、
  WiFi 状态块、设置页信息块。状态栏此前每秒无脑重写 4 标签 + 4 次改色。
- `calendar_sync_ntp()` 与 `word_save_progress()` 的**头注释明确标注"禁止在 LVGL 任务调用"**，
  避免后来人再把同步版本接回 UI 路径。
- **切页动画全部删除，改为同帧原子替换**（`ui_core`）。原来有 220/240 ms 的
  `translate_x`/`translate_y` + 整屏 `opa` 双向动画。删除理由：
  1. **正确性**：两个页面容器同尺寸且不透明，动画期间只要不完全重叠就会把上一页从缝隙里
     露出来；更糟的是**旧页的隐藏只依赖退场动画的完成回调**，而 `page_anim_reset()` 会
     `lv_anim_delete()` 掉那个动画（回调一并删除），于是"退场结束前切走或切回"就永久留下
     残影 —— 这正是用户报告的"旧页面残留/重影不消失"。
  2. **成本**：整屏 800×480 alpha 动画让每个像素走软件混合路径十几帧。
  新增 `pages_show_only()`：每次切页把其余容器复位并隐藏，把"任意时刻只有一个页面容器可见"
  这条不变量显式钉死（不再依赖动画回调的时序）。
  副作用（已记录在 `idle_face/README.md`）：`idle_face_standby_exit()` 的睁眼过冲因此
  **在线下不再可见**，因为待机页同帧就消失了。这是为切页正确性付出的代价，不是 bug。

### Fixed
- **切页重影**（用户实测报告）：见上方 Changed。

### Added
- **待机动画新增四层连续/反射性动作**（`idle_face`）。此前所有层（眨眼/注视/扫视/呼吸/
  自发表情/困倦/瞳孔缩放/整脸漂移）都是**离散事件**，事件之间眼睛完全静止，盯久了仍能看出
  "在摆姿势"。四层全部是加法、有界、零分配（只扰动 `apply_params()` 已算出的值）：
  1. **连续视线游移**：两轴 Lissajous 缓慢漂移，频率故意不可通约，路径永不闭合；
  2. **眼睑回弹**：眨眼睁开后眼睑过冲 5.5% 再回落（260 ms），不再是"到位即停"；
  3. **注意力提振**：每 17–46 s 一次，眼睛放大 + 瞳孔收缩约 520 ms，像"注意到什么"。
     不改表情状态，所以读起来是反射而非情绪；
  4. **眨眼不对称**：30% 的眨眼两片眼睑深浅不同，且哪只眼更深随机。
  四层都会在任何真实表情到来时复位（否则进行中的 perk 会把 ERROR 脸拉宽）。
- **待机动画再增三层，打破"两只眼一起动"**（同样零分配、有界）：
  5. **呼吸联动**：呼吸的正弦同时把整对眼上下抬降 1.6 px。复用已算好的
     `breath_phase`，所以不存在第二个时钟，两者不会走散；
  6. **双眼扫视错峰**：微扫视时一只眼带衰减偏移，约 60 ms 后跟上，两眼从不完全同步。
     哪只眼迟到每次重抽（否则同一只眼总慢半拍，规律本身成为破绽）；
  7. **困倦点头**：困倦度 ≥ 0.55 后偶尔眼睑慢慢合上再抬起（520–900 ms）。
     曲线故意不对称（前 35% 合上、后 65% 抬起），才像"抗不住睡意"而非慢眨眼。
     三道门闩（困倦度阈值 / 8–26 s 随机间隔 / 45% 概率）让它保持为事件而非节奏。
- **`CONFIG_TODOHELPER_FPS_PROBE`**（默认关）：打开后状态栏的 1 Hz 定时器每秒打一行
  `TH:ui_core: fps N`，用于用数据而非印象判断流畅度。

### Fixed
- **切页重影**（用户实测报告）：整页过渡动画被完全移除。应用户要求切页改为**同帧原子替换**，
  详见下方 Changed 与 `AGENT.md §9.0.1.1`。
- **待机时眼睛发灰**：待机亮度默认值 20 → 100。眼睛本身一直是纯白
  （`UI_COLOR_IDLE_FG=0xFFFFFF`），发灰是背光被降亮造成的。设置页「待机亮度」滑杆保留，
  想调暗仍可用。
- **一次性 NVS 迁移**（`brt_migr` 标记位）：旧设备 NVS 里的 `idle_bright=20` 会盖过新默认值。
  用标记位而非直接 `erase` —— `nvs_erase_key()` 对不存在的 key 也返回 `ESP_OK`，
  无保护地调用会**每次开机都执行**，从而抹掉用户后来设置的亮度（实测复现）。

### 实测（COM34，插卡 `root=/sdcard`）
- 冷启动 ×4：`abort=0`、9 模块全 ready、`boot complete internal 64 KB (largest 20 KB)`、
  `int-low 52 KB` —— 与优化前的 67/24/55 KB 基本持平，说明异步化与新增动画没有吃掉内部 RAM
  （新任务都是按需创建、平时不占栈；新动画无新对象、无新定时器）。
- 触发条件复现 ×3（DNS 失败 + 12 路并发写 + weather/calendar 通知）：0 panic。
- HTTP 端到端：8 路并发 POST 全部落库、GET/DELETE 正常，设备稳定。
- **关键验证**：3.48 s 的 `POST /api/v1/weather/refresh` 进行期间，5 个 `/api/v1/todos`
  请求全部约 60 ms 返回（改前会排队到超时）。
- **待机动画**：打开 FPS 探针实测稳定 **28~30 fps**（设计值 `IDLE_FACE_TICK_MS=33`），
  无掉帧；用临时 INFO 探针确认新层确实执行：`attention perk` 与 `idle mood` 各触发 5 次，
  且困倦度正常累积（15%→28%→49%→67%）；另一轮 250 s 窗口确认
  **困倦点头按设计门闩触发**（第一次 `drowsy 70%`、第二次 `86%`，时长 690/865 ms，
  即它会等困倦度积累而不是按时钟乱放）。
- 用户目视确认：切页无残留、待机全亮纯白、新动画正常。

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
- **主观观感已由用户目视确认（2026-09-23，"可以了"）**：三种慢动作自然、退出待机时的睁眼
  可见、开机日志 → 主界面的淡入符合预期，无需再调参。

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
