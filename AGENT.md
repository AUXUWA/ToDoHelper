# AGENT.md

面向在本仓库工作的 AI/开发者的工作说明：**当前已实现的驱动与 App 功能**、本机环境、常用命令、
必须保留的补丁、验证基线、排错速查。上游 README 的概述不再重复，这里只写「实际是什么状态、怎么在这里把事做成」。

---

## 1. 项目是什么

- 来源：`espressif/esp-dev-kits` 的 `examples/esp32-s31-korvo/examples/factory_demo`（本机克隆在 `D:\project\ESP-IDFv6.1\esp-dev-kits`）。
- 内容：ESP32-S31-Korvo 板子的产测固件（RGB 屏 + GT1151 触摸的 LVGL 咖啡机 UI、OV3660/SC101 DVP 摄像头预览、音频录制回放、按键/WS2812/USB HID、SD 卡读写探测）。
- 芯片目标：**esp32s31**（preview 目标）。
- 应用名：`esp32_s31_korvo_factory_demo`。

## 2. 已实现的驱动与 App 功能

> 依据当前代码（`main/`、`ui/`、`components/` 与工程外的 Korvo BSP）整理。**改动驱动或功能后请同步本节。**

### 2.1 板级驱动（Korvo BSP：`..\common_components\esp32_s31_korvo`）

| 模块 | 实现方式 | 关键参数 |
|---|---|---|
| RGB 屏 | `esp_lcd_new_rgb_panel` → `esp_lv_adapter_register_display` / `esp_lv_adapter_start` | 800×480、16-bit 并口、PCLK 26MHz、H(BP40/FP20)、V(BP10/FP5)；背光/使能脚为 NC |
| 电容触摸 | `esp_lcd_touch_new_i2c_gt1151` → `esp_lv_adapter_register_touch` | GT1151，I2C SDA=GPIO0 / SCL=GPIO1；RST/INT 为 NC |
| 摄像头 | DVP + SCCB，`esp_video_init()`，对外 `bsp_camera_open/start/get_frame/return_frame/set_orientation` | 8-bit DVP（D0-7=GPIO46~53、PCLK54、XCLK55、VSYNC56、HSYNC57），XCLK 20MHz、SCCB 10kHz，默认 1280×720、2 帧缓冲 |
| 音频 | `i2s_new_channel` + esp_codec_dev 双句柄（`bsp_audio_codec_microphone_init` / `bsp_audio_codec_speaker_init`） | 默认 48kHz / 16bit / 2ch；MCLK2、SCLK3、LRCLK4、DSIN6、SDOUT5、PA_CTRL=GPIO7 |
| SD 卡 | SDMMC 4-bit（`bsp_sdcard_mount` / `bsp_sdcard_unmount`） | D0-3=GPIO20~23、CLK24、CMD25、电源开关 GPIO39（低有效）；挂载点 `/sdcard`、HIGHSPEED、16KB alloc |
| 按键 | ADC 单线梯形 4 键 + espressif/button（`bsp_iot_button_create`） | ADC=GPIO42；VOLUP 0.38V / VOLDOWN 0.82V / MODE 1.34V / SET 1.87V |
| WS2812 | led_strip（RMT），`bsp_led_set_rgb` / `bsp_led_clear` | GPIO37，单颗状态灯 |

注意：**GPIO20~25 与 SPI NAND flash 脚位复用**，同一块板只能装其中一个。

### 2.2 App 层（`main/`）

| 文件 | 职责 | 实现要点 |
|---|---|---|
| `main.c` | 总编排 + 工厂自检界面 | 顺序：摄像头（先占用共享 I2C）→ `bsp_display_start_with_config` → `coffee_assets_init` → 按键/LED → 音频 → USB HID → `ui_init()`。LVGL 800×480、buffer 高 160、`TEAR_AVOID_MODE_TRIPLE_PARTIAL`、开 PPA 加速、显示任务栈 16KB；100ms 定时器把各模块快照刷到 UI；咖啡机 UI ↔ 工厂界面通过独立任务切换 |
| `factory_demo_camera.c` | 摄像头预览渲染到 LVGL canvas | esp_video / V4L2 取帧（`linux/videodev2.h`）→ PPA `ppa_do_scale_rotate_mirror`(SRM) 缩放/旋转到 240×240 RGB565 → LVGL canvas；`esp_cache_msync` 维护 cache 一致性；支持镜像/翻转、JPEG 质量、渲染开关、首帧等待(3s)；状态机 IDLE/STARTING/PREVIEW/ERROR |
| `factory_demo_audio.c` | 录音 / 回放自检 | esp_codec_dev 打开 mic 与 speaker，48kHz/16bit/2ch，任务内 `esp_codec_dev_read` / `esp_codec_dev_write` 搬运，输入增益与输出音量可调；状态机 IDLE/RECORDING/PLAYING/ERROR + 快照（已录字节/容量） |
| `factory_demo_usb_hid.c` | USB HID Host 自检 | `usb_host_install` + `hid_host_install`，解析键盘/鼠标 usage（`hid_usage_keyboard.h` / `hid_usage_mouse.h`）；维护连接/断开/输入计数、鼠标坐标与左右键、设备名、最近输入；状态机 IDLE/WAITING/CONNECTED/DISCONNECTED/ERROR |
| `coffee_assets.c` | UI 资源加载 | esp_mmap_assets 从 `coffee_pjpg` 分区（0x810000）mmap PNG/字体，`coffee_assets_init()` / `coffee_assets_get_src()` 供 LVGL 使用 |

### 2.3 UI（`ui/`，SquareLine Studio 1.5.4 导出）

- **12 个屏幕**：Splash_screen、Home、Home_espresso、Home_ristretto、Customize、Customize2、Ready、Brewing、Brewing_Done、Add_water、Add_coffee、Settings。
- **15 个自定义组件**：button、ready_button、cta_container、header、home_sliders、homeslider、parameter、progress、selector、separator、slider1、slider3、steps 等。
- 工厂自检界面不是 SquareLine 生成的，而是 `main.c` 里手写 LVGL 卡片（摄像头画布、音频、SD、按键/LED、USB HID 状态区）。

### 2.4 依赖组件（`managed_components/`）

| 组件 | 版本 | 用途 |
|---|---|---|
| `lvgl__lvgl` | 9.5.0 | GUI 框架 |
| `espressif__esp_lvgl_adapter` | 0.5.3（**已打补丁**，见第 7 节） | LVGL ↔ LCD/触摸适配、撕裂控制、PPA/DMA2D 加速 |
| `espressif__esp_lv_decoder` / `espressif__esp_lv_fs` | 0.4.0 / 1.0.1 | 图片解码（PNG/JPEG）与文件系统接入 |
| `espressif__esp_mmap_assets` | 2.0.1 | UI 资源打包与 mmap |
| `espressif__esp_video` + `espressif__esp_sccb_intf` | 2.4.1 | V4L2 摄像头框架 / SCCB |
| `components/esp_cam_sensor`（本地覆盖托管组件） | 2.4.0 | 摄像头传感器驱动（ov3660、sc101iot 等） |
| `espressif__esp_codec_dev` | 1.6.2 | 音频编解码驱动 |
| `espressif__esp_lcd_touch_gt1151` / `esp_lcd_touch` | 1.1.1 | 触摸驱动 |
| `espressif__button` / `espressif__knob` | 4.2.1 / 1.1.0 | 按键 / 旋钮事件 |
| `espressif__led_strip` | 3.0.3 | WS2812 |
| `espressif__usb_host_hid` + `espressif__usb` | 1.2.1 / 1.5.0 | USB HID Host 协议栈 |
| `espressif__esp_new_jpeg` / `libpng` / `zlib` / `freetype` | — | 图像编解码与字体支撑 |

### 2.5 与驱动相关的配置开关（来自 `sdkconfig.defaults`）

- 目标与存储：`IDF_TARGET=esp32s31`、QIO + 16MB flash、PSRAM 250MHz + `SPIRAM_XIP_FROM_PSRAM`、FreeRTOS 1000Hz
- 显示/UI：`ESP_LVGL_ADAPTER_ENABLE_FS/DECODER/FPS_STATS`、`LV_USE_CLIB_{MALLOC,STRING,SPRINTF}`、字体 18/24/32/46、`LV_CACHE_DEF_SIZE=4MB`
- 摄像头：`CAMERA_OV3660`（240×240 RGB565 DVP）、`CAMERA_SC101IOT`（640×480 YUV422）
- USB：`USB_HOST_HUBS_SUPPORTED`

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

`idf.py` **不在 Git Bash 的 PATH 里**；且 IDF v6.1 在 Git Bash 下会打印
`MSys/Mingw is no longer supported` 警告（`MSYSTEM` 被 MSYS 运行时注入子进程）。
构建/烧录请走 PowerShell，或按下面第 4 节从 Bash 调用 PowerShell。

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
- 先 `Remove-Item Env:MSYSTEM` 再加载 profile，可去掉那条 MSYS 警告（不是错误，但会污染日志）。
- 编译耗时较长（本机约 2200 个编译目标、全量冷启动约数分钟，有 ccache 后增量很快），
  后台跑并重定向日志：`> build/dsh_build.log 2>&1`。
- 若要限制构建产物写在工程内（受沙箱/权限约束时），可额外设置
  `$env:CCACHE_DIR='D:\project\ESP-IDFv6.1\ToDoHelper\build\.ccache'`。

### 4.3 只烧录 / 只擦除 / 清理

```powershell
idf.py -p COM34 flash            # 只烧录（会自动先构建）
idf.py -p COM34 erase-flash      # 全擦
idf.py fullclean                 # 删 build/（managed_components 不动）
idf.py reconfigure               # 重跑 CMake + 组件管理器
```

## 5. 工程结构与关键文件

```
ToDoHelper/
├─ CMakeLists.txt              # EXTRA_COMPONENT_DIRS 指向 ../common_components
├─ partitions.csv              # nvs 0x9000/0x6000, phy_init 0xf000/0x1000,
│                              # factory 0x10000/0x800000, coffee_pjpg 0x810000/0x600000
├─ sdkconfig.defaults          # 真正的默认配置来源（IDF_TARGET/PSRAM/LVGL/摄像头/分区表…）
├─ sdkconfig                   # 生成物，不要手改；要改默认值改 sdkconfig.defaults
├─ dependencies.lock           # 组件管理器锁文件（target: esp32s31）
├─ main/                       # main.c(UI 装配) + factory_demo_{audio,camera,usb_hid}.c
│                              # + coffee_assets.c；CMakeLists 用 spiffs_create_partition_assets
│                              # 把 ../assets/coffee 打包成 coffee_pjpg 分区
├─ ui/                         # SquareLine Studio 1.5.4 导出的咖啡机 UI（ui.c/screens/fonts/…）
├─ assets/coffee/              # 85 个 png/ttf，LVGL 资源（经 esp_mmap_assets/mm）
├─ components/esp_cam_sensor/  # 本地组件，覆盖托管组件 espressif/esp_cam_sensor 2.4.0
├─ managed_components/         # 组件管理器下载的依赖（≈22 个）；除第 7 节补丁外勿手改
├─ patches/                    # 本工程必需的本地补丁（见第 7 节）
└─ build/                      # 构建产物；本次排障日志：dsh_build2.log / dsh_flash.log
```

工程外但必需：`D:\project\ESP-IDFv6.1\common_components\esp32_s31_korvo`（Korvo BSP，由
`EXTRA_COMPONENT_DIRS` 和 `main/idf_component.yml` 的 `path:` 引用）。**不要修改它**。

## 6. 依赖与版本约束

> 各依赖的**用途与已确认版本**见第 2.4 节；本节只讲约束来源与解析结果。

- `main/idf_component.yml` → `idf: ">=6.1.0"`，并对 `usb_host_hid`、`esp_mmap_assets`、
  `espressif/esp_lv_decoder` 有直接约束。
- Korvo BSP（工程外）的约束：`espressif/esp_lvgl_adapter: ^0.5.0`、`esp_video: 2.4.1`、
  `esp_cam_sensor: 2.4.*`、`esp_codec_dev: ^1.5.9`、`led_strip: ^3.0.3`、`button: ^4`、
  `esp_lcd_touch_gt1151: ^1`。
- 因此 `esp_lvgl_adapter` 被解析到 **0.5.3**（0.5.x 的最新版），LVGL 为 **9.5.0**。
- 想换版本：改 `main/idf_component.yml` 增加显式约束，然后 `idf.py reconfigure`；
  注意受 BSP 的 `^0.5.0` 限制，不修改工程外的 BSP 就无法升到 0.6.x。

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

**回滚/重新生成**：删除 `managed_components/espressif__esp_lvgl_adapter` 后 `idf.py reconfigure`，
组件管理器会重新下载官方 0.5.3（再按需重新打补丁）。

**替代方案（不推荐）**：升级到 `esp_lvgl_adapter` 0.6.4+（官方已改为按头文件存在与否自动判别，
与本补丁思路一致），但 Korvo BSP 的 `^0.5.0` 约束会与之冲突，需同时改工程外的 BSP。

## 8. 验证基线（判定「编译成功」）

日志出现：

```
Project build complete.
```

本次成功构建的数值（可用于回归对比）：

| 项 | 值 |
|---|---|
| app bin | `build/esp32_s31_korvo_factory_demo.bin`，`0x193a60` 字节（约 1.58 MB） |
| app 分区余量 | 0x800000 分区，剩 80% |
| bootloader | `build/bootloader/bootloader.bin`，`0x5a80`，余 6% |
| 编译警告 | 0 |
| 资源分区 | `build/mmap_build/coffee/coffee_pjpg/coffee_pjpg.bin`（约 227 KB） |

烧录成功判据：每个镜像后出现 `Hash of data verified.`，结尾 `Hard resetting via RTS pin...` + `Done`。

烧录地址（`idf.py flash` 自动处理）：
`0x2000` bootloader、`0x8000` 分区表、`0x10000` app、`0x810000` coffee_pjpg。

## 9. 注意事项与坑

- **不要修改** `..\common_components\esp32_s31_korvo`：它在工程目录之外，且被其它示例共用。
- 改动 `managed_components/` 下任何文件，都要把 diff 追加进 `patches/` 并在本节登记，
  否则依赖被重新下载后问题会「神秘复现」。
- `sdkconfig` 是生成物；改默认配置请改 `sdkconfig.defaults`
  （当前含 `CONFIG_IDF_TARGET="esp32s31"`、`SPIRAM_SPEED_250M`、`SPIRAM_XIP_FROM_PSRAM`、
  QIO/16MB flash、`CONFIG_CAMERA_OV3660`/`SC101IOT`、LVGL 字体与缓存等）。
- 重新设目标若提示 preview，用 `idf.py --preview set-target esp32s31`；本工程 `sdkconfig`
  已就绪，正常情况下无需重设。
- Git Bash 下直接跑 `idf.py` 仅有无害警告，但路径/工具链语义以 PowerShell 为准；排障时优先 PowerShell。
- 串口：认 **COM34**（CP210x）。`COM26`(CH340) 不是本板，占用时会报
  `Could not open COM26 ... port is busy`。
- 构建产物与日志都在 `build/`，体积大；不要把 `build/` 内容当作源码提交。

## 10. 排错速查

| 症状 | 处理 |
|---|---|
| `fatal error: esp_async_fbcpy.h: No such file` | 第 7 节补丁被覆盖，重新 `patch -p1` 并 `idf.py build` |
| `idf.py: command not found` | 未加载 IDF 环境，见第 4 节 |
| `MSys/Mingw is no longer supported` | 无害警告；PowerShell 中先 `Remove-Item Env:MSYSTEM` |
| `Could not open COMxx ... busy` | 关掉占用串口的监视器/其它工具；确认用的是 COM34 |
| 组件管理器重新下载依赖 | 打补丁后 `idf.py reconfigure && idf.py build` |
| 需要从零复现 | `idf.py fullclean` → 应用补丁 → `idf.py build` |
