# maimemo_client

**L5 业务组件** — 墨墨背单词（MaiMemo）开放 API 客户端：云端今日队列、账号真实进度、
助记/例句预取、把不会的词推回学习计划。

本组件是**可选**的：`main/app_main.c` 的模块表里标了 `optional = true`，**没有令牌就静默
降级** —— 云端模式关闭，`word_service` 继续用本地词库，UI 与 HTTP 都不报错。

- 头文件：`include/maimemo_client.h`
- 依赖：`sys_utils`、`storage_manager`、`http_router`、`cjson`、`esp_http_client`、
  `esp-tls`、`mbedtls`
- 被谁依赖：`word_service`（方案 A 的云端队列，已在它的 `REQUIRES` 里）、
  `ui_pages`（`page_word.c` 的云端进度条与助记/例句）、`main`（模块表 + 开机自动同步）
- 端点基址：`https://open.maimemo.com/open/api/v1`（`MM_BASE`），响应上限 24 KB
  （`MM_MAX_RESPONSE`，64 词的今日队列约 8 KB），单次请求超时 15 s

## 三个方案（用户选定的功能范围）

| 方案 | 做什么 | 用到的端点 | 默认 |
|---|---|---|---|
| **A 学习同步** | 今日词队列与账号**真实进度**来自墨墨，而不是本地词库 | `/study/get_study_progress`、`/study/get_today_items`、`/study/query_study_records` | 需要令牌，NVS `mm_mode=cloud` 时生效 |
| **B 助记/例句预取** | 助记（`/notes`）与例句（`/phrases`）缓存到 SD 卡，卡片离线也能显示 | `/notes`、`/phrases` | 需要令牌**且**令牌有读取作用域 |
| **C 反向推送** | 评为「不认识」的词用 `add_words` 推回墨墨学习计划 | `/study/add_words` | **默认关闭**，NVS `mm_push` 显式开启 |

### 方案 A：学习同步

- `maimemo_fetch_progress()` → `POST /study/get_study_progress`，得到 `finished` / `total` /
  `study_time_ms`；计划词数来自 `POST /study/query_study_records`，body 固定
  `{"as_count":true}`（只要计数，不拉全部记录）。
- `maimemo_fetch_today()` → `POST /study/get_today_items`，每条含 `voc_id`、`voc_spelling`、
  `is_new`、`is_finished`、`first_response`（`FORGET` / `FAMILIAR` / `UNSPECIFIED`）。
  队列上限 `MAIMEMO_MAX_TODAY = 64`（面板一次只显示一张卡）。
- 结果由 `word_service` 接住：`word_cloud_refresh()` 把云端词用 `voc_spelling` **匹配回本地
  词库**，匹配得上就继续用本地的 level / 复习间隔；匹配不上的词标成 `WORD_ID_CLOUD`，
  仍可在设备上评分，只是本地不记进度。
- `maimemo_get_cached_progress()` 返回**上次成功同步的结果**且不碰网络，所以
  `GET /api/v1/maimemo/status` 与 `page_word.c` 的进度条都能安全地高频调用。

### 方案 B：助记/例句预取

- 缓存文件是 **`<app_dir>/words/enrich.json`**（`MM_CACHE_REL`，相对路径，经
  `storage_read_text()` / `storage_write_file()` 读写，后者是 **tmp + rename 原子写**）。
- **整个文件一次性读进 PSRAM，改完整文件重写**；读盘上限 256 KB（`MM_MAX_CACHE_FILE`）。
  不做"一个词一个小文件"：这台设备的内部 RAM 只有 ~310 KB，批量数据必须显式走 PSRAM
  （`AGENT.md §9.0.5` 那次插卡崩溃就是这么来的）。
- `maimemo_enrich()` 命中缓存就**完全不碰网络**（`from_cache = true`），这是常见路径。
  未命中才把 `spelling` 解析成 `voc_id` 再去问 `/notes` 与 `/phrases`。
- **⚠️ 助记/例句的读取端点是 `GET`，不是 `POST`**：
  `GET /api/v1/notes?voc_id=...` 与 `GET /api/v1/phrases?voc_id=...`。
  `POST /api/v1/notes` **也存在，但它是"创建笔记"**（body `{"note":{voc_id,note_type,note}}`），
  拿 POST 去读会得到完全不同的东西（甚至可能**改写**用户在墨墨里的笔记）。
  已对照官方 CLI 源码（`@maimemo/memo-api` v1.2.0：
  `api("/api/v1/notes", {method:"GET", query:{voc_id}})`，响应按 `res.notes.map(...)` 取）核实。
  响应结构是 `{"data":{"notes":[{"note":"..."}]}}` 与 `{"data":{"phrases":[{"phrase":"..."}]}}`
  —— `mm_fetch_field()` 取数组第 0 项的文本字段。
  本组件为此专门有 `mm_get()`（与 `mm_post()` 并列），因为"这个调用是 GET 还是 POST"
  在墨墨这里**不是风格问题，而是语义问题**。
- 拼写解析用的是 `GET /vocabulary?spelling=`（`mm_resolve_voc_id()`），也是 GET。
- 卡片正面不需要这些数据，所以 `page_word.c` 是在**翻到下一张卡时**就在后台任务里预热，
  用户回忆这个词的时间足够抓完。

### 方案 C：反向推送

- `maimemo_push_unknown()`（单张卡评分路径用）与 `maimemo_push_words()`（HTTP 用，
  一次最多 1000 条）。
- **必须显式开启**（`maimemo_push_enabled()` / `mm_push`）：它会**修改用户在云端的学习计划**，
  不是只读操作，所以默认是关的。没开启时 `maimemo_push_unknown()` 直接返回
  `ESP_ERR_NOT_SUPPORTED` 且**不碰网络**。
- 回报的是**服务器自己的 `added_count`**，不是我们发过去的条数（见下面的坑）。

## 令牌与作用域模型

- 令牌存 NVS `mm_token`，经 `sys_config_set_str()` 写入，从网页控制台录入。
  **与 AI Key 同一条规则（需求 §8.8）：不硬编码、不写日志、任何接口都不回读。**
  `GET /api/v1/maimemo/status` 只返回 `configured`（bool）与掩码提示
  （`token_hint`，形如 `g_zV****`，`mask_token()` 只留前 4 个字符）。
- **令牌的作用域决定哪些方案能用，而且这是一等一的正常情况，不是错误。**
  常见的学习作用域令牌：`/study/*` 与 `/vocabulary` 可用，`/notes`、`/phrases` 返回
  `common_permission_denied`。处理方式是：
  - 映射成 **`ESP_ERR_NOT_SUPPORTED`**，语义是"这个令牌不许做这件事，别再问了"；
  - **按词缓存为不可用**（`enrich.json` 里的 `unavailable: true`），所以每个词只会被问一次；
  - `GET /api/v1/maimemo/enrich` 返回 `unavailable: true` 而不是报错，
    UI 只是**不显示**助记块。
  理由很直接：一个"同步了队列但不显示助记"的设备仍是有用的，一个"报错"的设备不是。
- 无令牌时 `mm_token()` 失败 → 返回 `ESP_ERR_NOT_FOUND`，云端模式**无声关闭**。

## 错误模型（这里最容易写错）

**墨墨的接口即使权限错误也返回 HTTP 200**，错误写在信封里：

```json
{ "success": false, "data": null, "errors": [ { "code": "common_permission_denied", "msg": "..." } ] }
```

所以**只看状态码会漏掉全部权限/参数错误**。`mm_classify()` 检查 `success` 与
`errors[].code`：含 `permission_denied` → `ESP_ERR_NOT_SUPPORTED`，其余 → `MM_ERR_OTHER`。
HTTP 层的映射是：`401`/`403` → `ESP_ERR_INVALID_STATE`（令牌过期/不被允许）、
非 200 → `ESP_ERR_INVALID_RESPONSE`、响应超限 → `ESP_ERR_INVALID_SIZE`。

### `/study/add_words` 的 `advance` 是**必需**属性

实测（线上 API）：**漏掉 `advance` 即使请求其它部分完全合法也会失败** ——

```
common_invalid_param: {base} must have required property 'advance'
```

即它是个**必填**字段，不是可选项。所以本组件**永远发送**它（`false` 也发）。
`advance: true` 会让词进入可立即复习的状态（降低等级门槛）。

请求体形状是 **`{"words":[{"id":<voc_id>},...],"advance":bool}`** ——
**按 `id` 加词，不是按拼写**，所以设备侧先用 `GET /vocabulary?spelling=` 把拼写解析成
`voc_id`，解析不到的词跳过并 `SYS_LOGD`。这与官方 CLI 的用法一致
（`add_words` 走 `advance` + `words[].id`，已对照 `@maimemo/memo-api` v1.2.0 核实）。

## 线程规则（**最重要的一条**）

**本组件里所有会做 TLS 的函数都是阻塞的，绝不允许从 LVGL 任务调用。**
LVGL 的回调（控件事件、`lv_timer`）在**适配器锁内**运行（`AGENT.md §9.0.1` / §9.0.1.1），
在里面调 `maimemo_fetch_today()` 就是持锁等一次 HTTPS 往返 → 整个 UI 冻结。

- 唯一对 UI 安全的入口是 **`maimemo_sync_start()`**：它在 worker 任务上跑，立刻返回，
  完成时发 `MAIMEMO_EVENT_SYNC_DONE` / `_FAILED`；`maimemo_has_token()` 与
  `maimemo_get_cached_progress()` 只读内存/NVS，也可以从 UI 调。
- `page_word.c` 的做法：`lv_async_call` 起一个一次性任务抓助记，结果在 LVGL 任务里应用。

## 事件（基址 `MAIMEMO_EVENT`）

| 事件 | payload | 说明 |
|---|---|---|
| `MAIMEMO_EVENT_SYNC_STARTED` | — | 同步开始 |
| `MAIMEMO_EVENT_SYNC_DONE` | `maimemo_progress_t` | 同步完成，含账号进度 |
| `MAIMEMO_EVENT_SYNC_FAILED` | `esp_err_t` | 失败原因 |
| `MAIMEMO_EVENT_PUSH_DONE` | `int` | 服务器接受的词数 |

## NVS 键

| 键 | 宏 | 说明 |
|---|---|---|
| `mm_token` | `SYS_KEY_MAIMEMO_TOKEN` | 开放 API 令牌（**不回读**，只回掩码提示） |
| `mm_mode` | `SYS_KEY_MAIMEMO_MODE` | `cloud` / `local`；默认 `local`。`word_cloud_mode()` 读它 |
| `mm_push` | `SYS_KEY_MAIMEMO_PUSH` | 方案 C 开关，`"1"` / `"0"`；默认关 |

`word_cloud_mode()` 要求 **`mm_mode == "cloud"` 且有令牌**，两者缺一就退回本地书。

## HTTP 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/maimemo/status` | `configured`、`token_hint`、`push_enabled`、`synced` + 嵌套 `progress`（`finished` / `total` / `remaining` / `study_time_ms` / `study_time_min` / `plan_count`）。**不碰网络**（用上次同步的缓存），可安全轮询 |
| POST | `/api/v1/maimemo/config` | body `{"token":"...","push":bool,"verify":bool,"clear":bool}`。`verify` 会真的访问一次墨墨（5~15 s），所以是**显式请求**而不是保存就校验；返回里多一个 `verify: "ok" \| <esp_err_t>`。**校验失败不等于令牌坏**（学习作用域令牌对方案 A 完全可用） |
| POST | `/api/v1/maimemo/sync` | 异步发起一次方案 A 同步，返回 `{"started":true}`；已有同步在跑返回 400。结果看 `status` |
| GET | `/api/v1/maimemo/today` | **实时**拉取今日词表（`items[]` + `count`）。会阻塞约一次 HTTPS 往返 |
| GET | `/api/v1/maimemo/enrich?spelling=x` | 助记 / 例句。返回 `mnemonic`、`example`、`from_cache`、`unavailable` |
| POST | `/api/v1/maimemo/push` | body `{"spellings":[...],"advance":bool}`，最多 1000 条；返回 `{"accepted":N}` |

没有令牌时 `sync` / `today` / `push` 返回 **400**（`no token configured`）；
`status` 与 `enrich` 照常返回 200（`enrich` 会把 `unavailable` 置 true）。

## 测试方法

```bash
# 构建（Git-Bash 下别再直接调 idf.py，见根 README 与 AGENT.md §4.2）
bash tools/build.sh build
```

```powershell
# 1) 存入令牌并当场校验（会真的访问墨墨，等 5~15 秒）
curl.exe -X POST http://todohelper.local/api/v1/maimemo/config `
  -H "Content-Type: application/json" -d "{\"token\":\"g_zV...\",\"verify\":true}"

# 2) 看状态（不含明文令牌，只看 configured / 掩码 / 上次同步的进度）
curl.exe http://todohelper.local/api/v1/maimemo/status

# 3) 同步今日队列，然后回读
curl.exe -X POST http://todohelper.local/api/v1/maimemo/sync
curl.exe http://todohelper.local/api/v1/maimemo/status
curl.exe http://todohelper.local/api/v1/maimemo/today

# 4) 助记 / 例句（学习作用域令牌会得到 unavailable:true）
curl.exe "http://todohelper.local/api/v1/maimemo/enrich?spelling=abrupt"

# 5) 反向推送（需要先开启方案 C）
curl.exe -X POST http://todohelper.local/api/v1/maimemo/config `
  -H "Content-Type: application/json" -d "{\"push\":true}"
curl.exe -X POST http://todohelper.local/api/v1/maimemo/push `
  -H "Content-Type: application/json" -d "{\"spellings\":[\"abrupt\"],\"advance\":false}"
```

期望串口日志：

```
I (xxx) TH:maimemo: ready (token=set, push=off)
I (xxx) TH:maimemo: token OK: 10/20 finished today
I (xxx) TH:word: cloud queue: 20 words (12 also in the local book)
I (xxx) TH:maimemo: pushed to the study plan: 1 accepted of 1 resolved (advance=0)
```

未配令牌时初始化照样成功，只是 `token=unset`（这是**刻意**的：可选组件失败不得阻止启动，
`app_main.c` 的模块表里它标了 `optional = true`）。

**验证状态（诚实说明）**：以上端点**都已用真实令牌对线上 API 实测**（`get_study_progress`
返回 10/20、`get_today_items` 返回 20 词、`add_words` 成功并回报 `added_count`；
`/notes`、`/phrases` 确认返回 `common_permission_denied` 并被正确映射）。
**但这些都没有在真机上跑过** —— 本轮没有烧录设备（见 `CHANGELOG.md` 1.3.0 的
`### Verified（真机）` 与 `### Known gaps`）。方案 B 的"快乐路径"（真的在卡片上显示出一条助记）
**尚未被观察到**，因为当前令牌读不了 `/notes`。
