# http_router

**L1 HTTP 路由表** — 让业务组件自己发布 `/api/v1/...` 路由，`sync_server` 不必认识它们；同时独占全局响应信封。

- 头文件：`include/http_router.h`
- 依赖：`sys_utils`、IDF `esp_http_server`（只用 URI 注册与 `esp_err_t`，**不**建 server）
- 被谁依赖：`todo_manager`、`calendar_service`、`weather_service`、`word_service`、`sync_server`

## 为什么注册表在 L1 而不是 `sync_server`

需求 §7.12 要求"业务组件自己发布路由，避免 `sync_server` 直接依赖所有组件"。若把注册表放进 `sync_server`（L4），每个 L3 服务都得**向上**依赖 L4；放到 L1 后依赖方向对所有人都是向下的：

```text
todo_manager (L3) --> http_router (L1) <-- sync_server (L4)
```

`sync_server` 只负责 `httpd_start()` 之后调用一次 `http_router_attach()`，把 L1 的 catch-all 挂上去。

## API 摘要

| 分组 | 函数 |
|---|---|
| 注册 | `http_router_register()`、`http_router_register_not_implemented()`、`http_router_require_auth()` |
| 挂载 | `http_router_attach()`、`http_router_count()` |
| 鉴权 | `http_router_set_auth_check()`、`http_router_check_auth()` |
| 响应 | `http_reply_json()`、`http_reply_ok()`、`http_reply_err()`、`http_reply_bad_request()`、`http_reply_not_found()`、`http_reply_method_not_allowed()`、`http_reply_unauthorized()`、`http_reply_unavailable()`、`http_reply_not_implemented()`、`http_reply_esp_error()` |
| 请求 | `http_read_body()`、`http_get_path()`、`http_query_get()`、`http_query_get_i64()`、`http_path_segment()`、`http_path_segment_u32()` |

## 关键约定

1. **只有一个 handler**。`http_router_attach()` 在 server 上注册 `/*` + `HTTP_ANY`；分发逻辑全在 `router_dispatch()` 里。
   重复 attach 遇到 `ESP_ERR_HTTPD_HANDLER_EXISTS` 返回 `ESP_OK`，所以 `sync_server` 重启不需要额外判断。

   > ⚠️ **handler 里不能"长期不返回"**：`esp_http_server` 的 URI handler 在**唯一** httpd 任务里
   > **串行**执行，所以一个 SSE / 流式 handler 会把整台服务器冻住 —— 实测表现是 `ping` 得通、
   > 串口正常，但**任何 TCP 连接都建不上**（浏览器"设备不可达"，`GET /` 也超时）。
   > 长连接必须用 `httpd_req_async_handler_begin()` 把 socket 交给自己的任务，handler 立即返回，
   > 结束时 `httpd_req_async_handler_complete()`（不 complete 会永久泄漏会话槽，最终不再 accept）。
   > 参考实现：`components/sync_server/src/sync_server.c` 的 `events_get_handler()` + `sse_client_task()`；
   > 完整背景见 `AGENT.md §9.0.1` 第 4 条。
2. **路由表是定长静态数组**：`HTTP_ROUTER_MAX_ROUTES`（48）项，路径长度上限 `ROUTER_PATH_MAX`（64）。
   同一 `path + method` 重复注册 = **替换** handler（`init()` 幂等）；表满返回 `ESP_ERR_NO_MEM` 并 `ESP_LOGE`。
3. **通配符只有一个 `*`**，语义是"前缀 + 任意字符串 + 后缀"：
   - `*` 必须至少吞掉一个字符，所以 `/api/v1/todos/*` **不会**命中 `/api/v1/todos`；
   - 精确匹配得分为 `1000 + 路径长度`，通配符得分为 `前缀长 + 后缀长`，**分高者胜**（最具体者胜）；
   - 因此更"具体"的 pattern 胜出：路径 `/api/v1/todos/12/done` 下，
     `/api/v1/todos/*/done` 得分 14 + 5 = 19，`/api/v1/todos/*` 得分 14 + 0 = 14，前者胜。
4. **404 / 405 / 501 契约**（需求 §8.1）：先按"路径命中且方法匹配"找最佳项；一个都没命中但路径命中过 → **405**；
   路径完全没命中 → **404**；命中但 handler 为空或带 hint（`http_router_register_not_implemented()`）→ **501**，
   并且 `data` 里带 `{"detail":"not implemented","hint":"..."}`。
   客户端据此区分"功能没做"与"URL 写错"。
5. **鉴权由 L1 统一执行，策略来自 `sync_server`**。`http_router_set_auth_check()` 安装 hook（`sync_server` 持有 token 与 §8.8 策略）；
   写方法（POST / PUT / PATCH / DELETE）**无条件**过 hook，`http_router_require_auth()` 在此基础上让**读方法**也必须过
   （`sync_server` 用它保护 `/api/v1/device/reboot`、`/api/v1/device/auth`、`/api/v1/ai/config`、`/api/v1/ai/history`）。
   hook 缺失时 fail-open（`http_router_check_auth()` 返回 `true`），只影响不装 server 的测试。
   失败统一 `http_reply_unauthorized()` = 401 + `{"code":401,"msg":"unauthorized","data":null}`。
6. **`OPTIONS` 不进路由表**：直接返回 `204 No Content` 并带上
   `Access-Control-Allow-Origin: *`、`Allow-Methods: GET,POST,PUT,PATCH,DELETE,OPTIONS`、
   `Allow-Headers: Content-Type,X-Auth-Token`。所有 JSON 响应同样带 CORS 头 + `Cache-Control: no-store`。
7. **响应信封**：`{"code":<http status 或 0>,"msg":"...","data":{...}}`；`200/201` → `code = 0`，其余 `code = HTTP 状态码`。
   `msg` 会被 `json_escape()` 处理，`data` 是**预序列化** JSON（传 `NULL` 表示 `null`）。
   除 `OPTIONS` 外不要手写 `httpd_resp_send()`——绕过 `http_reply_*` 就破坏了全局契约。
8. **请求助手边界**：`http_read_body()` 只接受 `content_len <= 16 KB` 的 body，返回 `malloc` 的 NUL 结尾缓冲（调用方 `free()`）；
   `http_get_path()` 自动剥掉 `?query`；`http_path_segment()` 的段号**从 0 开始**
   （`/api/v1/todos/12/done` → index 0=`api`、2=`todos`、3=`12`、4=`done`）。

## 测试方法

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build
idf.py -p COM34 flash monitor
```

另开一个 PowerShell（PC 与设备同一局域网）：

```powershell
curl.exe -i http://todohelper.local/api/v1/todos              # 200，code=0
curl.exe -i -X DELETE http://todohelper.local/api/v1/todos    # 405 method not allowed
curl.exe -i http://todohelper.local/api/v1/nope               # 404 not found
curl.exe -i -X OPTIONS http://todohelper.local/api/v1/todos   # 204 + CORS 头
curl.exe -i http://todohelper.local/ws/events                 # 501 + data.hint
```

期望串口输出（路由注册发生在各 `init()` 里，`sync_server` 最后打印总数）：

```
I (xxx) TH:todo_http: 7 todo routes registered
I (xxx) TH:sync: listening on port 80, N route(s), auth=off, url=http://todohelper.local
```

`N` 是全部组件注册后的总数，应等于 `http_router_count()`，可在 `GET /api/v1/device/status` 的 `routes` 字段核对。
