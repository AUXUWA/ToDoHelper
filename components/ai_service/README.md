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
| 对话 | `ai_service_chat()`（阻塞）、`ai_service_chat_async()`（UI 用，推荐）、`ai_service_chat_stream()`（逐段回调，**必须跑在独立任务里**，见下方关键约定第 3 条） |
| 结果 | `ai_service_get_last_reply()`、`ai_service_get_state()`、`ai_service_state_name()` |
| 历史 | `ai_service_history_count()`、`ai_service_history_get()`、`ai_service_clear_history()`（**只清内存**，不删磁盘存档）、`ai_service_save/load_history()` |
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
   - `ai_service_chat_stream()` — 逐段回调，**必须跑在独立任务里**（见下方第 3 条）。

3. **AI 的 HTTP 接口是异步的：对话期间设备其它 HTTP 接口照常可用**
   （v1.5.0 修掉 B7）。

   背景：`esp_http_server` 只有**唯一**的 httpd 任务，URI handler 在里面**串行**执行。
   handler 不返回，连 `accept()` 都不再跑 —— 表现为「`ping` 得通、串口正常，
   但任何 TCP 连接都建不上」。AI 一轮请求超时上限 **45 s**
   （`CONFIG_TODOHELPER_AI_TIMEOUT_MS`），搬之前这段时间整台设备的 HTTP 全废；
   而且对话存档（写 SD 卡）也是在这段时间里、在 httpd 任务里做的。

   做法（照抄 `components/sync_server/src/sync_server.c` 的
   `events_get_handler()` + `sse_client_task()` 范式）：

   | 步骤 | 位置 | 说明 |
   |---|---|---|
   | 1 | handler | 读干净 body、把 prompt **复制**到任务自己的缓冲区 |
   | 2 | handler | `httpd_req_async_handler_begin(req, &job->req)`：socket 交给自有任务，**此后 `req` 失效** |
   | 3 | handler | `xQueueSend()` 把活派给 `th_ai_http` 任务，**handler 立刻返回** |
   | 4 | 任务 | 跑 `ai_service_chat_stream()` / `ai_service_chat()`，往 socket 写 chunk 或 JSON |
   | 5 | 任务 | 流式：`httpd_resp_send_chunk(req, NULL, 0)` 结束 chunked body |
   | 6 | 任务 | `httpd_req_async_handler_complete(req)` **把会话槽还回去** |

   > ⚠️ 第 6 步**必须**做，异常/取消/早退路径也要做。不 complete 会永久占住 httpd 的
   > 会话槽，服务器最终**完全不再 accept 新连接**（IDF 头文件里明确警告过，
   > AGENT.md §9.0.1 最后一条记的就是这个坑）。本组件把「回错误」也放在任务里做，
   > 保证每条路径都走到 complete。

   对外语义不变：chunk 格式（`event: chunk` / `data: {"delta":...}`）、
   `done` 事件、`error` 事件、`/api/v1/ai/abort`、HTTP 状态码语义都照旧，
   客户端感知不到这次改动。只有一种新情况：排队排不进去时返回
   `code: 503`（流式是 `event: error` + `code: 503`）。

   > 只用**一个**工作任务（`th_ai_http`）+ 一个队列：AI 请求本来就不该并发
   > （`s_abort` 是全局标志，并发会互相踩），串行化既省内部 RAM，语义也更干净。
4. **中断是真的**：`ai_service_abort()` 置标志，`ai_service.c` 的 HTTP 事件回调检测到后返回
   非 `ESP_OK`，`esp_http_client_perform()` 立即结束，调用方拿到 `ESP_ERR_TIMEOUT`。
   （中断接口本身在 httpd 任务里跑，所以 AI 跑着的时候也能立刻响应。）
5. **流式解析在设备侧完成**：按行重组 SSE（`data: {...}` 会被 TCP 分片切断），
   取 `choices[0].delta.content`，累加后同时用于回调与历史。
6. **上下文裁剪**：保留 `ai_history_rounds`（默认 10）轮，即 `1 + 2×rounds` 条消息；
   system 提示固定留在 `messages[0]`，溢出时成对丢弃最旧的 user+assistant。
7. **离线是正常状态**：无网络时直接返回 `ESP_ERR_INVALID_STATE` 并进入 `AI_STATE_ERROR`，
   不影响其他功能（需求 FR7 的降级要求）。
8. **小眼睛联动**：状态变化时调用 `idle_face_express()` —— 聆听→`LISTENING`、
   识别/思考→`THINKING`、回答→`SPEAKING`、出错→`ERROR`，回到空闲时交还给 `ui_core` 的策略。
9. **外部输入都走带深度上限的 JSON 解析**：`sys_json_parse(text, 32)`，不用 `cJSON_Parse()`。
   HTTP body、SSE 记录、SD 卡上的存档文件都算外部输入 —— 畸形 JSON 的解析深度
   能把 6 KB 的 httpd 栈吃穿（AGENT.md §9.0.1.1 铁律一的同款坑）。

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
| POST | `/api/v1/ai/chat` | 阻塞问答，返回 `{"reply":...,"state":...}`（**在自有任务里跑**，handler 立即返回） |
| POST | `/api/v1/ai/chat/stream` | SSE 流式：`event: chunk` / `event: done` / `event: error`（同上） |
| GET | `/api/v1/ai/history` | 当前上下文（含 role/content/count/model/state） |
| DELETE | `/api/v1/ai/history` | 清空对话（**强制鉴权**）：内存 + 磁盘存档 `ai/chat_history.json` 一起清 |
| POST | `/api/v1/ai/config` | 设置 key / prompt / model / 轮数（**强制鉴权**） |
| POST | `/api/v1/ai/abort` | 中断当前请求 |

> `/api/v1/ai/config` 与 `DELETE /api/v1/ai/history` 在 `sync_server.c` 的
> `s_always_auth[]` 里：它们能改凭据或删数据，所以即使 `auth_enabled=false` 也要求 token。
>
> `DELETE /api/v1/ai/history` 以前只清内存、不删磁盘上的存档，重启后对话又回来
> （用户以为「清空」其实没清）。现在存档文件一起删。删文件要碰 SD 卡，
> 所以这一步在 AI HTTP 工作任务里做，不占着 httpd 任务。
>
> 设备端 UI 的「新对话」按钮（`ui_pages/page_ai_chat.c`）调的仍是
> `ai_service_clear_history()`，**只清内存**：删文件要碰 SD 卡，不能在 LVGL 回调里做
> （AGENT.md §9.0.1.1 铁律一）。清完之后下一次对话会把存档文件覆盖掉，
> 所以只有在「清空之后一直不聊天就重启」的情况下，旧对话才会回来。

## 内存占用：24 KB 的对话历史去哪儿了（v1.5.0 修掉 B6）

| 项目 | 以前 | 现在 |
|---|---|---|
| 对话历史 `s_messages[24]` | **常驻 .bss（内部 DRAM）24,960 B** | **按需分配 + 优先 PSRAM**，正常情况下内部 DRAM 占用 0 |
| `th_ai` worker 任务 | 开机无条件创建，**8 KB 内部 RAM 栈** | **第一次 `ai_service_chat_async()` 时才建**（用完保留） |
| `th_ai_http` AI HTTP 任务 | 不存在（AI 跑在 httpd 的 6144 B 栈上） | 第一次 AI 的 HTTP 请求时才建，8192 B 内部 RAM 栈 |

- 内部 DRAM 是本机最紧的资源（开机后只剩 ~66 KB），这 24 KB 曾经一口气吃掉 **37%**，
  而 AI 只是可选的 P2 功能。现在历史缓冲区用
  `heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)` 分配
  （可变参 API：先试 PSRAM，失败再退回默认堆），第一次真正用到对话时才分配。
- 缓冲区**一旦分配就不再释放**（进程生命周期内），所以
  `ai_service_get_last_reply()` / `ai_service_history_get()` 拿到的指针
  生命周期仍然是「到进程结束为止」，调用方不需要自己管。
- `th_ai` 改成按需创建：开机不白占 8 KB 栈，第一次从 UI 发消息时才建，
  之后保留（对话一轮一轮来，反复重建更折腾）。

## 任务栈为什么是这个尺寸

| 任务 | 栈 | 依据 |
|---|---|---|
| `th_ai`（UI 路径 worker） | 8192 B | 同上 |
| `th_ai_http`（AI 的 HTTP） | 8192 B | 同上 |

依据：**`esp_http_client` 没有自己的任务**，mbedTLS 的握手直接消耗**调用者**的栈
（IDF v6.1 的 `esp_http_client.c` 里没有 `xTaskCreate`）。所以凡是跑 DeepSeek
请求的任务，栈都要给到能吃下 TLS 握手。8192 B 是本工程跑同一条 TLS 路径
已经验证过的尺寸（AI 跑在 httpd 的 6144 B 栈上时是能用但很紧），
**不要再往下砍**。栈只能从内部 RAM 出（PSRAM 栈碰 flash 会重启，AGENT.md §9.0.1）。

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

### 验一验「AI 跑着的时候别的接口还能用」（B7 的验收点）

开着流式请求**不要关**，另开一个窗口打设备状态 —— 应当在 1 s 内返回：

```powershell
# 窗口 A：流式（会跑十几秒）
curl.exe -N -X POST http://todohelper.local/api/v1/ai/chat/stream `
  -H "Content-Type: application/json" -d "{\"text\":\"写一篇 500 字的短文\"}"

# 窗口 B：AI 还在跑的时候，其它接口必须照常秒回
curl.exe -w "\n%{time_total}s\n" http://todohelper.local/api/v1/device/status
curl.exe -w "\n%{time_total}s\n" http://todohelper.local/api/v1/ai/history
```

改造之前窗口 B 会**连不上**（TCP 超时）；现在应当在几百毫秒内返回。
窗口 A 的输出仍然是 `event: chunk` … `event: done`。

期望串口日志：

```
I (xxx) TH:ai: ready (model 'deepseek-chat', key=no, rounds 10, save_history=0)
I (xxx) TH:ai_http: AI HTTP worker started
I (xxx) TH:ai: API key stored
D (xxx) TH:ai: state -> thinking
D (xxx) TH:ai: state -> speaking
```

未配置 Key 时 `POST /api/v1/ai/chat` 返回 **400**（`no API key configured`）；
断网时返回 **503**（`offline: AI is unavailable without a network`）；
Key 错误时返回 **503** 且日志出现 `DeepSeek rejected the API key (HTTP 401)`。
