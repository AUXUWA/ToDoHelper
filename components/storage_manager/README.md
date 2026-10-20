# storage_manager

**L1 存储抽象** — 对上层提供"一个逻辑根目录 + 两种物理后端"。

- 头文件：`include/storage_manager.h`
- 依赖：`sys_utils`、`bsp`（SD 挂载）、`esp_partition`、`fatfs`；私有依赖 `littlefs`
- 后端选择（`storage_manager_init()` 内一次性完成）：

| 顺序 | 角色 | 挂载点 | 条件 |
|---|---|---|---|
| 1 | 存储根（SD 优先） | `/sdcard` | 插卡且挂载成功 |
| 1' | **pinned 后端**（片内，始终挂载） | `/littlefs` | 无条件（首次自动格式化） |
| 2 | 存储根（无卡时） | `/littlefs` | SD 不可用 |
| 3 | 无 | — | 两者都失败：文件操作返回 `ESP_ERR_INVALID_STATE`，**不崩溃** |

> 片内 LittleFS 现在**总是**挂载：它要么是存储根（没插卡），要么是待办清单的
> 固定落点（pinned）。见下面「B13 待办固定存片内」。

## API 摘要

| 分组 | 函数 |
|---|---|
| 生命周期 | `storage_manager_init()`、`storage_manager_deinit()` |
| 查询 | `storage_root()`、`storage_app_dir()`、`storage_pinned_app_dir()`、`storage_available()`、`storage_sd_available()`、`storage_pinned_available()`、`storage_free_bytes()`、`storage_total_bytes()` |
| 路径 | `storage_path()`、`storage_abspath()`、`storage_mkdir_p()` |
| 文件 | `storage_read_file()`、`storage_write_file()`、`storage_append_file()`、`storage_read_text()`、`storage_delete_file()`、`storage_exists()`、`storage_backup_exists()`、`storage_promote_backup()` |
| 缓存 | `storage_read_cache()`、`storage_write_cache()` |
| 缓存（固定片内） | `storage_read_cache_pinned()`、`storage_write_cache_pinned()` |

## 关键约定

1. **禁止硬编码 `/sdcard`**。所有路径用 `storage_path(buf, len, "cache/todos.json")` 拼；
   该函数会加上 `<root>/todohelper/` 前缀。需求 §9.2/§12。
2. **缓存写失败不算错**：`storage_write_cache()` 只 `SYS_LOGW`，不影响主流程。
3. **目录树**按需求 §9.2 自动创建：`fonts/ images/weather/ anim/idle_face/ words/ ai/ tts/ cache/ backup/ logs/`。
4. **硬件约束**：GPIO20~25 与 SPI NAND flash 复用，二者互斥；
   所以"有 SD 卡"与"用片内分区"必须各自独立可用，不能互相假设。
5. **文件操作整体串行化**：内部有一把递归互斥锁，双槽协议（见下）必须整套执行，
   两个任务同时写同一个文件会把中间状态搅乱。锁只覆盖文件 I/O，不含网络/睡眠。

---

## B2 落盘原子性：双槽 + 启动恢复

### 为什么不是"写 .tmp 再 rename"

老实现是 `fclose(tmp)` → `remove(path)` → `rename(tmp, path)`。
掉电落在 `remove` 与 `rename` 之间，**上一份有效清单和新的临时文件一起没了**。

也不能简单改成"删掉 remove、改成覆盖式 rename"：
**FatFs 的 `f_rename()` 在目标已存在时返回 `FR_EXIST` 而不是覆盖**
（`esp-idf/components/fatfs/src/ff.c`，FAT 分支 `res = FR_EXIST`；
ESP-IDF 的 `vfs_fat.c` 把它映射成 `EEXIST`）——在 SD 卡上会直接失败。
LittleFS 的 `lfs_rename_()` 倒是同一次 commit 里带 `LFS_TYPE_DELETE` + `LFS_TYPE_CREATE`，
本身就是原子替换。两个后端行为不一致，所以不用"覆盖式 rename"这条路。

### 现在的协议（对所有走 `storage_write_file()` 的文件生效）

写 `X`：

1. 新内容写进 `X.new`（此刻 `X` 还是完整的旧内容）；
2. `X` 改名成 `X.bak`（**失败就到此为止**：`X` 还在，数据没丢，只丢一次更新）；
3. `X.new` 改名成 `X`（此刻 `X` 已不存在，FatFs 才肯改名）。

成功后目录里长期只有三个名字：`X`（当前）、`X.bak`（上一代）、以及写过程中的 `X.new`，
**每个名字只留一个，不会无限增长**。

### 启动恢复（`storage_manager_init()` 里自动做）

扫描存储根与 pinned 目录树（深度 2 级），按下面的规则收敛：

| 状态 | 处理 |
|---|---|
| `X` 在 + `X.new` 在 | `X.new` 是半截货或已被取代 → **删掉** |
| `X` 不在 + `X.new` 在（且非空） | `X.new` 是完整的新内容 → **提上来** |
| `X` 不在 + `X.new` 不在 + `X.bak` 在 | 只有上一代 → **用备份顶上** |
| `X` 在 + `X.bak` 在 | 正常状态，**保留 `.bak`** |

先处理 `.new` 再处理 `.bak`：新内容优先，只有没有新内容时才用备份顶。

### 主文件坏了怎么办

storage 层只保证 `X.bak` **一直在**，不替调用方判断"坏没坏"（它不懂 JSON）。
调用方解析失败时自己决定要不要回退：

```c
if (parse_failed) {
    if (storage_backup_exists("cache/todos.json")) {
        storage_promote_backup("cache/todos.json");  /* 坏文件留成 *.corrupt */
    }
}
```

`storage_promote_backup()` 会把坏掉的正式文件改名成 `X.corrupt`（留作事后查看），
再把 `X.bak` 提上来。

### 删除语义变了

`storage_delete_file()` 现在**三个槽位一起删**。只删正式文件的话，
启动恢复会拿 `.bak` 把它"复活"。

### 影响面

词库、进度、AI 历史、待办——凡是走 `storage_write_file()` 的文件都吃这套协议。
代价：每个文件多留一份上一代内容（片内分区 2 MB，只放 pinned 的待办清单，够用）。

---

## B13 待办固定存片内（pinned）

### 问题

存储根二选一（SD 优先）。插卡 → 待办写到卡上；拔卡 → 设备去读片内同名文件，
**待办全没了**（数据在卡里没丢，但用户以为丢了）。卡坏/被格式化则真的全丢。

### 选定的方案：**两个文件系统同时挂载**（不是"根切换时搬运/合并"）

| | 存储根（跟着插拔卡变） | pinned（固定在片内） |
|---|---|---|
| 挂载点 | `/sdcard` 或 `/littlefs` | 恒为 `/littlefs` |
| 谁用 | 词库、字体、AI 历史、词书… | **待办清单** |
| API | `storage_path()` / `storage_read_cache()` / `storage_write_cache()` | `storage_read_cache_pinned()` / `storage_write_cache_pinned()` |

**为什么选同时挂载，而不是"根切换时搬运/合并"：**

- 内部分区 `storage`（2 MB littlefs）本来就在 `partitions.csv` 里，挂载点
  `/littlefs` 与 SD 的 `/sdcard` **不冲突**，两个后端各管各的分区，没有共享状态；
- 片内 LittleFS 挂载只多花约 512 B 缓存（`CONFIG_LITTLEFS_CACHE_SIZE=512`），
  分配走 `heap_caps_calloc_prefer(..., SPIRAM, DEFAULT)`，不占内部 RAM；
- "搬运/合并"方案要在每次根切换时做数据搬运，等于把 B13 的复杂度搬到
  运行时路径上，还要处理"两个介质各有一份、谁新谁旧"的合并语义；
- 同时挂载后，**pinned 路径在插卡/拔卡前后完全不变**，待办清单天然不会消失。

挂载顺序：先试 SD（决定存储根），**再无条件挂片内**。
片内挂不上时（分区坏了）会退化：pinned 落回存储根，
日志 `internal partition unavailable; pinned files fall back to ...`，
功能不坏，但"固定"这个保证失效——`storage_pinned_available()` 会返回 false。

### pinned 的文件位置

`<internal>/todohelper/cache/<name>`，与存储根的布局一致（`cache/` 子目录）。
片内只需要 `cache/` 一个目录（`storage_prepare_pinned_tree()` 建）。

### 升级兼容：自动迁移

老版本（v1.4.x）把待办写在 SD 的 `todohelper/cache/todos.json`。
升级后第一次启动时，`storage_migrate_cache_to_pinned()` 会把 **SD 上 `cache/`
目录里片内还没有的文件** 搬一份到片内：

- 单文件上限 256 KB、总量上限 1 MB（片内分区只有 2 MB）；
- SD 上的**原件保留**，万一片内那份出问题还有得退；
- 片内已经有同名文件时，比对修改时间：SD 那份**明显更新**才覆盖，
  时间戳读不出来（时钟没校准）就以片内为准；
- 迁移日志：`migrated 'todos.json' (12345 bytes) from the SD card to the internal partition`。

另外，`storage_read_cache_pinned()` 在片内**完全没有**这份文件时，会退回读
存储根上的同名文件（`pinned cache '...' missing; served the storage-root copy`），
免得迁移没跑成时清单变空；**写永远写片内**。

### 谁该用 pinned

- **待办清单**：`storage_write_cache_pinned(CONFIG_TODOHELPER_TODO_CACHE_FILE, ...)`
  / `storage_read_cache_pinned(...)`（`todo_manager` 负责切换，本组件只提供接口）。
- **其它一切**（词库、字体、词书、AI 历史、天气缓存）：继续用
  `storage_write_cache()` / `storage_write_file()`，仍在 SD 上，语义没变。

---

## 事件

`STORAGE_EVENT`：`MOUNTED`（payload 为根路径字符串）、`UNMOUNTED`、`LOW_SPACE`（free KB）。
同时发 `SYS_EVENT_STORAGE_READY`。

## 测试方法

```powershell
idf.py build && idf.py -p COM34 flash monitor
```

- 插卡：`TH:storage: SD card mounted at /sdcard` + `internal LittleFS mounted at /littlefs`
  + `ready: root=/sdcard app=/sdcard/todohelper pinned=/littlefs/todohelper ...`
- 不插卡：`TH:storage: SD card not available (...)` + `internal LittleFS mounted at /littlefs`
  + `ready: root=/littlefs ... pinned=/littlefs/todohelper ...`
- 两者都失败：`no usable storage backend; running without persistence`，UI 仍应正常启动，
  状态栏显示"无存储"。
- 升级迁移：`TH:storage: migrated 'todos.json' (xxxxx bytes) from the SD card to the internal partition`
- 掉电恢复：手工把 `todos.json.new` 留在卡上再开机，应看到
  `recovered '...' from interrupted write` 或 `dropped stale '...' (formal file is present)`。
