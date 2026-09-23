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
| 生命周期 | `todo_manager_init()`、`todo_save()` |
| 增删改 | `todo_add()`、`todo_update()`、`todo_set_done()`、`todo_delete()`、`todo_clear_done()` |
| 查询 | `todo_get()`、`todo_list()`、`todo_list_changed_since()`、`todo_free_list()`、`todo_stats()` |
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
- 导入时若 `id == 0` 会补 `max_id + 1`；`next_id` 取 `max(文件里的 next_id, max_id + 1)`，且下界为 1。

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
   - **客户端声明整表已同步** → `POST /api/v1/sync` → `todo_mark_all_synced()`，把所有非 `OK` 的项改判为 `OK`。
   `todo_stats_t.pending` 统计的是 `sync_state != TODO_SYNC_OK`，状态栏/首页据此显示"待同步"。
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

`GET` 响应的 `data` 里额外带 `count`、`incremental`、`server_ts`。
`todo_manager_register_http()` 与 `init()` 分开，是为了让组件在没有 HTTP server 的构建里也能跑：
路由表本身只是张表，注册与否都不影响 UI。

## Kconfig

- `CONFIG_TODOHELPER_TODO_CACHE_FILE`（默认 `todos.json`）— `<app_dir>/cache` 下的缓存文件名。
- `CONFIG_TODOHELPER_TODO_BACKUP_KEEP`（默认 7，范围 0–30）— `backup/` 里保留的每日备份份数；0 关闭。
  备份文件**不得**包含 WiFi 密码、API Key、auth token（需求 §8.8）。

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
I (xxx) TH:todo_http: 7 todo routes registered
```

带缓存重启时第一行变为 `imported N items (next id M)`。

手工验证：

```powershell
curl.exe -X POST http://todohelper.local/api/v1/todos -H "Content-Type: application/json" -d '{\"title\":\"test\"}'
curl.exe http://todohelper.local/api/v1/todos
curl.exe -X PATCH http://todohelper.local/api/v1/todos/1/done -H "Content-Type: application/json" -d '{\"done\":true}'
curl.exe -X POST http://todohelper.local/api/v1/sync
```

- 新增后 `sync_state` 为 `pending`（客户端推的项会被 handler 立即改判 `ok`）；
- 在设备 UI 上勾选一项后，`GET /api/v1/todos` 应显示 `"sync_state":"pending"`；
- 执行 `/api/v1/sync` 后全部变 `ok`；
- 断电重启后清单仍在（`cache/todos.json`）。若 `storage` 未挂载，应看到
  `storage unavailable: todos will not survive a reboot`，且重启后清单为空。
