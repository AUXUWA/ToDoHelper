# AGENT.md

面向在本仓库工作的 AI/开发者的工作说明：**当前已实现的功能**、本机环境、常用命令、
必须保留的补丁与配置、验证基线、排错速查。上游 README 的概述不再重复，这里只写
「实际是什么状态、怎么在这里把事做成」。

> **本仓库已从 Korvo 产测固件（factory_demo）重构为 ToDoHelper 桌面助手。**
> 需求见 `docs/TODOHELPER_REQUIREMENTS.md`，框架设计见 `docs/TODOHELPER_FRAMEWORK.md`，
> 组件接口与 HTTP API 见 `docs/API.md`，**交接说明见 `docs/HANDOVER.md`**，
> 变更历史见 `CHANGELOG.md`。
> 优先级：**代码实际行为 > 本文件 > 框架设计 > 需求文档**；发现不一致时以左侧为准并回改右侧。
>
> **当前版本 `1.1.0`**（工程根目录 `version.txt`；规则与发布步骤见 §8.1）。

---

## 1. 项目是什么

- 目标：基于 ESP32-S31-Korvo 板卡实现桌面助手（ToDoHelper）。
- 芯片目标：**esp32s31**（ESP32-S31，RISC-V，preview 目标）。
- 应用名：**`todohelper`**（产物 `build/todohelper.bin`）。
- 框架：ESP-IDF **v6.1.0**；UI 为 LVGL **9.5.0** + `espressif__esp_lvgl_adapter` 0.5.3（**已打本地补丁**，见 §7）。
- 硬件来源：`espressif/esp-dev-kits` 的 `examples/esp32-s31-korvo`。板级 BSP 位于工程外
  `..\common_components\esp32_s31_korvo`（**只读，禁止修改**）。

**已实现的功能范围**（详见 `docs/API.md` 与需求文档 FR1~FR7）：

| 功能 | 状态 |
|---|---|
| WiFi 扫描/连接/NVS 保存/开机自动重连/断线退避重试 | ✅ |
| 待办清单：设备端列表+勾选、`cache/todos.json` 持久化、完整 REST API | ✅ |
| 局域网 HTTP Server + mDNS（`todohelper.local`）+ 内嵌浏览器待办页 + SSE 推送 | ✅ |
| 天气（Open-Meteo，**免 API Key**）+ 城市地理编码 + IP 定位 + 定时刷新 + 缓存降级 | ✅ |
| 日历：SNTP 校时 + POSIX 时区 + 月历（**无 RTC，断电不保持**） | ✅ |
| 单词：内置示例词库 + 翻卡 + 三档评分 + 间隔复习 + 进度持久化 | ✅ |
| 待机小眼睛：纯程序化绘制，11 种表情 | ✅ |
| 设置页：亮度/待机/时区/单词数/HTTP token/设备信息 | ✅ |
| AI 对话（DeepSeek，流式）+ 小眼睛联动 | ✅ |
| 语音：录音回放真实可用；ASR/TTS 走**可配置 HTTP 端点**（选型未定） | ⚠️ 半成品 |

---

## 2. 组件结构（实际状态）

```text
components/
  sys_utils/          L0  日志/共享事件循环/时间/sys_status/sys_notify/NVS 配置/错误码
                          /堆诊断（`sys_heap_log()` 含内部 RAM 最低水位 `int-low`）
                          /cJSON PSRAM 钩子（`sys_json_use_psram()`，见 §9.0.5）
  http_router/        L1  HTTP 路由表 + 统一响应 {code,msg,data} + 404/405/501 + 鉴权钩子
  storage_manager/    L1  SD(SDMMC 4bit) → 片内 LittleFS 回退、原子写、缓存 API
  bsp/                L1  th_bsp_ 门面（显示/软件调光/按键/待机看门狗/LED/SD/音频）
  ui_assets/          L2  assets 分区打包（本组件的 CMakeLists 拥有）+ 图标按基名解析
  ui_core/            L2  页面注册表/导航/状态栏/递归锁/事件编组/主题/字体链/卡片提示
  wifi_manager/       L3  FR1（含 /api/v1/wifi/* 路由）
  todo_manager/       L3  FR2（含 /api/v1/todos* 路由、JSON 导入导出）
  calendar_service/   L3  FR4（SNTP + 时区 + 月历）
  weather_service/    L3  FR3（可插拔 provider，默认 Open-Meteo + IP 定位）
  word_service/       L3  FR5（词库装载 + 间隔复习）
  sync_server/        L4  httpd 生命周期 + 设备 API + SSE + 内嵌 Web 页 + mDNS + 鉴权
  idle_face/          L4  FR6 待机眼睛（自带 UI_PAGE_IDLE_FACE 注册 + 通知触发表情）
  ui_pages/           L4  11 个页面（组合层，可依赖任意非 UI 组件）
  ai_service/         L5  FR7 DeepSeek（阻塞/流式/历史/中断）
  voice_service/      L5  FR7 录音回放 + ASR/TTS 端点桥接
  esp_cam_sensor/     —   本地组件，覆盖托管组件（BSP 需要）
main/
  app_main.c              表驱动模块编排（9 个模块，全部 after_ui），无可选模块失败阻塞 UI
assets/ui/                扁平 PNG 资源（w_*.png），打包进 assets 分区
tools/gen_cjk_font.py     生成内嵌 CJK 子集字体
docs/                     需求 / 框架 / API / 交接
```

### 2.1 分层依赖规则

```text
L5 ai_service, voice_service
L4 sync_server, idle_face, ui_pages
L3 wifi_manager, todo_manager, weather_service, calendar_service, word_service
L2 ui_assets, ui_core
L1 http_router, storage_manager, bsp
L0 sys_utils
--------------------------------------------------> 依赖只能向下
```

- L3 组件之间**不得**互相 `REQUIRES`：用事件或 `sys_notify()` 通信；
- **HTTP 路由反向注册**：业务组件向 L1 `http_router` 注册，`sync_server` 只负责起服务；
- **跨层通知**：L3 想通知 UI/Web 时调 L0 `sys_notify()`，不要调 `sync_server_*`；
- `ui_pages` 是**组合层**，可以依赖任意非 UI 组件（只要不成环）；`ui_core` / `ui_assets` /
  `idle_face` 不得被业务组件依赖。

---

## 3. 本机环境（已实测确认，勿凭猜测改动）

| 项 | 值 |
|---|---|
| IDF | `D:\ESP-IDF\v6.1\esp-idf`（ESP-IDF **v6.1.0**，tag `v6.1`，commit `fff9895c`） |
| `IDF_TOOLS_PATH` | `C:\Espressif\tools` |
| Python venv | `C:\Espressif\tools\python\v6.1\venv`（idf.py 用它运行） |
| 激活脚本 | `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`（eim 生成） |
| 板子串口 | **COM34** = Silicon Labs CP210x = ESP32-S31 板（MAC `1c:29:04:xx:xx:xx`） |
| 另一串口 | COM26 = CH340，**不是**本板（且常被其它程序占用） |
| 工具链 | riscv32-esp-elf `esp-15.2.0_20251204`（esp32s31 走 RISC-V） |
| 组件源 | `IDF_COMPONENT_LOCAL_STORAGE_URL=file://C:\Espressif\tools`（本地镜像，部分组件不可得） |

**本地镜像里没有的组件**（决定了技术选型，见 §9）：
`esp_websocket_client`（→ 用 SSE 代替 WebSocket）、`esp-sr` / `esp_tts`（→ 离线 ASR/TTS 不可用）。
可用的有：`joltwallet/littlefs`、`espressif/mdns`、`espressif/cjson`、`espressif/mqtt`。

`idf.py` **不在 Git Bash 的 PATH 里**；IDF v6.1 在 Git Bash 下会打印
`MSys/Mingw is no longer supported` 警告（`MSYSTEM` 被注入子进程）。**构建请走 PowerShell**。

---

## 4. 常用命令

### 4.1 PowerShell（推荐交互使用）

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build
idf.py -p COM34 flash
idf.py -p COM34 monitor          # 退出：Ctrl+]
```

### 4.2 从 Git Bash / 自动化调用（等价写法）

```bash
cd /d/project/ESP-IDFv6.1/ToDoHelper
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \
  "Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; \
   . 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1' | Out-Null; \
   Set-Location 'D:\project\ESP-IDFv6.1\ToDoHelper'; \
   idf.py -p COM34 flash"
```

要点：

- 先 `Remove-Item Env:MSYSTEM` 再加载 profile，可去掉那条 MSYS 警告。
- 编译耗时较长（约 2200 个目标，冷启动数分钟，有 ccache 后增量很快），
  后台跑并重定向日志：`> build/dsh_build.log 2>&1`。
- 若要限制构建产物写在工程内：`$env:CCACHE_DIR='D:\project\ESP-IDFv6.1\ToDoHelper\build\.ccache'`。
- **`idf.py reconfigure`**：改动 `assets/ui/` 下文件、增删组件、改 `idf_component.yml` 后需要
  （`assets` 镜像的依赖是 configure 期求值的 `GLOB_RECURSE`）。

### 4.3 只烧录 / 只擦除 / 清理

```powershell
idf.py -p COM34 flash            # 只烧录（会自动先构建）
idf.py -p COM34 erase-flash      # 全擦（会清掉 NVS 里的 WiFi/Key 配置）
idf.py fullclean                 # 删 build/（managed_components 不动）
idf.py reconfigure               # 重跑 CMake + 组件管理器
```

### 4.4 重新生成内嵌中文字体（可选）

```powershell
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' tools\gen_cjk_font.py
# 需要 fontTools + brotli：pip install fonttools brotli
# 默认：源 C:\Windows\Fonts\NotoSansSC-VF.ttf（OFL，可再分发），
#       把可变字体钉到 wght=400，取 GB2312 一级（3755 常用字）+ ASCII + 标点，
#       输出 components/ui_core/fonts/ui_cjk.ttf（约 1.2 MB）。
# 加 --level2 得到 6763 字（约 3.8 MB）。改完字体必须 idf.py build 重新嵌入。
```

---

## 5. 关键文件

```
ToDoHelper/
├─ CMakeLists.txt              # EXTRA_COMPONENT_DIRS 指向 ../common_components；project(todohelper)
├─ partitions.csv              # nvs 0x9000/0x6000, phy_init 0xf000/0x1000,
│                              # factory 0x10000/0x800000, storage(littlefs) 0x810000/0x200000,
│                              # assets(spiffs) 0xa10000/0x5f0000
├─ sdkconfig.defaults          # 真正的默认配置来源（**含两条不可删的必需项**，见 §9）
├─ sdkconfig                   # 生成物，不要手改
├─ dependencies.lock           # 组件管理器锁文件（target: esp32s31）
├─ main/app_main.c             # 模块编排表（9 个模块，全部 after_ui）
├─ components/                 # 见 §2
├─ assets/ui/                  # 扁平 PNG（w_*.png）；**不能有子目录**
├─ tools/gen_cjk_font.py       # CJK 子集字体生成脚本
├─ managed_components/         # 托管依赖；除 §7 补丁外勿手改
├─ patches/                    # esp_lvgl_adapter IDF6.1 支持补丁（必需）
├─ docs/                       # 需求 / 框架 / API / 交接（HANDOVER.md）
└─ build/                      # 构建产物；排障日志 dsh_build*.log
```

工程外但必需：`D:\project\ESP-IDFv6.1\common_components\esp32_s31_korvo`（Korvo BSP，由
`EXTRA_COMPONENT_DIRS` 和 `main/idf_component.yml` 的 `path:` 引用）。**不要修改它**。

---

## 6. 依赖与版本约束

- `main/idf_component.yml` → `idf: ">=6.1.0"`、Korvo BSP（path）、`espressif/esp_lv_decoder` 0.4.0。
- 业务组件的托管依赖放在各自 `components/<name>/idf_component.yml`：
  `joltwallet/littlefs`（storage_manager）、`espressif/mdns`（sync_server）、
  `espressif/cjson`（todo/weather/word/sync/ai/voice/wifi）、`esp_mmap_assets`（ui_assets）。
- **ESP-IDF v6.1 已无内置 `json` 组件**，cJSON 必须走 `espressif/cjson`（组件名为 `cjson`）。
- Korvo BSP（工程外）的约束：`espressif/esp_lvgl_adapter: ^0.5.0`、`esp_video: 2.4.1`、
  `esp_cam_sensor: 2.4.*`、`esp_codec_dev: ^1.5.9`、`led_strip: ^3.0.3`、`button: ^4`、
  `esp_lcd_touch_gt1151: ^1` → 因此 `esp_lvgl_adapter` 固定 0.5.3、LVGL 固定 9.5.0。
- 想换版本：改 `main/idf_component.yml` 增加显式约束后 `idf.py reconfigure`；
  注意 BSP 的 `^0.5.0` 限制，不修改工程外的 BSP 就无法升到 0.6.x。

---

## 7. 必须保留的补丁（重要）

**文件**：`patches/esp_lvgl_adapter_idf61_support.patch`

**原因**：`esp_lvgl_adapter` 0.5.3 用 IDF 版本号区分 DMA2D 异步驱动接口：

```c
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 2, 0)
#include "esp_async_color_convert.h"   /* 新接口 */
#else
#include "esp_async_fbcpy.h"           /* 旧接口，IDF 6.1 已删除该头文件 */
#endif
```

而 IDF **v6.1 已经是重命名后的新接口**（`components/esp_driver_dma/include/esp_async_color_convert.h`，
已无 `esp_async_fbcpy.h`），于是编译报：

```
display_bridge_common.h:39:10: fatal error: esp_async_fbcpy.h: No such file or directory
```

**补丁内容**（共 5 个文件）：
1. `src/display/bridge/common/display_bridge_common.c`（5 处）
2. `src/display/bridge/common/display_bridge_common.h`（2 处）
3. `src/display/bridge/v9/lvgl_bridge_v9.c`（4 处）
4. `src/display/bridge/v8/lvgl_bridge_v8.c`（4 处，当前 LVGL9 不走该路径，一并改保持一致）
   → 以上均把 `ESP_IDF_VERSION_VAL(6, 2, 0)` 改为 `ESP_IDF_VERSION_VAL(6, 1, 0)`。
5. `CMakeLists.txt`：不再用版本号判断，改为按头文件是否存在决定是否
   `PRIV_REQUIRES esp_driver_dma`：

```cmake
if(EXISTS "$ENV{IDF_PATH}/components/esp_driver_dma/include/esp_async_color_convert.h")
    list(APPEND ADAPTER_PRIV_REQ esp_driver_dma)
endif()
```

**何时需要重新应用**：只要 `managed_components/espressif__esp_lvgl_adapter` 被重新解压/下载
（删除该目录、删除整个 `managed_components/`、改动了组件版本约束）。正常 `idf.py build` /
`fullclean` / `reconfigure` **不会**覆盖它。

**应用补丁**（`patch` 与 `git apply` 均可用，已实测可从官方 0.5.3 干净应用）：

```bash
cd /d/project/ESP-IDFv6.1/ToDoHelper
patch -p1 < patches/esp_lvgl_adapter_idf61_support.patch
# 或： git apply -p1 patches/esp_lvgl_adapter_idf61_support.patch
```

**校验是否已应用**：

```bash
grep -c "ESP_IDF_VERSION_VAL(6, 1, 0)" \
  managed_components/espressif__esp_lvgl_adapter/src/display/bridge/v9/lvgl_bridge_v9.c   # 期望 4
grep "esp_async_color_convert.h" managed_components/espressif__esp_lvgl_adapter/CMakeLists.txt
```

---

## 8. 验证基线（判定「编译成功」）

> ✅ **已于真机（COM34，ESP32-S31-Korvo）完成首轮上机验证**：0 panic、0 task watchdog、
> 12 个功能模块全部 `ready`、中文正常显示（用户确认无方框）、SD 卡目录与文件写入成功、
> mDNS 发布 `todohelper.local`、HTTP 监听 80 端口共 37 条路由、音频 `mic=1 speaker=1`。
> **尚未验证**：WiFi 实际关联（需凭据）、联网后的 HTTP 全链路、触摸/按键交互、
> 待机超时与眼睛动画。详见 `docs/HANDOVER.md` §5。

日志出现：

```
Project build complete.
```

**ToDoHelper 当前基线**（可作回归对比基准）：

| 项 | 值 |
|---|---|
| app bin | `build/todohelper.bin`，`0x2db470` 字节（约 2.86 MB，含 `1.1.0` 版本号；字库已移出到 SD/`storage`，见 §9.7） |
| app 分区余量 | `0x800000` 分区，剩 **64%** |
| bootloader | `build/bootloader/bootloader.bin`，`0x5a80`，余 6% |
| 编译警告 | 0（`-Werror` 全开） |
| 资源分区 | `build/mmap_build/ui/assets/assets.bin`（8 个 PNG，约 8.9 KB） |
| 组件数 | 16 个自有组件 + 1 个本地覆盖组件（`esp_cam_sensor`） |

**内部 RAM 预算（真机实测，插卡 `root=/sdcard`——这是本工程最紧张的资源）**

| 启动阶段 | 内部空闲 | 最大连续块 | PSRAM 空闲 | 内部最低水位 |
|---|---|---|---|---|
| `startup` | 323 KB | 272 KB | 13561 KB | 311 KB |
| `bsp (display+LVGL)` | 267 KB | 224 KB | 11299 KB | 255 KB |
| `storage` | 267 KB | 220 KB | 11295 KB | 254 KB |
| `ui_assets` | 266 KB | 220 KB | 11295 KB | 254 KB |
| `ui_core` | 260 KB | 216 KB | 11255 KB | 247 KB |
| `ui_pages` | 255 KB | 212 KB | 11255 KB | 243 KB |
| `wifi_manager` | 123 KB | 80 KB | 11183 KB | 111 KB |
| `todo_manager` | 121 KB | 78 KB | 11183 KB | 108 KB |
| `weather_service` | 109 KB | 64 KB | 11183 KB | 96 KB |
| `calendar_service` | 107 KB | 61 KB | 11183 KB | 93 KB |
| `word_service` | 85 KB | 41 KB | 10618 KB | 69 KB |
| `sync_server` | 76 KB | 33 KB | 10613 KB | 64 KB |
| `ai_service` | 67 KB | 24 KB | 10613 KB | 55 KB |
| `boot complete` | **67 KB** | **24 KB** | 10293 KB | **55 KB** |

> 每一行由 `sys_heap_log()` 在 `app_main` 里输出。**排障时看「最大连续块」而不是总空闲量**：
> 首轮上机出现的全部 `ESP_ERR_NO_MEM` 都是内部 RAM 碎片化造成的——总空闲看着还有十几 KB，
> 但最大连续块是 0，于是 WiFi / mDNS / I2S DMA / SDMMC DMA 全线失败。
>
> **还要看第 4 列 `int-low`**（内部 RAM 的历史最低水位，2026-09 由 `esp_get_minimum_free_heap_size()`
> 这个被 PSRAM 主导、永远不动的值改过来）。它掉到个位数 KB 就说明**某个瞬时峰值把内部 RAM
> 抽干过** —— 那正是插卡启动崩溃的判据（§9.0.5），而当时的空闲量看起来还是健康的 107 KB。

烧录成功判据：每个镜像后出现 `Hash of data verified.`，结尾 `Hard resetting via RTS pin...` + `Done`。

烧录地址（`idf.py flash` 自动处理）：
`0x2000` bootloader、`0x8000` 分区表、`0x10000` app、`0x810000` storage(littlefs)、`0xa10000` assets。

### 8.1 版本号与发布（`version.txt`）

固件版本来自工程根目录的 **`version.txt`**。ESP-IDF 的取值优先级是
`PROJECT_VER` 变量 → **`version.txt`** → `git describe`（本工程没设 `PROJECT_VER`，
`sdkconfig` 里 `CONFIG_APP_PROJECT_VER_FROM_CONFIG` 也未开，所以 `version.txt` 是唯一来源）。
**不写这个文件时 IDF 会退回 `git describe`**，设备就会自报 `cfdf817-dirty` 这类字符串 ——
改一行代码就变，"板子上到底是哪个固件"完全看不出来（1.0.0 之前就是这样）。

- **当前版本：`1.1.0`**
- 版本进 `esp_app_desc_t`，于是**三处可见**：
  设备「设置」页顶部与开机页「固件 1.1.0 · IDF v6.1」、`GET /api/v1/device/info` 的 `version`、
  以及 `build/todohelper.bin` 的 app descriptor（离线核对：
  `riscv32-esp-elf-objdump -s -j .flash.appdesc build/todohelper.elf`）。
- 规则（三段式）：**MAJOR** = 一轮需求全部完成、或有破坏性变更（存储格式 / HTTP API /
  NVS 键语义）；**MINOR** = 新增功能或页面；**PATCH** = 修 bug、调参、文档。
- 发布动作：改 `version.txt` → `idf.py build`（它在 `CMAKE_CONFIGURE_DEPENDS` 里，会自动触发
  reconfigure）→ 烧录并确认 `device/info` → 写 `CHANGELOG.md` → `git tag -a vX.Y.Z -m "..."`。

### 8.2 抓串口日志（`idf.py monitor` 无法脚本化）

`idf.py monitor` 会独占终端、不能重定向，自动化验证请用工程自带脚本：

```powershell
# 复位开发板并抓取 N 秒串口输出（RTS→EN 复位，DTR 保持低以正常启动）
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' tools\monitor_capture.py `
    --port COM34 --seconds 30 --out build\boot.log
```

### 8.3 烧录速度：用 `tools/flash.ps1`，别用 `idf.py flash`

`idf.py flash` 在这个目标上比必要的**慢约 3.4 倍**。下表的耗时是当年 4.16 MB 镜像
（字库还在 app 里）实测的；**当前 2.85 MB 镜像用 `tools/flash.ps1` 实测约 32 s**。

| 方式 | 耗时 | 有效速率 |
|---|---|---|
| `idf.py flash`（460800 + `--no-stub`） | **147.9 s** | 224.6 kbit/s |
| 921600 + `--no-stub` | 98.7 s | 337.4 kbit/s |
| **921600 + stub loader（压缩）** | **35.8 s** | 930.9 kbit/s |

两个原因：

1. **`idf.py` 对 esp32s31 硬编码了 `--no-stub`** —— 走 ROM 下载协议，**不压缩**、
   分块更小。去掉它就启用 stub loader，镜像被压缩（4.16 MB → 2.57 MB）。
   这是主要收益：端到端 **148 s → 44 s**。
2. **上传波特率默认 460800，而它在 IDF v6 里不是 Kconfig 选项**
   （没有 `CONFIG_ESPTOOLPY_BAUD*` 符号可设），只能用 `-b` 或 `ESPBAUD` 环境变量。

所以本工程提供 `tools/flash.ps1` 封装这条快路径（并顺手关掉占用串口的 monitor）：

```powershell
pwsh tools/flash.ps1              # 构建 + 快速烧录
pwsh tools/flash.ps1 -NoBuild     # 只烧录
pwsh tools/flash.ps1 -Monitor     # 烧完直接进监视器
```

> **还能再快**：app 里 **1.2 MB 是内嵌的 CJK 字体**（占全镜像 30%），而这份字体
> 已经在 SD 卡上（`/sdcard/todohelper/fonts/ui_cjk.ttf`）。不再内嵌它可把镜像压到
> 约 2.9 MB。用 `idf.py size-components` 可随时复核体积构成。

---

## 9. 注意事项与坑

### 9.0 真机首轮上机的五条实测结论（**最重要，先读这节**）

本板内部 RAM 只有 **~310 KB**，而 PSRAM 有 **12.4 MB**。所有"内存不够"的问题都出在
内部 RAM，而 IDF 报错只说 `ESP_ERR_NO_MEM` / `not enough mem`，非常容易误判。
以下五条都是实测踩出来的：

1. **LVGL 的局部绘制缓冲固定从内部 RAM 分配**（`esp_lvgl_adapter` 的
   `display_manager.c:1095` 把 capability 硬编码成非 PSRAM）：
   `hor_res × buffer_height × 2`。咖啡机 demo 的 `buffer_height=160` 就是 **256 KB**，
   能占满 310 KB——它没出事只是因为它**从不启动 WiFi**。
   → 本工程改用 `ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL`，复用面板自己的 PSRAM
   帧缓冲当 LVGL 绘制缓冲，**内部占用降为 0**（面板本来就配了 `fb_count=3`，正好够）。
2. **`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 是决定性的开关**。IDF 默认 16384，即所有
   <16 KB 的分配优先走内部 RAM；而 `esp_wifi_init()` 会产生**大量**这种小分配，实测
   它把内部空闲从 172 KB 压到 15 KB（最大连续块 **0**），同时让
   `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` 形同虚设。
   → 本工程设为 **4096**：通用中小分配走 PSRAM，把内部 RAM 留给真正必须用它的东西
   （`MALLOC_CAP_DMA` 描述符、任务栈、net80211 的大块）。
3. **LVGL v9 的 FreeType 不支持内存字体**。适配器运行期会明确打印
   `LVGL v9 FreeType does not support memory-based fonts, will use file path` 然后
   `FreeType font initialization failed`，于是静默回退到 LVGL 内置 CJK 字库——那套字库
   只有约 2500 字，所以出现"部分汉字正常、部分方框"的假象（`未/同/步/天气/日历` 正常，
   `连/接/办/么/钟/存储/设置` 是方框）。**`EMBED_FILES` + 内存模式永远不可能成功。**
   → 内嵌子集首次启动写入 `<storage>/todohelper/fonts/ui_cjk.ttf`，再用
   `ESP_LV_ADAPTER_FT_FONT_FILE_CONFIG` 按**文件路径**加载。附带好处：换字体不用重烧。
4. **渲染任务栈 ≥32 KB**。`LV_DRAW_SW_DRAW_UNIT_CNT=1` 意味着 LVGL 没有独立绘制线程，
   FreeType 光栅化就跑在适配器的 LVGL 任务栈上，而 LVGL 自己的注释写着
   "If FreeType or ThorVG is enabled, it is recommended to set it to 32KB or more"。
   16 KB 时表现为**随机 panic**（Load access fault / Load address misaligned /
   Stack protection fault 在不同核上交替出现）。注意
   `ESP_LVGL_ADAPTER_FREETYPE_SMALL_RENDER_POOL` **恰好把这个构建期检查去掉了**，
   所以它不会提醒你。本工程用 32 KB 且 `stack_in_psram=true`。
5. **FatFs 对"已存在的目录"返回 `FR_INVALID_NAME`，而 IDF VFS 映射成 `EINVAL`（不是
   `EEXIST`）**（见 `vfs_fat.c` 的 `fresult_to_errno` 与 `ff.c` 的 `f_mkdir`）。
   所以 `if (errno == EEXIST) return OK;` 会把已存在目录当成硬失败。
   → 失败后用 `stat()` 确认是否已是目录，并重试一次。

### 9.0.1 四个已经把设备搞挂过的自伤 bug（别再犯）

- **PSRAM 任务栈绝对不能碰 flash。** 把 LVGL 任务栈放到 PSRAM
  （`stack_in_psram = true`）看起来能省 32 KB 内部 RAM，但**一点"连接 WiFi"就重启**：
  UI 任务会写 NVS（连接按钮存凭据、设置页存亮度/时区/待机时间），而 flash 写会关闭
  cache，PSRAM 随即不可访问，IDF 直接断言：
  ```
  assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
                (esp_task_stack_is_sane_cache_disabled())
    nvs_set_str <- sys_config_set_str <- wifi_manager_save_config <- connect_with
  ```
  → **只要任务可能做 flash 操作（NVS/OTA/SPI flash），栈就必须在内部 RAM**。同理
  `wifi_manager` 的扫描工作线程也用内部栈（Wi-Fi 驱动可能访问 NVS）。判据：崩溃时
  `SP` 落在 `0x50xxxxxx`（PSRAM 段）就是这个坑。
- **任何 LVGL 调用都必须要么在 LVGL 任务里、要么持锁。** `ui_lock()/ui_unlock()` 包着
  适配器的**递归**互斥锁，所以从 LVGL 任务内部再取一次是安全的。曾经 `app_main` 直接调
  `ui_statusbar_refresh()` 没持锁，在渲染任务遍历对象链表时改标签 → main 任务死循环、
  IDLE0 被饿死、每 5 秒刷一次 task watchdog。现在 `ui_statusbar_refresh()` **自己**加锁。
  另外**别用 `esp_lv_adapter_lock(0)` 做"试着拿一下"**：拿不到时它会打 `ESP_LOGE`
  （"Failed to acquire LVGL lock"），把日志刷满。周期性工作请用 `lv_timer`（本来就在
  LVGL 任务里，根本不需要加锁）。
- **不要在 WiFi 事件处理器里干活。** 它跑在共享的 `sys_evt` 任务上，栈只有约 2.8 KB。
  曾经在 `WIFI_EVENT_SCAN_DONE` 里放了一个 `wifi_ap_record_t[24]` 局部数组（约 2.7 KB）
  并调用 `esp_wifi_scan_get_ap_records()` → 栈溢出重启。现在处理器只往队列里发一条命令，
  扫描记录的拷贝由 `wifi_manager` 的专用工作线程（4 KB 栈）完成。
- **长连接的 URI handler 会冻住整个 Web 服务器（SSE / 流式接口都算）。**
  `esp_http_server` 的 handler 是**在唯一的 httpd 任务里串行执行**的：v6.1 虽然是
  `select()` 事件循环，但 `httpd_server() → httpd_sess_process() → uri->handler(req)`
  一路都是内联调用，只有 `for_async_req` 的会话才会被跳过。所以一个"一直不返回"的
  handler 会拦死**所有**请求，连 `accept()` 都不再执行 —— 表现为
  **设备 ping 得通、串口正常，但任何 TCP 连接都建不上**（浏览器显示"设备不可达/读取失败"）。
  真机实测（插卡）：挂上 1 个 `curl -N /api/v1/events` 之后，`GET /` 与全部 API 超时；
  客户端一断开立刻恢复 200。
  → 所有长期/流式 handler 都必须用 **`httpd_req_async_handler_begin()`** 把 socket 交给
  自己的任务，handler 立即返回，结束时 `httpd_req_async_handler_complete()`
  （**必须** complete：不 complete 会永久占用会话槽，`httpd` 最终完全不再 accept，
  IDF 头文件里明确写了这一点）。参考实现：`components/sync_server/src/sync_server.c`
  的 `events_get_handler()` + `sse_client_task()`（每客户端一个 3 KB 任务、一个队列）。
  **同类隐患仍在**：`ai_service_http.c` 的 `/api/v1/ai/chat/stream`（以及阻塞版
  `/api/v1/ai/chat`）在一次 AI 回复期间会把整台设备的 HTTP 全部堵住 —— 修法同上。

### 9.0.2 日志：`esp_log_writev()` 是无前缀、无换行的原样输出

`sys_log_write()` 原先用 `esp_log_writev()`（图 va_list 方便），但它是**原样输出**：
既没有 `I (1234) TH:tag:` 前缀，也**没有结尾换行**。串口是行缓冲的，于是：

- 所有应用日志互相粘连成一行，完全没法看；
- **最后一条 IDF 日志之后的应用日志会卡在缓冲区里不出来**——表现为"扫描完成"、
  "待机超时"这类消息**看起来根本没发生**，害我误判了两个功能是坏的。

改用 `esp_log_write(level, tag, "%s\n", line)`（先 `vsnprintf` 格式化，再用
`ESP_LOGx` 宏同一条格式化路径）。修好后启动日志从"1 行"变成 **782 行**，
`scan done: 18 AP(s)` 立刻就看到了。**排障时如果怀疑"某段代码没执行"，先确认日志格式。**

### 9.0.3 内部 RAM 的最紧处：WiFi PHY 重新使能需要约 30 KB 连续块

`esp_phy_enable()`（扫描/唤醒等射频活动时触发）会先分配自己的缓冲，**然后**才
`esp_timer_create()` 一个 PLL 跟踪定时器。若此时内部 RAM 最大连续块不足，这个
**100 字节**的分配会失败，而 IDF 用的是 `ESP_ERROR_CHECK`，直接 abort：

```
ESP_ERROR_CHECK failed: esp_err_t 0x101 (ESP_ERR_NO_MEM)
  at phy_track_pll_init (phy_common.c:126)
  expression: esp_timer_create(&phy_track_pll_timer_args, &phy_track_pll_timer)
```

**判据是「最大连续块」，不是总空闲量**。实测：boot 结束内部 77 KB 空闲但最大连续块
只有 33 KB 时就会崩；把 `after voice_service` 从 78 KB/33 KB 提到
**97 KB/52 KB** 后不再出现。所以：

- **boot 阶段别占用内部 RAM**。`voice_service` 曾经注释写着"lazy"却在 init 里调了
  `ensure_audio()`，白占 20 KB 整个运行期——现已改为真正按需打开（首次录音/回放/
  查询状态时才初始化 codec）。
- **`esp_wifi_set_ps(WIFI_PS_NONE)`**：默认省电模式会让 PHY 反复 power-down/up，
  正是上面那条路径。本设备是市电供电的桌面设备，关掉省电既省内部 RAM 抖动也降低延迟。

### 9.0.4 字体缺字：用 fallback 链，不要逐个控件打补丁

内嵌的 CJK 子集只有 GB2312 一级 + ASCII，**没有 LVGL 的符号字形**
（`LV_SYMBOL_*` 在 U+F000 区），所以屏幕键盘的退格/确定/切键盘键全变成空方框。
正确做法是在 `ui_fonts.c` 里给每个角色的 FreeType 字体挂 fallback
（`lv_font_t.fallback` 字段，LVGL 9.5 **没有** `lv_font_set_fallback()` 函数）：

```
FreeType 子集 --fallback--> Montserrat(同尺寸) --fallback--> 内置 CJK 16px
```

这样符号、少见的汉字都能显示，且**全局生效**，不用在键盘上写特例。

### 9.0.5 启动期 `abort()`：插 SD 卡即重启循环（**已修复并上机验证**）

> **一句话根因**：批量 JSON 解析（cet6 词库 546 KB / 833 词 / 约 7000 个 cJSON 节点）把
> ~310 KB 的**内部 RAM 抽干**，而同一时刻正在连网的 WiFi 驱动要一小块**只能来自内部 RAM**
> 的内存（`esp_timer_create()`）→ `ESP_ERR_NO_MEM` → 外面包着 `ESP_ERROR_CHECK` → `abort()`
> → 重启循环。**与锁无关**（见文末「两处被推翻的归因」）。

**真机抓到的 panic 文本（答案就写在里面）**

```
I (5910) TH:app: module 'calendar_service' ready
I (5915) TH:heap: after calendar_service internal    107 KB (largest     61 KB)  psram 11183 KB ...
ESP_ERROR_CHECK failed: esp_err_t 0x101 (ESP_ERR_NO_MEM) at 0x50127178
file: "/IDF/components/esp_timer/src/ets_timer_legacy.c" line 58
func: ets_timer_setfn
expression: esp_timer_create(&create_args, (esp_timer_handle_t*) & (ptimer->timer_arg))
abort() was called at PC 0x2f00f08f on core 0
```

**怎么把它读对（两步，都不难但极易读错）**

1. `abort() was called at PC 0x...` 里的地址是 **`__builtin_return_address(0) - 3`**
   （`esp_libc/src/abort.c:27`），所以 `0x2f00f08f` 的真实调用者是 **`0x2f00f092`**。
   `nm --numeric-sort` 显示 `2f00f072 _esp_error_check_failed` / `2f00f092 esp_system_abort`，
   反汇编则是 `2f00f08e: jal 2f00c898 <abort>` —— **`_esp_error_check_failed` 的末尾在调用
   `abort()`**，也就是 **`ESP_ERROR_CHECK` 失败**。上面那四行 `ESP_ERROR_CHECK failed:` 才是
   全部答案；别被 "abort at PC" 吸引走注意力，否则会跑去猜锁。
2. 寄存器 dump 里的 `RA = 0x2f00f098` **不是**「上下文来自 tick 钩子」：`nm` 显示
   `2f00f098 esp_vApplicationTickHook` 紧跟在 `esp_system_abort` 后面，而 `esp_system_abort`
   的 `sw ra,12(sp)` 保存的正是 `2f00f096: jal panic_abort` 的下一条指令 → RA 恰好等于
   0x2f00f098。**纯粹是函数相邻的巧合**，与 tick 钩子无关。

**完整因果链（每一环都有实测证据）**

1. `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`：`heap_caps_malloc_default()` 把 **≤4096 B**
   的请求**优先**丢进内部堆，只有内部堆满了才回退 PSRAM（`heap_caps.c` 的
   `malloc_alwaysinternal_limit`，由 `esp_psram.c` 用该 Kconfig 调
   `heap_caps_malloc_extmem_enable()` 设置）。
2. cJSON 每个元素 ≈56 B 节点，另外每处键与字符串值各一份拷贝。`cet6.json` 是
   **546 KB / 833 词 / 约 7000 个节点**，于是 `cJSON_Parse()` 运行期间内部 RAM 被抽到接近 0。
   （同一窗口里另外两个大分配不背这个锅：`storage_read_text()` 的 546 KB 缓冲与
   `word_item_t[]` 都 >4096 B，本来就优先走 PSRAM。）
3. 插卡时这条解析路径出现在启动期（词库只存在于卡上，见 §9.5）；同一时刻 WiFi 正在连网，
   `ets_timer_setfn()` → `esp_timer_create()` 要的是
   `heap_caps_calloc(1, sizeof(esp_timer), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)`
   —— **不允许回退 PSRAM**，于是返回 NULL → `ESP_ERR_NO_MEM` → `ESP_ERROR_CHECK` 直接 abort。
4. 拔卡时解析的是内置 32 词的小词库，内部 RAM 不会被抽干 → 启动干净。这就是「只有插卡才崩」。
   「间歇 / 时序相关」则是因为崩溃取决于那一次 WiFi 内部分配是否正好落在解析窗口内；
   本机 3/3 必崩只是这个 ~5.9 s 的窗口每次都重合。

**修复（已实现）**

- 新增 `components/sys_utils/src/sys_json.c`：`sys_json_use_psram()` 用 `cJSON_InitHooks()`
  把 cJSON 的分配改成 **先 PSRAM、失败再回退默认堆**。cJSON 钩子是全局的，本项目所有 JSON
  用户处理的都是批量设备数据（不是协议小帧），也没有任何在 ISR / 关 cache 时解析 JSON 的
  路径。`main/app_main.c` 在最早期（`sys_log_init()` 之后、任何模块之前）调用它。
- `word_service`：`items[]` / `progress[]` 改为显式
  `heap_caps_malloc_prefer/calloc_prefer(..., MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)`；
  并在 `cJSON_Parse()` 返回后加一行 `book tree alive: internal N KB free` 诊断 ——
  这一行就是「解析有没有掏空内部 RAM」的判据。
- **删除**了 `app_main.c` 里那段「启动期预初始化 crypto 锁」的代码：它基于下面被推翻的归因，
  实测从未起作用，留着只会把下一个接手的人带偏。
- `sys_heap_log()` 最后一列由 `esp_get_minimum_free_heap_size()`（被 12 MB PSRAM 主导、
  永远不动）改为 **`int-low`：内部 RAM 的历史最低水位**。这一类 bug 的判据就是它。

**验证（真机，插卡 = 7.4 GB SDHC，`root=/sdcard`）**

| 项 | 修复前 | 修复后 |
|---|---|---|
| 冷启动 ×3 | `abort=3/3`、`TH:app: ready = 0/3` | **`abort=0/3`、`TH:app: ready = 3/3`** |
| 词库 | 解析途中 abort，从未加载成功 | `loaded 833 words from 'words/cet6.json' (120311 B)` 3/3 |
| 解析中（JSON 树存活时）内部 RAM | 被抽到接近 0 | **86 KB 空闲 / 最大连续块 40 KB** |
| 启动结束 | — | 9 个模块全 ready、`storage=/sdcard`、`int-low 55 KB` |
| 端到端 | — | `words/stats` → `book=cet6 total=833`；`wifi/status` → connected；`POST+GET wifi/scan` → 22 个 AP 且设备存活 |

#### 两处被推翻的归因（保留作为教训）

1. **`locks.c:77` 不是 `assert(lock->owner == LOCK_FREE)`**：本机 IDF v6.1 的
   `esp_libc/src/locks.c:77` 恰恰就是 `lock_init_generic()` 里的
   `abort(); /* No more semaphores available or OOM */` —— **它本来就是 OOM 分支**；而那段
   惰性初始化全程由 `lock_init_spinlock` 保护，**不存在「两个上下文同时首次 acquire」的竞态**。
   当年那两条「线索」其实指向同一个结论：**内存不够，不是锁**。
   （`esp_crypto_lock.c:39` 更是数据段里静态锁变量的地址，根本不是返回地址。）
2. **panic 栈里每个 32 位字都要先判断「代码地址还是数据」**：本次栈里的 `0x2f0203f8` 其实是
   `nm` 里的 `b s_log_cache`（数据），`0x5018xxxx` 是 `.flash.text`（WiFi 库代码），
   `0x502d2068` 之类是数据指针。逐个地址单独跑 `riscv32-esp-elf-addr2line -f -C` 之后，
   **还要拿 `nm --numeric-sort` 核对它是否落在某个函数的地址区间内**，否则就会像上面那样把
   「函数相邻」读成「调用关系」。

抓现场的完整流程（下次照抄）：

```powershell
# 1) 抓含 panic 的启动日志（会复位一次）
python tools\monitor_capture.py --port COM34 --seconds 26 --out build\boot.log
# 2) 逐个地址单独解析（不要用 -pfiaC 两行一取：内联会输出多行，配对会静默错位）
riscv32-esp-elf-addr2line -f -C -e build\todohelper.elf 0x2f00f08f 0x2f00f092
# 3) 用符号表核对地址落在哪个函数区间（区分代码 / 数据）
riscv32-esp-elf-nm -C --numeric-sort --defined-only build\todohelper.elf > build\nm_all.txt
```

### 9.1 三条**不可删**的 `sdkconfig.defaults` 配置

1. **`CONFIG_LOG_MAXIMUM_LEVEL_VERBOSE=y`（且 `LOG_MAXIMUM_EQUALS_DEFAULT=n`）**
   原因：esp32s31 + `CONFIG_SPIRAM_XIP_FROM_PSRAM` 下，当 `LOG_MAXIMUM_LEVEL == LOG_DEFAULT_LEVEL`
   时 IDF 会把 `ESP_LOGD/ESP_LOGV` 的格式串放进 `.rodata_wlog_*` 段，而生成的 `sections.ld`
   把这两个段排在 `_rodata_reserved_end` **之后**，于是紧随其后的 `.ext_ram.dummy` 段必须把
   位置计数器往回移，链接直接失败：
   ```
   sections.ld:1280 cannot move location counter backwards (from 5022b8f0 to 50227480)
   collect2.exe: error: ld returned 1 exit status
   ```
   提高到 VERBOSE 后这些串留在普通 rodata，链接正常。**运行时输出不变**（`LOG_DEFAULT_LEVEL`
   仍是 INFO），额外好处是可以运行时用 `sys_log_set_level()` 把单个 TAG 提到 DEBUG。

2. **`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`** —— 见 §9.0 第 2 条。改回默认 16384 会
   让 WiFi 吃掉几乎全部内部 RAM，然后 mDNS/AI/I2S/SD 全部失败。

3. **FreeType + 32 KB 渲染栈**（`LV_USE_FREETYPE` / `ESP_LVGL_ADAPTER_ENABLE_FREETYPE` /
   `..._SMALL_RENDER_POOL` / `..._MINIMAL_BUILD` / `TODOHELPER_FONT_FREETYPE` /
   `TH_BSP_DISPLAY_TASK_STACK = 32 KB`）
   `SMALL_RENDER_POOL` 把 FreeType 渲染池压到 4 KB 并去掉 LVGL 的 32 KB 绘制栈
   **构建期检查**；`MINIMAL_BUILD` 去掉用不到的字体驱动与压缩流支持。
   **改成 16 KB 栈会随机 panic**（§9.0 第 4 条），而关掉 `SMALL_RENDER_POOL` 会因
   绘制栈检查而链接失败——两项必须与 32 KB 栈配套。

### 9.2 素材与字体

- **`assets/ui/` 必须是扁平目录**：`esp_mmap_assets` 的打包脚本用 `os.listdir()`
  只扫一层，且只打包 `MMAP_FILE_SUPPORT_FORMAT` 里列出的后缀。所以天气图标命名为
  `w_sunny.png` 这种"前缀分类"形式。**字体（.ttf）不能放这里**，因为 `.ttf` 不在支持
  后缀里。
- 增删素材后镜像可能不刷新，执行一次 **`idf.py reconfigure`**。
- 字体解析链（`ui_core/src/ui_fonts.c`）：
  1. `<storage>/todohelper/fonts/ui_cjk.ttf`——**存在即用**（>1 KB 才认），
     所以放一个自己的 TTF 进去就能换字体，无需重新烧录；
  2. 不存在则把内嵌子集写进去再用（首次启动多花约 1~2 秒写 1.2 MB）；
  3. 以上全部失败（例如没有存储）→ 回退 LVGL 内置 Source Han Sans SC 16 px，
     **只有约 2500 字，罕见汉字会是方框**，日志会明确警告。
- 重新生成子集：`tools/gen_cjk_font.py`（默认 GB2312 一级 3755 字，`--level2` 6763 字）。

### 9.3 硬件约束

- **GPIO20~25 与 SPI NAND flash 脚位复用，同一块板只能装其中一个**：用 SD 卡就不能用 SPI
  NAND，反之亦然。这决定了字体/词库/素材放 SD 还是片内分区。
- 屏幕与触摸**不**占用 GPIO20~25（RGB 并口 + I2C GPIO0/1），冲突对象只有 SPI NAND。
- **SD 卡实测可用**：`SD16G`/SDHC、4-bit、40 MHz，挂载在 `/sdcard`，
  应用目录 `/sdcard/todohelper/`（实测根目录只有 `System Volume Information` 和
  `todohelper`，说明 VFS 会剥掉挂载点前缀，卡上不会多出一层 `sdcard/`）。
- **无 RTC**：断电后时间不保持，`sys_time_ts()` 在 SNTP 成功前返回 **0**，
  UI 必须显示"时间未同步"（需求 FR4）。
- **无 GPS / 无温度传感器**：天气全部来自网络 API，定位只能"手动城市 / IP 定位"。
- **背光脚为 NC**：亮度用 `lv_layer_top()` 上的黑色遮罩做软件调光（已去掉 CLICKABLE，
  变暗后触摸仍可用）。

### 9.5 SD 卡目录结构（`/sdcard/todohelper/`）

存储根由 `storage_manager` 决定：优先 SD（`/sdcard`），失败则片内 LittleFS（`/littlefs`）。
应用数据一律在 **`<root>/todohelper/`** 下（VFS 会剥掉挂载点前缀，所以卡上就是根目录下的
一层 `todohelper/`，实测卡根只有 `System Volume Information` 和 `todohelper`，
不会多出一层 `sdcard/`）。

首次启动由 `storage_prepare_tree()` 自动创建，**不需要手动准备任何目录**：

```text
<卡根>/todohelper/
├─ fonts/
│   └─ ui_cjk.ttf        # 1.2 MB，首次启动从固件内嵌子集写出。
│                        # ★ 放自己的 TTF 覆盖同名文件即可换字体（>1 KB 才被采用）
├─ images/weather/       # 预留：SD 覆盖天气图标（当前图标在 assets 分区里）
├─ anim/idle_face/       # 预留：位图版待机动画（当前纯程序化绘制，不需要）
├─ words/
│   ├─ cet4.json         # 内置 32 词示例，首次启动写出，可编辑替换
│   └─ progress.json     # 复习进度
├─ ai/
│   ├─ rec_<uptime>.pcm  # 录音（16 kHz/16-bit/单声道/无头），Audacity 按 Raw 导入
│   └─ history.json      # AI 对话历史（受 ai_service 的 save_history 控制）
├─ tts/                  # 预留：TTS 缓存
├─ cache/
│   ├─ todos.json        # 待办（原子写：先写 .tmp 再 rename）
│   └─ weather.json      # 天气缓存（断网降级用，带 stale 标记）
├─ backup/               # 预留：todos_*.json 滚动备份
└─ logs/                 # 预留
```

**要点**

- 卡只要 FAT32 且能挂载即可，容量不敏感（实测 SD16G/SDHC、4-bit、40 MHz 可用）。
- 换中文字体：把任意 `.ttf`（建议含常用汉字、体积越小越好）放到
  `todohelper/fonts/ui_cjk.ttf`，重启即生效，**不需要重新编译烧录**。
- 换词库：替换 `todohelper/words/cet4.json`（格式见 `docs/API.md` 的 word 章节）。
- 拔卡也能跑：回退片内 LittleFS（`storage` 分区 2 MB）；再失败则"无持久化"运行，
  状态栏会显示存储状态。

### 9.6 其它

- **不要修改** `..\common_components\esp32_s31_korvo`（工程外，被其它示例共用）。
  需要覆盖 LVGL 适配器配置时，用"抢先 `esp_lv_adapter_init()`"的手法（BSP 只在适配器
  未初始化时才自己初始化），见 `th_bsp.c` 里 `stack_in_psram` 的用法。
- 改动 `managed_components/` 下任何文件，都要把 diff 追加进 `patches/` 并在本节登记。
- `sdkconfig` 是生成物；改默认配置请改 `sdkconfig.defaults`（改完删 `sdkconfig` 再 build
  才会从默认值重新生成）。
- 重新设目标若提示 preview，用 `idf.py --preview set-target esp32s31`；本工程 `sdkconfig`
  已就绪，正常无需重设。
- 构建产物与日志都在 `build/`，体积大；不要把 `build/` 内容当源码提交。

---

### 9.7 剩余待办（下一轮从这里开始）

> **已完成**（原先列在这里的两项）：
> - ~~把 1.2 MB 字库移出 app~~ —— 已做：app 4.16 MB → 2.94 MB，烧录 43.6 s → 31.6 s。
>   字体改为只从 `<storage>/todohelper/fonts/ui_cjk.ttf` 加载；`ui_core/CMakeLists.txt`
>   的注释写了如何恢复内嵌（需与 `ui_fonts.c` 的写入分支同时恢复，否则链接失败）。
> - ~~删除天气页不可达的输入浮层代码~~ —— 已做：`page_weather.c` 里
>   `lv_keyboard_create` / `s_input_overlay` / `s_input_area` / `input_*_cb` 全部清除，
>   全工程只剩 `page_common.c` 的 `ui_keyboard_create()` 在创建键盘。
>
> **原"新增 L0 `SYS_EVENT_TIME_SYNCED`"已降级为可选**：`weather_task` 现在会等时钟
> 最多 75 秒再发 IP 定位请求，已经覆盖了"SNTP 比启动晚"这个场景。只有 SNTP 超过
> 75 秒才需要它。要做的话仍是三步：`sys_utils.h` 加事件 → `calendar_service.c` 的
> `sntp_sync_cb()` 里额外 post → `weather_service.c` 订阅并入队 `WEATHER_REQ_DETECT`。

**① 待机眼睛动画** —— ✅ **已实现并两次加密**（表情交叉淡入 + 全随机节奏 + 慢动作三层，
仍**只要眼睛、零位图、零新对象**）。设计见 `components/idle_face/README.md`。

- **基础层**：`face_transition_t` 做 180/420 ms 的 `smoothstep` 交叉淡入（从"当前屏幕真实值"
  起步，中途被打断不会跳变）；眨眼 2.6~7.5 s / 110~165 ms、18% 连眨、**28% 只眯一半**、
  9% 慢眨 240~380 ms；左右看 4~12 s（**22% 立刻往另一侧再看一眼**）；微扫视 1.8~5.2 s；
  呼吸 12~19 s 每周期重抽。
- **1.1 慢动作三层**（解决"看一分钟就那几下"）：① **自发表情**每 13~34 s 随机演一个
  320~1100 ms 的表情（HAPPY/SLEEPY/LISTENING/THINKING/SPEAKING，优先级
  `眨眼 > 通知/AI 表情 > 自发表情 > 基础状态`）；② **困倦度** 0→1（约 2.5 分钟攒满：
  眨眼更密更慢、眼睛矮 10%、视线下垂 5 px，任何真实交互立刻清零，**自发表情不清零**）；
  ③ **瞳孔缩放**（5~11 s 重抽 0.91~1.14）与**整脸 ±3/±2 px 缓慢漂移**（表情占用时缓动归零）。
- 内存：全部只改已有对象属性，**无分配**；实测 `int-low` 未变（55 KB）。

> **"待机保持清醒"是刻意的设计决定。** 初版让眼睛闭合成 SLEEPY 姿态并整个待机期保持，
> 那会**抑制眨眼/扫视 —— 即让待机画面动画变少**，与"丰富待机动画"的要求相反。所以
> `idle_face_standby_enter()` 现在只做记账（用一个常量 1.0 包络保留 `begin_motion()` 调用，
> 以免触发 `-Werror=unused-function`），脸部保持 NORMAL、全部动画照常跑，靠困倦度慢慢"沉"。
> 想要"睡觉的脸"，同时改这三行：
> `s_state = IDLE_FACE_SLEEPY;` / `begin_motion(now, IDLE_FACE_SLEEP_MS, 1.0f, 0.0f);` /
> `s_current = *params_for(IDLE_FACE_SLEEPY);`

**用户已确认基础层观感正常（2026-09-23），1.1 的三层慢动作与退出过场/交接淡入也已确认**
（"可以了"）。三个已修的坑（都在 1.1）：
1. `idle_face_standby_exit()` 以前要求"处于 SLEEPY 姿态"，而 `standby_enter()` 已不再进入
   SLEEPY → 该函数**恒返回 `ESP_ERR_INVALID_STATE`**，属于死代码。现在只要求没有有效表情占用；
2. 动画器定时器是**页面创建时**才起的，所以第一次进待机可能看到一个几分钟前就该过期的
   临时表情（实测 t=7.7 s 的 2.5 s `wifi_connected` 表情在 t=82 s 仍被当成有效，
   **首次进待机被静默拒绝**）。现在 `standby_enter/exit` 都先调 `override_expire_due()`。
3. 退出过场**现在真的可见**：`ui_core` 的切页改成交叉淡入，离场页面淡出 240 ms 后才隐藏
   （以前同一帧就 `HIDDEN`，一帧都渲染不出来）。

**② 单词书 cet4 → cet6** —— ✅ **已完成**。`WORD_DEFAULT_BOOK` 已改为 `"cet6"`
（`word_service.h`）。卡上是 `words/cet6.json`：**833 条自编精选**（BrE 音标 + 简体释义 +
例句），**不是**官方约 2000 词表，不做完整性承诺；仓库副本在 `assets/words/cet6.json`，
与卡上文件 sha256 一致（`3B2277…`）。833 条 × ~656 B ≈ **546 KB**，走 `MALLOC_CAP_SPIRAM`
（`WORD_MAX_ITEMS` 上限 2000，接近上限时要注意 PSRAM 占用）。

> **已存在的设备必须切书**：NVS 里存的是书名，`WORD_DEFAULT_BOOK` 只对空 NVS 生效，
> 所以要 `POST /api/v1/words/config {"book":"cet6"}`，否则仍读 `cet4`。
> **顺带修掉一个串味 bug**：`progress_key()` 原本生成裸 `"w<id>"`，而每本书的 id 都从 1 开始，
> CET4 的复习进度会套到 CET6 的前 32 个词上（且加载时会误报"这本书还没有进度"）。
> 现已改为 `"<book>#w<id>"`。

**其余已知但优先级低的问题**

- **IP 定位在本机网络不可达**：时钟已同步时 `POST /api/v1/weather/locate` 仍失败
  （返回 502，语义已正确）。已确证是设备（Windows 热点后）出网到 `ipapi.co` 不通。
  换服务商**无需改代码**：写 NVS 键 `geo_url` 即可（解析器兼容
  `latitude/lat`、`longitude/lon/lng` 与嵌套 `location`）。
- **`update_ts` 可能为 0**：表示最近一次抓取时间，城市刚改过、尚未重新抓取时为 0
  （`server_ts` 正常）。

## 10. 排错速查

| 症状 | 处理 |
|---|---|
| `fatal error: esp_async_fbcpy.h: No such file` | §7 补丁被覆盖，重新 `patch -p1` 并 `idf.py build` |
| `sections.ld:1280 cannot move location counter backwards` | `sdkconfig.defaults` 的 `LOG_MAXIMUM_LEVEL_VERBOSE` 丢了，见 §9.1 |
| `Failed to resolve component 'json'` | IDF v6.1 无内置 json；改成 `cjson` 并声明 `espressif/cjson` |
| `ESP_ERR_NO_MEM` 但日志说 `free heap: 10 MB` | **内部 RAM 碎片化**，看"最大连续块"；见 §9.0 第 2 条 |
| 插卡启动就 `ESP_ERROR_CHECK failed: ESP_ERR_NO_MEM`（`ets_timer_setfn` / `esp_timer_create`），拔卡正常 | 批量 JSON 解析把内部 RAM 抽干，**不是锁**；确认 `sys_json_use_psram()` 被调用；见 §9.0.5 |
| 网页显示"设备不可达/读取失败"，但设备 `ping` 得通、串口也正常 | 有 SSE / 流式 handler 占住了 httpd 单任务（v6.1 的 handler 串行执行）；见 §9.0.1 最后一条 |
| `TH:heap:` 的 `int-low` 掉到个位数 KB | 有瞬时峰值抽干过内部 RAM（旧版是 546 KB 词库的 cJSON 解析）；见 §9.0.5 |
| 设备 ping/HTTP 时通时断，但设备自称 `link: connected rssi=-3x` | Windows 移动热点丢包，不是设备问题；先 `ping` 看丢包率，见 HANDOVER §3 需求 1 |
| `wifi:alloc net80211 funcs fail` | 同上，`SPIRAM_MALLOC_ALWAYSINTERNAL` 太大 |
| `mdns: failed to allocate task stack` | 打开 `MDNS_TASK_CREATE_FROM_SPIRAM` + `MDNS_MEMORY_ALLOC_SPIRAM` |
| `i2s_alloc_dma_desc` / `allocate_dma_buf` 失败 | 内部 RAM 最大连续块不足；见 §9.0 第 1、2 条 |
| **随机 panic**（Load access fault / misaligned / Stack protection） | LVGL 渲染栈太小，FreeType 需 ≥32 KB；见 §9.0 第 4 条 |
| 部分汉字是方框、部分是好的 | 走了内置字库（只有约 2500 字）；FreeType/字体文件没加载成功，见 §9.0 第 3 条 |
| `FreeType font initialization failed` | 用了内存模式；v9 只支持文件路径，见 §9.0 第 3 条 |
| `mkdir ... failed (errno 22)` | FatFs 用 `FR_INVALID_NAME` 表示"已存在"；改用 `stat()` 判断 |
| main 任务卡死 / `task_wdt: IDLE0` | 有 LVGL 调用没持锁；见 §9.0.1 |
| 中文显示成方框 | 内嵌字体没生成/没重新构建；跑 `tools/gen_cjk_font.py` 后 `idf.py build` |
| 天气图标不显示 | `assets/ui/` 里有子目录，或改了素材没 `idf.py reconfigure` |
| `idf.py: command not found` | 未加载 IDF 环境，见 §4 |
| `Could not open COMxx ... busy` | 关掉占用串口的监视器/其它工具；确认用的是 COM34 |
| 组件管理器重新下载依赖 | 打补丁后 `idf.py reconfigure && idf.py build` |
| 需要从零复现 | `idf.py fullclean` → 应用补丁 → `idf.py build` |
