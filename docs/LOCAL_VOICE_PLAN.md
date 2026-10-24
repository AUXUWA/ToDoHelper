# 本地语音方案评估：关键词唤醒 + 本地命令词 + 云端自由对话

> 2026-10-24 · 回应「本地关键词唤醒 + 本地语音转文本送 DeepSeek」的诉求。
> 参照系：xiaozhi-esp32（78/xiaozhi-esp32，官方支持 ESP32-S31 变体，IDF 6.1+）。

## 1. 先校准一个事实：xiaozhi 的「本地」只有唤醒

xiaozhi-esp32 的架构（README 与 main/protocols/ 源码）：

| 环节 | xiaozhi 在哪 | 说明 |
|---|---|---|
| 唤醒词检测 | **本地** | esp-sr WakeNet（wn10_nihaoxiaozhi「你好小智」） |
| 音频前端 AFE | **本地** | AEC/NS/VAD（esp_audio_front_end） |
| 音频编码 | **本地** | Opus 流式推到服务器（WebSocket/MQTT） |
| **语音转文本 ASR** | **云端** | 服务端（流式 ASR 或 Realtime 端到端模型） |
| 对话/LLM | 云端 | 服务端 |
| TTS | 云端 | 服务端 |

**它不是本地语音转文本。** 参考「本地语音对话」的可行形态 = 本地唤醒 + 本地音频前端
（+ 可选本地命令词）+ 云端 ASR/LLM/TTS。

## 2. 「本地语音转文本」的物理边界

自由语音转写（说任意话→任意文字）在 ESP32 级 MCU 上不可行，这是尺寸账：

- Whisper-tiny int8 权重 ≈ 42 MB —— 本板 PSRAM 可用堆仅 ~8.3 MB，**权重都放不下**；
- 就算放得下，300 MHz 双核 RISC-V 推理一句话是分钟级，不是对话；
- 乐鑫 esp-sr 提供的最接近物是 **MultiNet 命令词识别**：固定短语表（现成中文表
  313 条，可自定义拼音短语扩展），毫秒级离线出结果——它是「命令」，不是「转写」。

结论：自由文本的转写必须云端；本地能做的是**唤醒**与**命令词**两层。

## 3. 本板可行性：esp-sr 官方支持 ESP32-S31（关键事实）

AGENT.md 里「esp-sr 不可用」的结论**已过时**——那是本地组件镜像的局限。
实测拉取 esp-sr 2.6.0（GitHub）：

- `lib/esp32s31/` 预编译库齐全（wakenet/multinet/afe/vadnet…）；
- CMakeLists target 列表显式含 esp32s31；
- 官方基准（docs/en/benchmark/README.rst，esp32s31 节）：

| 项 | s31 实测数字 | 我们的余量 | 判定 |
|---|---|---|---|
| WakeNet10 内部 RAM | **16 KB** | boot 后空闲 ~150 KB / 最大连续 ~80 KB | ✅ |
| WakeNet10 PSRAM | 523 KB | 空闲 ~9.5 MB | ✅ |
| WakeNet10 CPU | 19.9%（单核，32 ms 帧 / 6.4 ms 算） | 双核 300 MHz，LVGL 在另一核 | ✅ |
| AFE（LOW_COST） | 数十 KB 内部 + VAD | 紧张，需实测闸门 | ⚠️ |
| MultiNet7 中文 PSRAM | ~2.6 MB 常驻 | 9.5 MB − 523 KB 之后仍够 | ✅（代价明确） |
| 准确率 | 1 m 安静 98%、3 m 噪声 94%、误触发 12 h 一次 | S3-Korvo 板测得 | 参考 |

- 现成中文唤醒词模型（不能自造词，自造要乐鑫定制服务）：
  `wn10_nihaoxiaozhi`（你好小智）、`wn10_xiaoaitongxue`、`wn10_tu2tu2ni3hao3`，
  每个打包约 **1.6 MB**；
- MultiNet7 中文模型约 **2.6 MB**（mn6_cn 3.6 MB）。

## 4. 资源底数（本板，真机口径）

- 内部 RAM：boot complete 后空闲 ~150–200 KB、最大连续 ~80–150 KB
  （v1.8.5 优化后口径，v1.15 真机日志 internal 196 KB / largest 148 KB）；
- PSRAM：boot 后空闲 ~9.5 MB（总量 8.3 MB 可见堆 + 保留区）；
- 分区：app 两槽各 3.97 MB（当前固件 3.3 MB，余 ~0.67 MB）；
  **assets 5.9 MB 只用了 8.9 KB** —— 模型的家在这里，划一块出去零压力；
- CPU：AI/天气/网络都是间歇负载，常驻 ~35% 单核（AFE+WakeNet）可承受。

## 5. 分层方案（推荐）

### Phase 1：本地关键词唤醒（建议先做，价值最高）

- 依赖进工程：vendor `esp-sr 2.6.0` + `esp-dl 3.3.12` + `dl_fft` 到 components/
  （本地镜像没有，git 拉；cjson 已有；esp-dsp 对 s31 无需）；
- `voice_service` 实装 `VOICE_WAKE_KEYWORD`（枚举早预留了）：
  AFE(MR, SR, LOW_COST：VAD→WakeNet10) 常驻任务（feed/fetch）；
- 模型：三个现成中文唤醒词选一，放 **model 分区**（从 assets 尾部划 ~2 MB，
  assets 5.9→3.9 MB）。**分区表变更 = MAJOR 语义**，需要一次性擦烧而非 OTA；
- 唤醒后动作：`VOICE_EVENT_WAKE`（事件已存在，语义正好复用）→ 点亮屏幕
  （th_bsp_report_activity）→ 提示音 → 自动进入 AI 对话录音（复用按钮路径）；
- 内存闸门：audio 任务栈与 AFE 缓冲按「先量后给」（§9.0.1.4 第 4 条的老规矩），
  int-low 水位红线在 CHANGELOG 的 Verified 里回填实测数。

### Phase 2：本地命令词（可选增量，第二优先）

- MultiNet7 中文挂同一 model 分区；唤醒后**不用等云端**：
  「现在几点/打开天气/记一条待办xx」毫秒级离线出结果；非命令语句回落云端 ASR；
- 代价：PSRAM 常驻 ~2.6 MB + 链接体积；命令表自定义（拼音短语）。

### Phase 3：自由对话链路（云端，现有组件接上即可）

- 录音 → `voice_service` ASR 端点（可配置 HTTP 桥已就绪，**端点选型仍开放**：
  自建 FunASR / 阿里云 / 其他）→ `ai_service`(DeepSeek) → TTS 端点 → 播放；
- 唤醒只是给这条链路加了一个免接触的入口。

### 版本与发布语义

- Phase 1 含分区表变更 → **MAJOR（2.0.0）**：老固件 OTA 到新分区布局不兼容，
  需一次 USB 擦烧；CHANGELOG 写明；
- Phase 2/3 不动分区表 → MINOR。

## 6. 风险清单

1. S31 是预览芯片：esp-sr 官方给了 s31 基准、xiaozhi 有 s31 变体，风险低但
   模型加载路径（分区 mmap）需真机验证；
2. 内部 RAM 是本项目最紧资源：AFE + 音频任务栈 + I2S DMA 的连续块需求
   必须真机量（老规矩：先量后改，看 int-low 而非空闲总量）；
3. AEC（边放边听）：需要 playback 参考通道，Phase 1 不开（唤醒场景没在放音），
   全双工对话留到链路通了再评；
4. 常驻采集的隐私与功耗：屏幕唤醒后 CPU/CPU 载荷增加；监听只在「唤醒开关打开」
   时运行，设置页可关。
