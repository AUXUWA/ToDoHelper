# ai_service

**L5 业务组件（FR7 / P2）** — DeepSeek 多轮对话，含流式输出、历史管理、中断与"小眼睛"联动。

- 头文件：`include/ai_service.h`
- 依赖：`sys_utils`、`storage_manager`、`http_router`、`idle_face`、`cjson`、`esp_http_client`、`esp-tls`、`mbedtls`
- 被谁依赖：`ui_pages`（AI 对话页）、`main`

## API 摘要

| 分组 | 函数 |
|---|---|
| 生命周期 | `ai_service_init()` |
| 配置 | `ai_service_set_api_key()`、`ai_service_has_api_key()`、`ai_service_set_system_prompt()`、`ai_service_get/set_model()`、`ai_service_get/set_history_rounds()` |
| 对话 | `ai_service_chat()`（阻塞）、`ai_service_chat_async()`（UI 用，推荐）、`ai_service_chat_stream()`（逐段回调，**必须跑在独立任务里**，见下方关键约定第 2 条） |
| 结果 | `ai_service_get_last_reply()`、`ai_service_get_state()`、`ai_service_state_name()` |
| 历史 | `ai_service_history_count()`、`ai_service_history_get()`、`ai_service_clear_history()`、`ai_service_save/load_history()` |
| 控制 | `ai_service_abort()` |
| HTTP | `ai_service_register_http()` |

## 事件（基址 `AI_EVENT`）

| 事件 | payload | 说明 |
|---|---|---|
| `AI_EVENT_STATE_CHANGED` | `ai_state_t` | 状态变化，UI 据此刷新 |
| `AI_EVENT_REPLY_CHUNK` | `const char *` | 文本片段，**需立即拷贝** |
| `AI_EVENT_REPLY_DONE` | — | 完整回复见 `ai_service_get_last_reply()` |
| `AI_EVENT_ERROR` | `esp_err_t` | 失败原因 |

## 关键约定

1. **API Key 只进不出**：存 NVS `ai_api_key`，**不硬编码**、**不写日志**、
   **任何接口都不回读**（需求 §8.8）。`POST /api/v1/ai/config` 只返回 `has_api_key: true/false`。
2. **三级执行路径**，避免任何一条阻塞 UI：
   - `ai_service_chat_async()` — UI 页面用，入队后立刻返回，结果走事件（worker 任务执行）；
   - `ai_service_chat()` — 阻塞，供 worker 或 HTTP 任务调用；
   - `ai_service_chat_stream()` — 逐段回调。

   > ⚠️ **`/api/v1/ai/chat` 与 `/api/v1/ai/chat/stream` 目前是"已知同类隐患"（1.0.0 未修，
   > 用户明确决定先不动）**：它们在 **httpd 任务里**跑完整轮 AI 回复，而
   > `esp_http_server` 的 handler 是**串行**执行的 —— 一次 AI 回复（十几秒）期间整台设备的
   > HTTP 都会不可用（`ping` 通、TCP 全超时）。网页上位机不使用这两个接口，所以不影响
   > 浏览器页面。修法与 SSE 相同：`httpd_req_async_handler_begin()` 把 socket 交给独立任务，
   > 由那个任务调用 `ai_service_chat_stream()`，结束再 `httpd_req_async_handler_complete()`。
   > 参考 `components/sync_server/src/sync_server.c` 的 `events_get_handler()` + `sse_client_task()`，
   > 背景见 `AGENT.md §9.0.1` 第 4 条与 `docs/HANDOVER.md §7` 第 3 条。
3. **中断是真的**：`ai_service_abort()` 置标志，`ai_service.c` 的 HTTP 事件回调检测到后返回
   非 `ESP_OK`，`esp_http_client_perform()` 立即结束，调用方拿到 `ESP_ERR_TIMEOUT`。
4. **流式解析在设备侧完成**：按行重组 SSE（`data: {...}` 会被 TCP 分片切断），
   取 `choices[0].delta.content`，累加后同时用于回调与历史。
5. **上下文裁剪**：保留 `ai_history_rounds`（默认 10）轮，即 `1 + 2×rounds` 条消息；
   system 提示固定留在 `messages[0]`，溢出时成对丢弃最旧的 user+assistant。
6. **离线是正常状态**：无网络时直接返回 `ESP_ERR_INVALID_STATE` 并进入 `AI_STATE_ERROR`，
   不影响其他功能（需求 FR7 的降级要求）。
7. **小眼睛联动**：状态变化时调用 `idle_face_express()` —— 聆听→`LISTENING`、
   识别/思考→`THINKING`、回答→`SPEAKING`、出错→`ERROR`，回到空闲时交还给 `ui_core` 的策略。

## NVS 键

| 键 | 类型 | 说明 |
|---|---|---|
| `ai_api_key` | str | DeepSeek Key（**不回读**） |
| `ai_model` | str | `deepseek-chat` / `deepseek-reasoner` |
| `ai_prompt` | str | system prompt（角色设定） |
| `ai_rounds` | i32 | 保留轮数，默认 10 |
| `ai_save_hist` | i32 | 是否把对话存到 `ai/chat_history.json` |

## HTTP 接口（需求 §8.5）

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/v1/ai/chat` | 阻塞问答，返回 `{"reply":...,"state":...}` |
| POST | `/api/v1/ai/chat/stream` | SSE 流式：`event: chunk` / `event: done` / `event: error` |
| GET | `/api/v1/ai/history` | 当前上下文（含 role/content/count/model/state） |
| DELETE | `/api/v1/ai/history` | 清空对话（**强制鉴权**） |
| POST | `/api/v1/ai/config` | 设置 key / prompt / model / 轮数（**强制鉴权**） |
| POST | `/api/v1/ai/abort` | 中断当前请求 |

> `/api/v1/ai/config` 与 `DELETE /api/v1/ai/history` 在 `sync_server.c` 的
> `s_always_auth[]` 里：它们能改凭据或删数据，所以即使 `auth_enabled=false` 也要求 token。

## 测试方法

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build; idf.py -p COM34 flash monitor
```

设备联网后（状态栏 WiFi 显示 dBm），在电脑上执行：

```powershell
# 1. 配置 Key 与模型
curl.exe -X POST http://todohelper.local/api/v1/ai/config `
  -H "Content-Type: application/json" `
  -d "{\"api_key\":\"sk-xxxxxxxx\",\"model\":\"deepseek-chat\",\"system_prompt\":\"你是一个简洁的桌面助手\"}"

# 2. 单次问答
curl.exe -X POST http://todohelper.local/api/v1/ai/chat `
  -H "Content-Type: application/json" -d "{\"text\":\"用一句话介绍你自己\"}"

# 3. 流式（逐段打印）
curl.exe -N -X POST http://todohelper.local/api/v1/ai/chat/stream `
  -H "Content-Type: application/json" -d "{\"text\":\"数到三\"}"

# 4. 查看/清空上下文
curl.exe http://todohelper.local/api/v1/ai/history
curl.exe -X DELETE http://todohelper.local/api/v1/ai/history
```

期望串口日志：

```
I (xxx) TH:ai: ready (model 'deepseek-chat', key=no, rounds 10, save_history=0)
I (xxx) TH:ai: API key stored
D (xxx) TH:ai: state -> thinking
D (xxx) TH:ai: state -> speaking
```

未配置 Key 时 `POST /api/v1/ai/chat` 返回 **400**（`no API key configured`）；
断网时返回 **503**（`offline: AI is unavailable without a network`）；
Key 错误时返回 **503** 且日志出现 `DeepSeek rejected the API key (HTTP 401)`。
