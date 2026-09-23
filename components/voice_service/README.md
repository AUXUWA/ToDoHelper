# voice_service

**L5 组件（FR7 / P2）** — 麦克风采集、回放，以及 ASR/TTS 的**可配置 HTTP 桥接**。

- 头文件：`include/voice_service.h`
- 依赖：`sys_utils`、`storage_manager`、`bsp`（音频门面）、`http_router`、`cjson`、`esp_http_client`、`esp-tls`、`mbedtls`、`esp_codec_dev`
- 被谁依赖：`main`

## 为什么是"半成品"——以及这不是偷懒

按 `AGENT.md` / 需求 §4 的实测：I2S + `esp_codec_dev` **已经实现**，所以**录音与回放是真的**。
但需求 §17.2 有两个**尚未决定**的选型：

| 未定项 | 影响 | 本组件的处理 |
|---|---|---|
| 麦克风型号、是否带喇叭/功放（§17.2 #2） | 决定离线 ASR 是否可行、TTS 能否播报 | 采集/回放能力在运行时探测，不可用就返回 `ESP_ERR_NOT_SUPPORTED`，由调用方降级 |
| ASR / TTS 选型（§17.2 #3） | 决定离线还是云端 | **不猜**：暴露两个可配置端点 + 明确的报文协议，选型确定后只填 URL |

另外本地组件镜像里**没有** `esp-sr` / `esp-tts`，所以离线方案在当前环境不可行。

## API 摘要

| 分组 | 函数 |
|---|---|
| 生命周期 | `voice_service_init()`（惰性打开音频，失败不阻塞启动） |
| 能力查询 | `voice_service_microphone_available()`、`voice_service_speaker_available()` |
| 录音 | `voice_service_start_record()`、`voice_service_stop_record()`、`voice_service_is_recording()` |
| 回放 | `voice_service_play(path)`、`voice_service_stop_play()` |
| 唤醒 | `voice_service_set/get_wake_mode()`、`voice_service_wake_mode_name()` |
| ASR/TTS | `voice_service_set_asr_endpoint()`、`voice_service_set_tts_endpoint()`、`voice_service_asr_configured()`、`voice_service_tts_configured()`、`voice_service_transcribe_last()`、`voice_service_speak()` |
| HTTP | `voice_service_register_http()` |

## 事件（基址 `VOICE_EVENT`）

| 事件 | payload | 说明 |
|---|---|---|
| `VOICE_EVENT_WAKE` | — | 开始采集时立即发（按键/触摸唤醒路径） |
| `VOICE_EVENT_RECORD_DONE` | `voice_recording_t` | 采集结束，带样本指针/时长/落盘路径 |
| `VOICE_EVENT_ASR_DONE` | `const char *` | 识别文本；失败或未配置时为 `""` |
| `VOICE_EVENT_PLAY_DONE` | — | 回放结束 |

## 采样格式与重采样（关键实现细节）

BSP 的 I2S 通道固定为 **48 kHz / 16 bit / 2ch**，而 ASR 普遍要 **16 kHz 单声道**。
所以本组件**不改 I2S 时钟**，而是在软件里换算：

```text
采集：codec 读 48k/16/2ch  →  左右声道求平均得单声道  →  3 点平均抽取  →  16k/16/1ch
回放：16k/16/1ch 文件      →  每个样本重复 3 次并写两遍  →  48k/16/2ch  →  codec 写
```

- 抽取用 3 点均值而不是直接丢弃，起一点抗混叠作用；回放用最近邻插值，语音可懂度足够。
- 采集缓冲在 **PSRAM**：10 s × 16 kHz × 2 B = **320 KB**，内部 RAM 放不下。
- 单次录音硬上限 `VOICE_MAX_RECORD_MS`（默认 10 s），防止麦克风卡住时耗尽内存。

## ASR / TTS 报文协议（选定服务后照此对接）

```text
ASR:  POST <asr_url>   Content-Type: application/octet-stream
     body = 原始 16 kHz / 16-bit 小端 / 单声道 PCM（无 WAV 头）
     200  → {"text":"识别结果"}

TTS:  POST <tts_url>   Content-Type: application/json
     body = {"text":"要念的内容","sample_rate":16000}
     200  → audio/pcm 原始 16 kHz / 16-bit 小端 / 单声道 PCM
```

端点存 NVS（`asr_url` / `tts_url`），可经 `POST /api/v1/voice/config` 配置，**不硬编码**。

## 降级路径（需求 FR7 强制要求，全部实现）

| 缺失项 | 行为 |
|---|---|
| 无麦克风 | `voice_service_start_record()` → `ESP_ERR_NOT_SUPPORTED`；调用方改用屏幕键盘 / HTTP 文本输入 |
| 无喇叭 | `voice_service_play()` / `voice_service_speak()` → `ESP_ERR_NOT_SUPPORTED`；回复只上屏 |
| 未配置 ASR 端点 | `voice_service_transcribe_last()` 发一个空文本的 `VOICE_EVENT_ASR_DONE`，UI 提示改用文字输入 |
| 未配置 TTS 端点 | `voice_service_speak()` → `ESP_ERR_NOT_SUPPORTED`；跳过 SPEAKING 状态的音频部分 |
| 无网络 | ASR/TTS 请求失败，其余功能不受影响 |

## NVS 键

| 键 | 类型 | 说明 |
|---|---|---|
| `voice_wake` | i32 | 唤醒方式（0 无 / 1 按键 / 2 关键词；关键词唤醒当前不可用） |
| `asr_url` | str | ASR 端点 |
| `tts_url` | str | TTS 端点 |

## HTTP 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/voice/status` | 麦克风/喇叭/端点是否可用、是否在录音、最近一次时长 |
| POST | `/api/v1/voice/config` | 设置 `asr_url` / `tts_url` / `wake_mode` |

## 测试方法

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build; idf.py -p COM34 flash monitor
```

期望日志（音频硬件正常时）：

```
I (xxx) TH:voice: ready (mic=1 speaker=1 asr=0 tts=0 wake 'button')
```

若硬件缺失则出现：

```
W (xxx) TH:bsp_periph: microphone codec unavailable; voice input will be skipped
I (xxx) TH:voice: ready (mic=0 speaker=0 asr=0 tts=0 wake 'button')
```

此时 `GET /api/v1/voice/status` 返回 `"microphone":false`，这是**预期**的降级状态。

配置端点后手工验证：

```powershell
curl.exe -X POST http://todohelper.local/api/v1/voice/config `
  -H "Content-Type: application/json" `
  -d "{\"asr_url\":\"https://your-asr.example/v1/asr\",\"tts_url\":\"https://your-tts.example/v1/tts\"}"
curl.exe http://todohelper.local/api/v1/voice/status
```

录音文件（若 `CONFIG_TODOHELPER_VOICE_SAVE_PCM=y`）落在
`<storage>/todohelper/ai/rec_<uptime_ms>.pcm`，可用 Audacity 以
**16 kHz / 16-bit / 单声道 / 无头（Raw）** 导入试听。

> ⚠️ **实机验收状态**：音频硬件在真机上枚举通过（开机日志 `mic=1 speaker=1`），
> 但**录音抽取的边界、回放的主观音量/音质、ASR/TTS 端点协议都还没有可复核的实机结论** ——
> 上一轮交接称"录音回放真实可用"，未留下日志或录音文件证据，**不要当作已验证**。
> 另注意：本组件按需惰性打开音频（不在 `init` 里占内部 RAM，见 `AGENT.md §9.0.3`）。
