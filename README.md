# ToDoHelper

**ESP32-S31-Korvo 上的局域网待办助手**：待办清单 / 天气 / 日历 / 单词书 / 待机小眼睛 / AI 对话，
一块 800×480 触摸屏 + 一个内嵌浏览器上位机，数据落在 SD 卡（或片内 LittleFS）。

> **当前版本：`1.1.0`**（工程根目录 `version.txt`，同时出现在设备「设置」页、开机页、
> `GET /api/v1/device/info` 的 `version` 与 `build/todohelper.bin` 的 app descriptor 里）。
> 版本规则与发布步骤见 `AGENT.md §8.1`，变更历史见 `CHANGELOG.md`。

> 本工程最初由乐鑫 `esp-dev-kits` 的 Korvo 产测固件（factory_demo）重构而来；
> 那份 README 已被本文取代，上游代码保留在 git 历史与 `components/esp_cam_sensor` 等本地覆盖组件里。

## 功能

| 功能 | 状态 |
|---|---|
| WiFi 扫描/连接/NVS 保存/开机自动重连 | ✅ |
| 待办清单：设备端勾选 + 删除、网页端增删改查、`cache/todos.json` 持久化 | ✅ |
| 局域网 HTTP Server + mDNS（`todohelper.local`）+ 内嵌上位机 + SSE 实时推送 | ✅ |
| 天气（Open-Meteo，免 Key）+ IP 定位 + 定时刷新 + 缓存降级 | ✅ |
| 日历：SNTP 校时 + POSIX 时区 + 月历（无 RTC，断电不保持） | ✅ |
| 单词：CET-6 词库（833 词，放 SD 卡）+ 翻卡 + 三档评分 + 间隔复习 | ✅ |
| 待机小眼睛：纯程序化绘制，11 种表情（眨眼/半眯/慢眨/二连看/呼吸 + 自发小表情/困倦度/瞳孔缩放/整脸漂移） | ✅ |
| 设置页：固件版本/亮度/待机/时区/单词数/HTTP token/设备信息 | ✅ |
| AI 对话（DeepSeek，流式）+ 小眼睛联动 | ✅ |
| 语音：录音回放真实可用；ASR/TTS 走可配置 HTTP 端点（选型未定） | ⚠️ 半成品 |

## 快速开始

```powershell
# 1) 加载 IDF 环境（工程用 ESP-IDF v6.1，目标 esp32s31，串口 COM34）
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper

# 2) 构建 + 快速烧录（tools/flash.ps1 用 921600 + stub 压缩，比 idf.py flash 快约 4 倍）
idf.py build
pwsh tools\flash.ps1 -NoBuild

# 3) 抓开机日志（idf.py monitor 不能脚本化）
python tools\monitor_capture.py --port COM34 --seconds 20 --out build\boot.log
```

开机后浏览器打开 **`http://todohelper.local/`**（或设备 IP，本机实测 `192.168.137.110`）。
SD 卡只需 FAT32 可挂载：`<卡根>/todohelper/` 下的词库、字体、待办缓存由固件首次启动自动创建
（目录结构见 `AGENT.md §9.5`）。**拔卡也能跑**：回退片内 LittleFS。

## 硬件与前置

- 板卡：**ESP32-S31-Korvo**（800×480 RGB + GT1151 触摸 + ES8389 音频 + SDMMC 4-bit）
- 芯片目标：`esp32s31`（RISC-V，preview 目标），UI 为 LVGL 9.5.0
- 工程外依赖：`..\common_components\esp32_s31_korvo`（Korvo BSP，**只读，勿改**）
- `GPIO20~25` 与 SPI NAND 复用：**用 SD 卡就不能用 SPI NAND**

## 文档地图

| 文档 | 什么时候看 |
|---|---|
| **`AGENT.md`** | 动手前必读：本机环境/命令、**§9.0 起全部实测坑**（内部 RAM、字库、HTTP handler、启动崩溃）、验证基线、版本号规则 |
| **`docs/HANDOVER.md`** | 接手交接：当前真实状态、已修/待修、只有人能做的验证、踩坑清单、建议顺序 |
| `CHANGELOG.md` | 每个版本改了什么（含两处曾经误判的根因更正） |
| `docs/TODOHELPER_REQUIREMENTS.md` | 需求书（含实现偏差记录） |
| `docs/TODOHELPER_FRAMEWORK.md` | 分层设计（L0~L5、表驱动注册点） |
| `docs/API.md` | 组件接口与全部 HTTP 路由 |
| `components/*/README.md` | 单个组件的契约与坑 |

## 许可

应用层代码 SPDX `Apache-2.0`（见各源文件头）。随工程分发的第三方组件
（`managed_components/`、`components/esp_cam_sensor`、Korvo BSP）遵循各自许可证；
内嵌中文字体由 `tools/gen_cjk_font.py` 从 NotoSansSC（OFL）生成。
