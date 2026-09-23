# ui_assets

**L2 资源组件** — 把 `assets/ui/` 的 PNG 打进 `assets` 分区，并给 LVGL 一个"按基名取图"的入口。

- 头文件：`include/ui_assets.h`
- 依赖：`sys_utils`、`lvgl`、`esp_lvgl_adapter`、`esp_mmap_assets`（托管组件：`esp_mmap_assets ^2.0.0`）
- 私有：`src/mmap_generate_assets.h`（由 `esp_mmap_assets` 的打包步骤生成，**不要手改**）
- 被谁依赖：`ui_pages`（`page_todo.c`、`page_weather.c`）、`main`

## 为什么打包写在本组件的 CMakeLists 里

`spiffs_create_partition_assets(assets ...)` 生成 `src/mmap_generate_assets.h`，
而 `ui_assets.c` 里 `#include "mmap_generate_assets.h"` 用到 `MMAP_ASSETS_FILES` / `MMAP_ASSETS_CHECKSUM`。
把打包调用放在**本组件自己的** `CMakeLists.txt`（而不是 `main/`）里，才能保证编译本组件之前头文件已经生成：

```cmake
spiffs_create_partition_assets(assets "${TODOHELPER_ASSETS_DIR}" FLASH_IN_PROJECT
    MMAP_FILE_SUPPORT_FORMAT ".png"
    IMPORT_INC_PATH "${CMAKE_CURRENT_LIST_DIR}/src")
add_dependencies(${COMPONENT_LIB} assets_assets_bin)
```

- `TODOHELPER_ASSETS_DIR` = `<项目根>/assets/ui`；目录不存在直接 `FATAL_ERROR`（宁可配置期失败也不要烧出空分区）。
- `add_dependencies(... assets_assets_bin)` 是必须的：否则并行构建时 `ui_assets.c` 可能先于镜像生成而编译失败。

## 关键约定

1. **`assets/ui/` 必须是扁平目录**。`esp_mmap_assets` 的打包脚本用 `os.listdir()` **只扫描一层**，
   不递归子目录，也**只把 `MMAP_FILE_SUPPORT_FORMAT` 列出的后缀**打进镜像
   （本组件只声明了 `.png`）。所以用**文件名前缀**分类，例如 `w_sunny.png` = 天气-晴。
   字体（`.ttf`）不能放这里，要走 `EMBED_FILES`（`ui_core` 的做法）或 SD 卡。
2. **增删素材后要 `idf.py reconfigure`**。该自定义命令的 `DEPENDS` 依赖 configure 期求值的
   `GLOB_RECURSE`，只加文件而不重新 configure，镜像不会更新。
3. **按基名查询**。`ui_assets_init()` 遍历分区里的文件，把扩展名去掉存成表项，
   `src` 则是 LVGL 路径 `A:<文件名>`：

   | 分区里的文件 | `base` | `ui_assets_src()` 返回 |
   |---|---|---|
   | `w_sunny.png` | `w_sunny` | `A:w_sunny.png` |

   映射表是定长 `ASSET_MAX_ENTRIES`（64）项，超出部分忽略。基名区分大小写、不带路径。
4. **缺图必须优雅降级，不能黑屏**（需求 §12）。`ui_assets_init()` 失败（分区没烧、挂载失败、
   LVGL fs 挂载失败）不致命：只 `SYS_LOGW`，`ui_assets_ready()` 变 false，之后所有 getter 返回 `NULL`。
   `ui_assets_image_create()` 在找不到图标时**仍然创建 image 控件，但加上 `LV_OBJ_FLAG_HIDDEN`**——
   这样周围布局不跳，调用方可以在同一格放文字兜底。
5. **只做资源，不做主题**。PNG 本身是"白色 + 透明底"，不需要重新着色，所以这个组件**不依赖 `ui_theme`**；
   颜色语义留在 `ui_theme.h`。
6. **天气图标有一层回落**。`ui_assets_weather_src(icon_base)` 先查传入的基名，再依次尝试
   `w_overcast` → `w_cloudy` → `w_sunny`，全都没有才返回 `NULL`。

## API 摘要

| 函数 | 说明 |
|---|---|
| `ui_assets_init()` | 挂载 `assets` 分区 + 注册 LVGL fs（`esp_lv_adapter_fs_mount`，盘符 `A`），建基名索引 |
| `ui_assets_ready()` | 分区是否可用 |
| `ui_assets_count()` | 打进镜像的文件数（未初始化时为 0） |
| `ui_assets_src(base_name)` | 基名 → LVGL 源字符串（如 `A:w_sunny.png`），未打包则 `NULL` |
| `ui_assets_image_create(parent, base_name, size)` | 建 `size × size` 的 image；找不到就建好后隐藏 |
| `ui_assets_weather_src(icon_base)` | 带回落链的天气图标查询 |

当前打包内容（`assets/ui/` 的 8 个文件，与生成的 `MMAP_ASSETS_FILES == 8` 一致）：
`w_sunny.png`、`w_cloudy.png`、`w_overcast.png`、`w_rain.png`、`w_snow.png`、`w_fog.png`、
`w_thunder.png`、`w_night.png`。

## 测试方法

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build
idf.py -p COM34 flash monitor
```

期望串口输出：

```
I (xxx) TH:ui_assets: 8 asset(s) available
```

（把日志级别调到 DEBUG 会逐条打印 `asset 'w_sunny' -> 'A:w_sunny.png'`。）

故障验证：把 `assets/ui/` 里某个 PNG 临时改名为 `.png.bak`（或换一份没烧 assets 的固件），
重新 `idf.py reconfigure` + build 后：

- 串口不再列出该图，`ui_assets_count()` 减 1；
- 天气页/待办页对应位置**不显示图标但不塌陷**，文字仍在（走 `LV_OBJ_FLAG_HIDDEN` 分支）；
- 分区完全没烧时应看到 `assets partition not mounted: ...`，页面仍可正常使用。
