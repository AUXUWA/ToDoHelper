# storage_manager

**L1 存储抽象** — 对上层提供"一个逻辑根目录 + 两种物理后端"。

- 头文件：`include/storage_manager.h`
- 依赖：`sys_utils`、`bsp`（SD 挂载）、`esp_partition`、`fatfs`；私有依赖 `littlefs`
- 后端选择（`storage_manager_init()` 内一次性完成）：

| 顺序 | 后端 | 根路径 | 条件 |
|---|---|---|---|
| 1 | SD 卡（SDMMC 4-bit） | `/sdcard` | 插卡且挂载成功 |
| 2 | 片内 LittleFS（分区 `storage`） | `/littlefs` | SD 不可用（首次自动格式化） |
| 3 | 无 | — | 两者都失败：文件操作返回 `ESP_ERR_INVALID_STATE`，**不崩溃** |

## API 摘要

| 分组 | 函数 |
|---|---|
| 生命周期 | `storage_manager_init()`、`storage_manager_deinit()` |
| 查询 | `storage_root()`、`storage_app_dir()`、`storage_available()`、`storage_sd_available()`、`storage_free_bytes()`、`storage_total_bytes()` |
| 路径 | `storage_path()`、`storage_abspath()`、`storage_mkdir_p()` |
| 文件 | `storage_read_file()`、`storage_write_file()`、`storage_append_file()`、`storage_read_text()`、`storage_delete_file()`、`storage_exists()` |
| 缓存 | `storage_read_cache()`、`storage_write_cache()` |

## 关键约定

1. **禁止硬编码 `/sdcard`**。所有路径用 `storage_path(buf, len, "cache/todos.json")` 拼；
   该函数会加上 `<root>/todohelper/` 前缀。需求 §9.2/§12。
2. **原子写**：`storage_write_file()` 先写 `<path>.tmp` 再 `rename()`，
   保证掉电时不会把上一个有效文件写坏（`cache/todos.json`、`words/progress.json` 依赖此性质）。
3. **缓存写失败不算错**：`storage_write_cache()` 只 `SYS_LOGW`，不影响主流程。
4. **目录树**按需求 §9.2 自动创建：`fonts/ images/weather/ anim/idle_face/ words/ ai/ tts/ cache/ backup/ logs/`。
5. **硬件约束**：GPIO20~25 与 SPI NAND flash 复用，二者互斥；
   所以"有 SD 卡"与"用片内分区"必须各自独立可用，不能互相假设。

## 事件

`STORAGE_EVENT`：`MOUNTED`（payload 为根路径字符串）、`UNMOUNTED`、`LOW_SPACE`（free KB）。
同时发 `SYS_EVENT_STORAGE_READY`。

## 测试方法

```powershell
idf.py build && idf.py -p COM34 flash monitor
```

- 插卡：`TH:storage: SD card mounted at /sdcard` + `ready: root=/sdcard ...`
- 不插卡：`TH:storage: SD card not available (...)` + `internal LittleFS mounted at /littlefs`
- 两者都失败：`no usable storage backend; running without persistence`，UI 仍应正常启动，
  状态栏显示"无存储"。
