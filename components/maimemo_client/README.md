# maimemo_client

**L5 业务组件** — 墨墨背单词（MaiMemo）开放 API 客户端：云端今日队列、账号真实进度、
释义/助记/例句预取、把学过的词回传复习计划、探测令牌能力。

本组件是**可选**的：`main/app_main.c` 的模块表里标了 `optional = true`，**没有令牌就静默
降级** —— 云端模式关闭，`word_service` 继续用本地词库，UI 与 HTTP 都不报错。

- 头文件：`include/maimemo_client.h`
- 依赖：`sys_utils`、`storage_manager`、`http_router`、`cjson`、`esp_http_client`、
  `esp-tls`、`mbedtls`
- 被谁依赖：`word_service`（方案 A 的云端队列，已在它的 `REQUIRES` 里）、
  `ui_pages`（`page_word.c` 的云端进度条与助记/例句）、`main`（模块表 + 开机自动同步）
- 端点基址：`https://open.maimemo.com/open/api/v1`（`MM_BASE`），响应上限 24 KB
  （`MM_MAX_RESPONSE`，64 词的今日队列约 8 KB），单次请求超时 15 s

## 墨墨开放 API 的完整能力边界（v1.4.0 核实）

**这一节是理解本组件能做什么、不能做什么的前提。**依据
[官方开放 API 功能页](http://memodocs.maimemo.com/docs/PNvOw2E1AivPtlkRlQgce1W1n2c) 与
[官方 CLI 源码](https://github.com/maimemo/memo-api-cli)（`src/commands/*.ts` 与其
`types/*.ts`，类型注释标明来自 `openapi/*/v1/*.proto`）：

| 端点 | 方法 | 作用 | 本组件是否使用 |
|---|---|---|---|
| `/study/get_study_progress` | POST | 今日已学/计划/时长 | ✅ `maimemo_fetch_progress()` |
| `/study/get_today_items` | POST | **今日词表**（只给 `voc_id` + `voc_spelling`）| ✅ `maimemo_fetch_today()` |
| `/study/query_study_records` | POST | 计划内记录（计数/列表，非 beta）| ✅ 兜底 + 计划词数 |
| `/study/add_words` | POST | 加词进计划，`advance` **必填** | ✅ 方案 C |
| `/study/advance_study` | POST | **把词提前到待复习** | ✅ `maimemo_advance_words()`（v1.4.0）|
| `/vocabulary?spelling=` | GET | 拼写 → voc_id。**只回 `{id, spelling}`** | ✅ `mm_resolve_voc_id()` |
| `/vocabulary/query` | POST | 批量按 spellings/ids 查（同样只有 id+spelling）| ❌ 未用（批量解析可考虑）|
| `/interpretations?voc_id=` | GET | **释义**（`interpretation`/`tags`/`status`）| ✅ `maimemo_fetch_meaning()`（v1.4.0）|
| `/notes?voc_id=` | GET | 助记 | ✅ 方案 B |
| `/phrases?voc_id=` | GET | 例句 | ✅ 方案 B |
| `/notepad/*` | CRUD | 生词本管理 | ❌ |

### ⛔ 不存在的能力：提交复习评分

**墨墨开放 API 没有任何端点可以上报「我把这个词评为认识/模糊/不认识」。**`study` 下只有上面
那五个操作，`study.proto` 里也没有对应类型。因此：

- 设备上背完的词**不会**让墨墨 App 的「今日 N/M」增加；
- 想"上传学习情况"，在平台能力内**只能**用 `advance_study` 把学过的词排回复习计划，
  让云端继续安排它们 —— 这就是 `maimemo_advance_words()` 做的事，也是网页面板里
  把话说清楚的那段文字的依据。

**不要**为了"看起来同步了"而在文档或 UI 里暗示评分被提交了。

### ⚠️ `/vocabulary` 不是内容源

它只返回 `{id, spelling}`（见 `types/vocabulary.ts`）—— 用来把拼写解析成 id 可以，
**拿不到释义/音标/例句**。云端词的中文释义必须走 `/interpretations`。

## 四个方案

| 方案 | 做什么 | 用到的端点 | 默认 |
|---|---|---|---|
| **A 学习同步** | 今日词队列与账号**真实进度**来自墨墨 | `/study/get_study_progress`、`get_today_items`、`query_study_records` | 需要令牌，NVS `mm_mode=cloud` 时生效 |
| **B 助记/例句/释义预取** | 助记、例句、**释义**缓存到 SD 卡 | `/notes`、`/phrases`、**`/interpretations`** | 需要令牌，按端点各自判定作用域 |
| **C 反向推送** | 评为「不认识」的词用 `add_words` 推回计划 | `/study/add_words` | **默认关闭**，NVS `mm_push` 开启 |
| **D 回传与探测**（v1.4.0）| 学过的词用 `advance_study` 排回复习计划；一次性探测令牌能用哪些端点 | `/study/advance_study` + 全部端点 | 回传跟随 `mm_push`；探测只读 |

### 方案 A：学习同步

- `maimemo_fetch_progress()` → `POST /study/get_study_progress`，得到 `finished` / `total` /
  `study_time_ms`；计划词数来自 `POST /study/query_study_records`，body 固定
  `{"as_count":true}`（只要计数，不拉全部记录）。
- `maimemo_fetch_today()` → `POST /study/get_today_items`，每条含 `voc_id`、`voc_spelling`、
  `is_new`、`is_finished`、`first_response`（`FORGET` / `FAMILIAR` / `UNSPECIFIED`）。
  队列上限 `MAIMEMO_MAX_TODAY = 64`（面板一次只显示一张卡）。
  - **请求体必须是 `{"is_finished":false,"limit":N}`，不能是 `{}`**（v1.3.1 修）。
    实测同一个令牌：发 `{}` 得到 `{"today_items":[]}`，发带过滤的 body 得到 **51 个词**。
    该端点是 **Beta**，官方文档写明"如果当日未打开 App 进行初始化则无法准确计算"，
    且 body 参数**全部可选**——所以**空数组不代表账号没有词**。
  - **为空时有兜底**：`mm_fetch_today_from_records()` 改用非 beta 的
    `POST /study/query_study_records`（`limit` + `next_study_date.end`）重建队列，
    `last_response` → `first_response`，`study_count == 0` → 新词。
  - 兜底的**请求条数上限是 `MM_RECORDS_MAX = 56`**：一条 record ≈ 350 B（60+ 字符的
    voc_id 占大头），24 KB 的 `MM_MAX_RESPONSE` 只装得下约 60 条；请求更多会让**响应**
    溢出、整次调用失败，**比队列短更糟**。实测 56 条 = 19656 B。
- 结果由 `word_service` 接住：v1.4.0 起 `word_cloud_refresh()` 会用
  `cloud_fill_content()` 给每个词**填满音标/释义/例句**（本地书优先，缺的才查云端），
  并把当日词表落成 `words/maimemo.json`。详见 `components/word_service/README.md`。
- `maimemo_get_cached_progress()` 返回**上次成功同步的结果**且不碰网络，所以
  `GET /api/v1/maimemo/status` 与 `page_word.c` 的进度条都能安全地高频调用。

### 方案 B：助记/例句/**释义**预取

- 缓存文件是 **`<app_dir>/words/enrich.json`**（`MM_CACHE_REL`，相对路径，经
  `storage_read_text()` / `storage_write_file()` 读写，后者是 **tmp + rename 原子写**）。
- **整个文件一次性读进 PSRAM，改完整文件重写**；读盘上限 256 KB（`MM_MAX_CACHE_FILE`）。
  不做"一个词一个小文件"：这台设备的内部 RAM 只有 ~310 KB，批量数据必须显式走 PSRAM
  （`AGENT.md §9.0.5` 那次插卡崩溃就是这么来的）。
- `maimemo_enrich()` 命中缓存就**完全不碰网络**（`from_cache = true`），这是常见路径。
  未命中才把 `spelling` 解析成 `voc_id` 再去问 `/notes`、`/phrases` **和 `/interpretations`**。
- **v1.4.0 的关键改动：释义与助记/例句的作用域是分开的。**
  以前"助记被拒 → 整次调用按不可用缓存"，把释义一起牺牲了；但只有学习权限的令牌**常常
  读得到 `/interpretations`**，而释义恰是"本地词库没有的词"唯一的内容来源。现在：
  - `unavailable` 只表示 notes/phrases 被拒，`meaning_unavailable` 单独表示
    `/interpretations` 被拒；
  - 只有**两者都不可用**才返回 `ESP_ERR_NOT_SUPPORTED`；
  - 只要拿到任何一项（释义也行）就返回 `ESP_OK`。
- **⚠️ 助记/例句的读取端点是 `GET`，不是 `POST`**：
  `GET /api/v1/notes?voc_id=...`、`GET /api/v1/phrases?voc_id=...`、
  `GET /api/v1/interpretations?voc_id=...`。
  `POST /api/v1/notes` **也存在，但它是"创建笔记"**（body `{"note":{voc_id,note_type,note}}`），
  拿 POST 去读会得到完全不同的东西（甚至可能**改写**用户在墨墨里的笔记）。
  已对照官方 CLI 源码（`@maimemo/memo-api` v1.2.0 / `memo-api-cli`：
  `api("/api/v1/notes", {method:"GET", query:{voc_id}})`）核实。
  响应结构是 `{"data":{"notes":[{"note":"..."}]}}`、`{"data":{"phrases":[{"phrase":"..."}]}}`、
  `{"data":{"interpretations":[{"interpretation":"..."}]}}` —— `mm_fetch_field()` 取数组
  第 0 项的文本字段。本组件为此专门有 `mm_get()`（与 `mm_post()` 并列），因为"这个调用是
  GET 还是 POST"在墨墨这里**不是风格问题，而是语义问题**。
- 拼写解析用的是 `GET /vocabulary?spelling=`（`mm_resolve_voc_id()`），也是 GET。

#### 缓存命中条件（v1.5.0 修）

命中条件以前是

```
have_text && (meaning_unavailable || meaning != "" || unavailable)
```

也就是"**有助记/例句、但释义为空、且释义没被标记为无权**"这一类**永远不命中**——
而这恰恰是**学习作用域令牌**最常见的形状（助记读得到、释义读不到）。
后果是每翻到这类卡都要重跑 4 次 HTTPS 往返，并**整文件重写 `enrich.json`**。

现在的规则：

| 情况 | 结果 |
|---|---|
| 缓存里有任何一项内容，且释义上次不是**传输失败** | **命中**（`from_cache = true`） |
| 上一次请求"有结论"（API 回答了：给了数据 / 说没有 / 说无权 / 回答了错误） | **命中**（哪怕三项都空） |
| 上一次 `/interpretations` 是**传输层失败**（超时、断连） | 不命中，下次重试 |
| 助记与释义**都被拒** | `ESP_ERR_NOT_SUPPORTED`（不变） |

为此缓存条目多了两个字段（写进 `words/enrich.json`，只加不改）：

- `fetched`：上一次请求是否有结论（三个端点都回答了）；
- `meaning_retryable`：上一次 `/interpretations` 是否死在传输层（值得重试）。

**传输失败不许覆盖缓存**：某个字段的请求失败时，缓存里的旧值既不会被清空，
也会继续返回给调用方 —— 一次网络抖动不该把已经拿到的助记/释义弄丢。

**旧缓存文件没有这两个字段**：`fetched` 视为 false，于是这类词会被**再取一次**
然后按新格式重写（自愈）；已有内容的条目不受影响。

### 方案 C：反向推送

- `maimemo_push_unknown()`（需要知道结果时用）与 `maimemo_push_words()`（HTTP 用，
  一次最多 1000 条）。
- **`maimemo_push_unknown_async()` 才是给 UI 用的那个**（v1.3.1 新增）：
  廉价检查（开关 / 令牌）留在调用方，**阻塞 TLS 交给 worker 任务**。
- **必须显式开启**（`maimemo_push_enabled()` / `mm_push`）：它会**修改用户在云端的学习计划**，
  不是只读操作，所以默认是关的。没开启时两个版本都直接返回
  `ESP_ERR_NOT_SUPPORTED` 且**不碰网络**。
- 回报的是**服务器自己的 `added_count`**，不是我们发过去的条数。

### 方案 D：回传学过的词 + 能力探测（v1.4.0）

- **`maimemo_advance_spellings()` / `maimemo_advance_words()`**：`POST /study/advance_study`，
  body `{"voc_ids":[...]}`，回应 `advanced_count`。它把词**提前到待复习**，于是设备上背过的
  词会回到账号的复习计划里（也会出现在手机 App 的队列中）。
  - `maimemo_advance_enabled()` **复用 `SYS_KEY_MAIMEMO_PUSH`**：加词与推进都会改账号数据，
    用户拒绝了一个就是拒绝了两个。
  - `*_spellings()` 版本内部按 64 个词一批先做解析（voc_id 是前提），解析不到的词跳过。
    调用方应当**批量调用**，不要一个词一次 —— 那正是 v1.3.3 "把 Web 服务器冻死"的形状。
- **`maimemo_probe()` / `POST /api/v1/maimemo/probe`**：逐条试 6 个端点，返回
  `endpoints[]`（`endpoint` / `ok` / `status` / `detail`）+ `usable` / `total`。
  - **它不会因为一个端点失败就停下**，因为作用域互相独立：实测就是"队列能读、助记/例句被拒"。
  - `status` 直接给 `esp_err_to_name()` 的名字，因为**要区分的恰恰是那几种"不 ok"**：
    `ESP_ERR_NOT_SUPPORTED` = 作用域不够（正常），`ESP_ERR_INVALID_STATE` = 令牌被拒绝
    （要重新生成），`ESP_ERR_NOT_FOUND` = 没有数据。把这三者混成一个"失败"就是以前让用户
    以为令牌坏了的根源。
  - 网页面板的「探测令牌能力」按钮就是它，用法见下面的测试方法。

## 令牌与作用域模型

- 令牌存 NVS `mm_token`，经 `sys_config_set_str()` 写入，从网页控制台录入。
  **与 AI Key 同一条规则（需求 §8.8）：不硬编码、不写日志、任何接口都不回读。**
  `GET /api/v1/maimemo/status` 只返回 `configured`（bool）与掩码提示
  （`token_hint`，形如 `g_zV****`，`mask_token()` 只留前 4 个字符）。

### ⚠️ 两类令牌，权限不同（**想读释义/助记/例句必须看这一节**）

官方有**两条**获取令牌的路径，能力不一样：

| | **个人令牌**（App 内生成） | **开放平台应用令牌**（OIDC） |
|---|---|---|
| 获取方式 | 墨墨 App → 我的 → 设置 → 开放 API | [open.maimemo.com/app](https://open.maimemo.com/app) 创建应用 → 填**授权范围** → 审核 → OIDC 授权 |
| 权限选择 | **没有可勾选的界面**，由墨墨固定 | **申请时勾选 scope**，且"后续需要增加权限可以再次编辑" |
| 实测能力 | `study` ✅、`vocabulary` ✅、**`interpretations`/`notes`/`phrases` ❌** | 取决于获批的 scope |
| 适用 | 自己一个人用 | 分发给他人 / 需要完整能力 |

**关键事实：读释义/助记/例句是墨墨官方主推的核心能力**，官方原文（
[开放 API 功能介绍](http://memodocs.maimemo.com/docs/PNvOw2E1AivPtlkRlQgce1W1n2c)）：

> 「第三方工具可调取墨墨词库的单词**释义、例句、助记**内容，实现跨软件查词、收藏生词。」

[开放平台文档](https://memodocs.maimemo.com/docs/open) 也把「把单词、**例句、释义、助记**等数据
同步到用户的墨墨背单词 App，甚至直接将单词加入学习规划或者**提前复习**」列为典型场景
（"提前复习"即本组件的 `advance_study`）。

所以**个人令牌被拒不是平台没有这个能力，而是个人令牌没有给这些 scope**。
想要卡片显示中文释义，需要走开放平台应用并申请相应授权范围。

> ⚠️ **但本设备目前只实现了个人令牌**（把令牌字符串填进网页控制台即可用）。
> 开放平台应用走的是 **OIDC 授权码流程**（需要 client_id / client_secret / 回调地址 /
> scope），设备端没有实现这条链路 —— 见本文末尾"开放平台令牌的接入代价"。

- **无论哪种令牌，作用域都不该靠猜。** 用 `POST /api/v1/maimemo/probe` 或网页面板的
  「探测令牌能力」按钮，它会**逐条**给出每个端点的结论。处理方式是：
  - 映射成 **`ESP_ERR_NOT_SUPPORTED`**，语义是"这个令牌不许做这件事，别再问了"；
  - **按端点、按词缓存**（`enrich.json` 的 `unavailable` / `meaning_unavailable`）；
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
`voc_id`，解析不到的词跳过并 `SYS_LOGD`。

### `/study/advance_study` 用的是 `voc_ids`（注意不是 `words`）

```json
{ "voc_ids": ["<voc_id>", "..."] }
```

回应 `{"data":{"advanced_count":N}}`。**它和 `add_words` 的字段名不一样**
（`voc_ids` vs `words[].id`），这是照着官方 CLI 的 `types/study.ts` 抄的，不要凭印象写。

## 失败退避与重试策略（v1.7.0）

**一句话：网络不通 → 退避后重试（分钟级）；令牌无效 → 一律不重试，等用户改配置。**

### 为什么必须先分类

墨墨的错误模型让「重试」这件事变得有陷阱：

1. **权限错误也是 HTTP 200**，错误写在信封里（`errors[].code`）；
2. **HTTP 401 在 IDF 里会让 `esp_http_client_perform()` 直接失败** —— 401 没有
   `WWW-Authenticate` 头时，`esp_http_client_add_auth()` 打一句
   `This request requires authentication, but does not provide header information
   for that` 就返回 `ESP_ERR_NOT_SUPPORTED`
   （`esp-idf/components/esp_http_client/esp_http_client.c:2126`），
   于是 perform() 以失败告终 —— **状态码已经是 401，但函数返回的是「没拿到应答」**。

所以同一个「令牌无效」有两种完全不同的长相：401（perform 失败）与
200 + 信封里的 `unauthorized`/`token`。只看其中一处，就会把「重试也不会成功」
当成「网络抖了一下」。

### 现在的规矩

| 失败 | 分类 | 怎么处理 |
|---|---|---|
| HTTP 401 / 403，或信封里 `unauthorized`/`token` | **认证** | **不重试**。锁存「令牌被拒」，自动路径不再发请求，直到用户换令牌（`maimemo_set_token()` 清锁存） |
| 作用域不够（`common_permission_denied`）| 终态 | 不重试（也不锁存令牌 —— 令牌是好的，只是这个端点没授权） |
| 服务端给了别的错误信封、响应超限、不是 JSON | 终态 | 不重试 |
| DNS 解析失败 / 连不上 / 超时 / 握手失败 | **传输层** | **退避后重试**，间隔按轮数指数增长 |

- **传输层退避**：第 1 轮等 **60 s**，第 2 轮 **120 s**，第 3 轮 **240 s**，
  第 4 轮 **480 s**，**上限 15 分钟**。
  「一轮」的定义：相邻两次失败之间至少隔了当前的退避间隔 ——
  否则一次 `maimemo_enrich()` 里连着失败的三次请求会把退避一次顶到上限。
- **退避窗口内不再发请求**：`maimemo_fetch_today()` / `maimemo_fetch_progress()`
  直接返回上一次的错误码；`maimemo_enrich()` 按「查不到这个词」处理
  （网络不通时它本来也是这个结果）。**调用方看到的错误分类与真发一次完全一样**，
  省掉的是整条 TLS 往返（外加一次 DNS 查询）。
- **开机同步**（`maimemo_sync_start()` → `th_mm_sync`）最多试 **3 次**
  （60 s → 120 s），仍失败才发 `MAIMEMO_EVENT_SYNC_FAILED`。
  这解决了真机日志里那一幕：开机时还没有 IP，第一次必然失败，
  旧代码失败就完事，于是墨墨云端队列直到用户手动点同步才会出现。
- **一次成功就清零**：成功一次，退避计数与「令牌被拒」锁存一起清掉。

### 开机同步：先等地址，退避期间不占名额（v1.7.0 C1/C2 的落地）

- **先等地址再发请求**：worker 开头先看 `sys_status.wifi_ip`
  （判据与 `word_service` 的 `have_ip()` 一样：**不是空串**，也不能是 `"0.0.0.0"`
  —— `wifi_manager` 在还没有地址时发布的就是 `"0.0.0.0"`）。
  最多等 `MM_SYNC_IP_WAIT_MS = 30 s`；到点还没地址就照常发一次。
  没有地址时 DNS 根本解析不出来（真机日志：`getaddrinfo() returns 202`），
  开机那一次尝试因此从「注定失败」变成「大概率成功」。
- **拿到地址之后再沉降 2 秒**（`MM_SYNC_NET_SETTLE_MS`）：地址到了不等于网络能用了 ——
  DHCP 把 DNS 服务器地址、路由、lwIP 的解析器跟地址一起送到，比 IP 事件晚一点到；
  这一刻就发请求，DNS 可能还解析不出来。歇两秒再发。
- **退避期间任务就退出**：退避是分钟级的，如果 worker 在任务里 `vTaskDelay`，
  那 6 KB 的内部栈（FreeRTOS 的栈只能来自内部 RAM）会被占上一分钟，
  而且「一次只允许一个同步」的名额也会被占着 —— 网页控制台的「立即同步」
  会一直拿到 400 “a sync is already running”。
  现在失败后只排一个**一次性定时器**（`esp_timer`），worker 立刻退出
  （栈还回内部堆），到点再拉起一个新的 worker（这一轮算**续**，不重置次数）。
- **「忙」的判据是「电线上有请求」，不是「有一个任务」**：
  `maimemo_sync_start()` 只在**真的有一个请求在飞**时才返回
  `ESP_ERR_INVALID_STATE`；等地址、退避等待都不算。
  控制台的「立即同步」因此随时可用（它走 raw 路径，不会被退避闸门挡掉）。
- 同一时刻只发**一个**请求：多个 worker（开机同步 + 控制台同步）用一把
  互斥锁串起来，不会同时在电线上。

### 哪些是「自动路径」，哪些是「用户显式动作」

| 路径 | 退避/锁存生效吗 | 为什么 |
|---|---|---|
| 开机同步、云端词队列、翻卡片时的助记/释义补全 | **生效** | 没人盯着，重试只是白烧电台 |
| 网页控制台的「拉取今日词表」「助记/例句」「推送」「回传」 | 不生效 | 用户点的按钮必须真的发一次请求 |
| 网页控制台的「测试令牌」（`verify`） | 不生效 | 那是一次显式的连通性检查 |
| 「探测令牌能力」（`probe`） | 不生效 | 它的用途就是回答「这个令牌到底能做什么」，必须真问 |

显式动作怎么绕过闸门：`maimemo_allow_request_now()`（`maimemo_http.c` 的每个
worker 开头都调一次）把传输层的退避窗口关掉。**令牌锁存不受它影响** ——
锁存只由「用户改了配置」（`maimemo_set_token()`）解除。

### 关于 401 的那句日志

真机日志里的 `HTTP_CLIENT: This request requires authentication...` 之后
**IDF 并没有重发请求**：没有 `WWW-Authenticate` 头时 `add_auth()` 直接失败，
一次 401 = 一次往返。

**但应用层过去会一遍遍重问**：令牌被拒这件事没有被记下来，
于是每翻一张卡都重新问一次 `/vocabulary`，每次都拿回同一个 401。
现在这个 401 会被锁存住（`mm_resolve_voc_id()` 看到锁存就直接放弃），
自动路径不再重复问；等用户换了令牌再重新开始。

### 中间缓冲一律走 PSRAM（roadmap B3）

- HTTP 响应体（24 KB 上限）、词条数组（`MAIMEMO_MAX_TODAY` × `maimemo_item_t`）、
  推送/回传的词条指针数组：**显式 `MALLOC_CAP_SPIRAM`**，
  用 `heap_caps_calloc_prefer()`；释放用 `heap_caps_free()`
  （IDF 里 `free(p)` 与 `heap_caps_free(p)` 等价）。
- cJSON 的树与打印出来的文本走**全局钩子**（`sys_json_use_psram()`），
  本组件在 `maimemo_client_init()` 里再调一次，保证无论初始化顺序如何都是
  PSRAM 优先。
- 已经在用 `heap_caps_calloc_prefer()` 的地方（助记缓存数组、
  回传用的 id 数组）保持不变。

## 线程规则（**最重要的一条**）

**本组件里所有会做 TLS 的函数都是阻塞的，绝不允许从 LVGL 任务调用。**
LVGL 的回调（控件事件、`lv_timer`）在**适配器锁内**运行（`AGENT.md §9.0.1` / §9.0.1.1），
在里面调 `maimemo_fetch_today()` 就是持锁等一次 HTTPS 往返 → 整个 UI 冻结。

- 唯一对 UI 安全的入口是 **`maimemo_sync_start()`**：它在 worker 任务上跑，立刻返回，
  完成时发 `MAIMEMO_EVENT_SYNC_DONE` / `_FAILED`；`maimemo_has_token()` 与
  `maimemo_get_cached_progress()` 只读内存/NVS，也可以从 UI 调。
- **`maimemo_probe()` 会串行做最多 6 次 HTTPS 往返**（最坏 30+ 秒），
  所以它的 HTTP 路由**必须**走 `mm_run_async()`，绝不能留在 handler 里 ——
  这正是 v1.3.3 修掉的那个"冻结整个 Web 服务器"的形状。
- `page_word.c` 的做法：`lv_async_call` 起一次性任务抓助记，结果在 LVGL 任务里应用；
  v1.4.0 起只有"卡片确实缺内容"时才起这个任务。
- **异步 worker 必须自己释放 job**（v1.5.0 修）：`mm_run_async()` 把 job 交给 worker 后
  就不再管它，handler 返回后堆上的拼写副本只能由 worker 释放。
  `push_worker` 一直这么做，`advance_worker` 以前**忘了**，于是每成功调用一次
  `/api/v1/maimemo/advance` 就泄漏一份 job 和它里面的 `strdup` 拼写。

## 解析外部输入一律用 `sys_json_parse(json, 32)`（v1.5.0）

墨墨的响应、缓存文件 `words/enrich.json` 和三个 HTTP 请求体都是**外部输入**，
现在统一用 `sys_json_parse()` 而不是裸 `cJSON_Parse()`：cJSON 1.7.19 的嵌套上限是 1000、
每层约 96 字节栈帧，一个畸形响应就能打爆 worker 的栈（`AGENT.md §9.0.5` 那一类
"开机就崩"的故障）。解析失败一律记 WARN 并走降级路径（把这次查询当成"没有数据"）。

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
| `mm_push` | `SYS_KEY_MAIMEMO_PUSH` | 方案 C **与方案 D 的回传**共用开关；默认关 |

`word_cloud_mode()` 要求 **`mm_mode == "cloud"` 且有令牌**，两者缺一就退回本地书。

## HTTP 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/maimemo/status` | `configured`、`token_hint`、`push_enabled`、`synced` + 嵌套 `progress`。**不碰网络**（用上次同步的缓存），可安全轮询 |
| POST | `/api/v1/maimemo/config` | body `{"token":"...","push":bool,"verify":bool,"clear":bool}`。`verify` 会真的访问一次墨墨（5~15 s），所以是**显式请求**而不是保存就校验；返回里多一个 `verify: "ok" \| <esp_err_t>` |
| POST | `/api/v1/maimemo/sync` | 异步发起一次方案 A 同步，返回 `{"started":true}`；已有同步在跑返回 400。结果看 `status` |
| GET | `/api/v1/maimemo/today` | **实时**拉取今日词表（`items[]` + `count`）|
| GET | `/api/v1/maimemo/enrich?spelling=x` | 助记 / 例句 / **释义**。返回 `mnemonic`、`example`、`meaning`、`from_cache`、`unavailable` |
| POST | `/api/v1/maimemo/push` | body `{"spellings":[...],"advance":bool}`，最多 1000 条；返回 `{"accepted":N}` |
| POST | `/api/v1/maimemo/probe` | **（v1.4.0）** 逐条探测令牌能力；返回 `endpoints[]`、`usable`、`total`。异步，最坏 30 秒 |
| POST | `/api/v1/maimemo/advance` | **（v1.4.0）** body `{"spellings":[...]}`（1–64 条）；把学过的词排回复习计划，返回 `{"advanced":N}`。需先开启 `mm_push` |

没有令牌时 `sync` / `today` / `push` / `probe` / `advance` 返回 **400**
（`no token configured`）；`status` 与 `enrich` 照常返回 200（`enrich` 会把 `unavailable` 置 true）。

## 测试方法

```bash
# 构建（Git-Bash 下别再直接调 idf.py，见根 README 与 AGENT.md §4.2）
bash tools/build.sh build
```

```powershell
# 0) 【v1.4.0】先探测令牌能力：一次看清哪些端点可用、为什么不可用
curl.exe -X POST http://todohelper.local/api/v1/maimemo/probe
#   期望 endpoints[] 形如：
#     study/get_study_progress  ok=true
#     study/get_today_items     ok=true   detail=20
#     vocabulary                ok=true
#     interpretations           ok=true   ← 有了它，本地没有的词也能显示中文释义
#     notes                     ok=false  status=ESP_ERR_NOT_SUPPORTED（正常）
#     phrases                   ok=false  status=ESP_ERR_NOT_SUPPORTED（正常）

# 1) 存入令牌并当场校验（会真的访问墨墨，等 5~15 秒）
curl.exe -X POST http://todohelper.local/api/v1/maimemo/config `
  -H "Content-Type: application/json" -d "{\"token\":\"g_zV...\",\"verify\":true}"

# 2) 看状态（不含明文令牌，只看 configured / 掩码 / 上次同步的进度）
curl.exe http://todohelper.local/api/v1/maimemo/status

# 3) 同步今日队列，然后回读
curl.exe -X POST http://todohelper.local/api/v1/maimemo/sync
curl.exe http://todohelper.local/api/v1/maimemo/status
curl.exe http://todohelper.local/api/v1/maimemo/today

# 4) 助记 / 例句 / 释义（学习作用域令牌的 notes/phrases 会 unavailable:true，
#    但 meaning 仍可能有值 —— 这是 v1.4.0 的重点）
curl.exe "http://todohelper.local/api/v1/maimemo/enrich?spelling=abrupt"

# 5)【v1.4.0】把设备上学过的词排回复习计划（需先开启回传开关）
curl.exe -X POST http://todohelper.local/api/v1/maimemo/config `
  -H "Content-Type: application/json" -d "{\"push\":true}"
curl.exe -X POST http://todohelper.local/api/v1/maimemo/advance `
  -H "Content-Type: application/json" -d "{\"spellings\":[\"abrupt\",\"quagga\"]}"

# 6) 反向推送（方案 C）
curl.exe -X POST http://todohelper.local/api/v1/maimemo/push `
  -H "Content-Type: application/json" -d "{\"spellings\":[\"abrupt\"],\"advance\":false}"
```

期望串口日志：

```
I (xxx) TH:maimemo: ready (token=set, push=off)
I (xxx) TH:maimemo: token OK: 10/20 finished today
I (xxx) TH:word: cloud queue: 20 words (12 from the local book, 19 with a meaning)
I (xxx) TH:word: memo wordbook written: 20 cards, 4821 B
I (xxx) TH:maimemo: advanced 2 of 2 studied word(s) back into the plan
I (xxx) TH:maimemo: pushed to the study plan: 1 accepted of 1 resolved (advance=0)
```

未配令牌时初始化照样成功，只是 `token=unset`（这是**刻意**的：可选组件失败不得阻止启动，
`app_main.c` 的模块表里它标了 `optional = true`）。

**验证状态（诚实说明）**：

**v1.4.0 已上机实测**（真机 + 真实令牌，2026-09-28）：

| 项 | 实测结果 |
|---|---|
| `maimemo_probe()` | `get_study_progress` ok / `get_today_items` ok（**55 词**）/ `vocabulary` ok / **`interpretations`、`notes`、`phrases` 全部 `ESP_ERR_NOT_SUPPORTED`** |
| 探测接口本身 | 工作正常，**逐条**给出结论而不是一个笼统的"失败" —— 这正是它的设计目的 |
| `get_study_progress` | 返回 `0/55`，计划 62 词 |
| 云端队列 | `cloud queue: 55 words (2 from the local book, 2 with a meaning)`，`GET /words/today` 真的返回墨墨的词（`pathway`、`undergraduate`、`taxpayer`）|

**关键结论：当前这个令牌读不到释义。**所以"本地词库没有的墨墨词显示中文释义"这个能力
**对本令牌不生效** —— 卡上只有拼写。这是**令牌权限**，不是代码问题；换权限更宽的令牌
（能读 `/interpretations`）即可解锁。**建议上机第一件事就是点「探测令牌能力」看这一行。**

`/study/advance_study`（方案 D 的回传）**仍未在"可见成功"的情况下实测** ——
它需要账号里有可推进的词并已开启回传开关，本轮没有构造该场景。
`add_words`（方案 C）在 v1.3.3 已实测成功（`accepted:1`）。
方案 B 的"卡片上真的显示一条助记"因作用域被拒，**尚未被观察到**。

**上机踩过的坑（写代码时看不出来的，见 `CHANGELOG.md` 1.4.0）**：
1. 事件必须用**项目自己的** `sys_event_subscribe()` 注册 ——
   `sys_event_post_to()` 发到**自定义** `s_loop`，用 ESP-IDF 默认的
   `esp_event_handler_register()` 注册**永远不触发且不报错**。
2. 别在同步时给整队词拉内容 —— 见 `components/word_service/README.md` 的
   "为什么内容要懒加载"（实测把内部 RAM 打到 160 B，Web 服务器冻死）。


## 开放平台令牌（OIDC）的接入代价 —— 想升级权限前先读

官方升级权限的路径是 [open.maimemo.com/app](https://open.maimemo.com/app) 创建应用，
但它的接入方式与本设备现在用的**完全不是一回事**：

| 项 | 个人令牌（已实现） | 开放平台应用（未实现） |
|---|---|---|
| 拿到令牌 | 用户在 App 里复制一串字符串，填进网页控制台 | OIDC 授权流程 |
| 需要的东西 | 令牌本身 | `client_id` + `client_secret`（后端应用）或纯前端 PKCE、**HTTPS 回调地址**、获批的 scope |
| issuer | — | `https://accounts.maimemo.com/oidc` |
| 令牌有效期 | 长期 | `access_token` + `id_token`，可能需要刷新 |
| 审核 | 无 | 需提交审核，主页须是**已上线可访问的 HTTPS 页面** |

**对本设备的现实影响**：

1. **需要后端**：`client_secret` 绝不能放进固件（本仓库对 AI Key / 令牌的规则 §8.8 同理），
   所以稳妥做法是**另起一个服务**去完成 OIDC 换取 access_token，设备只拿结果。
   这已经超出"板子上跑一个客户端"的范围。
2. **需要 HTTPS 回调页面**：文档要求主页与回调同域名、已上线、建议已备案。
3. **`access_token` 会过期**：现在的实现把令牌当作长期凭据存在 NVS 里；
   OIDC 的令牌需要刷新逻辑，否则设备会在某个时间点静默失败。

**结论（诚实说明）**：如果只是自己用，最省事的顺序是

1. 先在 App 里**重新生成一个个人令牌**再探测一次 —— 个人令牌的权限由墨墨控制，
   不排除新生成的令牌范围不同（成本最低，先试）；
2. 若确认个人令牌拿不到 `/interpretations`，再决定是否做 OIDC 接入（工作量大，
   且需要一个常驻后端）。

**不建议**为了这一步就去实现 OIDC：在没有后端的前提下，把 `client_secret` 放进固件
是本仓库明确禁止的模式。真要做，应当先补一个最小的授权服务，再让设备通过它拿令牌。
