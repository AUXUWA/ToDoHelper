# word_service

**L3 业务组件（FR5）** — 单词卡：词库加载 + 三档评分 + 间隔复习。内置一份起始词库，替换成真实 CET4/CET6 词库即可。

- 头文件：`include/word_service.h`（私有 `src/word_default_book.h` 只导出内置词库字符串）
- 依赖：`sys_utils`、`storage_manager`、`http_router`、`cjson`
- 被谁依赖：`ui_pages`（`page_word.c`、`page_settings.c`）、`main`

## API 摘要

| 分组 | 函数 |
|---|---|
| 生命周期 | `word_service_init()`、`word_reload()`、`word_load_from_sd()` |
| 词库选择 | `word_get_book()`、`word_set_book()` |
| 今日队列 | `word_get_today()`、`word_next()`、`word_get()`、`word_restart_queue()`、`word_queue_position()` |
| 复习 | `word_review()`、`word_save_progress()`、`word_reset_progress()`、`word_review_name()`、`word_review_from_name()` |
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
  **id 是进度的主键**（进度按 `w<id>` 存），所以替换词库文件时务必保留 id 语义，否则旧进度会对错词。
- 超过 `WORD_MAX_ITEMS`（2000）会截断并告警，防止坏文件吃光内存。
- 完整格式同时记录在 `docs/API.md`。

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
    "w1": { "level": 2, "n": 3, "last": 2, "next": 1730086400, "at": 1730000000 }
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
`word_review()` 记录后立即 `word_save_progress()`，并发 `WORD_EVENT_REVIEWED`（payload `uint32_t id`）
与 `sys_notify("word_reviewed")`。

## 今日队列的构建顺序

`build_queue()` 三步，上限 `daily_count`（默认 10，`word_set_daily_count()` 夹在 **1–100**）：

1. **到期复习**：`review_count > 0 && next_review_ts <= now` 的词，随后用插入排序按 `next_review_ts` 升序；
2. **新词补位**：`review_count == 0` 的词，按词库顺序填满剩余名额；
3. **还不足**：把剩下的**已复习**词（即尚未到期的）按词库顺序补上，保证"今天总有卡可翻"。

`word_get_today(out, max, &count)` 会**重建**队列，并把 `*count` 设为**队列总长度**（不是拷进 `out` 的条数，
所以传 `NULL`/`max=0` 只查数量时也拿得到总数）。`word_next()` 用内部游标逐张发牌，走到末尾会**回绕**到第一张；
`word_queue_position()` 返回 1-based 游标位置与队列长度。`word_set_daily_count()` 会清空队列以强制按新容量重建。

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
