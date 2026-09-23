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
   收发超时各 5 秒。SSE 会长期占用 socket，所以并发数很小。
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
   - **每客户端一个 FreeRTOS 队列**（深度 8，元素是 `char *`），最多 **`SYNC_SSE_MAX_CLIENTS` = 4** 个，
     第 5 个客户端收到 **503** `"too many event subscribers"`；
   - 连接建立先发 `retry: 3000`；
   - 广播时给每个队列 `strdup()` 一份 payload，`xQueueSend` **零等待**：队列满就 `free()` 并**丢弃该事件**，
     **绝不阻塞发布者**（慢客户端只丢自己的事件）；
   - 空闲 15 秒（`SYNC_SSE_KEEPALIVE_MS`）发一行 `: keepalive` 注释，防止 NAT/代理掐连接；
   - 断连时 `sse_unregister()` 排空并删除队列，`sync_server_sse_clients()` 反映当前订阅数。
7. **鉴权模型（需求 §8.8）**：默认 `auth_enabled = false`（局域网内免鉴权，方便电脑端直接改待办）。
   一旦通过 `sync_server_set_auth_token()` 设过 token（NVS 键 `auth_token`，**不硬编码、不写日志**），
   **所有写方法**（POST/PUT/PATCH/DELETE）都必须带 `X-Auth-Token`。
   `sync_server_check_auth()` **只读 `X-Auth-Token` 头，绝不接受 URL query 传 token**（query 会进日志）。
   token 长度上限 63 字符；传 `NULL`/`""` 会清除并关闭鉴权。
   另有 4 条路径**无论鉴权开关如何都强制校验**（`s_always_auth`）：

   | 强制鉴权路径 | 原因 |
   |---|---|
   | `/api/v1/device/reboot` | 敏感操作 |
   | `/api/v1/device/auth` | 会改 token |
   | `/api/v1/ai/config` | 涉及 API Key |
   | `/api/v1/ai/history` | 对话记录 |

   失败统一 **401** + `{"code":401,"msg":"unauthorized","data":null}`，并记 `auth failed for <path>`
   与 `SYNC_EVENT_AUTH_FAILED`。传输是明文局域网、**不提供 HTTPS**。
8. **信息泄露红线**：`GET /api/v1/device/info` 返回型号、核数、版本、IDF 版本、MAC、IP、主机名、
   WiFi 状态、存储信息与 `auth_enabled`，**刻意不返回任何 API Key、token**（需求 §8.8 明确禁止回读）。
9. **/ws/events 是刻意的 501**。本 IDF 构建里没有 WebSocket 组件，所以它被
   `http_router_register_not_implemented()` 注册为占位：
   `hint = "WebSocket is not available in this build; use /api/v1/events (SSE) instead"`。
   这样客户端能区分"功能没做（501）"和"URL 写错（404）"（需求 §8.1 / §8.7 的降级要求）。
10. **重启走定时器**：`POST /api/v1/device/reboot` 先回 `{"rebooting":true}`，再用 700 ms 一次性
    `esp_timer` 调 `esp_restart()`——在 handler 里直接重启会把响应掐断。

## 路由表

本组件自有的路由（`s_own_routes[]`）：

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/`、`/index.html` | 否 | 内嵌浏览器页面 |
| GET | `/api/v1/device/info` | 否 | 静态设备信息（无密钥） |
| GET | `/api/v1/device/status` | 否 | 运行状态：`uptime_s`、`free_heap`、`min_free_heap`、`free_psram`、`routes`、`sse_clients`、`wifi_rssi`、`wifi_connected`、`time_synced`、`server_ts`、`url` |
| POST | `/api/v1/device/reboot` | **强制** | 700 ms 后重启 |
| POST | `/api/v1/device/auth` | **强制** | body `{"token":"..."}`；空串关闭鉴权 |
| GET | `/api/v1/events` | 否 | SSE 推送通道 |
| GET | `/ws/events` | 否 | **501 占位** |

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
I (xxx) TH:sync: SSE client attached (1/4)
I (xxx) TH:sync: SSE client detached (0 left)
I (xxx) TH:sync: HTTP authentication enabled        # 设置 token 后；token 本身不打印
I (xxx) TH:sync: auth failed for /api/v1/device/reboot
```

用 5 个并发 `curl -N /api/v1/events` 验证上限：第 5 个应立刻收到 503 `too many event subscribers`；
用 `curl.exe -X POST http://todohelper.local/api/v1/todos ...`（不带 token）验证开启鉴权后写操作返回 401。
