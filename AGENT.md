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
> **当前版本 `1.14.0`**（工程根目录 `version.txt`；规则与发布步骤见 §8.1）。

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
| 天气（Open-Meteo，**免 API Key**）+ 城市地理编码 + IP 定位 + 定时刷新 + 缓存降级 + 状态栏城市/天气/温湿度 | ✅ |
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
  maimemo_client/     L5  墨墨背单词开放 API（方案 A 同步 / B 助记例句 / C 反向推词，可选降级）
  debug_shell/        L4  **调试 shell**（`/api/v1/debug/exec`，默认关，见 §9.0.8）
  esp_cam_sensor/     —   本地组件，覆盖托管组件（BSP 需要）
main/
  app_main.c              表驱动模块编排（11 个模块，全部 after_ui），无可选模块失败阻塞 UI
assets/ui/                扁平 PNG 资源（w_*.png），打包进 assets 分区
tools/gen_cjk_font.py     生成内嵌 CJK 子集字体（含 IPA 字形合并 + `validate_font()` 校验）
tools/build.sh            从 Git-Bash 驱动 idf.py 的包装器（配 tools/build.ps1，见 §4.2）
tools/dsh.sh              发一条 debug_shell 命令并打印文本输出（自动处理令牌轮换，见 §9.0.8）
tools/dsh_debug_probe.sh  取设备当前 HTTP 鉴权令牌（先查日志，再读 NVS）
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
`MSys/Mingw is no longer supported` 警告（`MSYSTEM` 被注入子进程）并**拒绝运行**。
**构建走 PowerShell，或在 Git-Bash 里用 `bash tools/build.sh`**（v1.3.0 新增的包装器，
见 §4.2）。

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

**首选（v1.3.0 新增）**：`tools/build.sh` 把活交给原生 powershell 并**原样回传退出码**。

```bash
cd /d/project/ESP-IDFv6.1/ToDoHelper
bash tools/build.sh build          # 等价 idf.py build（默认动作就是 build）
bash tools/build.sh flash          # 等价 idf.py flash
bash tools/build.sh -v build       # 详细构建
bash tools/build.sh --version      # 只验证调用链是通的
```

脚本名之后的所有参数**原样转发**给 `idf.py`。输出同时重定向到 `build_last.log`
（已 gitignore），因为**原生 exe 不能接进 cmdlet 管道**（见下）。退出码是 `idf.py` 自己的，
所以可以直接 `if bash tools/build.sh build; then ... fi`。

手工等价写法（不装脚本时）：

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

#### 4.2.1 从 MSYS 驱动 `idf.py` 的三个坑（都会**静默失败**：空日志 + 退出码 0）

v1.3.0 加 `tools/build.sh` 时踩出来的。三个坑的共同症状是
**日志文件是空的、退出码却是 0** —— 看起来像"构建成功且没输出"，实际什么都没跑。
这类失败最难查，因为唯一的证据（退出码）在撒谎。

1. **`idf.py` 在 MSYS 下被明确拒绝**：ESP-IDF 的 python wrapper 检测到 MSYS 环境直接退出，
   打印 `MSys/Mingw is no longer supported`。所以必须经过原生 `powershell.exe`。
2. **profile 里的 `idf.py` 是函数，不是命令**：IDF 的 PowerShell profile 把 `idf.py` 定义成
   `Invoke-idfpy` 的**函数**，用 `@args` 转发参数。**这个简写不绑定**当调用方是
   `powershell -Command` / `-File` 时 —— 于是 `idf.py build` **静默什么都不做**。
   → 必须显式调用解释器：`& $python $idfPy @idfArgs`，其中
   `$python = C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe`、
   `$idfPy = D:\ESP-IDF\v6.1\esp-idf\tools\idf.py`。
3. **原生 exe 不能接进 cmdlet 管道，`env -i` 会杀掉一切输出**：
   - `& $python ... | Tee-Object -FilePath x.log` 报
     `Cannot run a document in the middle of a pipeline: ...python.exe`。
     → 用普通重定向（`*>`）落盘，再把内容读回来打印。
   - 曾经为了"隔离环境"用 `env -i` 起 powershell：powershell 正常启动并打印横幅，
     但**原生 python 子进程一点输出都没有**，且 `$LASTEXITCODE` 保持 `$null`
     → **空日志 + 脚本仍然 exit 0**。
     → **保留 bash 继承的环境**，隔离靠上面的 `Remove-Item Env:MSYSTEM` 就够了。
4. **`MSYSTEM` 必须在 PowerShell 里删**：在 bash 里 `unset MSYSTEM` **毫无用处** ——
   MSYS 运行时会把它（连同 `OSTYPE` 等）**重新注入每一个原生子进程**，
   所以 `powershell.exe` 永远继承得到。只有 Windows 侧删除才真的生效。
   注意 profile 要在删除**之后**加载，这样它设的 IDF 环境不受影响。

> **判据**：如果 `bash tools/build.sh build` 的日志是空的，几乎必然是上面四条之一，
> 而不是"构建太快没输出"。`bash tools/build.sh --version` 是最快的调用链自检。

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

**脚本末尾会自动跑 `validate_font()`**（v1.3.0 新增）：重新打开保存后的文件，逐表核对
`hmtx` / `vmtx` 的条目数与 `maxp.numGlyphs`、`numberOf*Metrics` 是否越界、以及必需的内容
字符是否都在 cmap 里。**校验不过就直接让构建失败**，不再出现"生成成功但字体是坏的"。
动机见 §9.0.4.1。

> **注意：正式部署字体的路径是"拷文件"，不是重新烧录。** 字体在运行期从
> `<storage>/todohelper/fonts/ui_cjk.ttf` 加载（§9.2），**没有烘进固件镜像** ——
> 所以改完字库只要把新文件拷到卡上覆盖同名文件、重启即可生效。
> 重新烧录只在你想更新内嵌的兜底副本时需要。

---

## 5. 关键文件

```
ToDoHelper/
├─ CMakeLists.txt              # EXTRA_COMPONENT_DIRS 指向 ../common_components；project(todohelper)
├─ partitions.csv              # nvs 0x9000/0x6000, otadata 0xf000/0x2000, phy_init 0x11000/0x1000,
│                              # ota_0 0x20000/0x3e0000, ota_1 0x400000/0x410000,
│                              # storage(littlefs) 0x810000/0x200000, assets(spiffs) 0xa10000/0x5f0000
│                              # （v1.11.0 起是两个 app 槽位，storage/assets 的偏移量没动）
├─ sdkconfig.defaults          # 真正的默认配置来源（**含两条不可删的必需项**，见 §9）
├─ sdkconfig                   # 生成物，不要手改
├─ dependencies.lock           # 组件管理器锁文件（target: esp32s31）
├─ build_last.log              # tools/build.sh 的构建日志（生成物，已 gitignore）
├─ main/app_main.c             # 模块编排表（10 个模块，全部 after_ui）
├─ components/                 # 见 §2
├─ assets/ui/                  # 扁平 PNG（w_*.png）；**不能有子目录**
├─ tools/gen_cjk_font.py       # CJK 子集字体生成脚本（含 IPA 合并与产物校验）
├─ tools/build.sh / build.ps1  # Git-Bash → idf.py 的包装器（见 §4.2）
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

> **v1.3.0 注意**：下表的 `todohelper.bin` 是**离线构建产物**的尺寸 ——
> 本轮**只构建、没有烧录**（详见 `CHANGELOG.md` 1.3.0 的 `### Verified（真机）`）。
> 表中的内部 RAM 数字仍是 **v1.2.0 真机实测**的，v1.3.0 未在设备上复核。

| 项 | 值 |
|---|---|
| app bin | `build/todohelper.bin`，`0x2e4fd0` 字节（约 2.90 MB，含 `1.3.0` 版本号；字库已移出到 SD/`storage`，见 §9.7） |
| app 分区余量 | `0x800000` 分区，剩 **64%** |
| bootloader | `build/bootloader/bootloader.bin`，`0x5a80`，余 6% |
| 编译警告 | 0（`-Werror` 全开） |
| 资源分区 | `build/mmap_build/ui/assets/assets.bin`（8 个 PNG，约 8.9 KB） |
| 组件数 | 17 个自有组件 + 1 个本地覆盖组件（`esp_cam_sensor`） |

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

> **v1.2.0 现行基线**（2026-09 流畅度优化后实测，同一张卡）：`boot complete`
> **internal 66 KB / largest 22 KB / int-low 54 KB**，`storage=/sdcard`。与上表基本一致，
> 说明「异步化 + 脏检查」没有吃掉内部 RAM（新增任务都是按需创建、平时不占栈）。

> 每一行由 `sys_heap_log()` 在 `app_main` 里输出。**排障时看「最大连续块」而不是总空闲量**：
> 首轮上机出现的全部 `ESP_ERR_NO_MEM` 都是内部 RAM 碎片化造成的——总空闲看着还有十几 KB，
> 但最大连续块是 0，于是 WiFi / mDNS / I2S DMA / SDMMC DMA 全线失败。
>
> **还要看第 4 列 `int-low`**（内部 RAM 的历史最低水位，2026-09 由 `esp_get_minimum_free_heap_size()`
> 这个被 PSRAM 主导、永远不动的值改过来）。它掉到个位数 KB 就说明**某个瞬时峰值把内部 RAM
> 抽干过** —— 那正是插卡启动崩溃的判据（§9.0.5），而当时的空闲量看起来还是健康的 107 KB。

烧录成功判据：每个镜像后出现 `Hash of data verified.`，结尾 `Hard resetting via RTS pin...` + `Done`。

烧录地址（`idf.py flash` 自动处理）：
`0x2000` bootloader、`0x8000` 分区表、`0x20000` app 槽位 ota_0（`0x400000` 是 ota_1）、
`0x810000` storage(littlefs)、`0xa10000` assets。
**v1.11.0 起 app 有两个槽位**（OTA），槽位比原来那个 8 MB 的 factory 分区小得多
（现在是 0x3E0000 = 3.97 MB，当前固件 3.28 MB，余量不多）——
**固件涨过 3.9 MB 就装不下了**。

### 8.1 版本号与发布（`version.txt`）

固件版本来自工程根目录的 **`version.txt`**。ESP-IDF 的取值优先级是
`PROJECT_VER` 变量 → **`version.txt`** → `git describe`（本工程没设 `PROJECT_VER`，
`sdkconfig` 里 `CONFIG_APP_PROJECT_VER_FROM_CONFIG` 也未开，所以 `version.txt` 是唯一来源）。
**不写这个文件时 IDF 会退回 `git describe`**，设备就会自报 `cfdf817-dirty` 这类字符串 ——
改一行代码就变，"板子上到底是哪个固件"完全看不出来（1.0.0 之前就是这样）。

- **当前版本：`1.14.0`**
- 版本进 `esp_app_desc_t`，于是**三处可见**：
  设备「设置」页顶部与开机页「固件 1.3.0 · IDF v6.1」、`GET /api/v1/device/info` 的 `version`、
  以及 `build/todohelper.bin` 的 app descriptor（离线核对：
  `riscv32-esp-elf-objdump -s -j .flash.appdesc build/todohelper.elf`）。
- 规则（三段式）：**MAJOR** = 一轮需求全部完成、或有破坏性变更（存储格式 / HTTP API /
  NVS 键语义）；**MINOR** = 新增功能或页面；**PATCH** = 修 bug、调参、文档。
- 发布动作：改 `version.txt` → `idf.py build`（它在 `CMAKE_CONFIGURE_DEPENDS` 里，会自动触发
  reconfigure）→ 烧录并确认 `device/info` → 写 `CHANGELOG.md` → `git tag -a vX.Y.Z -m "..."`。

#### 版本号是**对外契约**，不是进度计数器（别乱跳）

> **这一条是踩过坑才写的**：v1.6.0 之后的一个会话里连发了两轮
> 「性能优化」（v1.7.0、v1.8.0），两轮都各自吃了一个 MINOR。
> 按下面这套判据，它们其实**都是 PATCH** —— 存储格式没变、HTTP API 没变、
> NVS 键语义没变，也没有新功能或新页面。
> 用户明确提过：**「下次规划 git 时版本号不用跳变太大」**。

**发版前照着这三问走一遍**（顺序不能颠倒）：

1. **存储格式 / HTTP API / NVS 键语义变了吗？**（路由增删、参数含义、键的取值语义）
   → 变了 = **MAJOR**。
2. **有新的功能或新的页面吗？**（用户能用到的新东西）
   → 有 = **MINOR**。
3. **都不是** → **PATCH**（性能、bug 修复、参数调整、文档、内部重构）。

**一次会话里发多个版本怎么办**：**在同一个序列里往下加**，不要每轮跳一个 MINOR。
比如这一串是对的：`1.6.0 → 1.6.1 → 1.6.2 → 1.6.3`；
这一串是错的：`1.6.0 → 1.7.0 → 1.8.0`（每一轮都跳一大格，版本号就不再传达信息了）。

**照着这个表取号**：

| 这一轮干了什么 | 取号 |
|---|---|
| 只有性能优化（开机、内部 RAM、掉帧） | PATCH `1.6.1` |
| 只有修 bug（含真缺陷） | PATCH `1.6.1` |
| 性能 + 修 bug + 修 bug，还是没有新功能 | PATCH `1.6.1` |
| 加了一个新页面 / 一个新功能（用户能用到） | MINOR `1.7.0` |
| 改了 HTTP 路由的语义、或者改了 `todos.json` 的字段、或者改了某个 NVS 键的含义 | MAJOR `2.0.0` |
| 文档、注释、README | 不发版（跟着下一次一起走） |

**每个 tag 对应一份 CHANGELOG 条目、一次上机验证** —— 所以版本号该细就细，
一个 tag 就是一个能回滚、能对比的版本。反过来，一个 tag 里塞三件不相关的事，
以后「回到上一个能用��版本」就没得选。



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

### 8.4 发布到公网前：脱敏自查（**每次发版都要跑一遍**）

这个仓库要公开。**凭据类的东西本来就一条都不在里面**（DeepSeek Key、墨墨令牌、
WiFi 密码、HTTP 鉴权令牌全都只存在设备的 NVS 里，从不进仓库），
但**本机环境信息**会随着文档越写越多地漏出去 —— 已经漏过两轮：
2026-09-28 清理过一次（真实设备 IP），2026-10-21 又漏回了热点 SSID 和网段。

发版前跑这四条（**都要在当前树上跑，不是凭印象**）：

```bash
# 1. 凭据：必须为空
git grep -n -I -i -E "sk-[a-zA-Z0-9]{20,}|password\s*[:=]\s*\"[^\"]{4,}|wifi_pass|ai_api_key" -- .

# 2. 本机网络：真实 SSID / 真实热点网段 / 未打码的 MAC，必须为空
#    （这一条会匹配到它自己和 CHANGELOG 里的描述 —— 那两处是预期内的）
git grep -n -I -E "192\.168\.137\.|xuaoxe_computer|1c:29:04:[0-9a-f]{2}:[0-9a-f]{2}:[0-9a-f]{2}" -- .

# 3. 整个历史（删掉的东西还在历史里）
git log --all -p --no-color | grep -o -i -E "1c:29:04:[0-9a-f]{2}:[0-9a-f]{2}:[0-9a-f]{2}" | sort -u

# 4. 被跟踪的杂文件（日志、临时产物）
git ls-files | grep -iE "\.(log|bak|tmp|bin|elf)$"
```

**取舍**（和 2026-09-28 那次一致）：

* **保留**通用示例（`192.168.1.23`、`MyWiFi`、`1c:29:04:xx:xx:xx`）——
  它们是文档示例，不含真实环境信息；
* **清掉**真实 SSID、真实设备 IP/MAC、真实网段；
* **历史里的残留不改写**（`git filter-repo` 的代价是所有人重新 clone、
  所有 tag 都要重打），但要在发版说明里写清楚「历史里有、已打码」。

> 提交者身份（`xuaoxe` / 邮箱）会留在每个 commit 的元数据里 ——
> 那是 GitHub 账号的身份本身，改写历史也去不掉（除非改 `.mailmap`）。

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
  **同类隐患已在 v1.5.0 修掉**：`ai_service_http.c` 的 `/api/v1/ai/chat/stream` 与阻塞版
  `/api/v1/ai/chat`（超时上限 45 s）、`weather_service_http.c` 的 `/api/v1/weather/locate`
  （最长 12 s）都已改成「handler 立刻 `httpd_req_async_handler_begin()` → 自有任务干活 →
  `httpd_req_async_handler_complete()`」。**注意这是「请求-响应」语义**（不是流式）：
  任务里算好结果，再用异步句柄把响应发回去，不要用 SSE 那种「边写边关」的做法。

### 9.0.1.3 v1.5.0 补进来的五条实测结论（都踩过，别再踩）

1. **FatFs 的 `f_rename()` 在目标已存在时返回 `FR_EXIST`，不是覆盖。**
   LittleFS 的 `lfs_rename_()` 才是在同一次 commit 里带 `LFS_TYPE_DELETE` + `LFS_TYPE_CREATE`
   的原子替换。所以「写 tmp → `remove(目标)` → `rename(tmp, 目标)`」这个常见写法
   在 SD 卡上是**必要的**（不 remove 就会失败），代价是**掉电窗口**：
   掉在 remove 与 rename 之间，上一份有效文件和新临时文件**一起消失**。
   → 工程现在的做法是**双槽**：写 `X.new` → `X` 改名 `X.bak` → `X.new` 改名 `X`，
   启动时把残留的 `.new`/`.bak` 收敛回来（`storage_recover_tree()`）。
   **任何新写的「原子替换」都要先确认后端覆盖语义**，别照抄 POSIX 假设。
2. **cJSON 的 1000 层嵌套上限，在本工程的栈上是致命的。**
   用 `riscv32-esp-elf-objdump -d build/todohelper.elf` 实测：`<parse_value>` 每层递归
   栈帧 **96 字节**（`addi sp,sp,-96`）。开机主任务栈 12288 → 约 **128 层**就爆；
   httpd 任务栈 6144 → 约 **64 层**就爆。而 `todos.json` / 词库这类文件是用户能直接
   在卡上编辑的，一个畸形文件就能让设备**每次开机都崩**。
   → 解析外部输入一律走 `sys_json_parse(json, max_depth)`（解析前先扫文本量深度），
   **不要用 `cJSON_Parse()`**。
3. **`lv_msgbox_close()` 是同步删除，会中止同一次点击里的后续回调。**
   `ui_dialog()` 把关闭回调注册在**按钮**上，`dialog_close_cb` 里 `lv_msgbox_close(mbox)`
   会同步 `lv_obj_delete(mbox)`；LVGL 删除时标记按钮已删除，于是**排在它后面的
   调用方回调永远不会执行**。所以 `ui_dialog()` 的 `on_close` 不可靠。
   → 破坏性操作用 `ui_dialog_confirm()`：两个按钮分开，`on_ok` 收到的 `user_data`
   就是弹窗对象本身，回调里**先做业务再关**。
4. **`esp_http_client` 没有自己的任务，mbedTLS 握手直接吃调用者的栈。**
   查 `esp-idf/components/esp_http_client/esp_http_client.c` 里没有 `xTaskCreate` 就能确认。
   → 凡是跑 HTTPS 的任务（AI、天气、定位、NTP 校时），栈都要给够握手用；
   本工程的 httpd 任务栈只有 6144，把 AI 塞进去本身就是雷。
5. **`http_router_require_auth()` 只能在路由注册之后调用。**
   它是 `find_route(path, method)` 找到路由再改标志位，路由还没注册时返回
   `ESP_ERR_NOT_FOUND` —— **静默不生效**，没有任何告警。
   → 需要鉴权的路由由各组件在自己的 `init()` 里注册，而 `sync_server` 早于它们启动，
   所以标记要打**两次**：服务器启动时一次、`SYS_EVENT_READY`（所有模块初始化完）再一次。
   顺带记住：**只读路由默认不鉴权**（`http_router.c` 只对写方法强制过 hook），
   想保护 GET 必须显式 `require_auth()`，否则「开了鉴权」是假的。
6. **鉴权只有两档，别造第三档（v1.5.0 真机实测后定的）。**
   - **没设令牌** → 局域网默认不校验，**所有路由都通**；
   - **设了令牌** → **所有路由都要令牌，GET 也算**。
   中间那档「某些路径即使没令牌也要令牌」是**自找麻烦**：没有令牌可比对，
   它只会把「设置第一个令牌」「配 AI Key」「用调试 shell」这三条路堵死
   （实测：设令牌 → 网页就再也设不了；AI Key 只能走 HTTP，等于没法配）。
   → 令牌**只从 `X-Auth-Token` 头**读；令牌由**设备设置页**或**网页「连接设置」**生成；
   用户随时能在设置页清掉它，所以局域网里被人抢先设了令牌也锁不死人。

### 9.0.1.4 v1.7.0 补进来的六条实测结论（都踩过，别再踩）

1. **工作流里的构建闸门会「exit 0」但根本没构建。**
   `world.run("bash", ["tools/build.sh", "build"])` 返回 0，日志里还写着「构建通过」，
   但 `build/todohelper.bin` 的时间戳**没变** —— 那个二进制里根本没有最新代码。
   害处：烧上去的是**上一版**固件，然后你在新代码里找 bug。
   → **永远自己看一眼二进制的 mtime**，或者用一条新代码里的独有字符串去 `grep` ELF：
   ```bash
   grep -qa "某个新加的日志字符串" build/todohelper.elf && echo IN || echo MISSING
   ls -l --time-style=+%H:%M:%S build/todohelper.bin
   ```
   同理，`world.run` 的 stdout 也不能当成「构建日志」看。
2. **`-0x008D` 就是「内存不够」，不是 TLS 证书问题。**
   `esp-tls-mbedtls: mbedtls_ssl_setup returned -0x008D` ——
   esp-tls 打的是 `-0x%04X`，`-0x008D` = -141 = `PSA_ERROR_INSUFFICIENT_MEMORY`
   = `MBEDTLS_ERR_SSL_ALLOC_FAILED`。它出现在 `mbedtls_ssl_setup()` 里，
   也就是**分配 TLS 会话内存失败**，跟证书、跟 DNS 都没关系。
   → 本工程的病因是**内部堆被切碎**：地址刚到时天气、墨墨同步、云端队列三个
   HTTPS 同时握手，各要几 KB 连续空间，内部堆最大连续块只有 12~28 KB，
   于是其中一个握手分配失败。
   → 治法是**腾内部 RAM**（WiFi 静态缓冲降到 10、项目静态数组搬 PSRAM），
   **不是**加重试。
3. **任务栈水位那个数字，单位是「字节」不是「字」。**
   `uxTaskGetStackHighWaterMark()` 返回的是「从未碰过的 StackType 个数」，
   而本目标上 `sizeof(StackType_t) == 1`，所以 4096 字节的栈会报出 1800 左右
   （= 用了约 2300 字节）。看到「水位 1800」别以为栈有 1800 字节富余。
4. **跑 TLS 握手的任务栈不能低于 6 KB（实测）。**
   `th_word_cq`（云端队列）按「省 2 KB 内部 RAM」的直觉从 6144 降到 4096，
   真机直接 `***ERROR*** A stack overflow in task th_word_cq has been detected.`
   实测这个任务一次握手用了 **4184 字节**。6144 才是对的。
   → 「省内部 RAM」和「省出栈空间」只能二选一，**先量再改**：
   让任务跑完打印水位，数字在手再决定。
5. **`sys_status.wifi_ip` 在没有地址时是 `"0.0.0.0"`，不是空串。**
   `wifi_manager` 的 `s_ip` 初值就是 `"0.0.0.0"`，开机第一次 `push_status()`
   就把它抄进了 `sys_status`。所以「`wifi_ip[0] != '\0'` 就算有网」这个判据
   **恒为真** —— 我据此写的「等 IP 再发请求」那段代码一次都没进过，是死代码。
   → 判据必须是「非空 **且** 不等于 `"0.0.0.0"`」，工程里 `word_service` 的
   `have_ip()` 就是这么写的，抄它。
6. **拿到 IP 不等于网络能用了。**
   DHCP 把 DNS 服务器地址、路由、lwIP 的解析器跟地址一起送到，比 IP 事件晚一点到。
   刚拿到地址就发请求，DNS 可能还解析不出来（`getaddrinfo() returns 202`）。
   → 天气与墨墨在发请求前都要**歇一下**（天气 3 s 重试、墨墨 2 s 沉降），
   而且「地址到了」要能**补一次**请求（天气已订阅 `WIFI_MGR_EVENT_GOT_IP`）。

### 9.0.1.5 v1.8.0 补进来的四条实测结论（都踩过，别再踩）

7. **FreeRTOS 的任务函数不能直接返回。**
   返回等于「任务结束了，但栈还没还回堆」，调度器会踩到一块已经失效的栈。
   真机上的表现是**点了按钮设备就卡死**：网页连不上、串口不出字，只能复位 ——
   而且卡死的是「一个跟被点的功能毫无关系」的地方（设置页改时区，
   卡的是整个 Web 服务器）。
   这个 bug 在 `tz_apply_task()` 里躺了好几轮（本轮上机时点时区按钮才发现）。
   → **一次性任务写完必须 `vTaskDelete(NULL)`**。
   工程里的样板：`slider_save_task()`、`word_book_worker()`、`th_slider`。
8. **LVGL 9.5 的滑块只有「按下去」才改值。**
   改值的那一步在按压处理程序里（从输入设备读指针坐标），
   `lv_slider_set_value()` 本身**不发** `LV_EVENT_VALUE_CHANGED`。
   → 后果：调试 shell 的 `click` 对滑块无效（合成的��件没有坐标），
   「带去抖落盘」的那条链就只能靠人用手拖才能验证。
   → 本轮给 shell 加了 `slider <target> <value>`：改值之后**手动补发**
   `LV_EVENT_VALUE_CHANGED`，跟手指的效果一样。
9. **NVS 里同一个键可能是字符串也可能是整数，读之前要两种都试。**
   设置页的三个滑块写的是 **i32**（`sys_config_set_i32`），
   别的配置写的是字符串。调试 shell 的 `cfg get` 原来只读字符串，
   于是滑块的值读回来是「未设置」—— 一度让人以为**去抖落盘没生效**，
   白查了半小时。→ `cfg get` 现在字符串读不到就按整数再读一次。
10. **内部堆的最大连续块会被「打开某个页面」打下去。**
    WiFi 扫描页实测吃掉约 **50 KB** 内部 RAM（每个 AP 一行、每行四个控件，
    十几个 AP 就是上百个 LVGL 对象），扫完之后最大连续块掉到 **12 KB** ——
    这是 WiFi PHY 那 30 KB 需求剩下的最后一块缺口。
    设置页也差不多（一页 182 个对象）。
    → 治法跟待办列表一样：只渲染视口内的行。**下一轮做**。

### 9.0.1.6 v1.8.1 补进来的四条实测结论

11. **LVGL 9 的滚动方向是反直觉的。**
    - \`lv_obj_get_scroll_y()\` 返回的是 \`-scroll.y\`：**手指往上滑**（正常的往下翻）
      → 返回**正数**；\`lv_obj_scroll_by(obj, 0, +300)\` 反而是把内容往下移（返回负数）。
    - \`lv_obj_scroll_by()\` **不设边界**，而手指的滑动是设边界的
      （\`lv_indev_scroll_handler\` → \`scroll_limit_diff\`）。
      → 用它模拟手势会滑到内容外面去，看起来像「列表能滚进虚无里」。
      → **要模拟手势就用 \`lv_obj_scroll_by_bounded()\`**。
    - 调试 shell 新增了 \`scroll <target> <dy>\`（用 bounded 版），
      因为「窗口跟着滚动走」这件事**光看控件树看不出来**，必须能滚一下再看。
12. **LVGL 会在你删掉子对象之后把滚动位置「夹」一下。**
    \`lv_obj_delete\` → \`readjust_scroll_after_layout\` → \`lv_obj_scroll.c\` 的
    \`readjust_scroll\`：位置会被收到「剩余内容允许的最大值」。
    → 所以「记下位置 → 删掉内容 → 回来复原」这条路，复原的判据**不能**写成
    「当前值是不是 0」，要写成「当前值是不是我记的那个值」。
    → 还要记得在删之前停掉惯性滚动动画（\`lv_anim_delete(obj, NULL)\`），
    否则记住的是动画中途的值。
13. **反复销毁/重建 LVGL 对象会把内部堆切碎。**
    「页面隐藏时销毁行控件」是对的（省内存），但如果每次进出都销毁重建，
    碎片化会让**最大连续块**掉到几 KB —— 而 WiFi PHY 要的是**连续**块。
    → 真机实测：38 次进出待办页之后最大连续块**稳定在 27 KB 不再下降**，
    这就是「对的做法」的样子（对比：改之前同一串操作会一路掉到 2.5 KB）。
14. **NVS 没加密，密钥在 flash 上是明文。**
    \`CONFIG_NVS_ENCRYPTION\` 没开，而 \`wifi_pass\` / \`ai_api_key\` / 鉴权令牌
    都是 \`sys_config_set_str()\` 存的 → **在 flash 里是明文**。
    「不写日志、不通过接口读回」这条规矩管的是**接口与日志**，管不住 flash。
    → 这是硬件层面的事实（读 flash 需要物理接触），先记着；
      真要防，得开 NVS 加密或者改用 ESP32 的 eFuse/安全存储。
    → 顺带：本工程的调试 shell 能读出 NVS 里的任何键（\`cfg get\`），
      所以 **AI Key 被 \`cfg get ai_api_key\` 读回来过一次**（已修：只回答「设了没有」）。

### 9.0.1.7 v1.8.2 补进来的两条实测结论

15. **LVGL 的 FreeType 字形缓存是内存大户，而且能撑死设备。**
    缓存里装的是**每一个用过的汉字的位图**（28 px 约 784 B，48 px 约 2.3 KB），
    而 LVGL 用的是 C 库的 malloc（`CONFIG_LV_USE_CLIB_MALLOC=y`），
    所以这些位图全落在**内部堆**里（≤4096 B 的分配优先内部，见 §9.0.3）。
    → 真机实测（同一个固件，只改 `CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT` 这一个数，
    用一个临时的「自动巡览所有页面」任务绕开所有页面各走三遍）：

    | 缓存个数 | 绕完三遍之后 |
    |---|---|
    | 256（默认） | 内部堆 **0 KB**，最大连续块 0 KB，**设备崩了** |
    | 64 | 54~64 KB，最大连续块 5~17 KB |
    | **32** | **76~88 KB，最大连续块 22~35 KB**（过了 PHY 需要的 ~30 KB） |

    → 所以这个数**必须**压到 32。代价是被挤掉的字形要重新渲染
    （FreeType 每个汉字约 100~200 µs，一屏 30~40 个汉字 → 每帧多几毫秒）。
    → **（v1.8.5 更正：这里的结论只对了一半。真正卡的原因不是「位图太大」，
    而是「缓存是整个界面共用一份、64 条根本不够」—— 见 §9.0.1.8 第 17~18 条。
    现在这个数是 512，位图和元数据都搬去了 PSRAM。）**
    → 这个实验用的「自动巡览页面 + 打印堆」是临时加在 app_main.c 里的，
    **不要提交**。它也证明了另一件事：**页面巡览是个很好的内存体检**，
    比单看某个页面的读数有用得多。
    → v1.8.5 把它做成了常驻工具：`tools/page_tour.py`（走一遍所有页面并打印堆），
    再加上 debug shell 的 `ftstats`（字形缓存条目数）和 `bench`（重绘耗时）。
16. **中文字库放在 SD 卡上，SD 卡挂不上就变成方块。**
    字库 `fonts/ui_cjk.ttf`（1.25 MB）在卡上，挂载失败 → FreeType 用不了 →
    退回内嵌的 GB2312 一级子集（~2500 字）→ **生僻字变成方块**。
    实测 312 次开机里有 2 次 `SD card not available (ESP_ERR_TIMEOUT)`，
    这两次开机用户看到的就是满屏方块。
    → 已加**挂载重试**（3 次、间隔 500 ms；IDF 在挂载失败时会把 SDMMC host
    反初始化，所以重试是安全的）。
    → 还没做的更彻底的一步：**把字库复制一份到内部 flash**，
    让文字渲染彻底不依赖 SD 卡。

### 9.0.1.8 v1.8.5 补进来的五条实测结论

17. **LVGL 9 的 FreeType 字形缓存是「整个界面共用一份」，不是每个字号一份。**
    缓存的 key 是 **（字体文件, 样式, 渲染模式）**，`lv_freetype.c` 里的
    `search_key` **没有把字号算进去**。四个字号（16/20/28/48 px）来自同一个
    .ttf，所以**共用同一份位图缓存**。
    → 所以 `CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT=64` 的真实含义是
    「四个字号加起来 64 个字」，每个字号平均只有 16 个位置 ——
    而一屏就有 30~40 个汉字，缓存永远不够用。
    → 怎么一眼看出来：`ftstats`（debug shell 新增）把每个字号的缓存条目数
    都打出来，**四个数字一模一样**就说明是共用一份。
    → 教训：改 LVGL 的行为之前，先把它到底怎么组织的读一遍
    （`managed_components/lvgl__lvgl/src/libs/freetype/lv_freetype.c`），
    不要按名字猜。
18. **画一个字要 10 ms，缓存不够用时会涨到 45 ms。**
    真机实测（debug shell 的 `bench` 命令：设标签文本 → `lv_refr_now()`
    同步重绘 → 打印耗时）：
    - 缓存上限 512、开机后第一次画 33 个新字：一次重绘 **330 ms**
      → 约 **10 ms 一个字**（FreeType 渲染本身只有 100~200 µs，
      其余是**从 SD 卡读 TTF**）；
    - 缓存上限 64：同样 30 个新字要 **1.35 s** → 约 **45 ms 一个字**。
      差的这 5 倍是因为缓存**描述**的那一份也满了 —— 位图放不下，
      连「这个字形在哪、什么尺寸」都要重新从 TTF 里查一遍。
    → 这两个数是**不同设置**下测的，别混着算（初版文档并排放，看着像自相矛盾，
    是独立复核员指出来的）。
    → 顺带查清了一件想当然的事：切页慢（11~222 ms）**跟字形缓存无关** ——
    A/B 实证：同一串切页动作，缓存 64 和 512 两栏几乎一模一样。
    慢的是**重建控件**（v1.8.3 把页面控件隐藏时销毁了），不是画字。
    （v1.8.5 的初版把「切页 16~174 ms → 4~6 µs」归因于字形缓存，
    **那个归因是错的** —— 独立复核员指出来的，见 v1.8.6 的更正。）
    → 教训：**「测出来慢」和「想当然慢」经常不是一回事**，
    而 `lv_refr_now()` 是把「重绘耗时」变成数字的最便宜办法
    （光设文本不触发画字，HTTP 往返里根本看不到光栅化的成本）。
19. **手感看的是「稳态」，不是「冷启动」—— 测错了就会得出相反的结论。**
    一开始测的是「画 30 个新字要多久」，发现**改不改缓存上限都一样**
    （每次测的都是新字，跟缓存大小无关），差点得出「缓存大小没用」的错误结论。
    真正决定手感的是**重画一屏的稳态耗时**：一屏几十个字，缓存装不下的话，
    **每一帧**都要把同样的字重新光栅化一遍。
    → debug shell 的 `bench-screen`（整屏作废 + `lv_refr_now()` 同步重画 N 次）
    测出来：上限 64 时第 1 次 728 ms、**后面每次都是 727 ms**（稳态 = 冷启动，
    因为缓存根本装不下这一屏的字）；上限 512 时 24 ms / **23.5 ms**。**31 倍。**
    → 教训：**先问「这个数代表用户感受到的东西吗」**，再动手测；
    一个测不出差异的指标，多半是指标本身选错了。
    → 顺带一个更隐蔽的坑：debug shell 原来是自己用 `strstr`/`strchr` 抠 JSON 里的
    `"cmd"`，**不解析转义**。标准 JSON 编码器发中文都会写成 `\uXXXX`，
    于是命令到设备上变成了字面的反斜杠序列 —— 「30 个汉字只花 3 ms」就是这么来的：
    它压根没画汉字。**测出来的漂亮数字，先怀疑测量方法。**
20. **只要给一条窄路，不必动全局的分配阈值。**
    v1.8.2 试过把 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 压到 128，
    结果 **TLS 握手失败**（`-0x008D`）。v1.8.5 换了个做法：
    - **位图**走 LVGL 的 `font_draw_buf_handlers`（`lv_draw_buf_get_font_handlers()`），
      换成显式带 `MALLOC_CAP_SPIRAM` 的 `heap_caps_malloc` ——
      **不经过 `malloc()`**，全局阈值一个字没动；
    - **元数据**（红黑树节点 + 缓存条目 + draw_buf 结构，每个字形约 200 B，
      517 个组合就是 100 KB）走 `CONFIG_LV_USE_CUSTOM_MALLOC=y` 自定义分配器，
      但**只有「画字」这一个区间**改道（区间由 FreeType 的
      `get_glyph_dsc` / `get_glyph_bitmap` 两个回调包出来），
      其余分配行为跟原来一模一样。
    → 效果：页面巡游三遍之后，内部堆最大连续块 12 KB → **80 KB**，
    空闲 73 KB → **150 KB**。
    → 关键前提（查过 IDF 源码）：`CONFIG_SPIRAM_USE_CAPS_ALLOC` 没开也不妨碍
    —— PSRAM 仍然被 `esp_psram_extram_add_to_heap_allocator()` 用
    `MALLOC_CAP_SPIRAM` 注册进了 heap_caps 分配器；而 ALWAYSINTERNAL 的限制
    只作用在 `heap_caps_malloc_default()`（普通 `malloc()`），
    显式带 caps 的 `heap_caps_malloc()` 走 `heap_caps_malloc_base()`，不受它管。
    → 教训：**全局开关是最后手段**。先找那条「只有它需要」的窄路。
21. **位图搬走不等于缓存搬走：元数据才是内部堆的大头。**
    位图只是一半，缓存的元数据每个字形还要约 200 B 内部堆。
    只搬位图的话，缓存开到 256 一样会把内部堆吃光（v1.8.2 的实测就是这样）。
    → 教训：算一笔内存账时，**要把「一项开销」拆干净**：
    数据、索引、每条记录的固定开销，三块分别算。
22. **LVGL 的分配器可以自己接管，但「什么时候改道」要自己定。**
    `CONFIG_LV_USE_CUSTOM_MALLOC=y` 之后，LVGL 不再提供
    `lv_malloc_core` 等函数，得自己实现（照着
    `managed_components/lvgl__lvgl/src/stdlib/uefi/lv_mem_core_uefi.c` 抄形状：
    `lv_mem_init` / `lv_mem_deinit` / `lv_malloc_core` / `lv_realloc_core` /
    `lv_free_core` / `lv_mem_monitor_core` / `lv_mem_test_core`）。
    → `lv_malloc_core` 里**平时必须跟 `malloc()` 完全一样**
    （小分配留在内部堆），只有需要改道的区间才换地方；
    `lv_free_core` 用 `free()` 就够（它对任何堆的块都对）；
    `lv_realloc_core` **要小心**：普通 `realloc()` 对 ≤4096 B 的块是要内部 RAM 的，
    而 `heap_caps_realloc_base()` 只在「请求的 caps 与原堆兼容」时才原地扩 ——
    于是画字区间里从 PSRAM 分配的块，realloc 一下就被搬回内部堆。
    画字区间里要用 `heap_caps_realloc(p, n, MALLOC_CAP_8BIT)`（两个堆都满足，
    块留在原处）。
    → **别把整套 LVGL 分配都赶到 PSRAM**：LVGL 的控件对象每帧都要读，
    放 PSRAM 会让整个界面变慢。
23. **在 LVGL 任务里调 `lv_timer_handler()` 是重入调用，什么都测不到。**
    LVGL 的 `lv_timer_handler()` 第一件事就是重入保护
    （`lv_timer.c` 的 `already_running`）→ 直接 return 1。
    而调试 shell 的 UI 作业是**在 LVGL 的定时器回调里**跑的
    （`ui_event_drain_cb`），所以里面再调 `lv_timer_handler()` 一定被挡掉 ——
    「首次重绘耗时 1 µs」就是这么来的，测的是函数进出的开销。
    → **要同步重绘就用 `lv_refr_now(disp)`**（它不走那个重入保护）。
    改完之后量出来的数才有意义：某页的首次绘制 226~416 ms（冷，字要重新光栅化），
    而整屏重画的稳态是 24 ms。
    → 教训：**「测出来一个漂亮的小数字」先怀疑这个数字测的是不是你要的东西。**

### 9.0.1.11 v1.11.0 补进来的三条实测结论

31. **OTA 的三条安全线，缺一条设备就可能变砖。**
    真机把这三条都验过了：
    * **写进另一个槽位**（不覆盖正在跑的那个）；
    * **写完校验**（`esp_ota_end()` 拒掉截断/非法的镜像 —— 上传 1 MB 的
      残缺文件，设备当场报「固件校验不通过」，而且**没有重启**，继续跑旧固件）；
    * **开机回滚**（新固件起来要「认下」自己，认不了 bootloader 就退回上一个）。
      这一条是这么验的：故意做一个不认自己的镜像传上去，设备重启后跑的是它；
      **再重启一次**，bootloader 就把它退回去了，设备回到之前那个版本。
32. **几百 MB 的固件不能一次性读进内存。**
    固件是 3.3 MB，而内部堆只有 180 KB —— `http_read_body()` 会 malloc 3.3 MB，
    直接就没了。所以 OTA 的请求体是**分块读、分块写**（32 KB 一块，
    `httpd_req_recv()` + `esp_ota_write()`）。
33. **「上传」这种长请求不能占着 httpd 任务。**
    esp_http_server 只有一个 httpd 任务，URI handler 在里面串行执行：
    handler 不返回，连 accept() 都不跑（AGENT.md §9.0.1 最后一条）。
    一次上传是好几秒，所以用 `httpd_req_async_handler_begin()` 把 socket
    交给自有任务（和 ai_service 同一套做法）。

### 9.0.1.12 v1.12.0 补进来的六条实测结论

34. **FreeType 读字库用的是最普通的 `fopen()`，所以「字库文件在哪儿」
    直接等于「画一个字多快」。**
    v1.8.5 已经量过：画一个字约 10 ms，其中约 9.8 ms 是从 SD 卡读 TTF。
    → 这一版把字库搬进 PSRAM，用一个**只读的 VFS**（`esp_vfs_register_fs`）
    把它挂在 `/ramfont` 上，FreeType 一行都不用改（它照样 fopen，
    只是路径背后是内存）。
    → 实测（同一台设备、同一段代码、同一串 74 个从没画过的字，
    用「第一次画减第二次画」把布局和整屏刷新的开销减掉）：
    **SD 卡 7 816 µs/字 → PSRAM 604 µs/字，约 13 倍。**
    → 教训：**慢的地方往往不是「算法慢」，是「数据在远处」。
    先问「这个数据在哪儿」，再问「这段代码做了什么」。**

35. **LVGL 自带的 memfs 在这个工程里用不了 —— 打开它就链接不过。**
    LVGL 的内存文件系统（`lv_fs_memfs.c`）正是为「把慢 flash 里的字库搬进内存」
    写的，但它只在 `LV_FREETYPE_USE_LVGL_PORT` 打开时才生效；
    而打开它，**FreeType 和 LVGL 会各自导出一个 `FT_Stream_Open`**
    （ESP-IDF 的 freetype 组件永远编译自己的 `ftsystem.c`）→ 重复符号，链接失败。
    → 走得通的是**低一层**的做法：IDF 自己的 VFS（newlib 的 `fopen()`
    在 IDF 上就是走 VFS 的）。
    → 教训：**上游库给你的「官方开关」不一定能在你的依赖组合下成立** ——
    先把链接顺序和符号表看一眼（`build/build.ninja` 里的 LINK_LIBRARIES），
    比改代码便宜得多。

36. **A/B 的对照组，要验证「它确实是另一组」。**
    第一次做 A/B 时两边量出来**一模一样**（8.7 ms vs 8.7 ms），
    差点得出「这活儿白干了」的结论。真相是：改了 `sdkconfig` 里的开关，
    但那次构建里 `confgen` 又按 `sdkconfig.defaults` 把它写回去了 ——
    **「对照组」的固件里其实还带着实验组那条路**。
    → 两个信号：两边固件大小一模一样；固件里还找得到实验组的日志字符串
    （`strings build/todohelper.bin | grep "font in RAM"`）。
    → 教训：**改了配置之后，先证明二进制真的变了，再去相信它量出来的数。**
    （跟 v1.8.5 第 19 条是同一个病：先怀疑测量方法。）

37. **「忙」不能只看状态：状态必须在动手之前就置上。**
    OTA 原来只在「正在写入」时拒绝第二次上传，而
    **`esp_ota_begin()`（擦整个目标槽位，要好几秒）是在状态置位之前做的** ——
    那个窗口里第二次上传会被放行，而它要写的还是**同一个槽位**
    （还没重启，`esp_ota_get_next_update_partition()` 返回的就是另一个），
    于是刚校验通过的镜像被擦掉重写；更糟的是这时 otadata 已经指向那个槽位，
    重启就会从一个写了一半的镜像启动。
    → 现在用一个**单独的忙标志**：请求一被接受就置上，直到重启才清。
    擦除、写入、校验、上膛，四个阶段都在保护里。

38. **「下一个可以写的槽位」和「下次启动会跑的槽位」是两回事。**
    `esp_ota_get_next_update_partition()` 永远返回**另一个**槽位，
    哪怕根本没有待启动的固件 —— 用它判断「有没有升级等着启动」必然看错
    （网页上就一直显示「ota_0 → ota_1」，读起来像有个升级在等着）。
    → 真正要看的是 `esp_ota_get_boot_partition()`（读 otadata 那个）。
    → 教训：**函数名里的「next」指的是哪一头，要看头文件，不能靠猜。**

39. **量「画字多快」，布局和刷屏的代价会混进来 —— 而且它可能比要量的
    东西大两个数量级。**
    `bench` 命令给标签换一段字再同步重画；这个数里混着三样东西：
    ①**光栅化**（要的）、②**布局**（新字比旧字长 → 换行 → 页面重排）、
    ③**整屏刷新**（这台板子一次要刷 800×480，约 22 ms）。
    ②③跟固件无关，但 ③ 可能比 ① 大几十倍 ——
    第一次 A/B 量出来「SD 卡 4 583 µs/字 → PSRAM 85 µs/字（54 倍）」，
    其实两边当时的刷新底噪都是 0，比值被放大了；
    改成「同一段字画两次相减」（第二次全在缓存里，①②③都还在），
    真相是 **7 816 µs/字 → 604 µs/字（13 倍）** —— 比值小了，但**是真的**。
    → 办法：**文字长度固定**（避开 ②）+ **减掉刷新的底噪**（避开 ③）。
    `tools/bench_font_ram.py` 就是这么做的。
    → 教训：**先问「这个数代表用户感受到的东西吗」**（v1.8.5 第 19 条），
    这里的版本是：**再问「这个数里有没有混进别的东西」**。
    （同一个病第三次犯 —— 先怀疑测量方法。）

40. **复位会把「还没送到的答案」一起带走 —— 所以要重启的接口
    应该在「回答完了」之后再重启。**
    v1.11.0 的 OTA 是「上传 → 校验 → 定时器 1.5 秒后重启」，
    响应是在重启前 1.5 秒写进 socket 的 —— 但**芯片复位会把 WiFi 协议栈
    一起带走**，那个响应就丢了。网页上看到的是「上传失败：连不上设备」，
    而固件其实已经写好了、设备也确实换了版本。
    （这个现象查了很久：先怀疑是 curl 的问题，再怀疑是 async handler 的问题，
    最后才想到「答案在路上，电台被拔了」。）
    → 现在重启是**单独一步**：`POST /api/v1/ota/upload` 只写好并「上膛」
    （一定有响应），`POST /api/v1/ota/reboot` 才重启。
    → 教训：**凡是「做完一件大事顺手重启」的接口，
    都要先确认「调用方已经知道结果了」。**

### 9.0.1.15 v1.13.3 补进来的三条实测结论

45. **「后进先出」对通知是对的，对**内容**是错的。**
    跨任务事件队列（`ui_post_event()` → LVGL 任务）原本是 LIFO，
    理由是「一串通知不会把最新的饿死」——
    这话对**重绘通知**成立（每条都只是「重画我」，最后状态一样），
    但对**带顺序的内容**是错的：
    * AI 的流式回答是几十条 `REPLY_CHUNK`，LIFO 把它们**倒着**送 →
      屏幕上的文字成了答案的一份重排（真机实测：答案结尾
      「做智能家居、传感器节点等小项目。」显示成「。项目小等节点传感器、家居智能做」）；
    * 待办更新带整张新表，LIFO 可能先画旧的；
    * 「回答完成」是那批里最新的，队列满时它第一个被丢 → 页面等一个早就到了的回答。
    → 改成 FIFO，队列满时丢**最旧**的。
    → 教训：**队列的顺序策略要按「载荷有没有顺序」来定**，
    不能一个策略打天下。「最新的最重要」和「顺序最重要」会打架。

46. **借来的缓冲不能在你还在用它的时候清零。**
    流式气泡的 label 用 `lv_label_set_text_static()`（**借用**那块缓冲，
    不拷贝 —— 答案每来一段就不用重新分配）。
    而结束时把缓冲清零了 → **回答完成后气泡变空白**，
    下一个回答又会写进同一块缓冲，把上一个气泡顶掉。
    → 结束前先把文本**冻进** label（`lv_label_set_text()` 会拷贝），再清缓冲。
    → 教训：**「借用」和「拥有」要配对**：谁借的，谁负责在最后一刻把它固化下来。
    缓冲区清零这种「善后」动作，往往正好毁掉还在指向它的东西。

47. **别在多字节字符中间截断 —— 那会让整个 JSON 失效。**
    调试输出摘要在**字节**边界上截断，一个汉字是 3 字节，
    于是响应里混进半个 UTF-8 序列 → 浏览器 `JSON.parse` 直接抛异常，
    页面什么都不显示（而设备那边「看起来一切正常」）。
    → 摘要只切**完整字符**。
    → 教训：**「看起来是编码问题」和「JSON 整个坏了」是同一件事** ——
    凡是往 JSON 里塞设备上的中文，都要保证它一定是合法 UTF-8。

### 9.0.1.14 v1.13.2 补进来的两条实测结论

43. **「固定地址」不是只有 IP、网关和掩码 —— DNS 也是它的一部分。**
    v1.10.0 加了固定地址功能：设 IP、网关、掩码，DHCP 客户端停掉。
    **但 DNS 没跟着固定** —— 而 DNS 是 DHCP 租约里带过来的，
    DHCP 一停，**lwIP 就一个解析器都没有了**（`getaddrinfo()` 全部返回 202）。
    → 后果很绕：NTP 服务器写的是**域名**（`ntp.aliyun.com`），
    域名解析不了 → SNTP 查询发不出去 → **时钟永远同步不上**，
    而设备表面上只是「时间不对」。同一根因还让天气、墨墨这些要访问外网的接口全挂。
    → 修法：固定地址时**一并设 DNS**（用户填的 → 上次租约学到的 → 网关）。
    网关是兜底，因为家里路由器和 Windows 热点都把 DNS 放在网关上。
    → 教训：**换掉一个「自动获取」的东西时，要把它带来的**所有**东西都接上** ——
    DHCP 带来的不只是地址，还有 DNS（还有 MTU、租期）。
    只接了一半，就是这种「看起来是另一个功能坏了」的病。

44. **一个根因会伪装成好几个不相干的症状。**
    这次的表面症状是「时间同步不了」，但顺着查下去是 DNS，
    而 DNS 坏了还会让天气和墨墨一起坏 —— 三个看起来无关的功能，
    底下是同一行代码。
    → 教训：**遇到「某个功能坏了」先问「它依赖什么」**：
    NTP 依赖**域名解析**，域名解析依赖**DNS**，DNS 依赖**DHCP 租约**。
    顺着这条链往上走，比在报错的地方打转快得多
    （同 v1.7.0 第 7 条那个病：根因一律不在报错的地方）。

### 9.0.1.13 v1.13.0 补进来的两条实测结论

41. **原生对话框（`window.confirm`）不能当唯一防线。**
    上位机的「重启设备」按钮第一版用 `window.confirm()` 拦一下。
    **实测在嵌入式浏览器里它被自动接受了** —— 点一下，设备真的重启了，
    连确认框都没看见。（同一个对话框在桌面浏览器里是正常弹的。）
    → 改成**按钮自己上膛**：第一次点变成「再点一次确认」（变红），
    五秒内再点才真执行，超时自动取消。
    → 教训：**在嵌入式环境里，浏览器原生 UI 的行为不能靠假设**；
    危险操作要有一个**不依赖浏览器**的确认机制。

42. **「串口交互」不一定要真的插线 —— 先问「命令从哪儿来」。**
    这个项目的调试 shell（`/api/v1/debug/exec`，几十条命令）本来就只在 HTTP 上；
    串口上跑的是**日志**。要做网页端的「串口交互」，
    真插线（Web Serial + 设备端从 UART0 读命令）意味着：
    给设备加一条新的输入路径、动 UART0 的 RX FIFO
    （它和 IDF 控制台输出共用）、还要面对只有 Chromium 系浏览器支持 Web Serial。
    而走 HTTP 的话，命令、鉴权、LVGL 任务的 marshal 全是现成的。
    → 选了 HTTP。**设备的命令接口在哪，就在哪做界面**，
    别为了「像串口」去开一条新的路。
    → 真串口那一版留到以后：它的价值在**HTTP 挂了还能操作设备**
    （这个项目有过「ping 通、串口正常，但任何 TCP 都建不上」的时候）。

### 9.0.1.10 v1.10.0 补进来的两条实测结论

30. **「友好的名字」不一定比「难记的地址」好用。**
    设备发的是 mDNS（`todohelper.local`），但**这台 Windows 上根本解析不了**：
    `nslookup` 查不到（走公共 DNS）、`ping` 无应答、**浏览器里也是一页空白**
    （Windows 解析 `.local` 要靠 Bonjour，有些路由器还把组播丢掉）。
    → 于是：两个地址都留着，但**把一定能用的那个（IP）放前面**，
    友好的名字退成第二选择，并且说清楚为什么。
    → 设备设置页上的网址也一样换了顺序 —— 原来 mDNS 名字在前，
    而它恰恰是打不开的那个。
31. **DHCP 的地址每次重连都可能变 —— 一次下午就换了三次。**
    → 现在设备支持**固定地址**（`POST /api/v1/wifi/config {"ip":"..."}`），
    而且**网关和子网掩码不用用户填**：设备记着上次租约里的那两个值
    （热点和家用路由给每个人的都一样），地址在关联之前就设好，不被租约覆盖。
    → 固定地址有个陷阱：**地址填错了，网页端就连不上，而网页端正是填地址的地方。**
    所以设备的 WiFi 页上必须有「改回 DHCP」这道**逃生门**（拿到设备跟前也能救）。
    → 教训：**凡是把「配置入口」和「被配置的东西」绑在一起的功能，
    都要留一条不依赖那个入口的退路。**

### 9.0.1.9 v1.9.0 补进来的四条实测结论

24. **「设备没有这个硬件」和「没人问过」是两回事，别混。**
    语音状态接口原来报 `microphone: false`，因为音频是**惰性**起起来的 ——
    没起就报 false，看起来就像「这板子没麦克风」。
    → 状态接口现在会**真的把外设起起来再回答**（`voice_service_ensure_audio()`）。
    → 教训：**报状态之前，先让被问的那件事真的发生一次**。
25. **「能枚举」不等于「能用」。**
    v1.9.0 加了 `audio` 探针，真机量出：codec 起来了（`mic=1 speaker=1`，I2C 有应答），
    但**录 2 秒一个样本都没有**（不是静音，是 0 字节）。
    → 教训：**硬件体检要分三层**：枚举得到吗 → 有数据吗 → 数据对吗。
    只看第一层会得出「硬件 OK」的错结论。
26. **二进制响应不能靠 `strlen` 算长度。**
    TTS 返回的是原始 PCM，HTTP 客户端把它当 C 字符串处理，长度就错了
    （PCM 里一个 `0x00` 字节就把长度截断）。现在 `http_post()` 一并回传字节数。
    → 顺带：TTS 的响应缓冲搬去了 PSRAM（一句话可能几百 KB，内部堆放不下）。
27. **两个事件基的 id 可以撞号。**
    `AI_EVENT_*` 和 `VOICE_EVENT_*` 都从 0 开始 —— 绑到同一个页面的 `on_event` 上
    就分不清谁是谁。语音事件改用 `ui_event_subscribe()`（普通回调）单独订阅。
    → 教训：**多个事件基共用一个回调时，先看它们的 id 空间是不是独立的。**
28. **「硬件测不出东西」先怀疑自己的测量代码。**
    v1.9.0 下了结论「麦克风采集通路没打通」（真机 2 秒录到 0 个样本），
    差点把整个语音功能降级成「待定」。独立复核员查出来了：那 2 秒 0 个样本
    是**本工程的采集循环读错了返回值** ——
    `esp_codec_dev_read()` 返回的是**错误码**（0 = 成功，负数 = 失败），
    **不是字节数**；真正读了多少字节这个 API 根本不告诉你。
    原来的循环把 0 当成「没数据」→ **每一次成功读取都被扔掉**。
    改对之后：同一块板子、同一段固件，1466 ms、峰值 3124。
    → 而且症状本身就指向这个 bug：codec 哑了/时钟不对的话，I2S 的 RX 照样搬数据，
    症状应该是「时长 > 0、峰值 0」（一段静音）；只有 RX 根本没跑起来才是「0 字节」。
    → 教训：**一个恒为 0 的读数，通常是自己的 bug，不是硬件的判决。**
    → 顺带：`esp_codec_dev_read()` 成功时**不告诉你读了多少**，
    底层 `i2s_channel_read()` 是「填满缓冲区或超时」为止 ——
    所以「成功」就意味着整个缓冲区都是有效数据。
29. **在 httpd 任务里做的硬件握手，必须能超时。**
    v1.9.0 试过在 `components/bsp/` 里自己建一路 codec（怀疑 MCLK 没接上），
    结果**设备活着但整个 HTTP 服务器卡死了**：ping 通、串口日志照常，
    HTTP 一个请求都不回 —— 因为音频初始化是在 httpd 任务里做的，
    codec 不应答时那一步永远阻塞。
    → **一个诊断用的命令不该有本事把 HTTP 服务器带走。**
    现在音频初始化在自己的任务里做，最多等 3 秒；超时就当「没有音频硬件」。
    → 教训：**凡是会让硬件「不回答」的调用，都要有界。**

### 9.0.1.1 流畅度五条铁律（v1.2.0 加入，**改 UI 前必读**）

这五条是"界面卡顿/掉帧"的全部真实根因，都已在真机复现并修掉。它们是 §9.0.1
那两条锁规则的直接推论。

1. **`lv_timer` 回调与所有控件事件回调都在 LVGL 锁内运行。**
   适配器是这么驱动渲染的（`esp_lv_adapter.c`）：
   ```c
   if (esp_lv_adapter_lock(-1) == ESP_OK) {
       next_delay_ms_raw = lv_timer_handler();   /* 锁内！ */
       esp_lv_adapter_unlock();
   }
   ```
   所以**回调里做任何同步 I/O 都等于冻结整个 UI**。
   - 实测最坏例子：`page_calendar` / `page_settings` 的"校时"按钮直接调
     `calendar_sync_ntp(0)` → `esp_netif_sntp_sync_wait(10 s)`，持锁最多 **10 秒**
     零重绘，并且因为 `ui_post_event()` 当时也要取同一把锁，**整个事件系统一起冻住**。
   - → 校时用 `calendar_sync_ntp_async()`（结果走 `CALENDAR_EVENT_SYNC_RESULT` 事件）；
     落盘用 `word_save_progress_async()`；NVS/flash 写放一次性任务。
   - 判据：**回调里出现 SD 卡 / flash / 网络调用就是 bug。**
     `calendar_sync_ntp()` 与 `word_save_progress()` 的头注释都写明了"禁止在 LVGL 任务调用"。

2. **`lv_label_set_text*()` 即使文本完全相同也会 invalidate 对象。**
   任何周期性刷新都必须先比较再写。曾经的代价：状态栏 4 个标签 + 4 次改色每秒无脑重写，
   首页/日历/设置页各自再来一遍 → 整屏每秒重排，所有动画都被拖着。
   → 模式：缓存上次渲染值（字符串或字段），相同就 `return`。
   `ui_statusbar.c` / `page_home.c` / `page_calendar.c` / `page_wifi.c` /
   `page_settings.c` 都已按此改写。

3. **`lv_async_call()` 的池很小，不要用它做批量投递。**
   跨任务投递事件时若对每个节点调一次 `lv_async_call()`，通知突发（启动期 DNS 失败洪峰、
   WiFi 重连）会打满该池，循环退化成反复 malloc/free 并与 LVGL 内部锁叠加。真机实测复现
   过一次 **interrupt wdt timeout**（`MCAUSE=0x18`，两核栈都在 `vListInsert` /
   `xQueueSemaphoreTake`）。
   → 正确做法见 `ui_core.c` 的 `ui_event_drain_cb()`：**无锁单链表 + 一个 20 ms
   `lv_timer`，在回调内直接派发**（该回调本就在 LVGL 任务里且已持锁，正是 handler
   需要的上下文）。队列有上限（64），满了丢事件并告警——丢通知可接受，阻塞发布者不可接受。

4. **定时器要有主。**
   `page_settings` 曾经 `lv_timer_create(settings_tick, 250, NULL)` **不保存句柄**：
   页面隐藏后它仍在 4 Hz 重写 4 个标签，且永不释放。现在句柄存 `s_slider_timer`、
   `on_hide` 里 `lv_timer_pause()`、`on_show` 里 resume。
   → 页面级定时器一律保存句柄并实现 `on_hide`（`ui_core` 会暂停 `on_tick`，
   但**不管你自己创建的 timer**）。

5. **整屏 alpha 淡入是软件渲染最贵的操作，而且极易做成"重影"。**
   `ui_core` 曾经在切页时对两个 800×480 容器同时动画 `opa`（12~15 帧走混合路径，
   每像素读-改-写）。优化时把新页改成"不透明 + 只位移"，**结果出现严重重影**：
   两个容器同尺寸且不透明，新页从偏移处滑入，未重叠的边条与滑动过程中的每一帧
   都会把上一页露出来。而旧页的隐藏**只**挂在退场动画的完成回调上，
   `page_anim_reset()` 又会 `lv_anim_delete()` 掉那个动画（回调一起没），
   于是"退场结束前切走/切回"就永久留下残影。
   → **v1.2.0 已把整页动画全部删除，切页改为同帧原子替换**，并用
   `pages_show_only()` 把"任意时刻只有一个页面容器可见"这条不变量钉死。
   教训：**整页过渡不要靠"谁盖住谁"来正确性**；真要做，也必须有一个与动画无关的
   隐藏路径。内部元素的小范围动画（位移/缩放）仍然便宜，可以放心用。

**测量**：`CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS=y` 已开，可用
`esp_lv_adapter_fps_stats_enable()` + `esp_lv_adapter_get_fps()`（`esp_lv_adapter.h`）打点。
更方便的是打开 `CONFIG_TODOHELPER_FPS_PROBE`（默认关），状态栏那个 1 Hz 定时器会每秒打
一行 `TH:ui_core: fps N`，不用改代码。实测待机眼睛动画稳定在 **28~30 fps**
（即设计值 `IDLE_FACE_TICK_MS=33`），静止页面为 0（LVGL 没脏区就不重绘，属正常）。

### 9.0.1.2 排查性能问题时**不要**把全局日志调到 DEBUG

`CONFIG_TODOHELPER_LOG_LEVEL=3`（DEBUG）会让底层驱动日志淹没串口：实测 140 秒内
**2 万行**，其中绝大部分是 `adc_hal` 的每次采样输出。后果有两个，都很容易误判：

1. **应用自己的日志全被挤掉**（115200 波特下根本发不完），于是"改了代码却看不到日志"，
   看起来像新代码没执行；
2. **看起来像代码把 CPU 占满了**：实测出现
   ```
   E task_wdt: Task watchdog got triggered ... - IDLE0 (CPU 0)
   E task_wdt: CPU 0: lvgl
   ```
   即报 `lvgl` 任务饿死 IDLE0。这是**日志假象**（UART 写阻塞 + 底层高频打印），
   不是渲染缺陷 —— 同一份固件在 INFO(2) 下 **0 次 wdt**。

→ 要验证某条 DEBUG 日志是否真的执行，把它**临时改成 `SYS_LOGI`**（或只对该 tag
`esp_log_level_set`），验证完改回去。别动全局级别。

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

#### 9.0.4.1 音标（IPA）缺字："字库选字少了"是错的，源字体根本没有这些字形

**现象**：屏上 `/əˈbrʌpt/` 渲染成 `/ br pt/` —— 元音符号整片空白（v1.3.0 修复）。

**根因不在我们这边，而在源字体**：**Noto Sans SC 里一个 IPA 字形都没有**。
U+0259 ə、U+028C ʌ、U+026A ɪ、U+02D0 ː、U+02C8 ˈ 等**都不在它的 cmap 里**，
所以无论把字符集写多大都取不出来。**LVGL 内置的 Montserrat 兜底也救不了**：
它的 cmap 很稀疏，只有 FontAwesome 区段 —— 也就是说 §9.0.4 那条 fallback 链对 IPA 无效。

**修法**（`tools/gen_cjk_font.py` 的 `merge_ipa_glyphs()`）：把 IPA 字形从**一份拉丁字体**
合并进 CJK 子集。Latin 源按平台挑：Linux 用 DejaVu Sans，Windows 用
`segoeui.ttf` / `arial.ttf` / `calibri.ttf`。

四个必须记住的细节：

1. **`fontTools.merge` 用不了**。它断言两边的 `unitsPerEm` 相等，而拉丁字体是 **2048**、
   CJK 子集是 **1000** —— 必然不等。所以只能手写合并，并把坐标与 advance **缩放
   `1000/2048 = 0.488`**。实测从 `segoeui.ttf` 合入 **203 个字形**。
2. **字形必须展平成普通轮廓**（`flatten_components()`）。组件（composite glyph）会引用
   目标字体里**不存在的**字形，编译期直接炸 `KeyError: 'uni02BE'`。
3. **`hmtx` 与 `vmtx` 必须一起更新**，`hhea` / `vhea` 的计数要夹住 —— 见下一节，
   这是本次真正阴险的那个 bug。
4. **这份 CJK 子集带中文竖排度量**（`vmtx` / `vhea` 都在），所以第 3 条不是理论问题。

**验收数值**（v1.3.0）：词书 **1252 个不同字符，0 个缺失**；`/əˈbrʌpt/` 全部有真实轮廓，
advance 已正确缩放（ə=523 vs b=618 / r=388 / p=620）。

#### 9.0.4.2 合并字形时 `hmtx` / `vmtx` 必须同时写，而**只查 cmap 查不出来**

**这是 v1.3.0 修掉的第二个、潜伏的 bug，也是"检查写错了比没有检查更糟"的教材。**

第一版 `merge_ipa_glyphs()` 更新了 `hmtx` 却**漏了 `vmtx`**。`hmtx` 与 `vmtx` 都以
**字形名**为键、都**必须每个字形一条记录**，只补一半的文件本身就已经不合法：

```
not enough 'vmtx' table data: expected 10686 bytes, got 10280
```

即 **`vmtx` 少了 406 字节 = 2 字节 × 203 个新字形**，fontTools 连**重新解析**这个文件都做不到。

> **为什么它会出厂**：当时的校验只问了一句 **"文件里有没有 ə？"** —— 而坏字体
> **每一个 cmap 条目都在**，所以检查愉快地通过了。**cmap 只描述"这个码位映射到哪个字形名"，
> 完全不涉及度量表。** 一个只查 cmap 的检查无法发现任何度量表损坏。

**修法两半**：

1. 给每个合入的字形**同时写 `vmtx` 记录**（拉丁源没有 `vmtx` 时用 `vhea` 的默认
   advance + tsb 0 兜底），并把 `hhea.numberOfHMetrics` / `vhea.numberOfVMetrics`
   **夹到字形总数以内**（`min(现值, numGlyphs)`）。
2. 新增 **`validate_font()`**：在脚本最后**重新打开保存后的文件**，逐项核对
   - `len(hmtx.metrics) == maxp.numGlyphs`，`vmtx` 同理；
   - `hhea.numberOfHMetrics` / `vhea.numberOfVMetrics` **不得超过**字形总数；
   - 必需字符在 cmap 里。
   任何一条不过 → `sys.exit()`，**构建直接失败**。

**"必需字符"只算内容字符**（词书里的字符 + 有拉丁源时的 IPA），
**不要求**整个装饰性 Unicode 区段。否则检查会天天报一堆无关缺字，
最终所有人学会无视它 —— 那等于没有检查。

> **通用教训**：字体这类**多表互相索引**的二进制格式，"改一处"几乎总是"改好几处"。
> 事后校验必须**重新打开产物**并**按表逐项核对**，而不是在原对象上查一个字段；
> 而且校验的**覆盖范围要对着 bug 的类型设计**（这里：度量一致性问题，不是覆盖问题）。

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

### 9.0.8 板子上的调试 shell：让"只能靠看屏幕"的 bug 变成可脚本验证（v1.3.2）

**动机**：v1.3.1 修"鉴权弹窗关不掉"时，验证手段是**"请你在板子上点一下、告诉我看到什么"**。
板子不在手边就完全没法验证；而且以前**没有办法从脚本切页或按控件**，所以凡是
"点一下"后面的功能都是不可测的。`components/debug_shell/` 补上这一环：
一条命令进去、文本输出出来。

```bash
bash tools/dsh.sh heap                 # 内部 RAM / 最大连续块 / int-low
bash tools/dsh.sh page settings        # 切页（顺带打印切页耗时）
bash tools/dsh.sh widgets 生成         # 对象树（按文本过滤）
bash tools/dsh.sh click 生成并开启      # 按控件，并当场报出弹窗
bash tools/dsh.sh tap 745 162 click    # 按坐标（能按到没有文字的「×」）
bash tools/dsh.sh ftstats              # 字形缓存条目数（各字号）
bash tools/dsh.sh bench-screen 6       # 整屏重画 6 次（= 滚动一帧的代价）
```

> **v1.8.5 新增的三条测速命令。**
> `bench-screen` 整屏作废 + `lv_refr_now()` **同步重画** N 次 —— 这就是
> **滚动一帧的代价**；第 1 次是冷的（把没缓存的字光栅化一遍），
> 第 2 次起是**稳态**：稳态高 = 字形缓存装不下这一屏的字，每一帧都在重画同样的字。
> `bench` 会设标签文本 → 同步重绘 → 打印耗时（单个标签的辅助量法）。
> 光设文本是不触发的（画字发生在下一次刷新里），所以不强制重绘就量不到
> 光栅化的成本 —— 而「界面卡」几乎总是卡在这里。
> `ftstats` 打印 FreeType 字形缓存的条目数：四个字号报出来的数字
> **一模一样**是正常的（缓存是整个界面共用一份，见 §9.0.1.8 第 17 条）。
> 两个脚本 `tools/page_tour.py`（巡游所有页面并打印堆）和
> `tools/bench_glyphs.py`（报滚动一帧的稳态耗时）把这两条命令串了起来。

#### ① `click <标签>` 必须自己往上找**可点击祖先**

第一版把事件直接发给匹配到的对象 —— 而按钮的文字在**子 label** 上，于是
`click 生成并开启` 匹配到 label、对**不可点击**对象发事件、**静默什么都不发生**。
屏幕上看起来就是"点了没反应"，和它要诊断的那个 bug 一模一样（好在这回是工具的锅）。
LVGL 真实触摸是路由到最近的可点击祖先的，**shell 必须照做**，否则复现不了"手指点下去会怎样"。

#### ② 弹窗必须在**点击的同一个调用里**同步检查

隔一次 HTTP 请求再看，是**分不清**"没打开"和"已经关掉"的 —— 而那个按钮本身
**会轮换鉴权令牌**，客户端再取一次令牌早就来不及。所以 `click` 当场报
`top layer now: N task(s), M msgbox(es)`，并且在有弹窗时顺手打印它的整棵对象树。
**这一条是让"弹窗关不掉"可无人验证的关键**：一次请求同时证明"点击真的开了弹窗"
和"弹窗长这样"。

#### ③ `lv_obj_hit_test()` **不搜索**，用 `lv_indev_search_obj()`

前者只回答"这个点在不在这个对象里"。后者才是 LVGL 自己的命中测试，而且要
**先搜顶层**（顶层画在上面），这正是模态弹窗会吞掉页面点击的原因。
`tap <x> <y> click` 也是唯一能按到**没有文字的控件**的办法 —— 弹窗的「×」是个 image。

#### ④ UI 命令**必须回到 LVGL 任务**执行

`ui`/`widgets`/`click`/`text`/`page`/`tap` 都走 `ui_run_async()` + 信号量
"派回 LVGL 任务并等结果"。直接在 httpd 任务里调 LVGL 会破坏对象树（或触发
`lv_obj_has_class` 断言）；纯异步又会**在结果存在前就回复**。等待是安全的：
LVGL 任务从不反向阻塞 httpd 任务。

#### ⑤ 工具本身的三个坑（都会伪装成"设备有问题"）

1. **`out_printf` 不能固定 512 字节缓冲**：`help` 一次打印整张表，`vsnprintf`
   静默截断到 511 字节（输出停在 `"  http-auth "`）。**调试工具的静默截断是最糟的
   失败模式** —— 看起来像命令本身坏了。现在用 `vsnprintf` 的**返回值**（本应写入的
   长度）二次分配到堆上。
2. **`http_reply_ok()` 要的是"已序列化的 JSON"**：第一版把多行文本直接塞进去，
   得到 `{"code":0,...,"data":project ...\nversion 1.3.1\n}` —— **不是合法 JSON**，
   所有客户端解析全挂。用 cJSON 编码，且**别再包一层 `data`**（`http_reply_json`
   自己会包，否则变成 `data.data.out`）。
3. **读 NVS 会重启板子**：esptool 无法在应用运行时附加（`--no-reset` 报
   `Failed to connect ... No serial data received`），必须拉进 ROM 下载模式。
   于是"取一下鉴权令牌"会**在测试序列中间静默重启设备** —— 我因此把"切到设置页后
   又变回主页"误判成 UI bug，实际是工具在复位。`tools/dsh.sh` 现在**优先查日志**
   （设备生成令牌时会打一行），NVS 只作兜底，并注明这个副作用。

> **记法**：写调试工具时，"工具没生效"和"被测对象有问题"必须能从输出上区分开。
> 静默失败（截断、no-op、复位）在两边都最常见。

#### ⑥ 顺带测出来的一件事：首次进单词页吃掉 ~25 KB 内部 RAM

同一轮用 `heap` 前后对比（真机）：

| 时刻 | 内部空闲 | 最大连续块 |
|---|---|---|
| 刚启动 | 74 KB | 18 KB |
| 首次进入单词页后 | 49 KB | **5 KB** |

离开页面**不回收**；反复采样**稳定**（不是持续泄漏）。那个 5 KB 正是
`page_word.c` 里**每张卡一个、5 KB 栈**的 `th_mm_enrich` worker ——
和 §9.0.7 ③ 给评分路径做 NO_MEM 降级时的判据完全一致。
**改单词页时请注意这里**：它是本板最紧的一处。

### 9.0.7 三个上机 bug 的共同教训：**根因一律不在报错的地方**（v1.3.1）

用户报了三个 bug，三个的"现象"与"根因"都不在同一处。这一节按"现象 → 真根因"写，
因为这三个坑的共同点就是**顺着报错看会看错方向**。

#### ① LVGL：事件回调里 `lv_event_get_current_target()` 是**回调注册的那个对象**

**现象**：设置页生成 token 的弹窗**关不掉**（点「知道了」没反应）。

**根因**：关闭回调是这样写的（v1.3.0 及以前）：

```c
static void dialog_close_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_current_target(e);  /* ← 这是"按钮"，不是消息框 */
    lv_msgbox_close(mbox);                            /* 对按钮调用 = 静默 no-op */
}
...
lv_obj_t *btn = lv_msgbox_add_footer_button(mbox, "知道了");   /* 返回的是按钮 */
lv_obj_add_event_cb(btn, dialog_close_cb, LV_EVENT_CLICKED, NULL);
```

`lv_msgbox_add_footer_button()` 返回 **footer 里的按钮**，回调也注册在它上面，所以
`lv_event_get_current_target()` 拿到的是按钮。`lv_msgbox_close()` 对一个按钮什么都不做，
**不报错、不警告**。所有"点了没反应"的控件回调都值得先查这一条。

**修法**：把消息框作为 **event user data** 显式传入：

```c
lv_obj_add_event_cb(btn, dialog_close_cb, LV_EVENT_CLICKED, mbox);
/* 回调里：lv_obj_t *mbox = lv_event_get_user_data(e); */
```

**并额外加一个 LVGL 自带的关闭按钮**：`lv_msgbox_add_close_button()` 内部装的
`msgbox_close_click_event_cb` 本来就把回调绑在消息框上，是天生的正确实现 ——
与其手写，不如复用它。两条路（「知道了」+「×」）都能关。

> 记法：`lv_event_get_target()` = 触发事件的对象（可能是子对象）；
> `lv_event_get_current_target()` = **回调被注册到的**对象；`lv_event_get_user_data()` = 注册时传的。
> 想关"容器"就**别**靠从按钮反推容器。

#### ② 弹窗"竖着被挤压"：面板是 800×480 **横屏**，弹窗也该横着排

`lv_msgbox` 默认**纵向堆叠**（`LV_FLEX_FLOW_COLUMN`）：标题 / 正文 / 内容 / footer 一层层往下。
480 px 高减去状态栏，装不下"说明 + 32 字符 token + 备注"，于是被挤。
（用户原话："面板挤压太长了，应该是横着的而不是竖着"。）

**修法**：把"说明 + token"放进一个 `LV_FLEX_FLOW_ROW` 的容器里两列并排。
实测几何（`ui_widgets.c`）：对话框 `94% × 800 = 752 px`，内宽 720 px，
减去列间距 14 → 两列各 **~353 px**；32 字符 token 在 BODY 字号下一行约需 **320 px**，
**正好一行放下**。定宽算式比"看着差不多"可靠，改字号时按同样的算式复核。

**样式统一**：LVGL 默认 msgbox 是浅色皮肤，和本工程的深色卡片完全不是一个东西。
`dialog_style()` / `dialog_style_button()` 把 `UI_COLOR_SURFACE`/`_ALT`/`_PRIMARY`、
`UI_CARD_RADIUS`、正文/标题字体一次性套上，**新弹窗直接复用这两个函数**。

> **`lv_msgbox_create(lv_layer_top())` 不会创建独立遮罩** —— 传 `NULL` 才会
> （`lv_msgbox.c` 里 `auto_parent` 分支）。所以此时 `lv_obj_get_parent(mbox)` 拿到的是
> `lv_layer_top()` 本身，而 `ui_toast()` 也挂在那里：给它上色会**把 toast 一起压暗**。
> 现在靠消息框自己的 `shadow` + 不透明底色和页面区分。

#### ③ `ESP_ERR_NO_MEM` ≠ 内存用光了：**是"任务栈只能来自内部 RAM"**

**现象**：单词背诵页点「认识 / 模糊 / 不认识」弹 `记录失败：ESP_ERR_NO_MEM`。

**真根因链**（每一步都有真机数字）：

1. `word_review()` → `word_save_progress_async()`，后者 `xTaskCreate(..., 4096, ...)`
   起一个落盘 worker；
2. **FreeRTOS 任务栈只能从内部 RAM 分配**（§9.0.1，且这个栈还要能碰 flash）；
3. 本板 boot 结束内部 RAM 实测只有 **57 KB 空闲、最大连续块 13 KB**（v1.3.1 实测，
   v1.3.0 记录的是 18 KB，可见它是浮动的）；
4. 于是 `xTaskCreate` 返回 `pdFAIL` → 函数 `return ESP_ERR_NO_MEM` → UI 原样弹出。

**判据**：报 `NO_MEM` 但 PSRAM 还有 10 MB 时，**不要去找"谁吃光了内存"**，
先问"这个分配是不是必须要内部 RAM / 必须要连续"。这里 4 KB 的落盘 worker 与 4 KB 的
enrich worker **可能同时存在**，撞上 13 KB 的连续块就是必然。

**修法（两条都做了）**：

- 建不出 worker **不再当作失败**：回退为**就地同步写盘**。慢，但绝不丢评分 ——
  而且评分其实早就生效了，只是没落盘，"记录失败"的提示本身是误导。
- **把阻塞 TLS 移出 LVGL 回调**：`grade_clicked_cb` 原来直接调
  `maimemo_push_unknown()`（阻塞 TLS + 建任务，全在**持 LVGL 锁**的回调里），
  这正是上面 NO_MEM 的触发窗口。新增 `maimemo_push_unknown_async()`：
  廉价检查（开关 / 令牌）留在调用方，网络交给 worker。

> **一般化**：凡是"回调里建任务"，都要假设它会失败并给出降级路径。
> 优先级排序：**丢数据 > 卡 UI > 报错弹窗** —— 报错弹窗是最差的一种，
> 因为它让用户以为功能坏了，而数据其实还在。

#### ④ 第三方 beta 端点：**空数组不等于"没有数据"，先查文档的请求体**

**现象**：墨墨"无法推送单词和获取今日单词"。

**根因**：`maimemo_fetch_today()` 发的请求体是 **`{}`**。用真实令牌对比实测：

```bash
POST /study/get_today_items  {}                         # -> {"today_items":[]}
POST /study/get_today_items  {"is_finished":false,...}  # -> 51 个词
POST /study/query_study_records {}                      # -> 58 条记录
```

官方文档（`maimemo/memo-skills` 的 `study-api.md`）写明该端点 **Beta 公测**：
"需要打开自动同步。**如果当日未打开 App 进行初始化则无法准确计算**"，
且 `get_today_items` 的 body 参数**全部可选**（`is_finished` / `is_new` / `voc_ids` /
`spellings` / `limit`）—— 文档给的"取今日队列"写法就是带 `is_finished` 的那条。
**空数组是"beta 端点今天没话说"，不是"账号没词"**（同一账号 `query_study_records`
明明有 58 条）。

**修法**：

- 请求体改为 `{"is_finished":false,"limit":<max>}`（官方写法）；
- **加兜底**：`today_items` 为空时改用**非 beta** 的 `POST /study/query_study_records`
  重建队列（`mm_fetch_today_from_records()`），`last_response` → `first_response`，
  `study_count==0` → 新词。这样"账号有计划词"与"屏幕有词可背"不再脱钩；
- **注意响应体积**：一条 record ≈ 350 B（60+ 字符的 voc_id 占大头），24 KB 的
  `MM_MAX_RESPONSE` 只装 ~60 条。所以 `MM_RECORDS_MAX=56` —— 请求更多会让**响应**
  溢出而整次调用失败，**比队列短更糟**。实测 56 条 = 19656 B，在 24 KB 内。

> **一般化**：对接 beta / 第三方 API 时，"我发的请求合法"与"对端会给我数据"是两件事。
> **要拿到"应当有数据"的那个端点的实测响应**（把文档给的 curl 原样打一遍），
> 而不是只验证自己的代码没报错。这已经是本组件第二次踩同类坑（见 §9.0.6 ①）。

### 9.0.6 对接第三方云 API：`maimemo_client` 踩到的四个坑（v1.3.0）

墨墨背单词开放 API 的形态很典型：**请求带 Bearer 令牌、响应统一是
`{"success":bool,"data":{...},"errors":[...]}` 信封**（多数端点是 POST + JSON body，
但**读助记/例句是 GET** —— 这正是坑 ③）。四个坑都很有代表性。

#### ① HTTP 200 也可能是失败：**只看状态码会漏掉全部权限/参数错误**

**实测**：权限不足时墨墨**照样返回 HTTP 200**，错误只写在信封里：

```json
{ "success": false, "data": null,
  "errors": [ { "code": "common_permission_denied", "msg": "..." } ] }
```

所以**必须解析信封**，不能拿状态码当判据。本项目在 `mm_classify()` 里统一做：
含 `permission_denied` → `ESP_ERR_NOT_SUPPORTED`，其余 → 其他错误。
HTTP 层的映射单独一层：`401`/`403` → `ESP_ERR_INVALID_STATE`、非 200 →
`ESP_ERR_INVALID_RESPONSE`、响应超限 → `ESP_ERR_INVALID_SIZE`。

- **`ESP_ERR_NOT_SUPPORTED` 的语义要定死**："这个令牌不许做这件事，**别再问了**"，
  与"网络抖动，稍后再试"**必须能被调用方区分开**。本项目的做法是按词**缓存为不可用**
  （`words/enrich.json` 里的 `unavailable:true`），于是每个词只会被问一次，
  而不是每次翻卡都去撞一次墙。
- **令牌作用域是一等一的正常情况，不是错误**。常见的学习作用域令牌：
  `/study/*` 与 `/vocabulary` 可用，`/notes`、`/phrases` 返回 `common_permission_denied`。
  一个"能同步队列但不显示助记"的设备**仍是有用的**，一个"报错"的设备不是 ——
  所以 UI 只是**不显示**助记块，不弹错误。
- **校验令牌失败 ≠ 令牌坏**：`POST /api/v1/maimemo/config` 的 `verify` 是**显式请求**
  （会真的访问一次，5~15 s），且失败只回一个 `verify` 字段，**不否定保存**。

#### ② `advance` 是**必需**属性：漏了它就报 `common_invalid_param`

**实测**：`POST /study/add_words` 漏掉 `advance`，**即使请求其它部分完全合法**也失败：

```
common_invalid_param: {base} must have required property 'advance'
```

`advance` 不是可选项，是**必填字段**。→ 本项目**永远发送**它（`false` 也发）。
`advance: true` 让词进入可立即复习的状态（降低等级门槛）。

请求体形状是 **`{"words":[{"id":<voc_id>},...],"advance":bool}`** —— **按 `id` 加词，不是按拼写**，
所以设备侧先用 `GET /vocabulary?spelling=` 把拼写解析成 `voc_id`（`mm_resolve_voc_id()`），
解析不到的词跳过。这与官方 CLI 一致（`advance` + `words[].id`）。

**并且回报服务器自己的 `added_count`，而不是我们发过去的条数** —— 服务器可能只收下一部分，
报"我发了 10 个"会造成假象。

> **通用教训**：对接第三方 API 时，**"哪些字段必填"只能靠实际打一次来确认**（文档常常
> 只列字段不讲必填），而且**返回值要以对端自报的为准**。所有"设备侧看起来成功"的结论，
> 都必须回到**对端的状态**去验证。

#### ③ GET 还是 POST **不是风格问题，是语义问题**

**墨墨的助记/例句读取端点是 `GET`**：
`GET /api/v1/notes?voc_id=...` 与 `GET /api/v1/phrases?voc_id=...`。

**同一个路径的 `POST /api/v1/notes` 是"创建笔记"**（body `{"note":{voc_id,note_type,note}}`）
—— 拿 POST 去"读"助记，轻则读到完全不同的东西，**重则改写用户在墨墨里的笔记**。
所以本组件为此并列了两个函数：`mm_get()` 与 `mm_post()`，而不是图省事全用 POST。

- 已对照官方 CLI 源码核实（`@maimemo/memo-api` v1.2.0：
  `api("/api/v1/notes", {method:"GET", query:{voc_id}})`，响应按 `res.notes.map(...)` 取）。
- 响应结构：`{"data":{"notes":[{"note":"..."}]}}` 与 `{"data":{"phrases":[{"phrase":"..."}]}}`
  （`mm_fetch_field()` 取数组第 0 项的文本字段）。
- 拼写 → `voc_id` 的解析也是 GET：`GET /vocabulary?spelling=`。

> **通用教训**：REST 里"读用 GET、写用 POST"在第三方 API 上常常**不是**这样。
> 遇到"同名路径既能读又能写"的接口，**先确认方法的语义再发请求** ——
> 一个自以为只读的 POST 可能正在改用户的数据。

#### ④ 顺带：TLS 客户端绝不能从 LVGL 任务调用

`maimemo_client` 的所有网络函数都是**阻塞 TLS**，而 LVGL 的控件事件 / `lv_timer` 回调都在
**适配器锁内**运行（§9.0.1.1）。所以：

- 唯一对 UI 安全的入口是 `maimemo_sync_start()`（起 worker 任务、立刻返回、结果走
  `MAIMEMO_EVENT_SYNC_DONE` / `_FAILED`）；`maimemo_has_token()` 与
  `maimemo_get_cached_progress()` 只读内存/NVS，也可以从 UI 调。
- `page_word.c` 的助记预取用 `lv_async_call` 起一次性任务，结果在 LVGL 任务里应用标签。
- **与 AI Key 同规则（需求 §8.8）**：令牌存 NVS `mm_token`，**不记日志、任何 API 都不回读**；
  `GET /api/v1/maimemo/status` 只回 `configured` 与掩码提示（`g_zV****`）。
- **无令牌静默降级**：`mm_token()` 失败返回 `ESP_ERR_NOT_FOUND`，云端模式无声关闭，
  本地词库照常工作 —— 可选组件失败不得阻止启动。

组件细节见 `components/maimemo_client/README.md`。

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
│   ├─ cet6.json         # 实际使用的 CET-6 精选词库（833 词，来自卡上/仓库 assets）
│   ├─ enrich.json       # ★ 墨墨助记/例句缓存（方案 B，整文件读进 PSRAM、整文件原子重写）
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

> **v1.8.5 之后最该做的三件事**（按收益排序，细节见 `docs/ROADMAP_v1.8.5.md`）：
>
> 1. **字库搬进内部 flash。** 现在光栅化一个字要 10 ms，其中约 9.8 ms 是
>    **从 SD 卡读 TTF**（FreeType 自己只要 100~200 µs）。字库进了内部 flash，
>    光栅化就能掉到 0.2 ms 上下 —— 缓存偶尔没命中也不卡，顺带解决
>    「SD 卡挂不上就满屏方块」。
> 2. **页面控件别销毁重建。** v1.8.3 为了内存放弃了常驻。
>    现在字形缓存不占内部堆了，账要重算：实测各页内容约
>    todo 8 KB / calendar 23 KB / settings 28 KB / 其余 2~6 KB，全部常驻约 80~90 KB；
>    内部堆现在有 150 KB 空闲 —— 够，但要给 WiFi PHY 留连续块（§9.0.3），
>    这笔账要单独验。
> 3. **TLS 连接复用。** 每次开机 3~4 次握手 ≈ 18~24 秒的射频时间。
>
> 其余老待办（TLS keep-alive、httpd socket 槽位、SD 写放大）见下面各条。

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

**③ 墨墨云端同步的"快乐路径"未在设备上验证（v1.3.0 留下的第一件事）** ——
代码已实现、接口已用真实令牌对**线上 API** 实测、权限拒绝的处理已验证，但**本轮没有烧录**。
具体缺口：

- **方案 B（助记/例句）读不到数据**：当前令牌是**学习作用域**的，`/notes` 与 `/phrases` 恒返回
  `common_permission_denied`，所以"卡片上真的显示出一条助记"**从未被观察到**。
  要验证需要一个能读 `/notes` 的令牌，或等墨墨放开该作用域。
- **字库修复未上机目视确认**：字体是运行期从 `<storage>/todohelper/fonts/ui_cjk.ttf` 加载的
  （**没有烘进镜像**），所以要验证 IPA 修复，**拷文件即可**，不必重新烧录。
  判据：卡片背面的音标里 `/əˈbrʌpt/` 的 ə、ʌ、ˈ、ː **不再是空白**。
- **云端模式与本地进度是两套记账**（刻意取舍）：云端词匹配回本地词库后用本地 level / 间隔；
  匹配不上的词（`WORD_ID_CLOUD`）在设备上评分只留痕在账号侧，本地不记进度。
  换回本地书时两边数字不会合并。
- **方案 C 没有反向删除**：接口只负责加词，推上去的词撤不回来（所以默认关闭）。

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
| **音标/IPA 符号是空白**（如 `/əˈbrʌpt/` → `/ br pt/`） | 源字体（Noto Sans SC）根本没有 IPA 字形，Montserrat 兜底也无效；跑 `tools/gen_cjk_font.py` 合并拉丁字形，见 §9.0.4.1 |
| `not enough 'vmtx' table data: expected N, got M` | 合并字形时只写了 `hmtx` 没写 `vmtx`；见 §9.0.4.2，跑 `validate_font()` 会先拦住 |
| 墨墨接口"成功"但数据不对 / 权限不足却看不到错误 | API **权限错误也返回 HTTP 200**，必须读信封的 `success`/`errors[].code`；见 §9.0.6 ① |
| `common_invalid_param: {base} must have required property 'advance'` | `/study/add_words` 的 `advance` 是**必填**，`false` 也要发；见 §9.0.6 ② |
| 墨墨助记/例句读不到、或"读"完用户笔记被改了 | 读取端点是 **GET** `/notes?voc_id=`、`/phrases?voc_id=`；**`POST /notes` 是创建笔记**；见 §9.0.6 ③ |
| **弹窗点按钮关不掉**（没反应、不报错） | 回调里用 `lv_event_get_current_target()` 反推容器拿到的是**按钮**；把容器作 user data 传进去，见 §9.0.7 ① |
| **弹窗样式不像本工程 / 内容被挤成竖条** | `lv_msgbox` 默认**纵向**堆叠 + 浅色皮肤；面板是 800×480 横屏，应两列横排并套 `dialog_style()`；见 §9.0.7 ② |
| 点单词评分弹 `记录失败：ESP_ERR_NO_MEM`（PSRAM 还剩 10 MB） | `xTaskCreate` 的**栈只能来自内部 RAM**，此处最大连续块仅 ~13 KB；见 §9.0.7 ③ |
| 墨墨 `/study/get_today_items` 返回**空数组**但账号明明有计划词 | 该端点 **Beta**，body 全可选；必须发 `{"is_finished":false,"limit":N}`，空时回退 `query_study_records`；见 §9.0.7 ④ |
| `bash tools/build.sh build` 日志是空的但退出码 0 | MSYS 调 `idf.py` 的四个坑（函数不绑定 / 管道 / `env -i` / `MSYSTEM` 重注入）；见 §4.2.1 |
| 天气图标不显示 | `assets/ui/` 里有子目录，或改了素材没 `idf.py reconfigure` |
| `idf.py: command not found` | 未加载 IDF 环境，见 §4 |
| `Could not open COMxx ... busy` | 关掉占用串口的监视器/其它工具；确认用的是 COM34 |
| 组件管理器重新下载依赖 | 打补丁后 `idf.py reconfigure && idf.py build` |
| 需要从零复现 | `idf.py fullclean` → 应用补丁 → `idf.py build` |
| **要验证"点一下才出现"的 UI**（弹窗/切页/控件） | 用调试 shell：`bash tools/dsh.sh click <标签>` / `page <名>` / `widgets`；见 §9.0.8 |
| `tools/dsh.sh` 一直 401 | 令牌被"生成 token"轮换了；脚本会自动重试与记缓存，必要时 `DSH_TOKEN=...`；见 §9.0.8 ⑤ |
| 切到某页后又"自己回到主页" | 先怀疑工具：读 NVS 取令牌会**复位板子**（§9.0.8 ⑤3）；否则看 `idle-timeout` |
