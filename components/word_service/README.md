# word_service

**L3 业务组件（FR5）** — 单词卡：词库加载 + 三档评分 + 间隔复习。
默认书 `WORD_DEFAULT_BOOK = "cet6"`，卡上 `words/cet6.json` 是 **833 条**自编精选
（BrE 音标 + 简体释义 + 例句，约 546 KB）；固件另带一份 32 词的起始词库作兜底，
替换成自己的词库**不需要重新烧录**（见下）。

- 头文件：`include/word_service.h`（私有 `src/word_default_book.h` 只导出内置词库字符串）
- 依赖：`sys_utils`、`storage_manager`、`http_router`、`cjson`
- 被谁依赖：`ui_pages`（`page_word.c`、`page_settings.c`）、`main`

## API 摘要

| 分组 | 函数 |
|---|---|
| 生命周期 | `word_service_init()`、`word_reload()`、`word_load_from_sd()` |
| 词库选择 | `word_get_book()`、`word_set_book()`、`word_count()` |
| 今日队列 | `word_get_today()`、`word_next()`、`word_get()`、`word_restart_queue()`、`word_queue_position()` |
| 云端队列 | `word_cloud_mode()`、`word_set_cloud_mode()`、`word_cloud_refresh()`、`word_cloud_count()`、`word_cloud_export()` |
| 复习 | `word_review()`、`word_save_progress()`、`word_reset_progress()`、`word_review_name()`、`word_review_from_name()` |
| 学习记录 | `word_history_append()`、`word_history_read()`、`word_history_clear()` |
| 统计/配置 | `word_stats()`、`word_get_daily_count()`、`word_set_daily_count()` |
| HTTP | `word_service_register_http()`（由 `init()` 自动调用） |

## 词库格式

词库文件放在 `<app_dir>/words/<book>.json`（默认 `<book>` = `WORD_DEFAULT_BOOK` = `cet6`，
NVS 键 `word_book` 可改）。`parse_book()` 接受的字段：

```json
{
  "name": "CET4 核心词汇（内置示例）",
  "version": 1,
  "words": [
    {
      "id": 1,
      "spelling": "abandon",
      "phonetic": "/əˈbændən/",
      "meaning": "v. 放弃；抛弃；遗弃",
      "example": "He abandoned his car in the snow."
    }
  ]
}
```

- 只有根对象的 **`words` 数组**是必需的；不是数组 → `ESP_ERR_INVALID_ARG`，空数组 → `ESP_ERR_NOT_FOUND`。
- 每个条目必须是对象；**`spelling` 必填且非空**，缺失的条目会被**静默跳过**；
  `phonetic`、`meaning`、`example` 可选（上限分别为 64 / 256 / 256 字节，见 `WORD_*_MAX`）。
- `id` 可选：给了数字就用它，否则用数组下标 `index + 1`。
  **id 是进度的主键**（进度键为 `"<book>#w<id>"`，1.0.0 起带书名词缀），所以替换词库文件时
  务必保留 id 语义，否则旧进度会对错词。
- 超过 `WORD_MAX_ITEMS`（2000）会截断并告警，防止坏文件吃光内存。
- 完整格式同时记录在 `docs/API.md`。

### 内存：词库是这台设备上最大的"批量数据"

板子只有约 310 KB 内部 RAM、却有 12.4 MB PSRAM，而 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`
让**≤4 KB 的分配默认落在内部 RAM**。解析一份真实词库（CET-6 = 546 KB / 833 词 / 约 7000 个
cJSON 节点）会把内部 RAM 抽干，并连带把当时正在连网的 WiFi 驱动打进 `ESP_ERR_NO_MEM` → abort。
**这就是"插 SD 卡启动崩溃"的真因**（`AGENT.md §9.0.5`）。因此：

- `items[]` 与 `progress[]` 都用 `heap_caps_malloc_prefer/calloc_prefer(..., MALLOC_CAP_SPIRAM,
  MALLOC_CAP_DEFAULT)` 显式要 PSRAM（833 词约 546 KB + 20 KB）；
- cJSON 的分配由 L0 的 `sys_json_use_psram()` 全局改成 PSRAM 优先（`app_main()` 最早调用）；
- `parse_book()` 在 `cJSON_Parse()` 返回后打印一行判据：
  `book tree alive: internal N KB free (largest M KB)` —— **JSON 树存活时的内部 RAM 余量**。
  1.0.0 实测 833 词时仍有 86 KB 空闲 / 40 KB 最大连续块（修复前该窗口被抽到接近 0）；
- `word_load_from_sd()` 的成功日志带字节数：`loaded 833 words from 'words/cet6.json' (120311 B)`。

## 内置起始词库与导出

`src/word_default_book.c` 里有 `WORD_DEFAULT_BOOK_JSON`（32 个示例词，格式与真实词库完全一致）。
`word_reload()` 的顺序是：

1. `<app_dir>/words/<book>.json` 存在 → 从存储加载（`word_load_from_sd()`）；
2. 否则，若 `CONFIG_TODOHELPER_WORD_EXPORT_TEMPLATE`（默认 `y`）打开，就把内置词库**写到存储**，
   给出一个可直接编辑/替换的模板文件；
3. 读不到卡上文件时（没插卡、文件缺失或 JSON 不合法），回退到**编译进固件的**
   `WORD_DEFAULT_BOOK_JSON`（32 个示例词），保证功能可用。注意这只是兜底：
   优先使用的始终是 `<app_dir>/words/<book>.json`。

需求 §17.2 仍把"词库来源"列为待确认项，所以这条"内置兜底 + 导出模板"的路径是刻意的：
换词库不需要重新烧录。`word_load_from_sd()` 同时接受相对路径和 `<app_dir>` 下的绝对路径。

## `words/progress.json` 结构

由 `word_save_progress()` 生成，走 `storage_write_file()`（tmp + rename **原子写**）：

```json
{
  "items": {
    "cet6#w1": { "level": 2, "n": 3, "last": 2, "next": 1730086400, "at": 1730000000 }
  },
  "version": 1,
  "book": "cet4",
  "daily_count": 10,
  "known_total": 5,
  "fuzzy_total": 2,
  "unknown_total": 1
}
```

- `items` 的键是 `w<id>`；**`review_count == 0` 的词不写进文件**（文件只存动过的词）。
- 每项字段：`level`（0–5）、`n` 复习次数、`last` 上次评分（0/1/2）、`next` 下次复习时间戳、`at` 上次复习时间戳。
- 顶层 `known_total` / `fuzzy_total` / `unknown_total` 是**累计**评分计数，`daily_count` 也随进度一起持久化。
- 载入时按 `w<id>` 逐词回填；词库里找不到的词进度会被忽略（换词库不会崩）。
- 时间戳同 `todo_manager`：时钟已同步用 Unix 秒，否则用开机秒数。

## 复习调度规则

`schedule()` 的三条分支（`now` 为当前时间戳）：

| 评分 | level | `next_review_ts` |
|---|---|---|
| `WORD_REVIEW_KNOWN`（认识） | `level + 1`，封顶 5 | `now + 2^level × 86400` 秒（按**自增后**的 level 计算） |
| `WORD_REVIEW_FUZZY`（模糊） | 不变 | `now + 86400` 秒（明天再来） |
| `WORD_REVIEW_UNKNOWN`（不认识） | 归 **0** | `now + 600` 秒（10 分钟后重来） |

即：认识一次间隔翻倍（1→2→4→8→16→32 天），模糊只推迟一天，不认识直接回炉。
`word_review()` 记录后立即 `word_save_progress_async()`（**异步**，见下），
并发 `WORD_EVENT_REVIEWED`（payload `uint32_t id`）与 `sys_notify("word_reviewed")`。

### 落盘：`word_save_progress_async()` 的降级路径（v1.3.1）

`word_save_progress()` 会把每个已复习的词序列化并**整文件重写** SD 卡上的
`words/progress.json`，**禁止在 LVGL 任务里调用**（见头文件注释）。所以评分路径走的是
`word_save_progress_async()`：它按需 `xTaskCreate()` 一个 4 KB 栈的 worker，并用
600 ms 去抖窗口合并连续评分（`WORD_SAVE_DEBOUNCE_MS`）。

**但任务栈只能来自内部 RAM**（`AGENT.md §9.0.1`），而本板 boot 结束内部 RAM 实测只剩
**57 KB 空闲、最大连续块 13 KB**——`xTaskCreate` 会失败。

**以前失败就直接 `return ESP_ERR_NO_MEM`**，UI 把它弹成"记录失败：ESP_ERR_NO_MEM"，
**看起来像按钮坏了，其实评分早就生效了，只是没落盘**（v1.3.1 用户报告的 bug ②）。

现在**建任务失败时回退为就地同步写盘**（并打 WARN）：慢一点，但绝不丢评分。
优先级是 **丢数据 > 卡 UI > 报错弹窗**——报错弹窗是最差的一种，
因为它让用户以为功能坏了，而数据其实还在。

## 云端队列（v1.4.0 重写）：墨墨词表 + 本地词库补内容

**用户要的是"在 ESP 上完成整个背单词流程，本地词库只在没法联网和展示的时候用"**
（原话："单词背诵里面没有墨墨单词的单词 只有本地词库的 其实我希望的功能是在 esp 上进行
单词背诵的整个流程 本地词库是在无法联网以及展示的时候使用"）。

### 旧实现的问题（已复现）

`get_today_items` **只返回拼写**，而旧代码把云端词当成"spelling + 一个本地书下标"：

```c
map[i] = local_index_for(items[i].spelling);   /* 命中才有内容，否则空卡 */
if (local == UINT16_MAX) { out[i].id = WORD_ID_CLOUD; }  /* 连释义都没有 */
```

于是**只有本地 CET-6 那 833 个词有内容**。实测线上 20 词队列里 12 词命中本地书，
`quagga` / `narwhal` / `committee` 不在书里 —— **那 8 个词在板上根本没有东西可背**。

### 新实现：构建时不联网，内容按需补（**读过再改**）

`word_cloud_refresh()`（阻塞，worker 上下文）给每个云端词建一张完整卡片，分**两个阶段**：

**阶段 1 —— 构建队列时，完全不联网：**

1. **本地书按拼写命中 → 用本地的**。零流量、零配额、离线可用，音标/释义/例句是人工精选的。
   同时保留本地 `id`，所以评分仍然更新同一条进度记录。
2. **云端独占词 → 只挂历史缓存**（`maimemo_enrich_cached()`，纯 PSRAM 读）。
   以前看过的词因此重启后仍有中文；没看过的就是空白，`meaning` 为空。

**阶段 2 —— 用户翻到那张卡时**（`page_word.c` 的每卡 worker）：
对仍缺内容的词调 `maimemo_enrich()`（释义 `/interpretations` + 助记 `/notes` + 例句 `/phrases`），
把结果写回卡片**并缓存到 SD 卡**，所以第二次看是离线的。

#### ⚠️ 为什么内容必须懒加载（这一版最重要的教训）

**第一版是在构建队列时对每个云端词都调 `maimemo_enrich()`。它把板子打死了。**
实测（55~64 词）：

- 每个词一次 `GET /vocabulary` + 2~3 次 TLS，整趟跑了几分钟；
- 每次 `maimemo_enrich()` 结尾都**整文件重写 `enrich.json`**，反复分配导致内部 RAM 碎片化；
- 结果：`internal largest` 掉到 **160 字节**、`int-low` 从 45 KB 掉到 **472 字节**；
- 随后 `task_wdt: IDLE0 (CPU 0) did not reset the watchdog` 反复刷屏、`CPU 1: httpd` 卡死，
  **设备 ping 得通但所有 TCP 连不上**（v1.3.3 那个"Web 服务器被冻死"的形状重演）。

改成懒加载后，同样的 55 词：`int-low` **15 KB**，队列构建后 TLS 与 Web 服务都正常。
**根本区别是代价与"用户看了多少张卡"成正比，而不是与"账号里有多少词"成正比。**

> 想在别处加"顺手把整队词的内容拉全"的优化之前，请重读这一节。

队列因此从"spelling 占位数组"变成 **`word_item_t` 完整数组**，`word_get_today()` 与
`word_next()` 只做一次结构体拷贝。

### 谁触发构建（v1.4.0 修的**用户原始抱怨根因**）

原来 `word_cloud_refresh()` **只被 `page_word.c` 调用**，于是"用户正好打开单词页"才建队列：
从网页点「立即同步」或开机自动同步时，**进度数字更新了但一张卡都没进队列**，设备继续用本地书。
实测复现过（`sync` 返回 `started:true`、串口 `study progress: 0/55`，而 `/words/today`
仍是 CET-6 的 `adverse/advocate/...`）。

现在 **`word_service` 自己订阅 `MAIMEMO_EVENT_SYNC_DONE`**（`on_maimemo_sync_done()`），
谁发起的同步都会重建队列。两点必须记住：

- **用 `sys_event_subscribe()`，不要用 `esp_event_handler_register()`。**
  本项目的事件发到**自定义** `s_loop`（`sys_event_post_to()` →
  `esp_event_post_to(s_loop, ...)`），用 ESP-IDF 默认循环注册**永远不触发，而且不报任何错**。
- 处理函数里**不能**直接建队列：它在 `sys_evt` 任务上（栈很小），而 `word_cloud_refresh()`
  要做 TLS 与 SD 写 → 交给一个一次性 worker（`th_word_cq`，6 KB）。

### `words/maimemo.json`（`word_cloud_export()`）

同步成功后把当日词表落成**普通词库文件**（与 `cet6.json` 同格式），于是：

- **重启后还在、断网也能背**（这才是"离线兜底"该有的样子）；
- 网页上位机 / SD 卡可以直接查看今天在背什么；
- 需要时可用 `POST /api/v1/words/export` 手动重写。

日志判据（**真机实测值**）：

```
I (xxx) TH:word: cloud queue: 55 words (2 from the local book, 2 with a meaning)
I (xxx) TH:word: memo wordbook written: 55 cards, 2043 B
```

> `with a meaning` 比 `from the local book` 大，是因为它把**从缓存里挂上内容**的云端词
> 也算进去了。**这两个数字都不高是当前令牌的正常结果**：它读不到 `/interpretations`
> （见 `components/maimemo_client/README.md` 的实测表），所以云端词只能等翻卡时补、
> 且补不到释义。换一个权限更宽的令牌后，`with a meaning` 会明显上升。

`word_count()` 顺手补上（控制台/调试用），`WORD_MAIMEMO_BOOK` = `"maimemo"`。

### 云端独占词的评分

`WORD_ID_CLOUD` 的语义变了：以前是"没内容的空卡"，现在是"**墨墨独占词**（有释义有例句，
只是本地没有进度记录）"。评分它时：

- `word_review()` 找不到本地记录 → 写进**学习记录**（见下），不再只是打一行 DEBUG 日志；
- `page_word.c` 的 **uplink** 路径把它排进墨墨复习计划（见 `maimemo_client` 方案 D）。

## 设备端学习记录 `words/review_log.jsonl`（v1.4.0）

`progress.json` 只存**每词最新状态**，回答不了"今天背了什么、结果如何"。而墨墨开放 API
**没有提交评分的接口**（见 `components/maimemo_client/README.md` 的"能力边界"），
所以这份记录是"设备端学习情况"唯一能被看到的地方。

- 一行一条：`{"id":..,"w":"..","r":"known|fuzzy|unknown","t":<unix>,"cloud":true?}`
- **追加写**（`storage_append_file`），不是整文件重写：JSON 数组要 O(n) 重写，
  而这个函数是从 LVGL 回调链上来的（`word_review` → `word_history_append`）。
  断电只会损失最后一行，读取端会跳过解析失败的行。
- **但追加也不能就地做**（v1.4.0 的第一版就是那么写的，是错的）：
  `word_review()` 跑在**持适配器锁的 LVGL 回调**里，而 `storage_append_file()` 是同步
  `fopen`/`fwrite`/`fclose` —— 那就是"每次评分卡一下 UI"的经典形状（`AGENT.md §9.0.1.1`）。
  现在：行在 RAM 里拼好 → 进 **16 条的定长环形队列**（`WORD_HISTORY_QUEUE`）→
  由**已有的**去抖保存 worker 排空（`word_history_flush_pending()`）。
  - **不新建任务**：本板内部 RAM 最大连续块只有约 13 KB，`xTaskCreate` 可能就是失败的那一个
    （`AGENT.md §9.0.7 ③`）。
  - 队列满时**丢最旧的**：刚评的卡比很久以前的更值得留。
  - `word_history_read()` 会**先排空队列再读**，否则"刚评完看不到"会像功能坏了。
  - `word_history_clear()` 会**先丢弃队列**，否则 worker 会把刚删掉的行又写回去。
- 超过 `WORD_HISTORY_MAX_READ`（128 KB）时**只打 WARN 不压缩** —— 压缩 = 读+重写整个文件，
  绝不能出现在评分路径上；空间由用户在网页点「清空记录」回收。
- `word_history_read(out, max, &count, today_only)` 只解析需要的那部分；`today_only`
  用 `day_start_stamp()`（`sys_time_ts()` 已含时区偏移，所以是整除 86400）。

## 今日队列的构建顺序

**本地书模式**：`build_queue()` 三步，上限 `daily_count`（默认 10，`word_set_daily_count()`
夹在 **1–100**）：

1. **到期复习**：`review_count > 0 && next_review_ts <= now` 的词，随后用插入排序按 `next_review_ts` 升序；
2. **新词补位**：`review_count == 0` 的词，按词库顺序填满剩余名额；
3. **还不足**：把剩下的**已复习**词（即尚未到期的）按词库顺序补上，保证"今天总有卡可翻"。

**云端模式**（`s_cloud_active`）则**直接使用墨墨那天的词表**，顺序由账号的计划决定，
本地书不参与排队（只在 `cloud_fill_content()` 里提供内容）。这两条路径是互斥的：
`word_get_today()` 与 `word_next()` 都先看 `s_cloud_active`。

`word_get_today(out, max, &count)` 在本地模式下会**重建**队列，并把 `*count` 设为
**队列总长度**（不是拷进 `out` 的条数，所以传 `NULL`/`max=0` 只查数量时也拿得到总数）。
`word_next()` 用内部游标逐张发牌，走到末尾会**回绕**到第一张；
`word_queue_position()` 返回 1-based 游标位置与队列长度，**云端模式下报的是云端长度**
（否则翻着 20 张卡却显示 3/10）。`word_set_daily_count()` 会清空队列以强制按新容量重建。

**`word_cloud_refresh()` 的失败降级**：任何失败（无令牌、网络、作用域）都会**丢掉云端队列并
退回本地书**，绝不留下空页面。这也是"本地词库在无法联网时使用"的落点。

## 其他约定

1. **内存**：词条本体优先用 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`，失败再退回内部 RAM——
   单词文本占空间，internal RAM 要留给 WiFi/HTTP。进度数组用 `calloc` 并按词库重建。
2. **评分名**：`known` / `fuzzy` / `unknown`，与 `word_review_name()` / `word_review_from_name()` 一一对应，
   HTTP 也用它。
3. **统计口径**（`word_stats()`）：`new_words` = 从未复习；`due` = 已复习且 `next_review_ts <= now`；
   `learned` = `level >= 3`；三个 `*_total` 是累计评分次数。
4. **存储不可用**：`word_save_progress()` 返回 `ESP_ERR_INVALID_STATE`，但内存里的复习照常进行，
   `word_review()` 会把该错误原样返回给调用方。
5. **事件**：`WORD_EVENT` 的 `LOADED`（payload `size_t` 词数）、`REVIEWED`（`uint32_t id`）、
   `PROGRESS_SAVED`（无 payload）、`LOAD_FAILED`（`esp_err_t`）。

## HTTP 路由表（需求 §8.4）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/words/today` | 今日队列 + `count`、`book`、`server_ts`、嵌套 `stats` |
| GET | `/api/v1/words/stats` | 统计（含 `book`、`daily_count`） |
| GET | `/api/v1/words/next` | 下一张卡 + `position`、`queue_total`；队空 → 404 |
| POST | `/api/v1/words/{id}/review` | body `{"result":"known"\|"fuzzy"\|"unknown"}`，返回该词最新状态 |
| POST | `/api/v1/words/config` | body 可含 `daily_count` / `book` / `reset:true`，返回统计 |
| GET | `/api/v1/words/history` | **（v1.4.0）** 设备端学习记录。查询参数 `today=1`、`limit=N`（≤200，默认 50）。返回 `items[]`（`spelling`/`result`/`at`/`cloud`）、`count`、`today_only`、`server_ts`。**没有记录不是错误**，返回空数组 |
| POST | `/api/v1/words/history` | **（v1.4.0）** body `{"clear":true}` 清空学习记录。做成 POST 以免浏览器预取误触发 |
| POST | `/api/v1/words/export` | **（v1.4.0）** 把当前云端队列重写成 `words/maimemo.json`；没有云端队列时 400 |

```powershell
# 背几张卡之后看记录（设备端）
curl.exe "http://todohelper.local/api/v1/words/history?limit=20"
# 只看今天
curl.exe "http://todohelper.local/api/v1/words/history?today=1"
# 把今天的墨墨词表落到 SD 卡
curl.exe -X POST http://todohelper.local/api/v1/words/export
```

## Kconfig

- `CONFIG_TODOHELPER_WORD_DAILY_COUNT`（默认 10，范围 1–100）— 每日张数，NVS 键 `word_daily` 可覆盖。
- `CONFIG_TODOHELPER_WORD_EXPORT_TEMPLATE`（默认 `y`）— 无词库文件时把内置示例写到
  `<storage>/todohelper/words/<book>.json`。

## 测试方法

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build
idf.py -p COM34 flash monitor
```

首次上电（无词库文件时）：

```
I (xxx) TH:word: starter wordbook written to 'words/cet6.json'
I (xxx) TH:word: using the compiled-in starter wordbook
I (xxx) TH:word: ready (book 'cet6', 32 words, 10/day)
```

第二次上电（文件已在存储上）：

```
I (xxx) TH:word: loaded 32 words from 'words/cet6.json'
I (xxx) TH:word: progress restored for 3 word(s)
```

手工验证：

```powershell
curl.exe http://todohelper.local/api/v1/words/today
curl.exe -X POST http://todohelper.local/api/v1/words/1/review -H "Content-Type: application/json" -d '{\"result\":\"known\"}'
curl.exe -X POST http://todohelper.local/api/v1/words/1/review -H "Content-Type: application/json" -d '{\"result\":\"unknown\"}'
curl.exe http://todohelper.local/api/v1/words/stats
```

- `known` 后该词 `level` 应为 1、`next_review_ts` 约为 now + 2 天；
- `unknown` 后 `level` 回到 0、`next_review_ts` 约为 now + 600 秒；
- `words/progress.json` 应被原子更新，重启后 `progress restored` 的条数与已复习词数一致。

### 云端模式的判据（v1.4.0，**尚未上机**）

```powershell
# 1) 先探测令牌能力（见 maimemo_client README），确认 interpretations 是否可用
curl.exe -X POST http://todohelper.local/api/v1/maimemo/probe

# 2) 开云端模式 + 同步（需要已存令牌）
curl.exe -X POST http://todohelper.local/api/v1/words/config -H "Content-Type: application/json" -d "{\"book\":\"maimemo\"}"
curl.exe -X POST http://todohelper.local/api/v1/maimemo/sync

# 3) 看设备上到底有没有内容：meaning 不该是空的
curl.exe http://todohelper.local/api/v1/words/today
```

期望串口：

```
I (xxx) TH:word: cloud queue: 20 words (12 from the local book, 19 with a meaning)
I (xxx) TH:word: memo wordbook written: 20 cards, 4821 B
```

**判据**：`with a meaning` 的数字应当**明显大于** `from the local book`；如果两者相等，
说明 `/interpretations` 对本令牌不可用（回第 1 步看探测结果），此时本地没有的词仍只有拼写。

背几张卡，然后：

```powershell
curl.exe "http://todohelper.local/api/v1/words/history?limit=20"
```

应当看到每评一张卡就多一条 `{"spelling":..,"result":..,"at":..}`，
云端独占词还会带 `"cloud":true`。文件是 `words/review_log.jsonl`（一行一条、追加写）。

> **重要**：这些记录**不会**出现在墨墨 App 的「今日 N/M」里 —— 开放 API 没有提交评分的接口。
> 开启回传开关后，设备会用 `advance_study` 把学过的词排回复习计划（见
> `components/maimemo_client/README.md` 方案 D）。**本轮（1.4.0）以上都没在真机上跑过。**
