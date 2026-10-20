# 变更历史（CHANGELOG）

版本号规则见 `AGENT.md §8.1`：**MAJOR** = 一轮需求全部完成或有破坏性变更（存储格式 / HTTP API /
NVS 键语义）；**MINOR** = 新增功能或页面；**PATCH** = 修 bug、调参、文档。
固件版本来自工程根目录 `version.txt`，并由 git tag `vX.Y.Z` 标记。

---

## [1.5.0] — 2026-10-20

**「待办清单」补成一条真正能用的主线：设备端看得到截止与优先级、能筛选、能撤销误删；
网页端能批量、能排序、能导出导入；落盘不再在点按的瞬间同步写卡；坏文件不再能让设备开机就崩。**

> 本轮不是「加功能」，是先把工程做了一次**六方向并行体检**（69 条问题，其中 13 条高危经独立复核**全部成立**），
> 再按性价比敲定范围施工。施工规格见 `docs/ROADMAP_v1.5.0.md`（含每条目的验收标准与全局硬约束）。
>
> **HTTP 契约**：新增 3 条路由、鉴权范围扩大（只读接口开始要 token）——见下方「行为变更」。

### 起因（用户原话）

> 完善这个 to do 列表功能，优化并升级 ui 以及其他功能，优化性能。

### 行为变更（不兼容，需要留意）

| 变更 | 说明 |
|---|---|
| **鉴权模型收敛成两档** | **没设令牌** → 局域网默认不校验（所有路由都通）；**设了令牌** → **所有路由都要带 `X-Auth-Token`，包括 GET**。此前是「写方法要令牌、读方法不要」——也就是**开了鉴权，待办清单和 AI 对话记录仍然是公开可读的**，而网页一直告诉用户相反的话。网页「连接设置」新增「让设备用这个令牌」「关闭设备鉴权」两个按钮（此前网页**无法**设置第一个令牌） |
| **落盘时机变了** | 待办的增删改**不再同步写卡**，改成去抖后台落盘（安静期 400 ms）。需要立刻落盘的入口（导入后、网页端改完、关机前）走 `todo_save()` |
| **新增 3 条路由** | `POST /api/v1/todos/ack`、`GET /api/v1/todos/export`、`POST /api/v1/todos/import` |
| **待办固定存片内** | 待办清单改存片内 LittleFS（不再跟着「存储根」跑），插拔 SD 卡不再让待办看起来凭空消失；首次启动会自动把 SD 上的旧清单迁移过来 |

### 必修 bug（本轮修掉的，都是已独立复核确认的）

| # | 位置 | 问题 | 怎么修的 |
|---|---|---|---|
| 1 | `todo_manager.c` | **设备端点一下复选框，就同步重写整份 `todos.json`**——LVGL 回调里做 SD/flash 写（违反流畅度铁律一），而 `todo_save()` 抢 `s_io_lock` 用的是 `portMAX_DELAY`，httpd 任务正在写卡时，LVGL 任务会**持着适配器锁无限期等** | 改成去抖后台落盘（`todo_mark_dirty()` + `th_todo_save` 线程，400 ms 安静期）；写锁等待上限 3 s，超时返回 `ESP_ERR_TIMEOUT` |
| 2 | `storage_manager.c` | **落盘不是原子的**：先 `remove(目标)` 再 `rename(tmp→目标)`，两步之间掉电，上一份有效清单和新临时文件**一起消失**。README 宣称的「原子写」当时并不成立 | 改成**双槽协议**：`X.new` → `X` 改名 `X.bak` → `X.new` 改名 `X`；启动时 `storage_recover_tree()` 把残留的 `.new`/`.bak` 收敛回来。（关键约束：FatFs 的 `f_rename()` 目标存在时返回 `FR_EXIST` 而不是覆盖，所以**不能**简单删掉 `remove()`） |
| 3 | `todo_manager.c` / 全工程 | **畸形 JSON 会爆栈**：`cJSON_Parse` 每层递归 96 字节（objdump 实测），cJSON 自带上限 1000 层 → 最坏 96 KB 栈，而开机主任务只有 12 KB。`todos.json` 是用户能直接编辑的文件，**一个坏文件就能让设备每次开机都崩** | 新增 `sys_json_parse(json, max_depth)`：解析前先扫文本统计嵌套深度，超限直接拒绝；所有解析外部输入的地方都改用它（待办/词库/墨墨/AI/网页推送） |
| 4 | `word_service_http.c` | `GET /api/v1/words/history` 把**栈上局部变量的地址**交给异步 worker，handler 返回后 worker 再解引用 → use-after-return，接口行为随机 | 参数改成按值复制进 worker 自己的结构体 |
| 5 | `word_service.c` | 云端独占词评分时，写进学习记录的是队列里**第一个**云端词的拼写，不是用户刚评的那张卡 → 设备端学习记录全错 | 把「刚评的那张卡的拼写」显式传进记录函数 |
| 6 | `ai_service.c` | AI 对话历史用静态数组占 **24.4 KB 常驻内部 DRAM**（开机后 66 KB 内部 RAM 余量的 37%），而 AI 只是可选的 P2 功能 | 改成按需分配到 PSRAM；8 KB 的 `th_ai` 任务改成按需创建 |
| 7 | `ai_service_http.c` | `POST /api/v1/ai/chat` 与 `/chat/stream` 在唯一的 httpd 任务里跑完整轮请求（超时上限 **45 s**），期间设备任何 TCP 连接都建不上；对话存档还在 httpd 任务里写 SD 卡 | 照 `sync_server.c` 的 SSE 模式改异步（`httpd_req_async_handler_begin()` + 自有任务 + `httpd_req_async_handler_complete()`），对外报文格式不变 |
| 8 | `weather_service_http.c` | `POST /api/v1/weather/locate` 同步跑最长 12 s 的 HTTPS 定位，整台设备的 HTTP 全程不可用；设备端的 `city_clicked_cb()` 还会把 UI 冻 12 s | 同样改成异步（请求-响应式，不是流式） |
| 9 | `sync_server.c` | 「即使读操作也强制鉴权」的名单**从未生效**，任何 GET 都不校验令牌 | 让名单真正生效：路由器在路由被标记时对 GET 也调鉴权钩子；`GET /api/v1/todos*` 与 `GET /api/v1/ai/history` 被标记 |
| 10 | `todo_manager.c` | 设备端改过的待办**永远停在「待同步」**，除非用户去网页手动点「标记已同步」——与 FR2「联网后自动同步」相悖 | 新增 `POST /api/v1/todos/ack`：客户端收到推送并把数据吃进本地列表后回执，设备据此标记这几条 |
| 11 | `todo_manager.c` | 导入时 priority 不夹取（可造出比「紧急」更高的优先级）、可造出**重复 id**（之后更新/删除会作用到错的条目）、标题按字节截断会把 UTF-8 **切成半截** | 统一夹取；导入时 id 去重；新增 UTF-8 安全截断 |
| 12 | `page_todo.c` | 删除按钮**一按即永久删除**，无确认无撤销，误触即丢数据 | 新增 `ui_dialog_confirm()`（取消 + 危险色确认），删除前必须确认 |
| 13 | `todo_manager_http.c` | 每次 HTTP 写操作把整份清单**写盘两遍**（`todo_add()` 一次、`todo_mark_synced()` 一次），httpd 是单任务，其它请求被整段堵住 | 两次变更合并成一次去抖写盘 |
| 14 | `todo_manager.c` / `Kconfig` | `CONFIG_TODOHELPER_TODO_BACKUP_KEEP` 是个**死配置**（没有任何代码读它），清单只有一份副本 | 导出/导入通道（`/api/v1/todos/export` `/import`）+ 存储层通用 `.bak` 兜底；Kconfig 的说明同步更正 |
| 15 | `idle_face.c` | 待机动画的 30 Hz 定时器**创建后永不暂停**：第一次进过待机后，即使回到别的页面，LVGL 任务仍被每秒唤醒 30 次去更新**隐藏对象** | 不在待机时暂停定时器，回到待机再恢复（对表情连续性做了保护） |
| 16 | `weather_service.c` | 天气响应缓冲首分配 2048 B 落内部 RAM，再用 `realloc` 长到 16 KB——`realloc` 复用内部堆，**不会自动搬到 PSRAM** | 改用 `heap_caps_malloc_prefer(..., SPIRAM, DEFAULT)` |
| 17 | `sync_server.c` | 令牌每个请求都从 NVS 重读（两遍 `nvs_get_str` + 全局配置锁） | 令牌缓存到内存，设置时刷新；令牌**不写日志** |
| 18 | `sync_server.c` | `s_sse_clients` 计数在 httpd 任务与 SSE 任务之间**无锁**读改写，丢一次更新后设备会丢弃所有推送事件，而网页仍显示「推送中」 | 加互斥量（临界区极短） |
| 19 | `word_service.c` | `review_log.jsonl` 追加失败时**那一行已经被出队**，永久丢失；掉电撕裂的半行会与下一次追加粘成一行 | 失败时把行放回队列；追加前先检查文件尾是否完整 |
| 20 | `maimemo_http.c` | `POST /api/v1/maimemo/advance` 每次成功调用**泄漏**一份 job 与其中的拼写字符串 | worker 结束时释放 |
| 21 | `maimemo_client.c` | 释义缓存命中条件写错：有助记但释义为空时永远不命中 → 每次翻卡重跑 4 次 HTTPS 往返并整文件重写 `enrich.json` | 修正命中条件 |
| 22 | `ai_service.c` | `DELETE /api/v1/ai/history` 只清内存、**不删磁盘文件**，重启后对话又回来 | 一并删除磁盘上的存档 |
| 23 | `ui_core.c` | 事件分发**不检查目标页是否可见**：用户在主页时，网页每改一条待办，设备就把整份清单复制一遍并逐行刷新 | 不可见时只记脏标记，等 `on_show` 再刷新 |
| 24 | `page_todo.c` | 勾选框 34×34、删除按钮 52×34，都小于工程自己规定的 44 px 触摸目标；点勾选框下缘会落到标题上变成「打开详情页」 | 全部改成 ≥ 44×44 |

### Added（待办主线）

- **设备端待办行现在看得到截止与优先级**：到期徽标（逾期 / 今天 / 明天 / M月D日，未校时时**不编造日期**）
  + 优先级色条（紧急=危险色、重要=警告色）。徽标与色条只在内容变化时更新。
- **设备端筛选栏**：全部 / 未完成 / 今天到期 / 已完成，取数走 `todo_list_filtered()`，
  统计行跟着筛选走。
- **首页「今日待办」卡片**：今天到期且未完成的条数与进度、最多 3 条标题、点击直达待办页；
  1 Hz tick 下用 `todo_version()` 做脏检查，内容没变一个字都不改。
- **详情页到期时间格式化**：`2026-10-21 09:00`（此前设备上直接显示 `到期 1750000000`），
  优先级显示中文（普通/重要/紧急），未校时显示「未校时」。
- **删除二次确认**（新增 `ui_dialog_confirm()`：取消 + 危险色确认，回调拿得到弹窗对象）。
- **网页端批量操作**：「清除全部已完成」「全部标记完成」，**串行**发送（并发会把设备唯一的
  httpd 任务打爆），带进度显示与二次确认。
- **网页端排序**：按到期 / 优先级 / 更新时间 / 添加顺序，语义与设备端一致。
- **网页端导出 / 导入**：一键下载整份清单 JSON，一键回灌。
- **待办导出 / 导入 HTTP 路由**：`GET /api/v1/todos/export`、`POST /api/v1/todos/import`。
- **待办回执路由**：`POST /api/v1/todos/ack`。
- **新的只读接口**：`todo_due_bucket()` / `todo_list_filtered()` / `todo_version()` /
  `todo_mark_dirty()` / `todo_count_due_today()` / `sys_json_parse()` /
  `storage_write_cache_pinned()` / `storage_read_cache_pinned()` / `ui_dialog_confirm()`。

### 体验与性能

- **设备端点一下待办不再卡**：勾选/删除只改内存并置脏，落盘交给后台线程。
- **列表渲染有上限**：最多建 30 行（每行约 8 个 LVGL 对象，全落内部 RAM，而内部 RAM 余量只有 66 KB），
  超出部分在底部提示「还有 N 条未显示（在电脑网页端查看全部）」。
- **页面不可见时不再整页刷新**。
- **待机动画定时器不再空转**（不在待机时暂停）。
- **网页端 SSE 加心跳看门狗**：连接半开时页面不再永远显示「推送中」而列表停在旧数据上。
- **网页端错误提示显示设备给的 `detail`**（此前只显示英文原文 `internal error`）。
- **网页端文案纠错**：「推送客户端 N / 4」→ 3（设备上限是 3）；「设备屏幕只读」→ 设备其实可以勾选/删除；
  鉴权说明与设备实际行为对齐。
- **网页端删除二次确认不再被列表重绘清掉**（此前「反复点删不掉」）。
- **网页端长耗时请求（墨墨校验/探测，30~60 s）不再堵住整页其它请求**。
- **天气每 15 分钟不再无条件重写 3 个 NVS 键**（值没变就不写 flash）。
- **AI 与天气任务的栈按 TLS 握手需要给足**（`esp_http_client` 没有自己的任务，握手吃调用者的栈）。

### 上机后补的三处（烧到板子上才发现的）

> 下面三条是**真机跑出来**的，不是纸面推演。烧上板子抓串口日志时才发现，
> 也因此才看清了旧版落盘协议到底怎么丢数据的。

1. **旧协议掉电丢掉的清单，现在能认领回来。**
   v1.4.0 的落盘协议是「写 `X.tmp` → `remove(X)` → `rename(X.tmp, X)`」，
   掉电落在 `remove` 与 `rename` 之间时，**正式文件被删了、只剩 `X.tmp`**——
   而 `X.tmp` 是写完的完整内容（先写完才改名），所以它是用户**丢掉的那份清单**。
   本次上机的板子上就是这样：卡上只剩 `todos.json.tmp`，正式文件没了。
   → `todo_manager` 启动时若片内没有清单，就去卡上找 `.tmp` / `.new` / `.bak`，
   **校验是合法 JSON 且含 items 数组**才认领（坏文件不能把开机路径搞崩）。
2. **「两份清单哪份更新」不能靠文件时间戳判断。**
   LittleFS 不保留源文件时间戳，两个后端的 mtime 都不可靠。实测就判错过：
   卡上是 4 条（next id 48，完整的那份），片内是 2 条（next id 3，某次没插卡的残留），
   mtime 规则却留了旧的那份。
   → 改成按**清单自己带的信息**比较：最新一条的 `updated_at` → `next_id` → 条数。
   存储层只负责「片内没有就补一份」，不再猜谁更新。
3. **每次开机都把同一个文件重迁一遍。**
   迁移判据同样用了不可靠的 mtime，导致开机日志里每次都出现
   `the SD copy of 'todos.json.tmp' is newer; replacing the internal one`。
   → 迁移只补**缺失**的文件，并且跳过协议临时件（`.tmp` / `.new` / `.bak`）。

### 文档

- 新增 `docs/ROADMAP_v1.5.0.md`：本轮的施工规格（范围、接口契约、验收基线）与**下一轮候选**。
- `docs/API.md`：登记 3 条新路由、鉴权矩阵的新形态、落盘协议的变化、新增的组件接口。
- 各组件 `README.md` 补上新的契约与新的坑（`todo_manager` / `storage_manager` / `ai_service` /
  `sync_server` / `word_service` / `maimemo_client`）。

### 验证

- `bash tools/build.sh build` → **0 error 0 warning**；`todohelper.bin` 0x2f8f20（app 分区剩 63%）。
- **已烧到真机（COM34）并抓串口日志**：开机无 abort / panic / 断言；
  SD 卡与片内 LittleFS 同时挂载成功；待办清单加载正常；
  内部存储分区从 1324 KB 降到 32 KB（不再把词库/进度重复搬进片内）；
  待办清单从「片内 2 条（陈旧）+ 卡上 4 条（完整）」正确收敛到 4 条。
  期间发现并修掉了上面那三处。
- **HTTP 端到端**（设备联网后实测，`192.168.137.95`）：
  | 用例 | 结果 |
  |---|---|
  | `GET /api/v1/todos` | 200，4 条待办 |
  | `GET /api/v1/todos/export` | 200 + `Content-Disposition: attachment; filename="todos.json"` |
  | `POST /api/v1/todos/import`（把 export 的内容原样回灌） | 200 `{"imported":4,"total":4,"pending":0}`，列表**逐条未变** |
  | `POST /api/v1/todos/ack` | 200 `{"acked":[4,45,46,47],"missing":[],"count":4}` |
  | 设备状态 | `routes: 61`（比 v1.4.0 多 3 条，正是上面这三条）、`time_synced: true`、`wifi_connected: true` |
  | 设了令牌后 `GET /api/v1/todos`（无令牌 / 错令牌 / 正确令牌） | **401 / 401 / 200** —— 那个洞在真机上被证实堵上了 |
  | `GET /api/v1/ai/history`（无令牌） | 401（v1.4.0 是 200） |
  | 调试 shell `POST /api/v1/debug/exec -d 'heap'` | 200，可读内部 RAM 实测值 |
  | **网页按钮真机点过** | 「让设备用这个令牌」→ 设备状态从「未开启」变「已开启」，列表与 SSE 自动重连；「关闭设备鉴权」→ 回到「未开启」。两个按钮都是本轮新增的 |
  | 测试结束后 | 已把设备恢复到原状（**鉴权关闭**、待办 4 条未动） |
- **UI 目视确认**：用户已在设备上确认，**没问题**。
- ⚠️ **仍未验证**：内部 RAM / 帧率的实测数值没复核（本板 `int-low` 46 KB、
  最大连续块 15 KB @ 运行期，比 §8 基线的 54/22 KB 低；**开机后**的读数是
  internal 87 KB / largest 42 KB / int-low 71 KB，比基线好，但运行期的碎片化需要再看）。
  判据见 `docs/ROADMAP_v1.5.0.md` §6 与 `AGENT.md` §8。

---

## [1.4.0] — 2026-09-28

**单词背诵主线改为「墨墨词表 + 本地词库补内容」：云端词终于带释义，学过的词回传复习计划。**
HTTP API 与 NVS 键**都是新增**，旧的调用与配置继续有效（无破坏性变更）。

### 起因（用户原话）

> 单词背诵里面没有墨墨单词的单词 只有本地词库的 其实我希望的功能是在 esp 上进行单词背诵的
> 整个流程 本地词库是在无法联网以及展示的时候使用

复现确认：设备上「单词背诵」即使开着云端模式，也只有本地 CET-6 词库那 833 个词**有内容**。
因为 `get_today_items` **只返回拼写**，卡片要靠 `local_index_for()` 匹配回本地书才拿得到
音标/释义/例句；匹配不上就只有光秃秃一个单词。实测线上 20 词队列里 12 词命中本地书，
`quagga` / `narwhal` / `committee` 都不在 —— **那 8 个词在板上没有东西可背**。

### 调研结论（决定了方案边界，均已对照官方源码核实）

| 结论 | 依据 |
|---|---|
| **墨墨开放 API 没有「提交复习评分」的接口** | `study` 只有 `progress` / `today` / `records` / `add` / `advance` 五个操作；`study.proto` 里没有任何上报端点的类型 |
| **`POST /study/advance_study` 存在**（本仓库此前未用） | `AdvanceStudyRequest{voc_ids}` → `advanced_count`，上限 1000/次 |
| **`GET /api/v1/interpretations?voc_id=` 能读释义** | `{interpretations:[{interpretation,tags,status,...}]}`；**这是"云端词补释义"唯一的正规来源** |
| `/vocabulary` **只返回 `{id, spelling}`** | `vocabulary.ts`：没有释义/音标/例句，所以它只能做「拼写→ID」解析 |
| 评分枚举还有 `VAGUE` / `WELL_FAMILIAR` / `CANCEL_WELL_FAMILIAR` | `study_model.ts` |

> 依据：[官方开放 API 功能页](http://memodocs.maimemo.com/docs/PNvOw2E1AivPtlkRlQgce1W1n2c)、
> [官方 CLI `memo-api-cli` 源码](https://github.com/maimemo/memo-api-cli)（`src/commands/study.ts`、
> `src/types/study_model.ts`、`src/types/vocabulary.ts`、`src/types/interpretation.ts`）。

### Added

- **`maimemo_fetch_meaning()`**（`maimemo_client`）：读 `/interpretations` 拿释义。
  它**与 `/notes`、`/phrases` 是不同的作用域**，所以代码不再"助记被拒就整个放弃"——
  只有学习权限的令牌常常读不到助记/例句，**却读得到释义**，而释义正是本地词库没有的词
  最需要的东西。两者按词分别缓存（`meaning_unavailable` 独立于 `unavailable`）。
- **`maimemo_advance_words()` / `maimemo_advance_spellings()`**：用 `advance_study` 把设备上
  学过的词**排回墨墨复习计划**。这是「上传设备端学习情况」在平台能力内**唯一**的合法形态。
- **`maimemo_probe()` + `POST /api/v1/maimemo/probe`**：一次调用逐条试 6 个端点，返回每个
  端点的结果与含义（可用 / 令牌无权 / 令牌被拒绝 / 没有数据）。把"我的令牌到底能做什么"
  从猜测变成可观测的事实 —— 不需要手工 curl。
- **`word_cloud_refresh()` 现在给每个云端词填满内容**（`cloud_fill_content()`）：
  **本地书命中就用本地**（零流量、离线可用），**否则才问墨墨**补释义/例句，并缓存到 SD 卡。
  云端队列因此从"只有拼写的占位卡"变成完整的 `word_item_t`。
- **`words/maimemo.json`（`word_cloud_export()`）**：把当日墨墨词表落成一个**普通词库文件**，
  于是它**重启后还在、断网也能背**，也可以在网页上位机里查看/编辑。
- **设备端学习记录 `words/review_log.jsonl`**（`word_history_append/read/clear`）：
  一行一条「词 / 评分 / 时间 / 来源」。`progress.json` 只存**每词最新状态**，答不了
  "今天背了什么"，这正是"上传学习情况"在设备侧需要的记录。用 **JSONL 追加**而不是 JSON 数组，
  因为数组要整文件重写 = 每评一张卡一次 O(n) flash 写；追加是 O(1)，且断电只损失最后一行。
  - **追加本身也不能就地写**：`word_history_append()` 是从 `word_review()`（LVGL 回调，
    **持适配器锁**）来的，而 `storage_append_file()` 是同步 `fopen`/`fwrite`/`fclose`。
    所以行在 RAM 里拼好、进一个 **16 条的定长环形队列**，由**已有的**去抖保存 worker
    一并排空（`word_history_flush_pending()`）—— 形状与 `word_save_progress_async()` 一致，
    **不新建任务**（本板内部 RAM 最大连续块只有约 13 KB，`AGENT.md §9.0.7 ③`）。
    读取前会先排空队列，否则"刚评完的卡看不到"会像功能坏了。
- **`GET/POST /api/v1/words/history`**、**`POST /api/v1/words/export`**、
  **`POST /api/v1/maimemo/advance`**。
- **网页上位机**：「探测令牌能力」按钮（逐条结果 + 一句人话解释）、「设备端学习记录」列表
  （最近 30 条，含"再点一次确认清空"的两段式防误触），并在面板上**明确写出**平台限制。

### Changed

- **`page_word.c` 的卡片**：云端卡显示真实释义/例句；没有释义时**区分原因**——云端词的提示指向
  「探测令牌能力」，本地词才显示「无释义」。进度行区分「墨墨今日」与「今日队列」（词表来源可见）。
- **`page_word.c` 的助记预取不再每张卡都建任务**：v1.3.2 记录的"进单词页内部 RAM 从 74 KB /
  最大连续块 18 KB 掉到 49 KB / 5 KB"就是**每张卡一个 5 KB worker**。既然同步阶段已把内容
  填进卡片，只有"卡片确实缺内容"时才值得起这个任务。
- **已学词的云端回传改为批量**：以前只有「不认识」会单张推（一词一次 resolve + 一次 add）；
  现在所有评分都进一个**固定 24 条的小队列**，满队或离开页面时**一次**发出，
  请求数与"一次学习会话"成正比，而不是与"评了几张卡"成正比。

### Known gaps（诚实说明）

- **墨墨开放 API 没有提交评分的接口**，所以**设备上背完的词不会让墨墨 App 的「今日 N/M」增加**。
  打开回传开关后，设备用 `advance_study` 把学过的词排回复习计划，让云端继续安排它们。
  这一点写进了网页面板的说明文字，而不是假装成功。
- **用户令牌读不到释义**：实测 `POST /api/v1/maimemo/probe` 里 `interpretations` 为
  `ESP_ERR_NOT_SUPPORTED`，所以**本地词库没有的词目前只有拼写，没有中文释义**。
  这是令牌权限而非代码问题，换权限更宽的令牌即可解锁 —— 见下文"Verified（真机）"。
- **`/study/advance_study` 尚未在"可见成功"的情况下实测**（需要账号里有可推进的词，
  且要先开启回传开关）。依据是官方 CLI 源码里的端点定义与类型。

### 实现细节（踩过的坑，别再犯）

- **5 KB 的栈数组会把这个 worker 打爆**：`maimemo_advance_spellings()` 原本用
  `char storage[64][80]` 局部数组存 voc_id（5120 B）+ `ids[64]`（512 B），而它是在
  **5 KB 的任务**里跑的（`th_word_up` / `mm_run_async` 的 worker），叠加 TLS 握手自身的栈
  就会溢出。已改成 `heap_caps_calloc_prefer(..., MALLOC_CAP_SPIRAM)`。
  判据：本板栈**只能来自内部 RAM**，而批量数据必须显式走 PSRAM（`AGENT.md §9.0.1` / §9.0.5）。
- **不要把"助记读不到"当成"释义也读不到"**：两者是**不同的作用域**，
  而释义恰恰是本地词库没有的词唯一的内容来源。`unavailable` 现在只管助记/例句。

### Verified（真机，本轮烧录后实测）

用户要求"烧录并告诉我应该看到的现象"，所以本版**做了真机验证**，并因此发现并修掉了
三个只有上机才会暴露的问题（见下面的"上机才发现的三个问题"）。

| 项 | 结果 |
|---|---|
| 固件版本 | `GET /api/v1/device/info` → `"version":"1.4.0"`，`sd_available:true`（8 GB 空闲）|
| 路由 | `device/status` → `routes:58`（上限 64，`HTTP_ROUTER_MAX_ROUTES`）|
| **云端队列真的在服务墨墨的词** | `GET /words/today` → `local:0 / CLOUD-ONLY:10`，词是 `pathway`、`undergraduate`、`taxpayer`、`kilogram` 等**不在 CET-6 词库里的** |
| 云端队列规模 | 串口 `cloud queue: 55 words (2 from the local book, 2 with a meaning)` |
| 词表落盘 | 串口 `memo wordbook written: 55 cards, 2043 B` |
| 令牌能力探测 | `get_study_progress`/`get_today_items`(55)/`vocabulary` = **ok**；`interpretations`/`notes`/`phrases` = `ESP_ERR_NOT_SUPPORTED` |
| 内存（构建队列后） | `internal 63 KB / largest 12 KB / int-low 15 KB` —— 未退化 |
| Web 服务器 | 队列构建**之后** `GET /` 仍 **HTTP 200**，TLS 仍可用（不再冻死）|

### 上机才发现的三个问题（都已修）

这一节是本次烧录最有价值的部分：**三个问题在编译期、甚至在看代码时都完全看不出来。**

1. **云端队列只在"用户正好打开单词页"时才构建** —— 这是**用户原始抱怨的真正根因**。
   原来唯一的调用点是 `page_word.c` 的 `MAIMEMO_EVENT_SYNC_DONE` 处理函数。于是从网页
   点「立即同步」（或开机自动同步、此时没人翻到单词页）时，**计划的词数、进度都更新了，
   但一张卡都没进队列**，设备继续用本地书。实测复现：`sync` 返回 `{"started":true}`、
   串口打出 `study progress: 0/55`，而 `GET /words/today` 仍然是 CET-6 的
   `adverse/advocate/...`。
   → **修法**：`word_service` 自己订阅该事件（依赖方向不变），谁发起的同步都会重建队列。

2. **`esp_event_handler_register()` 注册在默认事件循环上，而本项目用的是自定义循环。**
   `sys_event_post_to()` 内部是 `esp_event_post_to(s_loop, ...)`（自定义 `s_loop`），
   所以用 ESP-IDF 的默认注册 API **永远不会触发，而且不报任何错**。
   实测：同步成功、事件已发、handler 一次都没进（加日志才看出来）。
   → **修法**：改用项目自己的 `sys_event_subscribe()`。

3. **一次性给整队 55~64 个云端词拉内容会把板子打死。** 第一版在构建队列时对每个云端词
   调 `maimemo_enrich()`：每词 `GET /vocabulary` + 2~3 次 TLS，且**每次都整文件重写
   `enrich.json`**。实测内部 RAM 被碎片化到 **`largest` = 160 字节**、`int-low` 从 45 KB
   掉到 **472 字节**，随后 `task_wdt: IDLE0 (CPU 0) did not reset the watchdog` 反复刷屏、
   `CPU 1: httpd` 卡死，设备 **ping 得通但所有 TCP 连不上**（v1.3.3 那个形状的重演）。
   → **修法**：构建队列**完全不联网**。本地书命中就在构建时填满；云端独占词先用
   `maimemo_enrich_cached()`（纯 PSRAM 读）挂上历史缓存，没有的**等用户翻到那张卡时**再补，
   补完写缓存。代价与"用户看了多少"成正比，而不是与"账号里有多少"成正比。
   修后实测 `int-low 15 KB`，队列构建后 TLS 与 Web 服务都正常。

**另外两个不是本版引入、但顺手确认了的**：

- `/words/history`（读整个学习日志）与 `/words/export`（整文件写词表）原本在 httpd 单任务里
  同步做文件 IO —— 与第 3 条同类的隐患。已改为异步 handler（`word_run_async()`），
  与 v1.3.3 给墨墨路由做的处理一致。
- 开机那次同步可能撞上 **DNS 未就绪**（`esp-tls: getaddrinfo() returns 202`）而失败，
  属正常时序，下一次同步即成功。

### Known gaps（诚实说明）

- **用户令牌读不到释义**：实测 `interpretations` / `notes` / `phrases` 全部返回
  `ESP_ERR_NOT_SUPPORTED`。所以**本地词库没有的词，卡上目前只有拼写，没有中文释义**。
  「探测令牌能力」按钮就是用来一眼看清这件事的。
  - **这不是平台没有这个能力**：官方把"调取墨墨词库的单词**释义、例句、助记**内容"列为核心功能
    （[开放 API 功能介绍](http://memodocs.maimemo.com/docs/PNvOw2E1AivPtlkRlQgce1W1n2c)），
    所以这是**个人令牌没有这些 scope**。
  - **升级路径**：官方是 [open.maimemo.com/app](https://open.maimemo.com/app) 创建应用 →
    填**授权范围**→ 审核 → OIDC 授权（文档明确写"后续需要增加权限可以再次编辑"）。
    但那条路需要 `client_id`/`client_secret`、HTTPS 回调地址与一个后端，
    **sdk 端目前只实现了"个人令牌直接填"这一条**。代价与建议见
    `components/maimemo_client/README.md` 的"开放平台令牌（OIDC）的接入代价"。
  - **先试最省事的一步**：在 App 里**重新生成一个个人令牌**再探测一次 ——
    个人令牌的范围由墨墨控制，不排除新令牌不同，成本为零。
- `/study/advance_study` 本身**仍未在"可见成功"的情况下实测过**（需要账号里有可推进的词）。

### Verification（构建）

- `bash tools/build.sh build` 通过（`todohelper.bin` 0x2edd00，分区余 63%）。
- 网页 JS 用 `node --check` 校验语法通过；新增的 8 个元素 id 与脚本引用一一对应。

---

## [1.3.3] — 2026-09-28

**修：墨墨推送方向的请求会把整个 Web 服务器冻死。** 无破坏性变更。

### Fixed

- **`POST /api/v1/maimemo/push`（设备 → 墨墨）在设备上不可用**：
  推词时设备 **ping 得通但所有 TCP 连接都建不上** —— `GET /`、`/api/v1/device/info`
  全部超时，而且**不会自行恢复**，只能重启板子。真机复现：推 `narwhal` 后 HTTP
  完全无响应，等 45 s 仍是死的。

  **根因**是 `esp_http_server` 的 handler 在**唯一的 httpd 任务里串行执行**，而
  `push_post_handler` 在里面做阻塞 TLS：先按词逐个 `GET /vocabulary` 解析 `voc_id`，
  再 `POST /study/add_words` —— **10 个词就是 11 次 HTTPS 往返**，全程占着 httpd。
  这就是 `AGENT.md §9.0.1` 最后一条警告的实例（同组件的 SSE 早已按此修过）。

  **同一模式还影响另外三个路由**，一并修掉：

  | 路由 | 阻塞原因 | 最坏耗时 |
  |---|---|---|
  | `push` | 每词一次 resolve + 一次 add_words | 词数 × 往返 |
  | `today` | get_today_items（可能回退 query_study_records）| 1–2 次往返 |
  | `enrich` | 未命中缓存时 resolve + notes + phrases | 3 次往返 |
  | `config{verify:true}` | 活体校验 | 5–15 s |

  **修法**：新增 `mm_run_async()`，用 `httpd_req_async_handler_begin()` 把 socket 交给
  自己的 worker 任务、handler 立即返回；worker 在自己的 req 副本上写响应。
  `httpd_req_async_handler_complete()` 统一放在 trampoline 里 —— 漏掉它会永久占用
  会话槽、最终 httpd 连 `accept()` 都不再做（IDF 头文件明确写了），所以不能指望每个
  worker 自己记得。

  配套：push 的 spellings 要 `strdup`（原同步版本借用 cJSON 指针是安全的，因为一切
  都在同一函数内），enrich 也要复制查询串（handler 的栈缓冲在 worker 跑起来时已失效）。

### Verified（真机）

| 项 | 结果 |
|---|---|
| `POST /maimemo/push {"spellings":["quagga"]}` | `{"accepted":1,"count":1}` |
| **紧接着** `GET /api/v1/device/info` | 正常返回（**修复前这里必死**）|
| 墨墨端确认 | `query_study_records?spellings=quagga` → 已在计划内 |
| `GET /maimemo/today` | count 54，服务器存活 |
| `GET /maimemo/enrich?spelling=abrupt` | 正常返回 `unavailable:true`（学习作用域令牌读不到助记，属预期）|

### Notes

- **推送方向本身一直是好的**，坏的只是"从设备上触发"这条 HTTP 路径：
  同一令牌用 curl 直接打墨墨 API（resolve → add_words）实测 `added_count:1` 成功。
  修好后设备端与 API 端行为一致。
- 语义上 push 现在**立即返回**（真实结果仍写在同一个 socket 上，所以 `curl` 等响应
  看到的还是最终 `accepted` 数值）。
- `ai_service_http.c` 的 `/api/v1/ai/chat`（阻塞版）**仍有同类隐患**，未在本轮修。

---

## [1.3.2] — 2026-09-28

**新增板载调试 shell（`components/debug_shell/`）+ 两个配套脚本**，把"只能靠看屏幕确认"
的 UI bug 变成可脚本断言的对象。**无破坏性变更**：HTTP API 只增一个**默认不编译**的
调试路由，存储格式与 NVS 键都不变。

起因是 v1.3.1 修"鉴权弹窗关不掉"时，验证手段是"请你在板子上点一下、告诉我看到什么" ——
板子不在手边就没法验证，而且以前**没有办法从脚本切页或按控件**。这次补上了。

### Added

- **`components/debug_shell/`（L4）**：`POST/GET /api/v1/debug/exec`，一条命令进去、
  文本输出出来。命令分两类：
  - **只读**：`help` / `version` / `status` / `heap` / `tasks` / `routes` / `pages` /
    `time` / `storage` / `cfg` / `nvs` / `log`；
  - **驱动 UI**：`ui` / `widgets [过滤] [深度]` / `click <标签>` / `text` / `page <名>` /
    `tap <x> <y> [click]` / `idle` / `bright` / `idle-timeout` / `reboot`。
  - **默认关**（Kconfig `TODOHELPER_DEBUG_SHELL`，本仓库开发用 `sdkconfig.defaults`
    打开）；关掉时整个组件不参与编译，`debug_shell_init()` 是返回 `ESP_OK` 的空函数。
  - 路由用 `http_router_require_auth(..., true)` **强制要求鉴权，即使全局鉴权是关的** ——
    它能切页、按控件、改 NVS、重启，所以"编了调试固件"不等于"把设备控制权暴露在开放网络上"。
- **`tools/dsh.sh`**：发一条命令并打印文本输出。**自动处理令牌轮换** ——
  那个"生成 token"按钮本身就会轮换令牌，而本组件路由永远要求鉴权；脚本每次重新发现
  令牌、401 时换候选重试、成功者记进 `build/.dsh_tokens`（日志只在**生成**时才有一行）。
- **`tools/dsh_debug_probe.sh`**：单独取当前 HTTP 鉴权令牌。**优先查日志**，
  NVS 仅兜底 —— 因为读 flash 会**复位板子**（esptool 无法在应用运行时附加）。

### Verified（真机，全程无人值守）

用 shell 跑通 v1.3.1 那个"只靠看屏幕才能确认"的弹窗，一次请求同时证明四件事：

```
$ bash tools/dsh.sh click 生成并开启
clicked button (matched label '生成并开启')
top layer now: 2 child(ren), 1 msgbox(es)  <- dialog is OPEN

$ bash tools/dsh.sh widgets
  msgbox     24,136   752x208  [click]        ← 752 宽 / 208 高 = 横排
    label      45,145   685x34   HTTP 鉴权已开启
      image     739,150    12x24              ← 「×」在
    label      40,238   353x48   把下面的 token 填进…   ← 左列 353 px
      obj       407,226   353x72
        label     419,238   329x48   d84ad67d…ca      ← 右列 353 px，令牌可见
      label     370,306    60x24   知道了

$ bash tools/dsh.sh click 知道了
top layer now: 1 child(ren), 0 msgbox(es)    ← 关掉了
```

**结论：打开 → 令牌真的渲染 → 两列横排（各 353 px）→ 能关**，全部由脚本断言。

### Known issues

- **首次进入单词页会让内部 RAM 从 74 KB / 最大连续块 18 KB 掉到 49 KB / 5 KB**，
  离开页面不回收（反复采样稳定，不是持续泄漏）。那个 5 KB 正是 `page_word.c` 里
  **每张卡一个、5 KB 栈**的 `th_mm_enrich` worker，与 v1.3.1 给评分路径做 NO_MEM
  降级时的判据一致。这是本板最紧的一处，改单词页时请注意。
- `tasks` 的逐任务栈水位需要 `CONFIG_FREERTOS_USE_TRACE_FACILITY=y`，当前为**关**
  （它按任务占 RAM，而内部 RAM 是本板最紧的资源）；该命令会打印任务数与说明。
- 本轮的 bug 1/2/3 修复**仍按 v1.3.1 的结论有效**，本轮只加了验证手段。

---

## [1.3.1] — 2026-09-28

**修三个上机实测的 bug**（用户报告）：鉴权弹窗样式/布局/关不掉、单词评分报
`ESP_ERR_NO_MEM`、墨墨拉不到也推不上单词。**无破坏性变更**：HTTP API、存储格式、NVS 键
全部不变；只动了 `ui_core` 的对话框实现、`word_service` 的落盘回退、
`maimemo_client` 的取词请求体与新增一个异步推送入口。

三个 bug 的根因都不在"看起来出错的地方"，这是本轮的主要收获：
**弹窗关不掉是 LVGL 事件目标取错了对象**、**NO_MEM 不是内存真的没了而是任务栈要内部 RAM**、
**"今日单词为空"不是账号没词而是 beta 端点要显式过滤参数**。

### Fixed

- **鉴权弹窗：风格不匹配 / 被挤成竖条 / 关不掉**（`ui_core/src/ui_widgets.c`）
  - **关不掉是主 bug，也是三处里最隐蔽的**：`dialog_close_cb` 用
    `lv_event_get_current_target(e)` 取"消息框"，但回调是注册在 **footer 按钮**上的，
    所以拿到的是按钮本身；`lv_msgbox_close(button)` 是**静默 no-op**。
    → 改为把消息框作为 event user data 显式传入，并在标题栏加 LVGL 自带的关闭按钮
    （`lv_msgbox_add_close_button`，它的回调绑定的就是消息框，天然正确），
    于是「知道了」和「×」两条路都能关。
  - **横过来了**：面板是 800×480（横屏），原来标题/说明/token/备注**竖着堆**，
    高度很快用完就被"挤压"（用户原话"面板挤压太长了，应该是横着的而不是竖着"）。
    → `ui_dialog_secret()` 改为**两列横排**：左边短说明、右边 token 高亮框。
    实测几何：对话框宽 `94%×800 = 752 px`，内宽 720 px，两列各 ~353 px，
    32 字符 token 一行需要 ~320 px → **正好一行放下**，不再折行。
  - **风格统一**：新增 `dialog_style()` / `dialog_style_button()`，把卡片色
    （`UI_COLOR_SURFACE` / `_ALT` / `_PRIMARY`）、`UI_CARD_RADIUS`、正文/标题字体
    应用到消息框与按钮，取代 LVGL 默认的浅色皮肤。
  - 备注：`lv_msgbox_create(lv_layer_top())` **不会**创建独立遮罩（传 NULL 才创建），
    所以没有给"背景"上色——那会把 `ui_toast()` 一起压暗。已写进代码注释。

- **单词评分报 `ESP_ERR_NO_MEM`**（`word_service` / `page_word` / `maimemo_client`）
  - **根因**：评分回调里调 `word_review()` → `word_save_progress_async()`，而它要
    `xTaskCreate()` 一个 **4 KB 栈**的落盘任务；**任务栈只能来自内部 RAM**
    （AGENT.md §9.0.1），本机 boot 结束内部 RAM 只剩 **57 KB 空闲 / 最大连续块 13 KB**
    （真机实测，比 v1.3.0 记录的 18 KB 更紧）。任务建不出来就返回 `ESP_ERR_NO_MEM`，
    UI 原样弹成"记录失败"——**看起来像按钮坏了，其实评分已经生效，只是没落盘**。
    → 建任务失败时**回退为就地同步写盘**（慢一点，但绝不丢评分），并保留 WARN 日志。
  - **同一路径还有第二个问题**：`grade_clicked_cb` 里直接调了**阻塞 TLS**的
    `maimemo_push_unknown()`，即"在持 LVGL 锁的回调里做完整 TLS 往返 + 建任务"，
    这正是上面那个 NO_MEM 的触发窗口。→ 新增 `maimemo_push_unknown_async()`：
    廉价检查留在调用方，网络调用交给 worker 任务，评分路径改用它。
  - `enrich_start()` 建 worker 失败时现在只记 DEBUG 并跳过助记（卡片本就不必须），
    不再让可选的预取影响主流程。

- **墨墨拉不到今日词表 / 推不上单词**（`maimemo_client`）
  - **拉到的是空**：`maimemo_fetch_today()` 发的是 `{}`。实测（真实令牌）对比：
    ```
    POST /study/get_today_items  {}                          -> {"today_items":[]}
    POST /study/get_today_items  {"is_finished":false,...}   -> 51 个词
    POST /study/query_study_records {}                       -> 58 条记录
    ```
    即 `/study/get_today_items` 是 **beta** 端点，官方文档写明"如果当日未打开 App 进行
    初始化则无法准确计算"——空数组**不代表账号没词**。→ 改为发送
    `{"is_finished":false,"limit":<max>}`（官方文档给出的取队列方式）。
  - **兜底**：`today_items` 仍为空时，改用**非 beta** 的
    `POST /study/query_study_records` 重建队列（`mm_fetch_today_from_records()`），
    按 `limit` + `next_study_date.end` 取计划内的词，`last_response` 映射到
    `first_response`，`study_count==0` 视为新词。于是"账号有计划词"与"卡片有词可背"
    不再脱钩。
  - **响应体积**：一条 record ≈ 350 B（60+ 字符的 voc_id 占大头），24 KB 的
    `MM_MAX_RESPONSE` 只装得下约 60 条。→ 新增 `MM_RECORDS_MAX=56` 并写明理由：
    请求更多会让**响应**溢出而导致整次调用失败，比队列短更糟。实测 56 条 = 19.6 KB，
    在 24 KB 之内。
  - 推送侧的接口本身是对的（`{"words":[{"id":voc_id}],"advance":bool}`，
    `advance` 必填），实测 `added_count: 1`；本轮修掉的是**它被从 UI 回调同步调用**。

### Verified（真机）

| 项 | 结果 |
|---|---|
| 构建 | 0 warning（`-Werror` 全开），`todohelper.bin` `0x2e6000`，分区余 64% |
| 烧录 | `tools/flash.ps1` 31.4 s，`Hash of data verified.` |
| 启动 | 0 panic / 0 abort / 0 wdt，`TH:app: ready (storage=/sdcard)` |
| `GET /api/v1/maimemo/today` | **51 词**（修复前该账号拿到空表）；串口 `TH:maimemo: today: 51 words from the study plan` |
| 兜底路径 | 用真实令牌实测 `query_study_records` 兜底：56 条 / 19656 B，在 24 KB 上限内 |
| 推送 | 全链路实测（拼写→voc_id→add_words）：`{"added_count":1}`，`success:true` |
| 弹窗几何 | 752 px 宽、两列各 ~353 px、token 单行 ~320 px → 一行放下 |

### Known issues

- 弹窗的**目视确认**未做：设备没有"远程切页/点按钮"的 HTTP 路由，无法脚本化触发那个
  对话框（本轮只验证了编译、启动与几何计算）。请上机点一次「设置 → 生成并开启 token」
  确认横排观感与「知道了 / ×」都能关闭。
- `int-low` 从 v1.3.0 的 50 KB 降到 **44 KB**、最大连续块 13 KB（真机），
  内部 RAM 依旧是最紧张的资源；`word_save_progress_async()` 的回退路径因此不是理论分支。

---

## [1.3.0] — 2026-09-27

**单词学习接入墨墨背单词（开放 API）+ 音标字形修复。** 需求条目没有变化，改的是"这个词到底
从哪来、进度算谁的"这件事。**没有破坏性变更**：HTTP API 只增不改，存储格式向后兼容
（新增一个可选的 `words/enrich.json`），NVS 只新增 `mm_token` / `mm_mode` / `mm_push` 三个键。

两件事各有一条"根因写在现象之外"的教训：**音标缺失不是字库选字少了，而是 Noto Sans SC
根本没有 IPA 字形**；**构建失败不是代码错，是 MSYS 下 `idf.py` 根本没法被驱动**。

### Added
- **新组件 `components/maimemo_client/`（L5，墨墨背单词 OpenAPI）**，实现用户选定的三个方案：
  - **方案 A —— 学习同步**：今日词队列与账号**真实进度**改从墨墨取，不再只认本地词库。
    端点 `POST /study/get_study_progress`（finished/total/study_time）、
    `POST /study/get_today_items`（voc_id / voc_spelling / is_new / is_finished /
    first_response）、`POST /study/query_study_records`（body `{"as_count":true}` 取计划词数）。
    **实测（真实令牌）**：10/20 已完成、20 词队列。
  - **方案 B —— 助记/例句预取**：`/notes`（助记）与 `/phrases`（例句）缓存到
    `<app_dir>/words/enrich.json`，走 `storage_read_text()` / `storage_write_file()`
    （**原子写**，tmp + rename），所以卡片离线也能显示助记与例句。
    **整个文件一次性载入 PSRAM、整文件重写**，不做逐词小文件（这台设备的内部 RAM 只有
    ~310 KB，批量数据必须显式走 PSRAM，见 `AGENT.md §9.0.5`）。
  - **方案 C —— 反向推送**：评为「不认识」的词用 `POST /study/add_words` 推回墨墨学习计划。
    **默认关闭、必须显式开启**（NVS `mm_push`），因为它会**修改用户在云端的学习计划**，
    不是只读操作。
- **令牌的作用域被当成一等问题处理**：用户的令牌是**学习作用域**的，`/study/*` 与
  `/vocabulary` 可用，而 `/notes`、`/phrases` 返回 `common_permission_denied`。
  这被映射为 `ESP_ERR_NOT_SUPPORTED`（"这个令牌不许做这件事，别再问了"）并**按词缓存为
  不可用**，而不是当成错误反复重试。
- **新增 6 条 HTTP 路由**：`GET /api/v1/maimemo/status`、`POST /api/v1/maimemo/config`、
  `POST /api/v1/maimemo/sync`、`GET /api/v1/maimemo/today`、
  `GET /api/v1/maimemo/enrich?spelling=`、`POST /api/v1/maimemo/push`。
- **网页上位机新增「墨墨背单词（开放 API）」面板**（`components/sync_server/web/index.html`）：
  令牌录入（**只写不读**，已核实回显路径不存在）、推送开关、带轮询的「立即同步」按钮、
  今日词表查看器。
- **构建工具 `tools/build.sh`（bash 入口）+ `tools/build.ps1`**：
  `bash tools/build.sh build`（或 `flash`，或 `-v build`）。
  MSYS/Git-Bash **无法直接驱动 `idf.py`** —— ESP-IDF 的 python wrapper 会识别 MSYS 环境
  并拒绝运行（`MSys/Mingw is no longer supported`）。三个坑都会**静默产出空日志 + 退出码 0**，
  修法见本节 `### Fixed` 与 `AGENT.md §4.2.1`。`build_last.log` 已加入 `.gitignore`。
- **构建期字库校验 `validate_font()`**（`tools/gen_cjk_font.py`）：重新打开**已保存**的文件，
  当任一分字形度量表与 `maxp.numGlyphs` 不一致、`numberOf*Metrics` 超过字形数、
  或必需的内容字符不在 cmap 里时**直接让构建失败**。

### Fixed
- **「HTTP 鉴权已开启」弹窗整块溢出屏幕**（用户上机截图）：标题被切掉、正文从底部跑出屏幕
  以外，整页被它盖住且无法读完。
  根因是 `ui_dialog()` 建出来的 msgbox **按内容自适应尺寸**，既没有最大高度也没有滚动，
  而这个弹窗的正文是全 UI 最长的一段多行中文 —— 内容一长就必然超出 480 px 面板。
  现改为把 msgbox **夹到屏幕的 88%（高）/ 92%（宽）**，并让**正文自己可滚动**：
  这样无论正文多长，标题与「知道了」按钮始终可达 —— 这才是弹窗需要的性质，
  无上限增长不是。
- **同一个弹窗还犯了一个更直接的错：它让用户"复制下面的 token"，但正文里根本没有 token。**
  原文案只有一段解释（"见串口日志，或调用 GET /api/v1/device/status …"），
  用户拿不到那个 32 位令牌本身。
  新增 `ui_dialog_secret()`：把内容拆成**引导语 + 高亮令牌 + 灰色说明**三段，
  令牌单独放在底色方框里、**按 32 个十六进制字符换行**（不换行会在 480 px 上被截断），
  说明文字压到一行。生成令牌的按钮现在走这个弹窗。
- **设置页的"地址"一直藏在正文里，并且可能过期**：
  `sync_server_url()` 原来只在**首次调用**时计算并缓存，DHCP 换址后会一直显示旧地址。
  现在每次调用都重算（一次 `snprintf` 加一次网卡查询，代价可忽略）。
  同时在「局域网与安全」卡片给网址**单独一行、标题字号、主题色**（它是全页唯一
  需要用户"看一眼再手打"的字符串），mDNS 开启时**同时显示 `.local` 与纯 IP 两种**
  —— Windows 只有装了 Bonjour 才解析 `*.local`，部分网络还直接封多播，
  所以只给 `.local` 是不够的。新增 `sync_server_ip_url()` 提供纯 IP 形式。
- **音标（IPA）字形缺失：`/əˈbrʌpt/` 在屏上渲染成 `/ br pt/`**（元音符号全是空白）。
  **根因不是"字库选字少了"，而是 Noto Sans SC（CJK 子集来源）里根本没有 IPA 字形** ——
  U+0259 ə、U+028C ʌ、U+026A ɪ、U+02D0 ː、U+02C8 ˈ 等**不在它的 cmap 里**；
  LVGL 内置的 Montserrat 兜底也帮不上（cmap 稀疏，只有 FontAwesome 区段）。
  修法是在 `tools/gen_cjk_font.py` 新增 `merge_ipa_glyphs()`：把 IPA 字形从一份拉丁字体
  （Linux 用 DejaVu Sans，Windows 用 segoeui / arial / calibri）**合并进 CJK 子集**，
  坐标从拉丁 em 方格（2048）**缩放到** CJK 的（1000）。
  - `fontTools.merge` **不能用** —— 它断言两边 `unitsPerEm` 相等，这里必然不等，于是只能手写。
  - 字形必须**展平成普通轮廓**：组件（composite）会引用目标字体里不存在的字形，
    编译期就炸 `KeyError: 'uni02BE'`。
  - 实测从 `segoeui.ttf` 合入 **203 个字形**，缩放系数 **0.488**。
- **合并逻辑里还藏着一个潜伏 bug：只更新了 `hmtx`，没更新 `vmtx`。**
  两张表都以**字形名**为键、都必须**每个字形一条记录**；只补一半的结果是文件本身已经不合法
  —— `vmtx` 比应有长度**少 406 字节（2 字节 × 203 字形）**，fontTools 连**重新解析**都做不到：
  `not enough 'vmtx' table data: expected 10686 bytes, got 10280`。
  这份 CJK 子集**确实带中文竖排度量**（`vmtx` / `vhea` 都在），所以这不是理论问题。
  修法是给每个合入的字形**同时写 `vmtx` 记录**，并把 `hhea.numberOfHMetrics` /
  `vhea.numberOfVMetrics` **夹到字形总数以内**。
  - **这个 bug 是被"cmap 检查通过了"放过去的**：第一版校验只问"文件里有没有 ə？"，
    而坏字体**每个 cmap 条目都在**，于是检查通过、坏字体照样出厂。
    这就是新增 `validate_font()` 的动机 —— 必须**重新打开保存后的文件**逐表核对，而不是查 cmap。
  - `validate_font()` 只要求**内容字符**（词书字符 + 有拉丁源时的 IPA），不要求装饰性
    Unicode 区段，否则检查会变成人人忽略的噪音。
- **构建脚本的三个静默失败**（都会产出空日志 + 退出码 0，最难查的一类问题）：
  1. IDF 的 PowerShell profile 把 `idf.py` 定义成**函数**并用 `@args` 转发参数，而调用方是
     `powershell -Command` / `-File` 时该简写**不绑定** → `idf.py build` 静默什么都不做。
     改为显式调用解释器：`& $python $idfPy @idfArgs`。
  2. **把原生 exe 接进 cmdlet 管道会失败**：`Cannot run a document in the middle of a
     pipeline: ...python.exe`，所以 `& $python ... | Tee-Object` 用不了；改为重定向到
     `build_last.log`。
  3. **用 `env -i` 清空环境是致命一击**：powershell 能起来并打印横幅，但原生 python 子进程
     **一点输出都没有**，且 `$LASTEXITCODE` 保持 `$null`。可行做法是**保留 bash 继承的环境**，
     在 PowerShell **内部**删掉 MSYS 标记（`Remove-Item Env:MSYSTEM`）—— 在 bash 里 unset
     `MSYSTEM` 毫无用处，因为 MSYS 运行时会把它**重新注入到每一个原生子进程**。

### Changed
- **`page_word.c` 在云端模式下显示账号真实进度**：「墨墨今日 10/20 · 已用 59 秒 · 计划 20 词」，
  否则回退本地词库计数器。**小于 1 分钟时显示秒**，因为把 59 秒四舍五入成「0 分」
  读起来就是"你今天没学过"。
- **助记/例句显示在答案下方**（方案 B），拿不到时**整块隐藏**（不是显示空标签）。
- **评「不认识」时把词推回账号**（方案 C 开启时）。
- **线程规则在 `maimemo_client.h` 里写明**：所有做 TLS 的函数都是阻塞的，
  **绝不允许从 LVGL 任务调用**（LVGL 回调持有适配器锁，见 `AGENT.md §9.0.1`）。
  UI 路径把抓取交给 worker 任务，结果经 `lv_async_call` / 事件应用回界面。
- **`word_service` 新增云端队列**：`word_cloud_mode()`、`word_set_cloud_mode()`、
  `word_cloud_refresh()`、`word_cloud_count()`（NVS `mm_mode`）。云端词会用 `voc_spelling`
  匹配回本地词库，匹配不上也要能在设备上评分（`WORD_ID_CLOUD`）。
- **`main/app_main.c` 的模块表多一项** `maimemo_client`（`optional = true`，排在
  `word_service` **之前** —— 云端队列要先能读到令牌）；开机时若处于云端模式则自动发起一次同步。
- **`tools/gen_cjk_font.py` 现在会真正校验产物**，所以"字库生成成功"不再等于
  "字库能用"。数值：词书 **1252 个不同字符，0 个缺失**；`/əˈbrʌpt/` 全部有真实轮廓且
  advance 正确缩放（ə=523 vs b=618 / r=388 / p=620）。
- **令牌处理与 AI Key 同规则**（需求 §8.8）：存 NVS 用 `sys_config_set_str`，从网页控制台写入，
  **从不记日志、任何 API 都不回读**。`GET /api/v1/maimemo/status` 只返回 `configured`
  与掩码提示（形如 `g_zV****`）。
- **无令牌时静默降级**：云端模式无声关闭，本地词库照常工作（不报错、不弹窗）。

### Verified（真机）
- ⚠️ **本轮没有烧录到设备，所以以下几项都不是真机验证**，请按"已验证 / 未验证"分别理解：
  - **离线构建（已验证）**：`todohelper.bin` **0x2e4fd0 字节**，app 分区剩 **64%**；
    构建 **0 错误 0 警告**。
  - **字体产物（已验证）**：`validate_font()` 通过，词书 1252 字符 **0 缺失**，
    合入 203 个 IPA 字形，advance 缩放正确。
  - **墨墨接口（已验证，对线上 API 实测）**：用真实令牌 —— `get_study_progress` 返回 10/20、
    `get_today_items` 返回 20 词队列、`add_words` 成功（服务器自报 `added_count`）。
  - **令牌作用域（已验证）**：`/study/*` 正常、`/notes` 与 `/phrases` 返回
    `common_permission_denied`，设备侧正确映射为 `ESP_ERR_NOT_SUPPORTED` 并按词缓存为不可用。
  - **错误模型（已验证）**：确认 API **即使权限错误也返回 HTTP 200**，设备读的是信封里的
    `success:false` + `errors[].code`，不是状态码。
  - **`advance` 必填（已验证）**：漏掉它即使请求其它部分完全合法也会失败 ——
    `common_invalid_param: {base} must have required property 'advance'`。
    现在**永远发送**（含 `false`），并回报服务器自己的 `added_count` 而不是发送条数。
    请求体形状是 `{"words":[{"id":<voc_id>},...],"advance":bool}` ——
    **按 id 加词而非拼写**，设备侧先用 `GET /vocabulary?spelling=` 解析
    （与官方 CLI `@maimemo/memo-api` v1.2.0 的 `advance` + `words[].id` 一致）。
  - **助记/例句的读取端点是 `GET`，不是 `POST`（已验证）**：
    `GET /api/v1/notes?voc_id=...`、`GET /api/v1/phrases?voc_id=...`。
    同路径的 `POST /api/v1/notes` 是**创建**笔记（body `{"note":{voc_id,note_type,note}}`），
    用 POST 去读会拿到完全不同的东西（甚至可能改写用户的笔记），所以本组件为其单独实现了
    `mm_get()`。响应结构 `{"data":{"notes":[{"note":"..."}]}}` /
    `{"data":{"phrases":[{"phrase":"..."}]}}`，已对照官方 CLI 源码核实。
  - **网页面板令牌只写不读（已验证）**：已核实没有任何回显路径。
  - **未验证（需上机）**：字库在屏上的实际观感、设备端云端队列、方案 B 的助记显示、
    方案 C 的推送落地、无令牌降级的实机行为。

### Known gaps
- **方案 B 的"快乐路径"没有在设备上被观察到**：当前令牌是学习作用域的，`/notes` 与
  `/phrases` 恒返回 `common_permission_denied`。**代码路径已实现、权限拒绝的处理已验证**，
  但"卡片上真的显示出一条助记"这件事**尚未在设备上看到过**。
  要验证需要换一个能读 `/notes` 的令牌，或等墨墨放开该作用域。
- **本轮所有功能都没有上机**：字库是运行时从 `<storage>/todohelper/fonts/ui_cjk.ttf`
  加载的（**没有烘进固件镜像**），所以部署新字库是**拷文件而不是重新烧录** ——
  但也因此，"拷上去之后屏幕上真的没有方框了"这件事仍待目视确认。
- 墨墨云端模式与本地进度是**两套记账**：云端词匹配回本地词库后，本地的
  level / 复习间隔照常生效；匹配不上的词（`WORD_ID_CLOUD`）在设备上一评分就只留痕在账号侧，
  本地不记进度。这是刻意的取舍（不把云端 id 硬塞进本地进度键），但换回本地书时两边的数字
  不会合并。
- 方案 C 一旦开启就无法"撤销"已经推上去的词：接口只负责加词，没有反向删除。

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
