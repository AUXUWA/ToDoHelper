# ROADMAP v1.9.0 —— AI 对话与语音

> 需求：把 AI 功能补完整 —— **上位机配 AI Key**、**语音输入转文字**、**AI 用文字或语音回答**。
> 先评估资源够不够：够就干，不够就降为「待定」，转去优化现有功能。

## 1. 资源评估（真机数据，不是估算）

| 资源 | 现状（可复核的来源） | 结论 |
|---|---|---|
| Flash | `build/todohelper.bin` 3.1 MB / app 分区 8 MB（`build/partition_table/partition-table.bin`） | **够**。语音全走云端，本地没有模型，不占 flash |
| PSRAM | `heap` 命令报 psram free ≈ 9.8 MB | **够**。录音 10 s = 320 KB（PSRAM），TTS 一句话 ≈ 30 KB |
| 内部堆 | `heap` 命令报 free ≈ 183 KB、largest ≈ 100 KB | **够**。音频外设起来约 13 KB（free 183 → 170 KB），最大连续块不变 |
| CPU | 240 MHz | **够**。识别/合成都走云端，本机只做录音与播放 |
| 网络 | HTTPS 正常（天气/墨墨都在用） | **够** |

（数字都可以用 `debug/exec` 的 `heap` 命令当场复核；仓库里不留串口日志，
所以文档里只写能自己复算的数。）

**结论：资源够，开工。**

（关键前提：识别与合成都是**云端 HTTP 端点**，本地不跑模型。
工程里本来就没有离线识别组件（`managed_components/` 里没有 esp-sr / esp-tts），
所以这条路也是唯一现实的路。）

## 2. 硬件体检：麦克风其实是好的（差点被我自己的 bug 冤枉成「待定」）

v1.9.0 加了一条常驻的探针命令 `audio`，它在真机上量出：

```
audio init: ESP_OK (mic=1 speaker=1)     ← codec（ES8389）起来了，I2C 有应答
record 2000 ms -> ESP_OK
captured 0 ms, peak 0 / 32767  ← 2 秒里一个样本都没有（← 这个结论是错的，见下）
```

**修正（v1.9.3）：那个「0 样本」是本工程自己的 bug，不是硬件的问题。**
`esp_codec_dev_read()` **返回的是错误码，不是字节数** —— 0 表示「读成功」，
负数才是失败；真正读回来的字节数这个 API 根本不告诉你（底层 `i2s_channel_read()`
会一直等到缓冲区填满或超时）。

而原来的采集循环把返回值当成字节数：

```c
const int got = esp_codec_dev_read(mic, chunk, sizeof(chunk));
if (got <= 0) { continue; }        /* ← 每次「成功」都被当成「没数据」丢掉了 */
const size_t samples = (size_t)got / sizeof(int16_t);
```

于是**每一次成功读取都被当成 0 字节扔掉**，设备看上去「一个样本都收不到」。
改成按错误码判断之后，同样的固件、同一块板子：

```
record 1500 ms -> ESP_OK
captured 1466 ms, peak 3124 / 32767   ← 真实信号
```

连着三次、以及刚开机后的第一次，都稳定在 1461~1466 ms、峰值 670~3124。

→ 这条是独立复核员查出来的（复现员自己写的复刻程序把两条链路逐段核对了一遍）。
**教训**：硬件「测不出东西」的时候，先怀疑自己的测量代码 —— 一个恒为 0 的读数
通常是自己的 bug，不是硬件的判决。

| 现象 | 含义 |
|---|---|
| `mic=1 speaker=1` | codec 枚举成功，I2S 起来了，**不是「没有音频硬件」** |
| `captured 0 ms` | `esp_codec_dev_read()` 一直返回 0 —— I2S 的 RX 通道**一个数据都没收到** |
| 不是「0 样本的静音」 | 静音也会收到样本（时长 > 0、峰值 0）；这里是**连样本都没有** |

**两条待查的假设**（都不是本工程能单独定下来的）：

1. **MCLK 没接上。** BSP 把 ES8389 配成 `use_mclk = false`（codec 内部出 MCLK），
   而 I2S 侧又配了 MCLK 引脚（`BSP_AUDIO_I2S_MCLK = GPIO2`）。codec 侧没拿到
   有效时钟 → ADC 不出数据 → I2S RX 收不到任何东西。
   要验证就得在 `components/bsp/` 里自己建一路 codec 实例
   （`use_mclk = true`），不能改 `..\common_components\esp32_s31_korvo`（工程外、
   其它示例共用）。
2. **板子上是数字麦，模拟麦那一路是空的。** BSP 写的是
   `.digital_mic = false`（模拟麦走 ES8389 的 ADC）。如果这块板的麦其实是
   I2S/PDM 数字麦，ES8389 的 ADC 就永远是静音 —— 而采集通路会退化成
   「RX 收到 0 字节」。

**所以「麦克风采集通路待定」这条**已经撤销**（v1.9.3）：硬件是好的，
是本工程的采集循环读错了返回值。上面那两条硬件假设（MCLK / 数字麦）因此也不必再查。

**试过「自己建一路 codec」（当时按假设 1 查 MCLK），没成功，而且暴露了一个
更要紧的问题。**
在 `components/bsp/` 里自己建 I2C + I2S + ES8389（`use_mclk = true`，
顺便把「一颗芯片建两个 codec 实例」也改了）—— 结果**设备活着，但整个 HTTP
服务器卡死了**：ping 通、串口日志照常，HTTP 一个请求都不回。

原因很直白：音频初始化是在 **httpd 任务**里做的，而 codec 不应答时那一步会
**永远阻塞** —— 整台设备的 HTTP 就此停摆。**一个诊断用的命令不该有本事
把 HTTP 服务器带走。**

于是回退了这次实验（现在知道根本不需要动它），并给音频初始化加了保护（v1.9.1）：
**初始化放到自己的任务里做，最多等 3 秒**；超时就当「没有音频硬件」，
HTTP 服务器不受影响。`voice_service_ensure_audio()` 与调试 shell 的
`audio` 命令现在都走这条有界路径。

→ 这也是 AGENT.md §9.0.1.9 的一条教训：**凡是在 httpd 任务里做的硬件
握手，都要能超时。**

## 3. 这一轮做了什么

### 3.1 上位机（网页端）：AI 与语音的配置区（**用户点名要的那条**）

之前设备上的设置页写着「本机没有键盘，请在网页端配置」，AI 页写着
「POST /api/v1/ai/config {...}」—— **而网页端根本没有这个界面**，
普通用户根本配不了 Key。现在网页端多了一个「AI 对话与语音」分区：

- **DeepSeek API Key**（只写）：保存后设备只回「已配置 / 未配置」，
  本页不显示、也不写进 localStorage（与设备上的同一套规矩）。
- **模型**（chat / reasoner）、**系统提示词**、**保留对话轮数**、
  **对话历史存不存 SD 卡**。
- **「试一下」**：当场问一句，把回答显示在页面上（走 `POST /api/v1/ai/chat`）。
- **语音端点**：ASR（识别）与 TTS（合成）的 URL，各自写清楚协议。
  这两个 URL **不回显**（它们可能把令牌写在 query 里，与 AI Key 同一套规矩）。
- 折叠时不打扰设备，展开才拉状态。

设备侧补了一条 `GET /api/v1/ai/config`（读状态用 GET，不用 POST 空 body）。

### 3.2 设备端：AI 页

- **真的流式了。** 以前 worker 调的是阻塞版接口，整段回答作为「一个 chunk」
  一次性送到 UI —— 页面上那套逐字上屏的渲染代码写了却从没被喂过。
  现在 worker 走 `ai_service_chat_stream()`，delta 逐段上屏。
- **工具条多了两个按钮**：`语音`（录音 → 识别 → 当成提问发出去）、
  `朗读`（把上一条回答念出来）。
- 语音事件用 `ui_event_subscribe()` 单独订阅：VOICE_EVENT 与 AI_EVENT 的
  事件 id 各自从 0 开始，绑到同一个页面的 `on_event` 上会分不清谁是谁。

### 3.3 TTS 真的会播了

`voice_service_speak()` 以前把 TTS 返回的音频 `free()` 掉，然后**无条件返回
`ESP_ERR_NOT_SUPPORTED`** —— 也就是说「AI 用语音回答」这条路是断的。
现在：TTS 的响应是原始 PCM（16 kHz / 16 bit / 单声道），走和文件回放一样的
路径播出去。

配套修的两处：
- `http_post()` 现在**回传响应长度**（PCM 不是文本，不能用 `strlen` 算），
  响应缓冲也搬去了 PSRAM（TTS 一句话可能几百 KB，内部堆放不下）。

### 3.4 顺带的内存与规矩修正

- AI 的 12 KB 响应缓冲从内部堆搬到 **PSRAM**（内部堆是这台设备最紧的资源）。
- `voice_service` 的两处 `cJSON_Parse()` 换成 `sys_json_parse(body, 16)`
  （工程铁律一：畸形 JSON 不能爆栈）。
- 语音状态接口现在会**真的把音频外设起起来**再回答「有没有麦克风」——
  以前是惰性初始化，没起就报 false，看起来像「没麦克风」。

## 4. 验收

- [x] 网页端能配 Key / 模型 / 轮数 / 语音端点（真机浏览器实测）
- [x] Key 不回显、不落 localStorage
- [x] 设备端 AI 页工具条有「语音 / 朗读」，点击有正确的降级提示
- [x] `GET /api/v1/ai/config` 能读状态
- [x] 语音状态如实报告硬件（麦克风/扬声器就绪，识别/合成端点未配）
- [x] 音频外设起来后内部堆最大连续块不变（100 KB）
- [ ] **AI 端到端对话**（要一个真的 DeepSeek Key；本轮没有 Key，测不了）
- [ ] **语音闭环**（要 ASR/TTS 端点 + 麦克风通路；两样都还没有）

## 5. 语音端点：推荐与做法

设备端的协议是**故意厂商中立**的：

| 方向 | 设备发出 | 设备期待收到 |
|---|---|---|
| 识别 | `POST`，body 是裸 PCM（16 kHz / 16 bit / 单声道） | `{"text": "识别结果"}` |
| 合成 | `POST {"text": "...", "sample_rate": 16000}` | 裸 PCM 字节（16 kHz / 16 bit / 单声道） |

而**主流云服务没有一个是这个形状**（OpenAI/硅基流动要 multipart 文件、
合成默认回 MP3；讯飞/阿里云要 base64 塞 JSON 里；Edge TTS 回 MP3，
而设备没有 MP3 解码器）。所以中间必须有薄薄一层适配 ——
**`tools/voice_proxy.py` 就是它**（只用标准库，设备端一个字都不用改）：

```bash
# 假后端：先把设备这一侧的链路跑通，不花钱、不出网
python tools/voice_proxy.py --mock

# 接云服务（硅基流动的接口与 OpenAI 一样，都是 /v1 开头）
python tools/voice_proxy.py --asr-backend openai --tts-backend openai     --base-url https://api.siliconflow.cn/v1 --api-key sk-xxx

# 只接云识别，合成用 Edge TTS（免费、音色自然；要 edge-tts + ffmpeg）
python tools/voice_proxy.py --asr-backend openai --tts-backend edge     --base-url https://api.siliconflow.cn/v1 --api-key sk-xxx
```

然后在**网页端 → AI 对话与语音**里把两个端点填成
`http://<跑代理的那台机器>:8088/asr` 和 `.../tts`。

**选谁？**

| 用途 | 推荐 | 理由 |
|---|---|---|
| 识别（首选） | **硅基流动**（OpenAI 兼容的 `/v1/audio/transcriptions`） | 国内直连、有免费额度、中文模型好；接口与 OpenAI 完全一样，换一家只改 `--base-url` |
| 识别（质量优先） | OpenAI Whisper | 识别最准；国内要代理 |
| 识别（自建） | faster-whisper / whisper.cpp 服务端 | 暴露 OpenAI 兼容接口即可（同一套适配）；无按分钟费用，音频不出局域网 |
| 合成（首选） | **硅基流动 / OpenAI 的 `/v1/audio/speech` + `response_format: "pcm"`** | 注意那份 PCM 是 **24 kHz**，适配层重采样到 16 kHz（代理里已经做了） |
| 合成（免费） | Edge TTS | 音色自然、中文好；但回 MP3，要 ffmpeg 转成裸 PCM |
| 合成（自建） | CosyVoice / Fish-Speech / Piper | 音质最好、音频不外传；Piper 直接给 WAV（去头即裸 PCM） |

**两个要注意的点**：

1. **设备是「录完再发」**（一次最长 10 s），所以识别是批量的，
   典型延迟 1~3 秒。流式识别要改设备端的协议（另一件事）。
2. **合成音频要 16 kHz 单声道 PCM** —— 设备按 16 kHz 播放，
   别的采样率会变调（代理会按请求里的 `sample_rate` 重采样）。

## 6. 待定

1. ~~**麦克风采集通路**~~ —— 已撤销（v1.9.3：是本工程的 bug，硬件没问题）。
2. **AI 的函数调用**（tools）：需求里写着「解析『帮我添加待办 XXX』联动
   todo_manager」—— `build_request_json()` 里根本没有 `tools` 字段，一次都没实现。
   下一轮做（DeepSeek 兼容 OpenAI 的 tools 格式）。
3. **离线识别/合成**：`esp-sr` / `esp-tts` 没引，flash 与内存都够，但那是另一个量级。
4. **提示音**：工程里一个 wav/mp3 都没有，ui_assets 只会加载图片。
5. **TTS 缓存目录** `tts/` 预留了但没人写。

## 7. 下一版可以做的

1. **字库搬进内部 flash**（v1.8.5 留下的）：光栅化 10 ms → 0.2 ms。
2. **页面控件常驻**：切页 11~222 ms → 几 ms（v1.8.5 留下的）。
3. **AI 的函数调用**：让 AI 能直接加待办。
4. **唤醒词**：`VOICE_WAKE_KEYWORD` 声明了但没有 esp-sr，永远用不了。
