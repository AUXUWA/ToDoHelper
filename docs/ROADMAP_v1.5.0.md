# v1.5.0 实施规格（本轮敲定清单）

> 本文件是本轮（v1.5.0）的**施工依据**：主调度敲定范围与验收标准，
> 子代理按文件分区各自实现，最后由主调度统一复核、构建、提交。
>
> 基线：v1.4.0（提交 `53f7e22`）。构建命令：`bash tools/build.sh build`（必须 0 错误 0 警告）。
>
> 本轮范围来自一次六方向并行体检（69 条问题，13 条高危经独立复核全部成立），
> 主调度从中敲定**必做项**，其余记入 §7 留待下一轮。

---

## 0. 全局硬约束（所有子代理都必须遵守）

1. **LVGL 回调里禁止阻塞**：适配器在持有递归锁时调用 `lv_timer_handler()`，所有控件事件回调
   都在锁内。回调里做 SD / flash / 网络 / 阻塞等待就是 bug（AGENT.md §9.0.1.1 铁律一）。
   需要落盘或走网络时，改为**投递到工作线程**。
2. **内部 RAM 是最紧的资源**：基线 `boot complete: internal 66 KB / largest 22 KB / int-low 54 KB`
   （AGENT.md §9.0）。批量数据必须显式 `MALLOC_CAP_SPIRAM`；禁止新增大块静态数组。
   FreeRTOS 任务栈只能来自内部 RAM，**不要随手加任务**。
3. **HTTP 契约向后兼容**：已有的路由、字段、状态码语义不得改变；新增字段只能是「加」，
   旧客户端读不到新字段时必须仍能正常工作。
4. **UI 规范**：颜色/字号/间距一律取 `ui_theme.h` 的宏；触摸目标 **≥ 44×44**（`UI_TOUCH_MIN`）；
   中文文案与现有页面风格一致。
5. **代码风格**：与所在文件保持一致（中文注释，注释解释「为什么」而不是复述代码，
   沿用现有的 `/* ---- 分节 ---- */` 结构与函数头注释风格）。
6. **只改自己负责的文件**：不要动别人负责的文件；不要改 `version.txt`、`CHANGELOG.md`、
   `README.md`、`docs/API.md`（由主调度统一收口）。
7. **不新增组件**：本轮不引入新的 ESP-IDF 组件或第三方依赖；不改 `managed_components/`。
8. **诚实汇报**：做不完的写进 `not_done`，不要假装做完；不确定的写进 `risks`。

---

## 1. 必修 bug（本轮必须修完）

### B1. 待办落盘在 LVGL 回调里同步写 SD/flash（卡顿 + 违反铁律一）

- **位置**：`components/todo_manager/src/todo_manager.c` 的
  `todo_add` / `todo_update` / `todo_set_done` / `todo_delete` / `todo_clear_done`
  → `after_change()` → `todo_save()` → `storage_write_cache()`。
- **症状**：设备端点一下复选框，就在 LVGL 任务（持适配器锁）里**同步**把整份清单
  序列化成 JSON 并写进 SD 卡。清单越长越卡（256 项时 JSON 约 100 KB）。
  更糟：`todo_save()` 用 `portMAX_DELAY` 抢 `s_io_lock`，若 httpd 任务正在写同一文件，
  LVGL 任务会**持着适配器锁无限期等磁盘写完** → 整机 UI 冻住。
- **要求**：
  - 改成**去抖 + 异步落盘**：变更只置 dirty 标志并通知一个去抖保存工作线程，
    安静期（300~500 ms 无变更）后统一写一次；连续变更只写一次。
    对外接口 `todo_mark_dirty()`（见 §3.5）。
  - 工作线程用 `xTaskCreate` 创建，栈要够但别浪费（参考
    `components/word_service/src/word_service.c` 的 `word_save_worker`）。
  - **保留同步接口** `todo_save()` 给需要立即落盘的地方（导入 JSON 后、网页端改完、关机前），
    它要**绕过**去抖直接写盘。
  - 落盘失败 `SYS_LOGW` 记日志，不能静默吞掉。
  - HTTP 侧每次写操作目前会**写两遍盘**（`todo_add()` 写一次，`todo_mark_synced()` 又写一次），
    一并改成一次（见 B14）。

### B2. 落盘不是原子的：掉电窗口会把整份清单丢掉

- **位置**：`components/storage_manager/src/storage_manager.c` 的 `storage_write_file()`（约 470~480 行）。
- **现状**：`fclose(tmp)` → `remove(path)` → `rename(tmp, path)`。
  掉电落在 `remove` 与 `rename` 之间，**上一份有效清单和新的临时文件一起没了**。
  README 里宣称的「原子写…掉电不会把上一份有效清单写坏」与代码不符。
- **关键约束（已核实 FatFs 源码）**：`esp-idf/components/fatfs/src/ff.c` 的 `f_rename()`
  在**目标已存在时返回 `FR_EXIST` 而不是覆盖**——所以**不能**简单地删掉 `remove()` 改成
  覆盖式 rename（那样 SD 卡上会直接失败）。而 LittleFS 的 `lfs_rename_()` 在同一次 commit 里
  带 `LFS_TYPE_DELETE` + `LFS_TYPE_CREATE`，本来就是原子替换。
- **要求**：改成**双槽 + 启动恢复**：
  - 写入新内容到 `X.new`；
  - 把当前正式文件 `X` 重命名成 `X.bak`（这一步失败也不能丢数据——此时 `X` 还在）；
  - 把 `X.new` 重命名成 `X`；
  - **启动时**（`storage_manager_init()` 或读缓存前）做恢复：
    `X` 缺失但 `X.new` 在 → 把 `X.new` 提升为 `X`；
    `X` 存在但解析失败 → 回退到 `X.bak`（这层由调用方决定，storage 只保证文件在）。
  - 这套机制对**所有**走 `storage_write_file()` 的文件生效（词库、进度、AI 历史、待办），
    写进组件 README。
  - 目录清理：`.new` / `.bak` 不能无限增长，每个名字只留一个（覆盖式）。

### B3. 畸形 JSON 深层嵌套会爆栈（可能变成开机重启循环）

- **位置**：`components/todo_manager/src/todo_manager.c` 的 `todo_import_json()` → `cJSON_Parse()`。
- **已实测的事实**：cJSON 1.7.19 的 `CJSON_NESTING_LIMIT` 默认 **1000**，
  用 `objdump` 核实 `<parse_value>` 每层递归栈帧 **96 字节**；
  开机主任务栈 `CONFIG_ESP_MAIN_TASK_STACK_SIZE=12288` → **约 128 层 `[` 就爆栈**；
  httpd 任务栈只有 6144 → 约 60 层就爆。而 `todos.json` 是用户可以直接在卡上编辑的文件，
  一个坏文件就能让设备**每次开机都崩**。
- **要求**：
  - 在 `sys_utils` 加**带深度上限的解析入口** `sys_json_parse()`（见 §3.5），
    解析前先扫文本统计 `[`/`{` 最大嵌套深度，超限直接返回 NULL。
  - 所有解析**外部输入**的模块都改用它：`todo_manager`、`word_service`、
    `maimemo_client`、`ai_service`、`sync_server`。
  - 解析失败要 `SYS_LOGW` 记原因，调用方走「保留旧数据 / 空数据」的降级路径，
    **绝不能让设备在开机路径上崩**。

### B4. `GET /api/v1/words/history` 把栈上局部变量的地址交给异步 worker（use-after-return）

- **位置**：`components/word_service/src/word_service_http.c`（约 412~428 行）。
- **症状**：`history_args_t args = {...}` 是 httpd 任务栈上的自动变量，
  `word_run_async(req, history_get_worker, &args, ...)` 只存了地址；
  handler 返回后栈帧被复用，worker 再解引用就是**未定义行为**（接口随机返回空/几条/NO_MEM）。
- **要求**：把参数**按值复制**进 worker 自己的结构体，不要传栈地址。
  在组件 README 记一条「异步 worker 的参数必须按值传递」。

### B5. 云端独占词评分时，学习记录记的是**别的词**

- **位置**：`components/word_service/src/word_service.c` 的 `word_review()`，
  `index == (size_t)-1` 分支（约 1339~1353 行）。
- **症状**：所有云端独占卡的 id 都是同一个 `WORD_ID_CLOUD`，
  代码靠「遍历队列找第一个 id 匹配的」拿拼写 → 必然拿到**队列里第一个**云端词的拼写，
  而不是用户**刚评的那张卡**。于是 `review_log.jsonl` 里的词全是错的。
- **要求**：把「用户刚评的那个词的拼写」显式传进记录函数，不能靠遍历队列取第一个。

### B6. AI 对话历史常驻 24.4 KB 内部 DRAM

- **位置**：`components/ai_service/src/ai_service.c`（约 53 行）
  `static ai_message_t s_messages[AI_HISTORY_MAX_MESSAGES]`（24 × 1040 B = 24,960 B，
  `nm` 核实为 `.dram0.bss` 里的 `0x6180` 字节）。
- **症状**：占开机后 66 KB 内部 RAM 余量的 **37%**，而 AI 只是可选的 P2 功能。
  同一文件里 `ai_service_init()` 还**无条件**创建 8 KB 栈的 `th_ai` 任务
  （只有 UI 走 `ai_service_chat_async()` 才用得上），开机即白占 8 KB 内部 RAM。
- **要求**：
  - 历史数组改成**按需分配 + PSRAM**（`heap_caps_malloc_prefer(..., MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)`），
    或者把容量砍小并按需分配。优先 PSRAM。
  - `th_ai` 任务改成**按需创建**（第一次 `ai_service_chat_async()` 时建，用完可保留），
    参照 voice_service 的「按需打开」做法。
  - 注意 `ai_service_get_last_reply()` 等返回指针的生命周期。
  - `DELETE /api/v1/ai/history` 现在只清内存、**不删磁盘文件**，重启后对话又回来
    （`ai_service_clear_history()` 没有 `storage_delete_file`）——一并修掉。

### B7. AI 对话与天气定位是同步 handler，会冻住整台设备

- **位置**：`components/ai_service/src/ai_service_http.c` 的
  `chat_stream_post_handler()`（:106）与 `chat_post_handler()`（:168）；
  `components/weather_service/src/weather_service_http.c` 的 `locate_post_handler()`（:215）。
- **症状**：`esp_http_server` 只有唯一 httpd 任务，handler 在里面串行执行。
  AI 一轮请求超时上限 **45 s**（`CONFIG_TODOHELPER_AI_TIMEOUT_MS`），
  定位请求 **12 s**——期间设备任何 TCP 连接都建不上。
  `ai_service_chat()` 成功后还在 httpd 任务里写 SD 卡（AI 对话存档）。
  另外 `page_weather.c` 的 `city_clicked_cb()` 也直接调这个阻塞函数，把 UI 冻 12 s。
- **要求**：照 `components/sync_server/src/sync_server.c` 的
  `events_get_handler()` + `sse_client_task()` 模式改：
  `httpd_req_async_handler_begin()` → 自有任务接管 → 完成后
  `httpd_resp_send_chunk(req, NULL, 0)`（流式）或正常发响应（请求-响应式）
  → `httpd_req_async_handler_complete()`。
  **异常/取消/早退路径也必须 complete**，否则漏掉 httpd 会话槽，
  最终服务器会停止接受连接（AGENT.md §9.0.1 最后一条写过这个坑）。
  - 流式（AI chat/stream）：与现有 chunk 格式完全一致。
  - 请求-响应式（AI chat、weather locate）：任务里算好结果，再通过异步句柄发响应。
  - 设备端那个调 `weather_detect_location()` 的 LVGL 回调也要改成异步投递 + 事件回调刷新。

### B8. 「即使读操作也强制鉴权」的名单没有生效

- **位置**：`components/sync_server/src/sync_server.c`（:31~37 名单、:153~161 鉴权判断、:719~721 路由注册）
  与 `components/http_router/src/http_router.c`（:247 `need_auth` 判定）。
- **症状**：开了 HTTP 鉴权之后，**任何 GET 都不校验令牌**：
  `GET /api/v1/todos`（你的待办清单）、`GET /api/v1/ai/history`（AI 对话记录）
  都能被局域网任意客户端直接读走。网页端还反过来告诉用户
  「开启鉴权后所有请求（含 GET）都要带令牌」，与设备实际行为矛盾。
- **要求**：
  - 让 `always_auth` 名单**真正生效**：把名单里的路由用 `http_router_require_auth()`
    标记，并让 `http_router` 对 **GET 也调用鉴权钩子**（当该路由被标记为需要鉴权时）。
  - 名单至少覆盖：`/api/v1/ai/history`、`/api/v1/todos`（含 `GET /api/v1/todos/{id}`）。
  - **注意兼容性**：开启鉴权后网页上位机必须能正常用——网页端本来就带 `X-Auth-Token`，
    SSE 也带（`sseViaFetch` 的 headers），要一并确认。
  - 同步更新网页端的鉴权说明文案，让它与设备实际行为一致。

### B9. 设备端改过的待办会永远停在「待同步」

- **位置**：`components/todo_manager/src/todo_manager.c` 的
  `todo_add`/`todo_update`/`todo_set_done`（置 `TODO_SYNC_PENDING`）
  与 `todo_mark_all_synced()`（唯一能把 PENDING 变回 OK 的入口，只被 `POST /api/v1/sync` 调用）。
- **症状**：设备端勾选一次后，`pending` 计数再也回不到 0，
  除非用户去网页上手动点「标记已同步」。FR2 要求的是「断网可查看和勾选，**联网后自动同步**」。
- **要求**（**推送即回执**）：
  - 设备把变更推给客户端（SSE `todo_changed`）后，客户端回一个 ack。
  - 协议二选一（主调度指定后由网页端与内核分别实现）：
    - **采用**：新增 `POST /api/v1/todos/ack`，body 为 `{"ids":[1,2,3]}`；
      设备收到后对这几个 id 调 `todo_mark_synced()`。
  - 网页端在收到 SSE `todo_changed` 并把数据吃进本地列表后，立刻发一次 ack（合并成一批，别每条一次）。
  - 没有客户端在线时，pending 保持不变是正确的（没人收到，不该标成已同步）。
  - `TODO_SYNC_FAILED` 目前**没有任何代码路径会置位**（死状态）——本轮要么补上失败语义
    （写盘失败时置 FAILED、重试成功置 OK），要么在 README 里注明它当前不可达。

### B10. 导入 JSON 时 priority 没夹取、还会造出重复 id、还会截断半个 UTF-8 字符

- **位置**：`components/todo_manager/src/todo_manager.c` 的 `todo_import_json()`
  （priority 约 :387、id 约 :382~399）与 `json_read_string()`（:285~293）。
- **症状**：
  - priority 直接取 `(uint8_t)`，写 3 得到 3（比「紧急」的 2 排得更前），写 256 变成 0，
    写负数变成 255（最高优先级）。HTTP 路径却有夹取，两边不一致。
  - 文件里 `id:0` 的条目会被补成 `max_id+1`，可能与后面的 id 撞车；
    文件里本来就有的重复 id 也不会被检出 → 之后的更新/删除作用到错误的那一条。
  - 标题/备注按**字节** `strlcpy` 截断，42 个中文字正好 126 字节，第 43 个字只塞进 2 字节，
    末字变乱码并被原样写进 `todos.json`。
- **要求**：
  - priority 统一用一套夹取逻辑（HTTP 解析与导入共用）。
  - 导入时对 id 去重：0 或已占用的 id 分配下一个空闲 id，并打日志。
  - 做一个**UTF-8 安全截断**辅助函数：截断点落在多字节序列中间就丢掉残字节。

### B11. 设备端删除待办没有二次确认（误触即永久丢失）

- **位置**：`components/ui_pages/src/page_todo.c` 的 `row_delete_cb()`。
- **症状**：一按就永久删除，无确认、无撤销；删除按钮只有 52×34，还紧挨着可点击的标题。
- **要求**：改成**二次确认弹窗**。
  - **必须先读 `components/ui_core/src/ui_widgets.c` 里 `dialog_close_cb` 上方那段注释**：
    现有 `ui_dialog()` 的 `on_close` 回调**永远不会执行**——因为 `dialog_close_cb`
    调 `lv_msgbox_close()` 是**同步** `lv_obj_delete(mbox)`，LVGL 在删除时标记按钮已删除，
    于是同一次点击里后续注册的回调被中止。所以**不能**沿用 `ui_dialog()` 的注册顺序。
  - 在 `ui_core` 新增（声明加到 `components/ui_core/include/ui_core.h`）：
    ```c
    lv_obj_t *ui_dialog_confirm(const char *title, const char *text,
                                const char *ok_text, const char *cancel_text,
                                lv_event_cb_t on_ok, void *user_data);
    ```
    - 复用现有的 `dialog_style()` / `dialog_style_button()` / `dialog_add_close_button()`；
      「取消」次要色，「删除」用 `UI_COLOR_DANGER`。
    - 回调必须能拿到弹窗对象：把 `mbox` 作为 `user_data` 传给 `on_ok`，
      回调里先做业务再 `lv_msgbox_close(mbox)`（**不要**在同一次点击里先关再业务）。
    - 弹窗要能正常关闭、不能溢出屏幕（`ui_dialog` 已处理尺寸夹取与文本滚动，照它的方式做）。

### B12. 待办没有备份 / 导出 / 导入通道

- **位置**：`components/todo_manager/Kconfig` 的 `CONFIG_TODOHELPER_TODO_BACKUP_KEEP`（默认 7）
  —— **全工程没有任何代码读它**，是个死配置；`docs/API.md` 也写明「当前没有任何代码写备份」。
- **要求**（最小可用版）：
  - **导出**：新增 `GET /api/v1/todos/export`，返回整份清单 JSON
    （`Content-Disposition: attachment`），网页端加「导出」按钮直接下载。
  - **导入**：新增 `POST /api/v1/todos/import`，body 就是那份 JSON，
    语义与 `todo_import_json()` 一致（整体替换）。
  - **自动备份**：落盘时顺便保留最近 N 份（用 Kconfig 的 `TODO_BACKUP_KEEP`），
    存在 `<storage>/todohelper/backup/todos_*.json`，按日期滚动；
    启动时主文件坏了自动回退到最近一份可用的备份。

### B13. 待办跟着「存储根」走：插拔 SD 卡会让待办看起来凭空消失

- **位置**：`components/storage_manager/src/storage_manager.c` 的根选择
  （`storage_try_sd()` :223 / `storage_try_internal()` :237 / init :318~331）。
- **症状**：存储根二选一（SD 优先）。插卡 → 待办写到卡上；拔卡 → 设备去读片内同名文件，
  **待办全没了**（数据在卡里没丢，但用户以为丢了）。卡坏/被格式化则真的全丢。
- **要求**：让**待办清单固定存片内 LittleFS**，词库/字体仍留在 SD 卡。
  - 在有 SD 卡时**也挂载内置 LittleFS**（内部分区本来就在 `partitions.csv` 里）。
  - 升级兼容：首次启动时若 SD 侧存在旧 `cache/todos.json` 而片内没有，**自动迁移**一份过去。
  - 若评估下来「同时挂载两个文件系统」风险不可接受，退而求其次：存储根切换时在两个介质之间
    **自动搬运/合并**待办。选了哪种方案都要在 README 写清楚。
  - 落盘频率靠 B1 的去抖落盘控制住，别把片内 flash 写坏。

### B14. 每次 HTTP 写操作都会把整份清单写盘两遍

- **位置**：`components/todo_manager/src/todo_manager_http.c` 的
  `todos_post_handler()`（:102 `todo_add` + :108 `todo_mark_synced`）、
  `todos_item_put_handler()`（:174 + :182）、`todos_item_done_handler()`（:212 + :220）。
- **症状**：一次变更写两遍整文件，而 httpd 只有唯一任务 → 其它 HTTP 请求被整段堵住。
- **要求**：一次变更只落盘一次（B1 的去抖机制自然解决：两次变更合并成一次写盘；
  HTTP 侧若需要立即落盘，用一次 `todo_save()` 而不是两次）。

---

## 2. 待办功能补强（本轮核心，设备端为主）

### F1. 设备端待办行显示到期日与优先级

- **位置**：`page_todo.c` 的 `row_create()` / `row_apply()`。
- **现状**：一行只有「勾选框 + 标题 + 删除 + 同步徽标」，**看不到截止日期，也看不到优先级**，
  而数据模型里两个字段都有。一屏 800×480 上无法一眼看出「今天该做什么、哪条紧急」。
- **要求**：
  - 行内增加**到期徽标**：无到期日不显示；逾期「逾期」（`UI_COLOR_DANGER`）；
    今天「今天」；明天「明天」；更远显示「M月D日」。
    分档**一律调用** `todo_due_bucket()`（见 §3.5），不要自己再算一遍日期。
  - 增加**优先级色条**：紧急=危险色、重要=警告色、普通=不着色。纯色小方块，零位图。
  - 徽标与色条只在内容变化时更新（`row_apply()` 已有的「只写真正变化的东西」原则）。

### F2. 设备端待办筛选栏

- **位置**：`page_todo.c`（列表上方加一行筛选 chip）。
- **要求**：
  - 至少四档：**全部 / 未完成 / 今天到期 / 已完成**（chip 形态，触摸目标 ≥ 44px 高）。
  - 取数统一走 `todo_list_filtered(filter, ...)`（见 §3.5）。
  - 切换筛选只影响显示，不改数据、不改持久化。
  - 选中态用 `UI_COLOR_PRIMARY` 明确区分；统计行跟着筛选结果走
    （例如「未完成 7 / 共 12」）。

### F3. 详情页把 Unix 时间戳变成人话

- **位置**：`page_todo.c` 的 `detail_refresh()`。
- **现状**：直接打印「到期 %lld」→ 设备上看到的是「到期 1750000000」；优先级也是数字。
- **要求**：格式化成 `2026-10-21 09:00`（本地时区）；未校时（`sys_time_synced()` 为假）
  显示「未校时」而不是编一个日期；优先级显示中文（普通/重要/紧急）。
  沿用 `page_home.c` / `page_calendar.c` 的 `localtime_r` 风格，不要引入新依赖。

### F4. 首页「今日待办」卡片

- **位置**：`components/ui_pages/src/page_home.c`。
- **现状**：只有一行文字「待办 N 项 · 待同步 M 项 · 存储 SD 卡」。
- **要求**：
  - 在时钟下面加一张卡片（用 `ui_card_create()`）：今天到期且未完成的条数与进度、
    最多 3 条标题 + 「还有 N 条」、空状态「今天没有到期的待办」。
  - 条数用 `todo_count_due_today()`，列表用 `todo_list_filtered(TODO_FILTER_DUE_TODAY, ...)`。
  - 卡片可点击 → `ui_nav_push(UI_PAGE_TODO)`；触摸目标 ≥ 44px。
  - **性能红线**：首页 1 Hz tick，重画必须有脏检查（用 `todo_version()` + 跨天判断）。

### F5. 设备端列表渲染要设上限

- **位置**：`page_todo.c` 的 `render_list()` / `row_create()`。
- **现状**：每行建 5~6 个 LVGL 对象，一次性把全部条目建出来，**没有视口裁剪**。
  256 条时是 1200~1500 个控件对象，全在内部 RAM（`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`，
  小对象优先内部堆），而内部 RAM 余量只有 66 KB。
- **要求**：
  - 加渲染上限常量（例如 30 行），超出部分不建控件，列表底部显示
    「还有 N 条未显示（在电脑网页端查看全部）」。
  - 被截断的必须是**排序后的最后 N 条**（排序规则不变，最紧急的仍在最前）。
  - 顺带修 `rows_reserve()` 每次都 realloc+memcpy 的 O(n²) 拷贝：改成按需一次性预留。

### F6. 触摸目标不达标 + 统计行无条件重写

- **位置**：`page_todo.c` 的勾选框（34×34）、删除按钮（52×34）、`render_stats()`、
  `row_apply()` 的颜色写入、`detail_refresh()` 的三个标签。
- **症状**：勾选框与删除按钮都小于工程自己规定的 44 px（`UI_TOUCH_MIN`，
  需求 §10.3 明确写了「待办列表页勾选框触摸目标 ≥44×44」）；
  点在勾选框下缘会落到标题上变成「打开详情页」。统计行、颜色、详情标签**无条件重写**，
  每次事件都白白让整页重排（违反铁律二：先比较再写）。
- **要求**：控件尺寸 ≥ 44×44；所有文本/颜色写入前先比较，值没变就不动。

### F7. 页面不可见时也会整页刷新

- **位置**：`components/ui_core/src/ui_core.c` 的 `ui_event_drain_cb()`（约 157~164 行）
  与 `page_todo.c` 的 `todo_on_event()`。
- **症状**：事件分发只判断 `on_event` 是否存在，**不检查该页当前是否可见**。
  用户在主页/天气页时，网页端每改一条待办，设备就把整份清单复制一遍并逐行刷新。
- **要求**：分发前检查目标页是否可见；不可见就只记脏标记，等 `on_show` 时再刷新。
  （`ui_core.c` 的改动要注意别影响其它页面的事件语义。）

### F8. 网页上位机补齐

- **位置**：`components/sync_server/web/index.html`。
- **现状**：网页端已经有增删改查、关键字搜索、全部/未完成/已完成筛选、优先级与到期 chip、
  删除二次确认、SSE 实时推送。**不要重写**，沿用现有的请求漏斗（`api()`）、样式类
  （`.chip`/`.row`/`.btn`）与错误提示（`toast`/`banner`）。
- **要求**：
  1. **批量操作**：工具栏加「清除全部已完成」与「全部标记完成」。
     设备端没有批量路由就逐条调 `DELETE /api/v1/todos/{id}` / `PATCH .../done`，
     但要**串行**发（一次并发 256 个请求会把设备唯一的 httpd 任务打爆），
     并显示进度、失败要能看清是哪几条。批量前二次确认。
  2. **排序**：提供「按到期 / 按优先级 / 按更新时间 / 按添加顺序」，
     语义与设备端一致（未完成在前 → 优先级高在前 → 截止早在前 → id 升序）。
     只影响显示顺序，不改设备数据。
  3. **窄屏适配**：手机宽度下操作按钮不换行溢出，标题与 chip 能正常折行。
  4. **SSE 心跳看门狗**：现在读到 chunk 才推进，连接半开时页面永远认为「推送中」，
     轮询兜底也被关掉，列表会无限期停在旧数据上。加「距上次收到帧超过 N 秒就判定断开」，
     触发重连 + 恢复轮询。
  5. **错误信息**：设备把真正的原因放在 `data.detail` / `data.esp_err`，
     页面只显示 `body.msg`（英文原文 like "internal error"）→
     要把 `detail` 显示出来并译成中文。
  6. **文案纠错**：
     - 「推送客户端 N / 4」→ 设备上限是 **3**；
     - 「设备屏幕只读」是错的（设备可以勾选/删除）→ 改成正确描述；
     - 鉴权说明要与设备实际行为一致（见 B8）。
  7. **删除二次确认会被列表重绘清掉**：`armDelete()` 打的 armed 标记在 `renderList()`
     重建节点时丢失，导致「反复点删不掉」。要改成不依赖 DOM 节点上的临时状态。
  8. **请求串行队列**：所有请求串在同一条 promise 链上，
     一次 30~60 秒的墨墨校验/探测会把整页其它请求全部堵住。
     把长耗时的墨墨请求从公共队列里摘出来。
  9. **ack 回执**（配合 B9）：收到 SSE `todo_changed` 并把数据吃进本地列表后，
     立刻批量发一次 `POST /api/v1/todos/ack`。
  10. 设备端学习记录面板把失败也显示成「没有记录」→ 要区分「设备不可达」与「真的没有记录」。

---

## 3. 性能专项

### P1. 天气 HTTP 响应缓冲吃内部 RAM

- **位置**：`components/weather_service/src/weather_service.c` 的 `http_event_handler()`
  （:204~224）：`malloc(2048)` 首分配 + `realloc` 长到 16 KB。
- **症状**：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` → ≤4 KB 一律优先内部堆，
  而 `realloc` 复用同一块所属的内部堆，**不会自动搬到 PSRAM** →
  等于在内部 RAM 最紧的时候临时吃掉 16 KB。
- **要求**：改成 `heap_caps_malloc_prefer(..., MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)`
  （正确做法参考 `components/sys_utils/src/sys_json.c:45`），扩容也走 PSRAM。

### P2. 待机小眼睛的 30 Hz 定时器永不暂停

- **位置**：`components/idle_face/src/idle_face.c`（:1694 附近创建，全文无
  `lv_timer_pause` / `lv_timer_delete` / `lv_timer_resume`）。
- **症状**：第一次进过待机之后，即使回到别的页面，LVGL 任务仍被 30 次/秒唤醒，
  对**隐藏对象**做约 26 次 LVGL 属性写——整机余生都在付这份 CPU 税。
  这正是 AGENT.md §9.0.1.1 第 4 条「定时器要有主」禁止的形态
  （对比 `ui_core.c` 对页面 tick 做的 pause/resume）。
- **要求**：页面不可见 / 不在待机时暂停该定时器，回到待机时恢复。
  改之前先确认待机动画的连续性（用户已经确认过观感正常，别改坏）。

### P3. 天气每 15 分钟无条件重写 3 个 NVS 键

- **位置**：`components/weather_service/src/weather_service.c` 的 `weather_do_refresh()`（:694~699）
  与 `weather_set_location()`（:543~550）。
- **症状**：值一个字节没变也 `nvs_set_str` + `nvs_commit`（一次 flash 写）。
- **要求**：值没变就不写。

### P4. 鉴权令牌每次请求都从 NVS 重读

- **位置**：`components/sync_server/src/sync_server.c` 的 `token_matches()`（:163~177）。
- **症状**：每个需要鉴权的请求都 `sys_config_get_str()`（内部读两遍 NVS + 全局配置锁）。
- **要求**：令牌在设置/鉴权开关变化时缓存到内存变量，校验时读缓存。

### P5. 设备端学习记录面板的「空」是假的（网页端）

见 F8 第 10 条。

### P6. AI 与天气任务的栈偏小

- **位置**：`ai_service.c` 的 `AI_TASK_STACK=8192`、`weather_service.c` 的
  `WEATHER_TASK_STACK=6144`、`weather_service_http.c` 的 4096、
  `calendar_service_http.c` 的 3072、`sync_server.c` 的 httpd 栈 6144。
- **事实**：`esp_http_client` **没有自己的任务**，mbedTLS 握手直接消耗**调用者**的栈
  （在 IDF v6.1 源码里 `esp_http_client.c` 没有 `xTaskCreate`）。
- **要求**：在把阻塞请求挪到自有任务时（见 B7），给这些任务**足够跑 TLS 握手**的栈，
  并在组件 README 里记下这个依据。本轮不要求重新测量，但不要在搬完之后用更小的栈。

---

## 3.5 并行实现的接口契约（**必须逐字照此实现**，否则并行子代理会对不上）

以下 API 加在 `components/todo_manager/include/todo_manager.h`，
实现在 `components/todo_manager/src/todo_manager.c`。

```c
/** 待办的截止时间分档。设备端用它决定徽标文案与颜色。 */
typedef enum {
    TODO_DUE_NONE = 0,   /**< 没有截止时间 */
    TODO_DUE_OVERDUE,    /**< 已逾期 */
    TODO_DUE_TODAY,      /**< 今天到期（本地时区 00:00~23:59:59） */
    TODO_DUE_TOMORROW,   /**< 明天到期 */
    TODO_DUE_LATER,      /**< 更晚 */
} todo_due_bucket_t;

/** 列表筛选口径。 */
typedef enum {
    TODO_FILTER_ALL = 0,   /**< 全部 */
    TODO_FILTER_OPEN,      /**< 只看未完成 */
    TODO_FILTER_DUE_TODAY, /**< 只看今天到期且未完成 */
    TODO_FILTER_DONE,      /**< 只看已完成 */
} todo_filter_t;

/** 按本地时区给 due_ts 分档。设备未校时（sys_time_synced() 为假）时，
 *  除 TODO_DUE_NONE 外一律返回 TODO_DUE_NONE —— 不要编造日期。 */
todo_due_bucket_t todo_due_bucket(int64_t due_ts);

/** 按筛选口径取快照，排序与 todo_list() 完全一致
 *  （未完成在前 → 优先级高在前 → 截止早在前 → id 升序）。 */
esp_err_t todo_list_filtered(todo_filter_t filter, todo_item_t **items, size_t *count);

/** 清单版本号。任何内容或结构变化（增删改、状态变化）都会自增。
 *  页面拿它做脏检查：版本没变就不用整表重建。 */
uint32_t todo_version(void);

/** 标记清单已变更，交给后台线程去抖落盘。
 *  LVGL 回调里必须调这个而不是直接写盘（铁律一）。
 *  HTTP 侧若需要立刻落盘，调 todo_save()（同步）。 */
void todo_mark_dirty(void);

/** 今天到期且未完成的条数（首页卡片用）。未校时时返回 0。 */
size_t todo_count_due_today(void);
```

另一个跨模块契约（`components/sys_utils/include/sys_utils.h`，实现在 `sys_json.c`）：

```c
/**
 * @brief 解析 JSON，但限制嵌套深度。
 *
 * cJSON 1.7.19 的 CJSON_NESTING_LIMIT 默认 1000，每层递归约 96 字节栈帧
 * （objdump 实测），最坏要 ~96 KB 栈；而开机主任务的栈只有 12 KB，
 * 一个畸形文件就能把它打爆（AGENT.md §9.0.5 那一类「开机就崩」的故障）。
 *
 * @param json      待解析文本
 * @param max_depth 允许的最大嵌套深度（待办这种两三层结构给 32 足够）
 * @return 成功返回 cJSON 根节点（调用方负责 cJSON_Delete），失败返回 NULL
 */
cJSON *sys_json_parse(const char *json, int max_depth);
```

补充约定：

- `todo_list_filtered()` 返回的数组由调用方用 `todo_free_list()` 释放；
  `todo_free_list()` 必须改用 `heap_caps_free()`（因为分配改成 PSRAM 偏好分配）。
- 快照数组一律用 `heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)`，
  禁止裸 `malloc()` 申请大块。
- `todo_due_bucket()` 与 `todo_list_filtered(TODO_FILTER_DUE_TODAY)` 必须用**同一套**
  「今天」的边界（本地时区当天 00:00:00 ~ 23:59:59），不许两处各算一套。
- **不要改 cJSON 组件本身**（`managed_components/espressif__cjson` 是外部依赖）；
  深度检查在解析前自己扫文本完成。
- 所有解析**外部输入**的模块都改用 `sys_json_parse()`：
  `todo_manager`、`word_service`、`maimemo_client`、`ai_service`、`sync_server`。
- 存储侧的「固定写片内」接口由存储分区提供：
  ```c
  esp_err_t storage_write_cache_pinned(const char *name, const void *buf, size_t len);
  esp_err_t storage_read_cache_pinned(const char *name, char **out, size_t *len);
  ```
  （「pinned」= 固定片内 LittleFS，不随存储根切换。）
  若实现方认为需要改名，必须在 `not_done` 里写清新名字，主调度统一收口。

---

## 4. 本轮不做（明确排除，避免子代理越界）

- 不引入 MQTT / 云同步 / 账号体系。
- 不做待办的子任务、标签、重复提醒（需要数据模型升版与迁移，留到下一轮）。
- 不做「AI 解析『帮我添加待办』」的指令联动（FR7 里写过，从未实现；见 §7）。
- 不做真机烧录与目视验收（需要人）；本轮只保证**构建通过 + 代码自洽**。
- 不动 `components/esp_cam_sensor`、不改字体与 `tools/gen_cjk_font.py`。
- 不重写网页上位机的整体结构（只做增量改动）。

---

## 5. 实施分区（按文件切开，子代理并行时互不踩脚）

| # | 分区 | 负责的文件 | 覆盖条目 |
|---|---|---|---|
| 1 | 待办内核 | `components/todo_manager/**` | B1、B9(内核侧)、B10、B12(内核侧)、B14、§3.5 待办契约 |
| 2 | 设备待办页 | `components/ui_pages/src/page_todo.c`、`components/ui_core/src/ui_widgets.c`、`components/ui_core/include/ui_core.h` | B11、F1、F2、F3、F5、F6 |
| 3 | 首页今日卡片 | `components/ui_pages/src/page_home.c` | F4 |
| 4 | 网页上位机 | `components/sync_server/web/index.html` | B9(网页侧)、F8 |
| 5 | AI 服务 | `components/ai_service/src/ai_service.c`、`components/ai_service/src/ai_service_http.c` | B6、B7(AI 部分) |
| 6 | 存储加固 | `components/storage_manager/src/storage_manager.c`、`components/storage_manager/include/storage_manager.h` | B2、B13 |
| 7 | JSON 加固 | `components/sys_utils/src/sys_json.c`、`components/sys_utils/include/sys_utils.h` | B3(基础设施) |
| 8 | 单词与墨墨 | `components/word_service/src/word_service.c`、`components/word_service/src/word_service_http.c`、`components/maimemo_client/src/maimemo_client.c`、`components/maimemo_client/src/maimemo_http.c` | B4、B5、B3(调用点)、F8 之外的相关修复 |
| 9 | 天气服务 | `components/weather_service/src/weather_service.c`、`components/weather_service/src/weather_service_http.c` | B7(天气部分)、P1、P3 |
| 10 | 鉴权与 SSE | `components/sync_server/src/sync_server.c` | B8、P4 |
| 11 | 待机与事件分发 | `components/idle_face/src/idle_face.c`、`components/ui_core/src/ui_core.c` | P2、F7 |

冲突点与约定：

- 分区 6 提供 `storage_write_cache_pinned()` / `storage_read_cache_pinned()`，
  分区 1 调用它们（接口名见 §3.5）。
- 分区 7 提供 `sys_json_parse()`，分区 1/5/8/10 各自在自己的文件里改调用点。
- 分区 2 与分区 11 都碰 `ui_core`：`ui_core.h` 与 `ui_widgets.c` 归分区 2，
  `ui_core.c` 归分区 11。分区 11 改 `ui_core.c` 时**不要**改 `ui_core.h`
  （新函数声明由分区 2 加）。
- 分区 4 与分区 10 都碰 sync_server：网页归分区 4，`sync_server.c` 归分区 10。
- 分区 1 与分区 2 共享 §3.5 的待办接口签名，**谁先实现谁说了算**，后来者照着用。

---

## 6. 验收基线（主调度负责）

1. `bash tools/build.sh build` → **0 error 0 warning**。
2. HTTP 路由总数不减少；新增路由（`/api/v1/todos/export`、`/api/v1/todos/import`、
   `/api/v1/todos/ack`）要在 `docs/API.md` 登记（主调度统一做）。
3. 每个分区都要在 `components/<comp>/README.md` 留下必要的契约说明
   （新增 API、新的行为约定、新的坑）。
4. 内部 RAM 基线不得劣化（`ai_service` 的 24 KB 静态数组是本轮要还回去的）。

### 6.1 本轮的实际验证结果（2026-10-20）

| 项 | 结果 |
|---|---|
| 构建 | ✅ 0 error 0 warning，`todohelper.bin` 0x2f8f20，app 分区剩 63% |
| 烧录 | ✅ 已烧到 COM34（`tools/flash.ps1`，32 s） |
| 开机 | ✅ 无 abort / panic / 断言；SD 与片内 LittleFS **同时挂载**；`TH:app: ready (storage=/sdcard)` |
| 待办数据 | ✅ 从「片内 2 条（陈旧）+ 卡上 4 条（完整）」正确收敛到 **4 条（next id 48）** |
| 内部存储分区 | ✅ 从 1324 KB 降到 32 KB（不再重复搬运） |
| 上机新发现 | ⚠️ 三个只有真机才暴露的洞，已修（见 `CHANGELOG.md`「上机后补的三处」） |
| UI 目视 | ✅ **用户已确认「没问题」** |
| HTTP 端到端 | ✅ **已实测**（设备联网，`192.168.137.95`）：export / import / ack 三条新路由、61 条路由、鉴权开→读接口 401 / 正确令牌 200、调试 shell 可用；测试后设备已恢复原状（鉴权关闭、待办 4 条未动） |
| 内部 RAM 实测 | ⚠️ 运行期 `int-low` 46 KB / 最大连续块 15 KB（§8 基线是 54/22 KB）；**开机后**读数是 internal 87 KB / largest 42 KB / int-low 71 KB（比基线好）。运行期的碎片化值得再盯一眼——WiFi PHY 重新使能需要约 30 KB 连续块 |
| 帧率 | ❌ 未测（需要设备端 FPS 探针 + 目视） |

**下一位接手的人，请优先补这一项**：运行期的**内部 RAM 碎片化**（`int-low` 46 KB、
最大连续块 15 KB）。开机后是 87/42/71 KB（比基线好），但跑一段时间后连续块掉到 15 KB，
而 WiFi PHY 重新使能需要约 30 KB 连续块（AGENT.md §9.0.3）——需要找出碎片是谁造成的。
另外「设备端快速添加」与「待办数据模型升版」见 §7。

---

## 7. 下一轮候选（本轮明确不做，但已核实存在的问题）

> 来源：同一次体检里被判为 medium/low 且本轮未安排的条目。留档以免下轮重复挖。

1. **设备端「快速添加」**：本板没有 IME，设备端完全无法新增待办。
   可考虑「常用预设 + 短文本键盘」的折中方案。
2. **待办数据模型升版**：`todo_item_t` 加标签 / 子任务 / 重复规则 / 排序字段，
   需要迁移策略，改动成本高，越晚做代价越大。
3. **`TODO_SYNC_FAILED` 是死状态**：写盘失败、推送失败都无处表达。
4. **`updated_at` 在 NTP 之前是「开机秒数」**：这类条目在时钟同步后永远不会被增量同步带出来。
5. **`s_sse_clients` 计数在 httpd 任务与 SSE 任务之间无锁**：丢一次更新后，
   设备会丢弃所有推送事件而网页仍显示「推送中」。
6. **`ui_dialog()` 的 `on_close` 回调永远不会执行**（`lv_msgbox_close` 同步删对象，
   LVGL 中止了同一次点击的后续回调）——本轮新加的 `ui_dialog_confirm()` 绕开了它，
   但 `ui_dialog()` 本身的这个坑还在，值得顺手修掉。
7. **启动日志缓冲写满后不再更新**（`page_splash.c` 的 5313 字节缓冲只追加不回收）。
8. **AI 对话存档**：写盘在 httpd 任务里（本轮随 B7 一起搬走，但存档开关本身的语义没理清）。
9. **天气 HTTP 任务的栈**与 TLS 握手需求，缺一次真机实测。
10. **AI 指令联动 todo_manager**（FR7 明确要求，从未实现）。
11. **`page_word.c` 的 enrich worker 直接改写 LVGL 正在渲染的 `s_word`**：
    无锁数据竞争，且会把上一张卡的释义写到当前卡上。
12. **`word_service` 的 `review_log.jsonl` 追加失败会丢行**；断电撕裂的半行会与下一次追加粘连。
13. **`maimemo_client` 的 enrich 缓存命中条件写错**：有助记但释义为空且未标记「无权」时
    永远不命中缓存，每次翻卡重跑 4 次 HTTPS 往返并整文件重写 `enrich.json`。
14. **`maimemo_http.c` 的 advance 接口每次成功调用泄漏一份 job 与其中的拼写字符串**。
15. **云端模式下卡片的 level / next_review 是快照**：评分后卡片与「今日队列 N/M」仍显示旧值。
