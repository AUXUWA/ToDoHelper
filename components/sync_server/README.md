# sync_server

**L4 局域网 HTTP 服务器** — 只做宿主：起 httpd、挂 L1 catch-all 路由、发 mDNS、吐浏览器页面、把 L0 通知转成 SSE。**不含任何业务逻辑。**

- 头文件：`include/sync_server.h`
- 依赖：`sys_utils`、`http_router`、`cjson`、`wifi_manager`、`storage_manager`、IDF `esp_app_format` / `esp_timer` / `esp_hw_support` / `esp_system` / `heap`；私有依赖 `mdns`
- 组件资源：`EMBED_FILES "web/index.html"`（浏览器页面直接烧进固件，不依赖 SD 卡）
- 被谁依赖：`ui_pages`（`page_settings.c` 显示/设置 URL 与 token）、`main`

## 为什么这里没有业务代码

需求 §7.12 与框架 §5.6：业务组件的 `/api/v1/...` 路由由**它们自己**注册到 L1 `http_router`，
所以"新增一个 API"永远不需要改这个组件。`sync_server_register_route()` 只是
`http_router_register()` 的一层薄封装；`sync_server_register_not_implemented()` 同理。
`sync_server` 自己只拥有设备级端点（`/`、`/api/v1/device/*`、`/api/v1/events`）。

## API 摘要

| 分组 | 函数 |
|---|---|
| 生命周期 | `sync_server_init()`、`sync_server_start()`、`sync_server_stop()`、`sync_server_is_running()`、`sync_server_handle()`、`sync_server_url()` |
| 路由 | `sync_server_register_route()`、`sync_server_register_not_implemented()` |
| 鉴权 | `sync_server_set_auth_token()`、`sync_server_auth_required()`、`sync_server_check_auth()` |
| 推送 | `sync_server_broadcast_event()`、`sync_server_sse_clients()` |

## 关键约定

1. **启动顺序**：`sync_server_start()` 先把 `sync_server_check_auth` 装进 router，再注册自己的路由与 501 占位，
   **然后**才 `httpd_start()`，最后 `http_router_attach(s_server)` 把 L1 的 `/*` catch-all 挂上去。
   `http_router_count() == 0` 时直接返回 `ESP_ERR_INVALID_STATE`（说明没人注册路由，配错了）。
   `init()` **幂等**，且把失败当作可恢复：`SYS_LOGW` 之后设备其余部分照常运行（需求 §15）。
2. **httpd 配置偏保守**：`uri_match_fn = httpd_uri_match_wildcard`、`lru_purge_enable = true`、
   `max_uri_handlers = 4`（catch-all 只占一个）、`max_open_sockets = 5`、`stack_size = 6144`、
   收发超时各 5 秒。SSE 会长期占用 socket，所以并发数很小（见第 6 条）。
   `lru_purge_enable` 在 socket 用满时会关掉**最久未用**的会话，但**异步会话（SSE）不会被回收**
   （`esp_httpd_priv.h`: `for_async_req` 的 socket 不参与 LRU purge），所以 SSE 不会被新连接踢掉。
3. **mDNS**：默认发布主机名 `todohelper`（`SYNC_SERVER_DEFAULT_HOSTNAME`）+ 服务 `_http._tcp`，
   于是浏览器可用 **`http://todohelper.local`**。`sys_server_url()` 在 mDNS 打开时返回
   `http://<hostname>.local`，否则退回 `http://<本机 IP>`（来自 `wifi_manager_get_ip()`）。
   主机名/端口/mDNS 开关分别来自 NVS 键 `mdns_enabled`（默认 true）、`http_port`（默认 80）。
4. **浏览器页面**：`GET /` 与 `GET /index.html` 直接吐 embedded 的 `web/index.html`
   （`text/html; charset=utf-8`，`Cache-Control: no-cache`）。页面通过
   `EventSource("/api/v1/events")` 订阅推送，写操作带上 `X-Auth-Token`。
5. **L0 通知中继**：启动时 `sys_notify_subscribe(notify_handler)`，任何组件 `sys_notify(name, payload)`
   都会被转成 SSE 事件（`sync_server_broadcast_event()`），这就是 `sync_server` 不需要依赖各业务组件的原因。
   没有 SSE 客户端时直接跳过格式化。
6. **SSE 设计（需求 §8.7）**：`/api/v1/events` 是 GET，返回 `text/event-stream`。
   - **必须走异步 handler**：`events_get_handler()` 调 `httpd_req_async_handler_begin()` 把 socket
     交给一个**每客户端一个的任务**（`sse_client_task`，3 KB 内部栈），然后**立即返回**。
     原因：`esp_http_server` 的 URI handler 在**唯一的 httpd 任务里串行执行**，一个不返回的
     handler 会把整台服务器拦死（实测：挂 1 个 `curl -N /api/v1/events` 后 `GET /` 与所有 API
     全部超时，客户端断开立刻恢复）。任务结束时必须 `httpd_req_async_handler_complete()`，
     否则会话槽永久泄漏、httpd 最终完全不再 accept（AGENT.md §9.0.1 最后一条）。
   - **每客户端一个 FreeRTOS 队列**（深度 8，元素是 `char *`），最多 **`SYNC_SSE_MAX_CLIENTS` = 3** 个
     （每个客户端 = 1 个 httpd 会话槽 + 1 个任务，服务器总共只有 5 个会话槽，所以至少给页面和
     REST 调用留 2 个），第 4 个客户端收到 **503** `"too many event subscribers"`；
   - 连接建立先发 `retry: 3000`；
   - 广播时给每个队列 `strdup()` 一份 payload，`xQueueSend` **零等待**：队列满就 `free()` 并**丢弃该事件**，
     **绝不阻塞发布者**（慢客户端只丢自己的事件）；
   - 空闲 15 秒（`SYNC_SSE_KEEPALIVE_MS`）发一行 `: keepalive` 注释，防止 NAT/代理掐连接；
     这也是**发现"客户端悄悄消失"的最坏延迟**（浏览器被关、WiFi 掉线时，只有发送失败才会察觉）；
   - 断连时 `sse_unregister()` 排空并删除队列，`sync_server_sse_clients()` 反映当前订阅数。
7. **鉴权模型（需求 §8.8）**：默认 `auth_enabled = false`（局域网内免鉴权，方便电脑端直接改待办）。
   一旦通过 `sync_server_set_auth_token()` 设过 token（NVS 键 `auth_token`，**不硬编码、不写日志**），
   **写方法与被标记的读方法**都必须带 `X-Auth-Token`。
   `sync_server_check_auth()` **只读 `X-Auth-Token` 头，绝不接受 URL query 传 token**（query 会进日志）。
   token 长度上限 63 字符；传 `NULL`/`""` 会清除并关闭鉴权。
   另有 5 条路径**无论鉴权开关如何都强制校验**（`s_strict_auth_paths`）：

   | 强制鉴权路径 | 原因 |
   |---|---|
   | `/api/v1/device/reboot` | 敏感操作 |
   | `/api/v1/device/auth` | 会改 token |
   | `/api/v1/ai/config` | 涉及 API Key |
   | `/api/v1/ai/history` | 对话记录 |
   | `/api/v1/debug/exec` | 调试 shell 能重启/改 UI（仅调试固件有这条路由） |

   失败统一 **401** + `{"code":401,"msg":"unauthorized","data":null}`，并记 `auth failed for <path>`
   与 `SYNC_EVENT_AUTH_FAILED`。传输是明文局域网、**不提供 HTTPS**。

   ### 鉴权矩阵（v1.5.0 起）

   | 路由 | 鉴权关闭（没设 token） | 鉴权开启（设了 token） |
   |---|---|---|
   | `GET /`、`/index.html` | 放行 | 放行 |
   | `GET /api/v1/device/info`、`/api/v1/device/status` | 放行 | 放行（不含任何密钥） |
   | `GET /api/v1/todos`、`GET /api/v1/todos/{id}` | 放行 | **要 token** |
   | `GET /api/v1/ai/history` | **401**（强制路径） | **要 token** |
   | `GET /api/v1/events`（SSE） | 放行 | **要 token** |
   | 所有 POST/PUT/PATCH/DELETE | 放行 | **要 token** |
   | `POST /api/v1/device/reboot`、`/api/v1/device/auth`、`/api/v1/ai/config`、`DELETE /api/v1/ai/history`、`/api/v1/debug/exec` | **401**（强制路径） | **要 token** |

   **网页端兼容**：网页上位机本来就带 `X-Auth-Token`（`api()` 的公共漏斗 + SSE 的 `fetch` 流），
   所以开启鉴权后照常工作；SSE 走 `EventSource`（浏览器不支持 fetch 流式时）带不了请求头，
   此时会拿到 401，页面自动降级为轮询并提示"需要令牌"。
8. **信息泄露红线**：`GET /api/v1/device/info` 返回型号、核数、版本、IDF 版本、MAC、IP、主机名、
   WiFi 状态、存储信息与 `auth_enabled`，**刻意不返回任何 API Key、token**（需求 §8.8 明确禁止回读）。
9. **/ws/events 是刻意的 501**。本 IDF 构建里没有 WebSocket 组件，所以它被
   `http_router_register_not_implemented()` 注册为占位：
   `hint = "WebSocket is not available in this build; use /api/v1/events (SSE) instead"`。
   这样客户端能区分"功能没做（501）"和"URL 写错（404）"（需求 §8.1 / §8.7 的降级要求）。
10. **重启走定时器**：`POST /api/v1/device/reboot` 先回 `{"rebooting":true}`，再用 700 ms 一次性
    `esp_timer` 调 `esp_restart()`——在 handler 里直接重启会把响应掐断。
11. **读方法怎么才走鉴权钩子（v1.5.0 修）**：`http_router` 的判定是
    `need_auth = route->auth_required || is_write_method(method)` ——
    **路由被 `http_router_require_auth()` 标记之后，GET 也会调鉴权钩子**；
    没被标记的 GET 仍然放行。所以"读操作也强制鉴权"完全取决于**标记**，
    而 `http_router_require_auth()` 按 (path, method) 精确查找，**必须在路由注册之后**调用。
   本组件因此维护一张"受保护读路由"表（`s_protected_routes[]`）：
   `GET /api/v1/todos`、`GET /api/v1/todos/*`、`GET /api/v1/ai/history`；
   `GET /api/v1/events` 是本组件自己的路由，在 `s_own_routes[]` 里直接标记。
   标记时机：路由归属方在各自 `init()` 里注册（`todo_manager` 在 `sync_server` 之前，
   `ai_service` 在之后），所以**标记做两次**——服务器启动时一次，
   所有模块 init 完毕（`SYS_EVENT_READY`）再一次。漏掉这一步的后果是
   `GET /api/v1/ai/history` 在启动后的几秒内不校验令牌。
12. **令牌缓存（v1.5.0 修，P4）**：令牌缓存在 `s_auth_token[]`（80 B 静态），
   只在**启动**和**令牌/鉴权开关变化**时刷新（`auth_cache_refresh()` /
   `sync_server_set_auth_token()`），校验时只读缓存。
   之前每个需要鉴权的请求都要 `sys_config_get_str()`：内部两次 `nvs_get_str()` 外加全局配置锁
   （`xSemaphoreTake(portMAX_DELAY)`），而 UI 任务写配置时也拿这把锁，
   于是 httpd 任务可能被一次 flash 写堵住。
   - 令牌是敏感值：**只写不读、不记日志**（与 AI Key 同规则），`device/info` 也不回读。
   - 缓存未命中（客户端令牌不对）时**才**回读一次 NVS：调试 shell 是绕过本组件直接写 NVS 的，
     没有这次回读，新令牌会被旧缓存挡在门外。
   - NVS 里的令牌比缓存长（只可能被人绕过本组件写坏）时**失败关闭**：
     鉴权算作"已开启"，谁都进不来，而不是把设备敞开。
13. **SSE 订阅表有锁（v1.5.0 修）**：`sse_register()` 在 httpd 任务里跑，
    `sse_unregister()` 在 SSE 任务里跑，两者原本无锁地读改写同一张表：
    丢一次更新，计数就和真实连接数对不上，而 `notify_handler()` 用"订阅数为 0"当判据，
    一旦漂移成 0，**所有推送事件会被直接丢掉**（网页却还显示"推送中"）。
    现在 `s_sse_lock`（互斥量）保护整张表，订阅数直接由表算出来（不再有独立的计数器），
    广播的整个扇出也在锁内完成——**队列可能在 SSE 任务里被删掉，向已删除的队列发送就是崩溃**。
    临界区只有几次指针赋值，队列的创建/删除放在锁外。
    另外 `sync_server_stop()` 现在只把队列**摘下来**而不删：SSE 任务正等在自己的队列上，
    删掉它属于未定义行为；任务自己会在发现连接断了以后删掉它。

## 路由表

本组件自有的路由（`s_own_routes[]`）：

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/`、`/index.html` | 否 | 内嵌浏览器页面 |
| GET | `/api/v1/device/info` | 否 | 静态设备信息（无密钥） |
| GET | `/api/v1/device/status` | 否 | 运行状态：`uptime_s`、`free_heap`、`min_free_heap`、`free_psram`、`routes`、`sse_clients`、`wifi_rssi`、`wifi_connected`、`time_synced`、`server_ts`、`url` |
| POST | `/api/v1/device/reboot` | **强制** | 700 ms 后重启 |
| POST | `/api/v1/device/auth` | **强制** | body `{"token":"..."}`；空串关闭鉴权 |
| GET | `/api/v1/events` | 开了鉴权就要 | SSE 推送通道（同样带 `X-Auth-Token`） |
| GET | `/ws/events` | 否 | **501 占位** |

别的组件的路由（由本组件打上"读方法也要鉴权"的标记）：

| 方法 | 路径 | 鉴权 | 归属 |
|---|---|---|---|
| GET | `/api/v1/todos` | 开了鉴权就要 | `todo_manager` |
| GET | `/api/v1/todos/{id}` | 开了鉴权就要 | `todo_manager` |
| GET | `/api/v1/ai/history` | **强制** | `ai_service` |

## 事件

`SYNC_EVENT`：`STARTED`、`STOPPED`、`AUTH_FAILED`（payload `const char *path`）、
`BROADCAST`（payload 为事件名）；`CLIENT_CONNECTED` 已在枚举里声明但当前实现未发布
（连接数请读 `GET /api/v1/device/status` 的 `sse_clients`）。

## 测试方法

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build
idf.py -p COM34 flash monitor
```

期望串口输出（WiFi 已连上后）：

```
I (xxx) TH:sync: mDNS published as todohelper.local
I (xxx) TH:sync: listening on port 80, N route(s), auth=off, url=http://todohelper.local
```

浏览器打开 `http://todohelper.local/`：应看到待办页面，且添加/勾选后设备端与网页端一致。

命令验证：

```powershell
curl.exe http://todohelper.local/api/v1/device/status
curl.exe http://todohelper.local/api/v1/device/info          # 不应出现任何 key/token
curl.exe -i http://todohelper.local/ws/events                # 501 + data.hint
curl.exe -N http://todohelper.local/api/v1/events            # 先收到 retry: 3000，之后空闲时每 15 s 一行 ": keepalive"
```

SSE 与鉴权：

```
I (xxx) TH:sync: SSE client attached (1/3)
I (xxx) TH:sync: SSE client detached (0 left)
I (xxx) TH:sync: HTTP authentication enabled        # 设置 token 后；token 本身不打印
I (xxx) TH:sync: auth failed for /api/v1/device/reboot
```

**回归测试（这一条最重要，别省）**：用 3 个并发 `curl -N /api/v1/events` 挂着，然后

```powershell
curl.exe -s -o NUL -m 8 -w "%{http_code}" http://todohelper.local/           # 必须 200，不能超时
curl.exe -s -o NUL -m 8 -w "%{http_code}" http://todohelper.local/api/v1/todos
curl.exe -m 8 http://todohelper.local/api/v1/device/status                   # sse_clients 应为 3
curl.exe -s -o NUL -m 8 -w "%{http_code}" http://todohelper.local/api/v1/events  # 第 4 个 → 503
```

再把 3 个客户端杀掉，约 15 秒后 `sse_clients` 应回到 0。**若挂上流以后普通请求超时，
说明 handler 又变回同步实现（整个服务器被冻住了）**。
用 `curl.exe -X POST http://todohelper.local/api/v1/todos ...`（不带 token）验证开启鉴权后写操作返回 401。

### 鉴权回归（v1.5.0 新增，别省）

在设备上"开启 HTTP 鉴权"拿到 token 之后：

```powershell
$env:T = "<token>"

# 没带 token：受保护的读接口必须是 401（v1.5.0 之前这里是 200，漏洞）
curl.exe -s -o NUL -w "%{http_code}`n" http://todohelper.local/api/v1/todos              # 401
curl.exe -s -o NUL -w "%{http_code}`n" http://todohelper.local/api/v1/todos/1            # 401
curl.exe -s -o NUL -w "%{http_code}`n" http://todohelper.local/api/v1/ai/history         # 401
curl.exe -s -o NUL -w "%{http_code}`n" http://todohelper.local/api/v1/events             # 401

# 带上 token：必须 200
curl.exe -s -o NUL -w "%{http_code}`n" -H "X-Auth-Token: $env:T" http://todohelper.local/api/v1/todos   # 200
curl.exe -s -o NUL -w "%{http_code}`n" -H "X-Auth-Token: $env:T" http://todohelper.local/api/v1/events  # 200（并收到 retry: 3000）

# 不受保护的接口始终放行
curl.exe -s -o NUL -w "%{http_code}`n" http://todohelper.local/                            # 200
curl.exe -s -o NUL -w "%{http_code}`n" http://todohelper.local/api/v1/device/info         # 200
```

关掉鉴权（设备上"关闭 HTTP 鉴权"）之后，`GET /api/v1/todos` 必须重新变成 **200**：
默认的"局域网免鉴权"是需求 §8.8 明确要求的默认状态。
`GET /api/v1/ai/history` 与 `POST /api/v1/device/reboot` 仍然是 **401**（强制路径）。

