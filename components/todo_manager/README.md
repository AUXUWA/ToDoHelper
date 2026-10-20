# todo_manager

**L3 业务组件（FR2）** — 待办清单的唯一权威存储：本地持久化 + 设备/电脑双向同步状态。

- 头文件：`include/todo_manager.h`（私有接口 `src/todo_internal.h`）
- 依赖：`sys_utils`、`storage_manager`、`http_router`、`cjson`（ESP-IDF v6.1 起 cJSON 来自组件仓库：`espressif/cjson ^1.7.19`）
- 被谁依赖：`ui_pages`（`page_todo.c` 同时实现列表页与详情页）、`main`

设备是权威副本（header 注释）：清单落在 `<storage_root>/todohelper/cache/todos.json`，
断电不丢、离线可用，并通过 HTTP 提供给电脑端（`sync_server` 只做宿主）。

## API 摘要

| 分组 | 函数 |
|---|---|
| 生命周期 | `todo_manager_init()`、`todo_save()`、`todo_mark_dirty()` |
| 增删改 | `todo_add()`、`todo_update()`、`todo_set_done()`、`todo_delete()`、`todo_clear_done()` |
| 查询 | `todo_get()`、`todo_list()`、`todo_list_changed_since()`、`todo_list_filtered()`、`todo_free_list()`、`todo_stats()`、`todo_version()` |
| 到期分档 | `todo_due_bucket()`、`todo_count_due_today()` |
| 同步状态 | `todo_mark_synced()`、`todo_mark_all_synced()`、`todo_set_sync_state()`、`todo_sync_state_name()`、`todo_sync_state_from_name()` |
| JSON | `todo_export_json()`、`todo_import_json()` |
| HTTP | `todo_manager_register_http()`（由 `todo_manager_init()` 自动调用） |

## `cache/todos.json` 结构

由 `todo_export_json()` 生成，`cJSON_PrintUnformatted()` 输出（无空白），键顺序如下：

```json
{
  "items": [
    {
      "id": 1,
      "title": "买牛奶",
      "desc": "",
      "due_ts": 0,
      "done": false,
      "priority": 0,
      "updated_at": 1730000000,
      "sync_state": "pending"
    }
  ],
  "version": 1,
  "next_id": 2
}
```

- `version` 目前恒为 `TODO_JSON_VERSION`（1）；`todo_import_json()` **忽略未知字段**，格式可以向后扩展而不破坏旧客户端。
- `sync_state` 是字符串，取值 `none` / `pending` / `ok` / `failed`（`todo_sync_state_name()`）。
- `due_ts`、`updated_at` 为 Unix 秒；`due_ts == 0` 表示无截止时间。
- 文件名可配：`CONFIG_TODOHELPER_TODO_CACHE_FILE`（默认 `todos.json`），实际路径由 `storage_write_cache()` 拼成 `cache/<名字>`。
- 导入时若 `id` 为 0、为负、为越界值或**与前面出现过的 id 重复**，会分配下一个空闲 id 并告警；
  `next_id` 取 `max(文件里的 next_id, max_id + 1)`，且下界为 1。

## 落盘语义（v1.5.0 起）

- **普通变更走去抖落盘**：增删改勾选这类操作只置 dirty 标记并唤醒一个后台保存线程，
  安静期（几百毫秒）内没有新变更时统一写一次盘。连续操作只写一次。
  这条是为了不违反 AGENT.md §9.0.1.1 铁律一：**LVGL 回调里不能同步写 SD/flash**——
  以前点一下复选框就会在持锁的 UI 任务里把整份清单序列化并写进卡里。
- **LVGL 回调用 `todo_mark_dirty()`**（异步落盘），**HTTP 侧需要立刻落盘时用 `todo_save()`**
  （同步、绕过去抖直接写盘）。导入 JSON 之后必须立即落盘，不能等去抖窗口。
- **一次变更只写一次盘**：HTTP handler 以前会写两遍（`todo_add()` 一次、
  `todo_mark_synced()` 又一次），现在靠去抖合并成一次；需要立即落盘的场合用**一次** `todo_save()`。
- 落盘失败只 `SYS_LOGW` 告警，不静默吞掉，也不把 UI 卡住。

## 只读接口（v1.5.0 新增，供设备端页面用）

| 接口 | 说明 |
|---|---|
| `todo_due_bucket(int64_t due_ts)` | 按**本地时区**给截止时间分档：`TODO_DUE_NONE` / `OVERDUE` / `TODAY` / `TOMORROW` / `LATER`；设备没校时（`sys_time_synced()` 为假）时除 `NONE` 外一律 `NONE`，不编日期 |
| `todo_list_filtered(todo_filter_t, todo_item_t **, size_t *)` | 按 `ALL` / `OPEN` / `DUE_TODAY` / `DONE` 取快照，排序与 `todo_list()` 一致；数组由调用方 `todo_free_list()` 释放 |
| `todo_count_due_today(void)` | 今天到期且未完成的条数（首页卡片用），未校时返回 0 |
| `todo_version(void)` | 清单版本号，任何内容/结构变化都自增；页面拿它做脏检查，版本没变就不重建整表 |

`todo_due_bucket()` 与 `todo_list_filtered(TODO_FILTER_DUE_TODAY)` 必须用**同一套**「今天」的边界
（本地时区当天 00:00:00 ~ 23:59:59），两处不许各算一套。

## 关键约定

1. **原子写**。落盘走 `storage_write_cache()` → `storage_write_file()`：先写 `<path>.tmp` 再 `rename()`，
   掉电不会把上一份有效清单写坏；`s_io_lock` 串行化两次写入，避免两个 writer 交错。
   写缓存失败只 `SYS_LOGW`，不影响主流程（`storage_manager` 的约定）。
2. **离线 = 内存模式**。`storage_available()` 为假时 `todo_save()` 直接返回 `ESP_OK` 并
   打印 `storage unavailable: todos will not survive a reboot`；UI 照常工作（FR2 离线路径）。
   缓存文件读坏（解析失败）时告警并从空清单启动，不崩溃。
3. **id / updated_at / sync_state 由组件管理**。`todo_add()` 忽略调用方传的这三个字段；
   `todo_update()` 只替换 title/desc/due_ts/done/priority。
4. **同步状态模型**（header 与 docs/API.md）：
   - **设备侧修改**（`todo_add` / `todo_update` / `todo_set_done`）→ `TODO_SYNC_PENDING`；
   - **客户端推来的修改** → HTTP handler 在调用 `todo_*` 之后立刻 `todo_mark_synced(id)`
     （见 `todos_post_handler` / `todos_item_put_handler` / `todos_item_done_handler`），因为客户端本来就有这个版本；
   - **设备推给客户端后客户端回执** → `POST /api/v1/todos/ack` → 对点名的 id 调 `todo_mark_synced(id)`，
     见上文「与网页端的约定」。这是「设备端改动也能自动回到 pending=0」的关键路径；
   - **客户端声明整表已同步** → `POST /api/v1/sync` → `todo_mark_all_synced()`，把所有非 `OK` 的项改判为 `OK`。
   `todo_stats_t.pending` 统计的是 `sync_state != TODO_SYNC_OK`，状态栏/首页据此显示"待同步"。
   `TODO_SYNC_FAILED` 目前**没有任何代码路径会置位**（死状态），看到它只可能是旧文件里带过来的。
5. **排序**由 `compare_items()` 固定为四段：
   `done` 升序（未完成在前）→ `priority` 降序（0 普通 / 1 重要 / 2 紧急）→ `due_ts` 升序（`0` 排在同级最后）→ `id` 升序。
   `todo_list()` 返回的是**快照**（`malloc` 的副本），调用方必须 `todo_free_list()`。
6. **容量与文本上限**：内存中最多 `TODO_MAX_ITEMS`（256）条，超出时 `todo_add()` 返回 `ESP_ERR_NO_MEM` 并告警；
   `TODO_TITLE_MAX` 128、`TODO_DESC_MAX` 256，超长由 `strlcpy` 截断。导入超过 256 条会截断并告警。
7. **时间戳**：`now_stamp()` 在时钟已同步时用 `sys_time_ts()`，否则用 `sys_time_ms() / 1000`（开机秒数）。
   后者远小于真实 Unix 时间，所以 NTP 之前客户端做 `?updated_since=` 增量同步会**收到全量**——这是安全方向。
8. **两把锁**：`s_lock`（静态互斥量）保护 item 数组，`s_io_lock` 串行化文件写。
   `publish_status()` 必须在**不持锁**时调用（它内部再调 `todo_stats()`）。
9. **事件与通知**：每次变更发 `TODO_EVENT_<具体事件>`（payload 为 `uint32_t id`）+ 无 payload 的
   `TODO_EVENT_LIST_CHANGED`，并调 `sys_notify("todo_changed")`；
   `sync_server` 把 L0 通知转成 SSE 推送，`todo_manager` 不需要知道它存在。
   同时用 `sys_status_update()` 刷新 `todo_total` / `todo_pending` 供状态栏读取。

## HTTP 路由表（需求 §8.2）

全部注册在 L1 `http_router` 上，响应信封由 `http_reply_*` 统一产生：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/todos` | 全量；带 `?updated_since=<unix秒>` 时只回 `updated_at > ts` 的项（`incremental:true`） |
| POST | `/api/v1/todos` | body `{title,desc,due_ts,priority}`，`title` 必填；成功 **201** + `code:0` |
| GET | `/api/v1/todos/{id}` | 单项，不存在 → 404 |
| PUT | `/api/v1/todos/{id}` | 全量更新，`title` 必填 |
| PATCH | `/api/v1/todos/{id}/done` | body `{"done":true}`（必须是布尔） |
| DELETE | `/api/v1/todos/{id}` | 删除，成功返回 `{"id":N,"deleted":true}` |
| POST | `/api/v1/sync` | 客户端声明整表已同步 → `todo_mark_all_synced()` |
| GET | `/api/v1/todos/export` | **导出**整份清单（见下） |
| POST | `/api/v1/todos/import` | **导入**，整体替换清单（见下） |
| POST | `/api/v1/todos/ack` | **回执**，把点名的 id 改判为已同步（见下） |

`GET` 响应的 `data` 里额外带 `count`、`incremental`、`server_ts`。
`todo_manager_register_http()` 与 `init()` 分开，是为了让组件在没有 HTTP server 的构建里也能跑：
路由表本身只是张表，注册与否都不影响 UI。

### `GET /api/v1/todos/export` — 导出

- **默认返回清单本体**（`{"items":[...],"version":1,"next_id":N}`，即 `todo_export_json()` 的输出），
  并带 `Content-Disposition: attachment; filename="todos.json"`，浏览器存下来的文件可以
  **原样** POST 回 `/api/v1/todos/import`，导出→导入是闭环的。
  这是本组件里**唯一不走统一信封**的路由：信封是给「程序读」的，导出是给「人存文件」的。
- 想拿标准信封（`{"code":0,"msg":"ok","data":{...}}`）就加 `?envelope=1`，
  此时 `data` 就是那份清单本体，`Content-Disposition` 仍然保留。

```powershell
curl.exe -o todos.json http://todohelper.local/api/v1/todos/export
```

### `POST /api/v1/todos/import` — 导入

- body 就是上面导出的那份 JSON；语义与 `todo_import_json()` 一致：**整体替换**，不是合并。
- 非法 JSON / 缺 `items` 数组 → **400**（`msg` 为 `invalid json` 或 `field 'items' must be an array`），
  设备不会崩、也不会动现有清单（先解析校验，再交给内核）。
- 也接受被信封包住的那份（`{"code":..,"data":{"items":[...]}}`），方便网页端直接把
  `?envelope=1` 的结果再传回来。
- 成功返回 `{"imported":N,"total":N,"pending":M}`。
- **body 上限 16 KB**：`http_read_body()` 的硬上限，超过就回 400
  （`missing or oversized body`）。清单很长时（比如 80 条以上、标题里带中文）
  导出的文件会超过这个尺寸，这时导入会失败 —— 这是 HTTP 层的通用限制，
  本轮没有改 `http_read_body()`（要改得动 `http_router`，不在本组件范围内）。

```powershell
curl.exe -X POST http://todohelper.local/api/v1/todos/import -H "Content-Type: application/json" --data-binary "@todos.json"
```

### `POST /api/v1/todos/ack` — 推送回执

- body：`{"ids":[1,2,3]}`；设备对这几个 id 调 `todo_mark_synced(id)`，
  把它们的 `sync_state` 从 `pending` 改判为 `ok`。
- 返回 `{"acked":[1,2,3],"missing":[],"count":3}`：
  `acked` 是**确实改判成功**的 id；`missing` 是设备上已经找不到的 id
  （客户端确认了一条已被删除的待办，不算错误，但要让网页端看得到对不上账）。
- 非法 JSON → 400；`ids` 不是数组 → 400；一次最多处理 256 个 id（超出部分丢弃并告警）。
- **没人在线时 pending 保持不变是正确的**：没人收到过的东西本来就不该标成已同步。

```powershell
curl.exe -X POST http://todohelper.local/api/v1/todos/ack -H "Content-Type: application/json" -d '{\"ids\":[1,2,3]}'
```

### 与网页端的约定（v1.5.0 起）

1. 设备端改动一条待办 → `sync_state = pending` → 设备通过 SSE `todo_changed` 推给网页端。
2. 网页端**把数据吃进本地列表之后**，立刻合并成一批发一次 `POST /api/v1/todos/ack`
   （不要每条一次，一次变更批次一次就够）。
3. 设备收到 ack → 只把被点名的那些改判为 `ok` → `todo_pending` 计数回落。
4. `POST /api/v1/sync`（整表声明已同步）仍然保留，作为「客户端做完全量同步」的粗粒度入口。


## Kconfig

- `CONFIG_TODOHELPER_TODO_CACHE_FILE`（默认 `todos.json`）— `<app_dir>/cache` 下的缓存文件名。
- `CONFIG_TODOHELPER_TODO_BACKUP_KEEP`（默认 7，范围 0–30）— `backup/` 里保留的每日备份份数；0 关闭。
  备份文件**不得**包含 WiFi 密码、API Key、auth token（需求 §8.8）。
  用户能自己拿到手的备份通道是上面的 `GET /api/v1/todos/export`（导出的文件自己存）。

## 测试方法

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build
idf.py -p COM34 flash monitor
```

期望串口输出（首次上电）：

```
I (xxx) TH:todo: no todo cache yet, starting empty
I (xxx) TH:todo: ready with 0 item(s)
I (xxx) TH:todo_http: 10 todo routes registered
```

带缓存重启时第一行变为 `imported N items (next id M)`。

手工验证：

```powershell
curl.exe -X POST http://todohelper.local/api/v1/todos -H "Content-Type: application/json" -d '{\"title\":\"test\"}'
curl.exe http://todohelper.local/api/v1/todos
curl.exe -X PATCH http://todohelper.local/api/v1/todos/1/done -H "Content-Type: application/json" -d '{\"done\":true}'
curl.exe -X POST http://todohelper.local/api/v1/sync
# 新增：导出 → 导入 → 回执
curl.exe -o todos.json http://todohelper.local/api/v1/todos/export
curl.exe -X POST http://todohelper.local/api/v1/todos/import -H "Content-Type: application/json" --data-binary "@todos.json"
curl.exe -X POST http://todohelper.local/api/v1/todos/ack -H "Content-Type: application/json" -d '{\"ids\":[1,2]}'
```

- 新增后 `sync_state` 为 `pending`（客户端推的项会被 handler 立即改判 `ok`）；
- 在设备 UI 上勾选一项后，`GET /api/v1/todos` 应显示 `"sync_state":"pending"`；
- 执行 `/api/v1/sync` 后全部变 `ok`；
- `POST /api/v1/todos/ack` 只把**被点名**的 id 改判为 `ok`，其余保持 `pending`；
- `GET /api/v1/todos/export` 的响应体就是清单本体（不是信封），带 `Content-Disposition: attachment`；
  `POST /api/v1/todos/import` 传非法 JSON 应回 **400** 且**不改动**现有清单；
- 断电重启后清单仍在（`cache/todos.json`）。若 `storage` 未挂载，应看到
  `storage unavailable: todos will not survive a reboot`，且重启后清单为空。

### 路由表余量

`http_router` 的路由表是**定长静态数组**（`HTTP_ROUTER_MAX_ROUTES = 64`），
满了之后**后面注册的路由会被静默丢掉**（注册失败只会打一条 `SYS_LOGE`）。
本组件占 10 条（7 条旧 + export / import / ack），全工程加起来约 60 条，余量不多：
以后再加待办路由前，先确认 `http_router_count()` 还有空间。
