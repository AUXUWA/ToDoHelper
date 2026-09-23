# AGENT.md

面向在本仓库工作的 AI/开发者的工作说明：**当前已实现的功能**、本机环境、常用命令、
必须保留的补丁与配置、验证基线、排错速查。上游 README 的概述不再重复，这里只写
「实际是什么状态、怎么在这里把事做成」。

> **本仓库已从 Korvo 产测固件（factory_demo）重构为 ToDoHelper 桌面助手。**
> 需求见 `docs/TODOHELPER_REQUIREMENTS.md`，框架设计见 `docs/TODOHELPER_FRAMEWORK.md`，
> 组件接口与 HTTP API 见 `docs/API.md`，**交接说明见 `docs/HANDOVER.md`**。
> 优先级：**代码实际行为 > 本文件 > 框架设计 > 需求文档**；发现不一致时以左侧为准并回改右侧。

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
  app_main.c              表驱动模块编排（12 个模块），无可选模块失败阻塞 UI
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
| 板子串口 | **COM34** = Silicon Labs CP210x = ESP32-S31 板（MAC `1c:29:04:d0:71:b6`） |
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
├─ main/app_main.c             # 模块编排表（12 个模块）
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

> ⚠️ **注意：截至当前，本工程从未烧录到真机、从未上过屏。** 以下仅是**编译基线**，
> 运行期行为全部未验证。第一次上机时请按 `docs/HANDOVER.md` §5 的清单逐项确认。

日志出现：

```
Project build complete.
```

**ToDoHelper 当前基线**（可作回归对比基准）：

| 项 | 值 |
|---|---|
| app bin | `build/todohelper.bin`，`0x3fc2c0` 字节（约 4.17 MB，含 1.2 MB 内嵌中文字体） |
| app 分区余量 | `0x800000` 分区，剩 **50%** |
| bootloader | `build/bootloader/bootloader.bin`，`0x5a80`，余 6% |
| 编译警告 | 0（`-Werror` 全开） |
| 资源分区 | `build/mmap_build/ui/assets/assets.bin`（8 个 PNG，约 8.9 KB） |
| 组件数 | 16 个自有组件 + 1 个本地覆盖组件（`esp_cam_sensor`） |

烧录成功判据：每个镜像后出现 `Hash of data verified.`，结尾 `Hard resetting via RTS pin...` + `Done`。

烧录地址（`idf.py flash` 自动处理）：
`0x2000` bootloader、`0x8000` 分区表、`0x10000` app、`0x810000` storage(littlefs)、`0xa10000` assets。

---

## 9. 注意事项与坑

### 9.1 两条**不可删**的 `sdkconfig.defaults` 配置

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

2. **FreeType 相关四项**（`LV_USE_FREETYPE` / `ESP_LVGL_ADAPTER_ENABLE_FREETYPE` /
   `..._SMALL_RENDER_POOL` / `..._MINIMAL_BUILD` / `TODOHELPER_FONT_FREETYPE`）
   `SMALL_RENDER_POOL` 把 FreeType 渲染池压到 4 KB 并去掉 LVGL 那个"32 KB FreeType 绘制栈"
   的构建期检查；`MINIMAL_BUILD` 去掉用不到的字体驱动与压缩流支持。关掉它们会因为
   绘制栈检查或栈不足而链接/运行失败。

### 9.2 素材与字体

- **`assets/ui/` 必须是扁平目录**：`esp_mmap_assets` 的打包脚本用 `os.listdir()`
  只扫一层，且只打包 `MMAP_FILE_SUPPORT_FORMAT` 里列出的后缀。所以天气图标命名为
  `w_sunny.png` 这种"前缀分类"形式。**字体（.ttf）不能放这里**，改用
  `idf_component_register(EMBED_FILES ...)` 内嵌（见 `ui_core`）。
- 增删素材后镜像可能不刷新，执行一次 **`idf.py reconfigure`**。
- 字体解析顺序（`ui_core/src/ui_fonts.c`）：`<storage>/todohelper/fonts/ui.ttf` →
  内嵌 `ui_cjk.ttf` → LVGL 内置 Source Han Sans SC 16 → Montserrat。**任何情况下都有中文字形**。

### 9.3 硬件约束

- **GPIO20~25 与 SPI NAND flash 脚位复用，同一块板只能装其中一个**：用 SD 卡就不能用 SPI
  NAND，反之亦然。这决定了字体/词库/素材放 SD 还是片内分区。
- 屏幕与触摸**不**占用 GPIO20~25（RGB 并口 + I2C GPIO0/1），冲突对象只有 SPI NAND。
- **无 RTC**：断电后时间不保持，`sys_time_ts()` 在 SNTP 成功前返回 **0**，
  UI 必须显示"时间未同步"（需求 FR4）。
- **无 GPS / 无温度传感器**：天气全部来自网络 API，定位只能"手动城市 / IP 定位"。
- **背光脚为 NC**：亮度用 `lv_layer_top()` 上的黑色遮罩做软件调光（已去掉 CLICKABLE，
  变暗后触摸仍可用）。

### 9.4 其它

- **不要修改** `..\common_components\esp32_s31_korvo`（工程外，被其它示例共用）。
- 改动 `managed_components/` 下任何文件，都要把 diff 追加进 `patches/` 并在本节登记。
- `sdkconfig` 是生成物；改默认配置请改 `sdkconfig.defaults`（改完删 `sdkconfig` 再 build
  才会从默认值重新生成）。
- 重新设目标若提示 preview，用 `idf.py --preview set-target esp32s31`；本工程 `sdkconfig`
  已就绪，正常无需重设。
- 构建产物与日志都在 `build/`，体积大；不要把 `build/` 内容当源码提交。

---

## 10. 排错速查

| 症状 | 处理 |
|---|---|
| `fatal error: esp_async_fbcpy.h: No such file` | §7 补丁被覆盖，重新 `patch -p1` 并 `idf.py build` |
| `sections.ld:1280 cannot move location counter backwards` | `sdkconfig.defaults` 的 `LOG_MAXIMUM_LEVEL_VERBOSE` 丢了，见 §9.1 |
| `Failed to resolve component 'json'` | IDF v6.1 无内置 json；改成 `cjson` 并声明 `espressif/cjson` |
| 中文显示成方框 | 内嵌字体没生成/没重新构建；跑 `tools/gen_cjk_font.py` 后 `idf.py build` |
| 天气图标不显示 | `assets/ui/` 里有子目录，或改了素材没 `idf.py reconfigure` |
| `idf.py: command not found` | 未加载 IDF 环境，见 §4 |
| `Could not open COMxx ... busy` | 关掉占用串口的监视器/其它工具；确认用的是 COM34 |
| 组件管理器重新下载依赖 | 打补丁后 `idf.py reconfigure && idf.py build` |
| 需要从零复现 | `idf.py fullclean` → 应用补丁 → `idf.py build` |
