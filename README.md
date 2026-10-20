# ToDoHelper

**ESP32-S31-Korvo 上的局域网待办助手**：待办清单 / 天气 / 日历 / 单词书 / 待机小眼睛 / AI 对话，
一块 800×480 触摸屏 + 一个内嵌浏览器上位机，数据落在 SD 卡（或片内 LittleFS）。

> **当前版本：`1.8.0`**（工程根目录 `version.txt`，同时出现在设备「设置」页、开机页、
> `GET /api/v1/device/info` 的 `version` 与 `build/todohelper.bin` 的 app descriptor 里）。
> 版本规则与发布步骤见 `AGENT.md §8.1`，变更历史见 `CHANGELOG.md`。

> 本工程最初由乐鑫 `esp-dev-kits` 的 Korvo 产测固件（factory_demo）重构而来；
> 那份 README 已被本文取代，上游代码保留在 git 历史与 `components/esp_cam_sensor` 等本地覆盖组件里。

## 功能

| 功能 | 状态 |
|---|---|
| WiFi 扫描/连接/NVS 保存/开机自动重连 | ✅ |
| 待办清单：设备端**看得到截止日与优先级**、可按「未完成 / 今天到期 / 已完成」筛选、勾选/删除带二次确认；网页端增删改查 + 批量操作 + 排序 + 导出导入；`cache/todos.json` 持久化（**固定存片内**，插拔 SD 卡不丢） | ✅ |
| 首页仪表盘：**今日待办**大卡 + **AI 对话 / 单词 / 日历**三张等宽状态卡（同级别、可点进对应页）+ 四个功能磁贴 | ✅ |
| 日历页：一行三段头部（大时间 + 日期 + 回到今天/校时）+ 月网格（带到期圆点）+ 选中日待办详情 | ✅ |
| 局域网 HTTP Server + mDNS（`todohelper.local`）+ 内嵌上位机 + SSE 实时推送 | ✅ |
| 天气（Open-Meteo，免 Key）+ IP 定位 + 定时刷新 + 缓存降级 | ✅ |
| 日历：SNTP 校时 + POSIX 时区 + 月历（无 RTC，断电不保持） | ✅ |
| 单词：CET-6 词库（833 词，放 SD 卡）+ 翻卡 + 三档评分 + 间隔复习 | ✅ |
| 单词云端同步：墨墨背单词（开放 API）今日队列 / 真实进度 / 助记例句缓存 / 反向推词 | ✅ 未上机 |
| 待机小眼睛：纯程序化绘制，11 种表情（眨眼/半眯/慢眨/二连看/呼吸 + 自发小表情/困倦度/瞳孔缩放/整脸漂移） | ✅ |
| 设置页：固件版本/亮度/待机/时区/单词数/HTTP token/设备信息 | ✅ |
| AI 对话（DeepSeek，流式）+ 小眼睛联动 | ✅ |
| 语音：录音回放真实可用；ASR/TTS 走可配置 HTTP 端点（选型未定） | ⚠️ 半成品 |
| 落盘可靠性：双槽写入 + 启动恢复（掉电不丢数据）、JSON 限深解析（坏文件不再让设备开机就崩） | ✅ |

> **墨墨背单词是可选功能**：令牌存 NVS（`mm_token`），从网页上位机的「墨墨背单词」面板录入，
> **只写不读、不记日志**（与 AI Key 同规则）。**不配令牌就静默降级**：云端模式关闭，
> 本地 CET-6 词库照常工作。云端队列用 NVS `mm_mode` 切换，反向推词（把「不认识」的词推回
> 墨墨学习计划）默认关闭，因为那会**改动账号数据**，需要显式用 NVS `mm_push` 开启。

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

**在 Git-Bash / 自动化里构建**（MSYS 下 `idf.py` 会直接拒绝运行）：

```bash
# 1.3.0 新增：bash 入口包装器，把活交给原生 powershell 并原样回传退出码
bash tools/build.sh build          # 等价于 idf.py build
bash tools/build.sh flash          # 等价于 idf.py flash
bash tools/build.sh -v build       # 详细构建
```

输出同时重定向到 `build_last.log`（已 gitignore）。三个会**静默产出空日志 + 退出码 0** 的坑
（profile 把 `idf.py` 定义成函数、原生 exe 接不进 cmdlet 管道、`env -i` 清空环境）
记录在 `AGENT.md §4.2`。

开机后浏览器打开 **`http://todohelper.local/`**，或打开设备「设置」页里显示的 IP
（地址由 DHCP 分配，换网络就会变，所以以设备自己报出的为准）。
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
| **`docs/ROADMAP_v1.8.0.md`** | 本轮（v1.8.0，内部 RAM / 开机时间 / 回调里不许做重活）的施工规格、**实测结果**与审计复核记录 |
| **`docs/ROADMAP_v1.7.0.md`** | 上一轮（v1.7.0，开机时间 / 内部 RAM / 无谓网络往返）的施工规格与验收基线 |
| **`docs/ROADMAP_v1.6.0.md`** | v1.6.0（首页/日历/设置三页 UI 改造）的施工规格、真机实测结果 |
| **`docs/ROADMAP_v1.5.0.md`** | v1.5.0（待办主线补强 + 24 项整改）的施工规格与接口契约 |
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
