# ToDoHelper API.md

## 说明

本文件是 ToDoHelper 的**接口与测试基准**：每个组件交付后在这里登记**头文件路径、公开函数签名、事件、最小测试步骤**；
HTTP 接口、NVS 键、运行时目录树也集中登记在这里。对应需求 `docs/TODOHELPER_REQUIREMENTS.md` §16 第 10 条。

- **优先级**（与框架文档一致）：代码实际行为 > `AGENT.md` > `docs/TODOHELPER_FRAMEWORK.md` > `docs/TODOHELPER_REQUIREMENTS.md`。
  本文件描述的是**代码里实际存在的签名与路由**，不是设计提案。
- **怎么读**：
  1. 先看 [§1 分层与依赖](#1-分层与依赖) 确认组件边界（依赖只能向下）；
  2. 只看你要用的那个组件小节（每个组件一节，按依赖顺序排列），函数签名可直接复制；
  3. 调 HTTP 看 [§2 HTTP API](#2-http-api)，加 NVS 键看 [§3 NVS 键登记表](#3-nvs-键登记表)，
     落文件看 [§4 文件与目录布局](#4-文件与目录布局)；
  4. 交付/验收前先扫一眼 [§5 已知限制与待办](#5-已知限制与待办)，避免把"没做"当成"坏了"。
- **怎么追加**：新增组件 → 在 `## 组件接口（按依赖顺序）` 下按依赖位置插一节；
  新增路由 → 更新 §2 表格并标注状态（尤其 **501 未实现**）；新增 NVS 键 → 更新 §3 表格，
  同时在 `sys_utils/include/sys_keys.h` 加宏、在需求 §9.1 加行。
- 本文件中的命令一律为 **Windows PowerShell**，`curl` 一律写成 **`curl.exe`**（PowerShell 里 `curl` 是
  `Invoke-WebRequest` 的别名，参数语义不同）。

---

## 0. 构建、烧录与运行

环境事实（详见 `AGENT.md` §3）：IDF `D:\ESP-IDF\v6.1\esp-idf`（**v6.1.0**），目标 **`esp32s31`**（RISC-V），
串口 **COM34**（Silicon Labs CP210x，板子 MAC `1c:29:04:d0:71:b6`）；`COM26`(CH340) 不是本板。

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build
idf.py -p COM34 flash monitor      # 退出监视器：Ctrl+]
```

- 构建成功判据：日志出现 `Project build complete.`（`AGENT.md` §8）。
- 烧录成功判据：每个镜像后 `Hash of data verified.`，结尾 `Hard resetting via RTS pin...` + `Done`。
- 只烧录 / 擦除 / 清理 / 重配：

```powershell
idf.py -p COM34 flash            # 只烧录（会自动先构建）
idf.py -p COM34 erase-flash      # 全擦（NVS 与文件系统一起清掉）
idf.py fullclean                 # 删 build/（不动 managed_components）
idf.py reconfigure               # 重跑 CMake + 组件管理器
```

- **增删 `assets/ui/` 下的文件后必须 `idf.py reconfigure`。**
  原因：`assets` 分区由 `components/ui_assets/CMakeLists.txt` 的 `spiffs_create_partition_assets()`
  生成，其 `DEPENDS` 依赖 **configure 期求值的 `GLOB_RECURSE`**；不重跑 configure 时镜像不会更新，
  表现为"换了 PNG 但屏幕上还是旧图"（`docs/TODOHELPER_FRAMEWORK.md` §8）。
- 未加载 profile 时报 `idf.py: command not found`；Git Bash 下直接跑 `idf.py` 会打印
  `MSys/Mingw is no longer supported`（无害警告，先 `Remove-Item Env:MSYSTEM` 可去掉）。

---

## 1. 分层与依赖

分层取自 `docs/TODOHELPER_FRAMEWORK.md` §2 / §2.1，并加入新增的 `http_router`（L1）与 `ui_assets`（L2）。
表中"依赖"列来自各组件 `CMakeLists.txt` 的 `REQUIRES`（**实际值**，与框架文档的设计意图一致处不再重复说明）。

| 层 | 组件 | 组件目录 | 依赖（`REQUIRES`） |
|---|---|---|---|
| L0 | `sys_utils` | `components/sys_utils/` | `esp_event` `nvs_flash` `esp_timer` `log` `esp_common` |
| L1 | `http_router` | `components/http_router/` | `sys_utils` `esp_http_server` |
| L1 | `storage_manager` | `components/storage_manager/` | `sys_utils` `bsp` `esp_partition` `fatfs`（私有 `littlefs`） |
| L1 | `bsp` | `components/bsp/` | `sys_utils` `esp32_s31_korvo`（工程外，只读）`esp_lvgl_adapter` `lvgl`（私有 `button` `esp_codec_dev`） |
| L2 | `ui_assets` | `components/ui_assets/` | `sys_utils` `lvgl` `esp_lvgl_adapter` `esp_mmap_assets` |
| L2 | `ui_core` | `components/ui_core/` | `sys_utils` `bsp` `storage_manager` `lvgl` `esp_lvgl_adapter` |
| L3 | `wifi_manager` | `components/wifi_manager/` | `sys_utils` `bsp` `esp_wifi` `esp_netif` `esp_event` `nvs_flash` |
| L3 | `todo_manager` | `components/todo_manager/` | `sys_utils` `storage_manager` `http_router` `cjson` |
| L3 | `calendar_service` | `components/calendar_service/` | `sys_utils` `esp_netif` `http_router` `lwip` `cjson` |
| L3 | `weather_service` | `components/weather_service/` | `sys_utils` `storage_manager` `http_router` `cjson` `esp_http_client` `esp-tls` `mbedtls` `esp_timer` |
| L3 | `word_service` | `components/word_service/` | `sys_utils` `storage_manager` `http_router` `cjson` |
| L4 | `sync_server` | `components/sync_server/` | `sys_utils` `http_router` `cjson` `wifi_manager` `storage_manager` `esp_app_format` `esp_timer` `esp_hw_support` `esp_system` `heap`（私有 `mdns`） |
| L4 | `idle_face` | `components/idle_face/` | `sys_utils` `bsp` `ui_core` `lvgl` |
| L4 | `ui_pages` | `components/ui_pages/` | `ui_core` `ui_assets` `sys_utils` `storage_manager` `wifi_manager` `todo_manager` `calendar_service` `weather_service` `word_service` `sync_server` `lvgl` |
| L5 | `ai_service` ⚠️ | `components/ai_service/` | `sys_utils` `storage_manager` `http_router` `idle_face` `cjson` `esp_http_client` `esp-tls` `mbedtls` `esp_timer` |
| — | `main`（编排，非组件） | `main/` | `REQUIRES` 上述除 `ai_service` 外的全部组件 |

**规则（框架文档 §2.1）**

- 依赖只能向下，**绝不向上或横向**；L3 组件之间不得互相 `REQUIRES`，需要对方数据时订阅事件或由 `main` 注入回调。
- **HTTP 路由反向注册**：业务组件向 L1 的 `http_router` 注册路由，`sync_server`（L4）只负责
  `httpd_start()` 后调用一次 `http_router_attach()`。因此 `todo_manager`(L3) 不必依赖 `sync_server`(L4)。
- **跨层通知**走 L0 `sys_notify()`，而不是调用 L4 的 `sync_server_broadcast_event()`。
- `ui_pages` / `idle_face` 不被任何组件依赖，只有 `main` 依赖它们。
- ⚠️ `ai_service`(L5) 的 `REQUIRES` 里含 `idle_face`(L4)，且它**未接入** `main/app_main.c` 的模块表
  （`main/CMakeLists.txt` 也没有 `REQUIRES ai_service`）——详见 §5。

---

## 组件接口（按依赖顺序）

---

### sys_utils（L0）

- **组件目录**：`components/sys_utils/`
- **头文件**：`components/sys_utils/include/sys_utils.h`（聚合头，含下列全部）；
  `sys_log.h`、`sys_err.h`、`sys_keys.h`、`sys_notify.h`、`sys_status.h`
- **依赖**：IDF `esp_event` / `nvs_flash` / `esp_timer` / `log` / `esp_common`；**无上游 ToDoHelper 依赖**

#### 公开函数

| 头文件 | 签名 | 说明 |
|---|---|---|
| `sys_utils.h` | `esp_err_t sys_events_init(void);` | 创建全项目唯一事件循环，幂等 |
| `sys_utils.h` | `esp_event_loop_handle_t sys_events_get_loop(void);` | 共享循环句柄，`sys_events_init()` 前为 `NULL` |
| `sys_utils.h` | `esp_err_t sys_event_post_to(esp_event_base_t base, int32_t event_id, const void *data, size_t data_size);` | 向任意 base 投递（非阻塞） |
| `sys_utils.h` | `esp_err_t sys_events_post(int32_t event_id, const void *data, size_t data_size);` | base 固定为 `SYS_EVENT` 的语法糖 |
| `sys_utils.h` | `esp_err_t sys_event_subscribe(esp_event_base_t base, int32_t event_id, esp_event_handler_t handler, void *handler_arg, esp_event_handler_instance_t *instance);` | 包装 `esp_event_handler_instance_register_with()` |
| `sys_utils.h` | `esp_err_t sys_event_unsubscribe(esp_event_base_t base, int32_t event_id, esp_event_handler_instance_t instance);` | 反注册 |
| `sys_utils.h` | `int64_t sys_time_ms(void);` | 单调毫秒（超时/动画） |
| `sys_utils.h` | `int64_t sys_time_ts(void);` | Unix 秒；**从未同步过时返回 0**，调用前必须查 `sys_time_synced()` |
| `sys_utils.h` | `bool sys_time_synced(void);` | 墙钟是否可用 |
| `sys_utils.h` | `void sys_time_set_ts(int64_t ts);` | 锚定墙钟（`calendar_service` SNTP 成功后调用） |
| `sys_utils.h` | `void sys_time_clear(void);` | 墙钟退回"未知" |
| `sys_utils.h` | `esp_err_t sys_config_init(const char *nvs_namespace);` | 打开 NVS 命名空间（`NULL` → `CONFIG_TODOHELPER_NVS_NAMESPACE`=`todohelper`），内部含 `nvs_flash_init()`，幂等 |
| `sys_utils.h` | `esp_err_t sys_config_get_str(const char *key, char *out, size_t len);` | 键缺失 → `ESP_ERR_NOT_FOUND`；`out` 太小 → `ESP_ERR_INVALID_SIZE` |
| `sys_utils.h` | `esp_err_t sys_config_get_str_alloc(const char *key, char **out);` | 读到新分配缓冲（调用方 `free`） |
| `sys_utils.h` | `esp_err_t sys_config_set_str(const char *key, const char *val);` | |
| `sys_utils.h` | `esp_err_t sys_config_get_i32(const char *key, int32_t *out);` | |
| `sys_utils.h` | `esp_err_t sys_config_set_i32(const char *key, int32_t val);` | |
| `sys_utils.h` | `esp_err_t sys_config_get_bool(const char *key, bool *out);` | |
| `sys_utils.h` | `esp_err_t sys_config_set_bool(const char *key, bool val);` | |
| `sys_utils.h` | `esp_err_t sys_config_erase(const char *key);` | |
| `sys_utils.h` | `int32_t sys_config_get_i32_or(const char *key, int32_t fallback);` | 带默认值，不返回错误 |
| `sys_utils.h` | `bool sys_config_get_bool_or(const char *key, bool fallback);` | 同上 |
| `sys_utils.h` | `esp_err_t sys_config_get_str_or(const char *key, char *out, size_t len, const char *fallback);` | 同上 |
| `sys_log.h` | `void sys_log_set_level(sys_log_level_t level);` | 运行时可调；初始值来自 `CONFIG_TODOHELPER_LOG_LEVEL`（默认 2=INFO） |
| `sys_log.h` | `sys_log_level_t sys_log_get_level(void);` | |
| `sys_log.h` | `void sys_log_write(int level, const char *tag, const char *fmt, ...);` | 一般用宏：`SYS_LOGE/W/I/D(tag, fmt, ...)`，自动加 `TH:` 前缀 |
| `sys_err.h` | `esp_err_t sys_err_to_esp(sys_err_t err);` | 内部细分原因 → 对外 `esp_err_t`（**禁止直接强转枚举**） |
| `sys_err.h` | `sys_err_t sys_esp_to_err(esp_err_t err);` | 反向尽力映射 |
| `sys_err.h` | `const char *sys_err_name(sys_err_t err);` | 供日志 / JSON `detail` |
| `sys_status.h` | `esp_err_t sys_status_get(sys_status_t *out);` | 拷贝当前快照 |
| `sys_status.h` | `esp_err_t sys_status_update(void (*mutator)(sys_status_t *status, void *arg), void *arg);` | 加锁改快照，然后发 `SYS_EVENT_STATUS_CHANGED` |
| `sys_status.h` | `esp_err_t sys_status_publish(void);` | 仅强制发 `SYS_EVENT_STATUS_CHANGED` |
| `sys_notify.h` | `esp_err_t sys_notify(const char *name, const char *payload);` | 发布 L0 变更通知；`payload` 可为 `NULL`（按 `{}`） |
| `sys_notify.h` | `esp_err_t sys_notify_subscribe(esp_event_handler_t handler, void *handler_arg, esp_event_handler_instance_t *instance);` | 订阅全部通知 |

#### 事件

| 基址 | 事件 id | payload |
|---|---|---|
| `SYS_EVENT` | `SYS_EVENT_READY`(0) | 无（`main` 在所有模块 init 完成后发） |
| `SYS_EVENT` | `SYS_EVENT_BUTTON` | `sys_button_event_t`（`{sys_button_id_t id; sys_button_action_t action;}`） |
| `SYS_EVENT` | `SYS_EVENT_BRIGHTNESS` | `uint8_t` 百分比 |
| `SYS_EVENT` | `SYS_EVENT_IDLE_TIMEOUT` | 无 |
| `SYS_EVENT` | `SYS_EVENT_WAKEUP` | 无 |
| `SYS_EVENT` | `SYS_EVENT_STORAGE_READY` | 无（`storage_manager` 发） |
| `SYS_EVENT` | `SYS_EVENT_CONNECTIVITY` | `bool online`（`wifi_manager` 发） |
| `SYS_EVENT` | `SYS_EVENT_STATUS_CHANGED` | 无（读 `sys_status_get()`） |
| `SYS_NOTIFY_EVENT` | 恒为 `0` | `sys_notify_t`（**按值拷贝**：`name[25]` + `payload[193]`） |

> ⚠️ 与 `docs/TODOHELPER_FRAMEWORK.md` §6 的差异：框架文档写的是 `BRIGHTNESS_CHANGED`，
> 实际枚举名是 **`SYS_EVENT_BRIGHTNESS`**；实际枚举还多了 `STORAGE_READY` / `CONNECTIVITY` / `STATUS_CHANGED`。
> 以 `sys_utils.h` 为准。
> `sys_notify_t` 上限 216 B，超出请让接收方回查 API（`SYS_NOTIFY_NAME_MAX`=24、`SYS_NOTIFY_PAYLOAD_MAX`=192）。

#### 最小测试步骤

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build
idf.py -p COM34 flash monitor
```

- 串口应出现（上电即打）：
  `I (xxx) TH:sys_config: config ready (namespace 'todohelper')` 与
  `I (xxx) TH:sys_events: shared event loop ready`。
- 未联网时 `sys_time_synced()==false` 且 `sys_time_ts()==0`（由 `GET /api/v1/device/status` 的
  `time_synced` / `server_ts` 侧证）。
- 键读写：`sys_config_set_i32(SYS_KEY_FIRST_BOOT, 1)` 后重启，`sys_config_get_i32` 应返回 1
  （当前无组件使用 `first_boot`，属于手工验证项）。

---

### http_router（L1）

- **组件目录**：`components/http_router/`
- **头文件**：`components/http_router/include/http_router.h`
- **依赖**：`sys_utils`、IDF `esp_http_server`（**只做 URI 注册，不建 server**）
- **被谁依赖**：`todo_manager`、`calendar_service`、`weather_service`、`word_service`、`sync_server`

#### 公开函数

| 分组 | 签名 | 说明 |
|---|---|---|
| 注册 | `esp_err_t http_router_register(const char *path, httpd_method_t method, http_route_handler_t handler);` | `path` 允许**一个** `*`；同 `path+method` 重复注册 = 替换 handler |
| 注册 | `esp_err_t http_router_register_not_implemented(const char *path, httpd_method_t method, const char *hint);` | 注册"已知但未实现"，命中回 **501**，`hint` 进 `data.hint` |
| 注册 | `esp_err_t http_router_require_auth(const char *path, httpd_method_t method, bool required);` | 让**读方法**也必须过鉴权（未注册的 path 返回 `ESP_ERR_NOT_FOUND`） |
| 挂载 | `esp_err_t http_router_attach(httpd_handle_t server);` | 注册 `/*` + `HTTP_ANY` catch-all；已挂过返回 `ESP_OK` |
| 查询 | `size_t http_router_count(void);` | 已注册路由数（`/api/v1/device/status` 的 `routes`） |
| 鉴权 | `void http_router_set_auth_check(http_auth_check_t check);` | 由 `sync_server` 安装 |
| 鉴权 | `bool http_router_check_auth(httpd_req_t *req);` | 未装 hook 时 **fail-open** 返回 `true` |
| 响应 | `esp_err_t http_reply_json(httpd_req_t *req, int status, const char *msg, const char *data_json);` | 统一信封；`200/201` → `code:0`，其余 `code=status`；`data_json==NULL` → `null` |
| 响应 | `esp_err_t http_reply_ok(httpd_req_t *req, const char *data_json);` | 200 + `code:0` + `msg:"ok"` |
| 响应 | `esp_err_t http_reply_err(httpd_req_t *req, int status, const char *msg);` | `data:null` |
| 响应 | `esp_err_t http_reply_bad_request(httpd_req_t *req, const char *msg);` | 400 |
| 响应 | `esp_err_t http_reply_not_found(httpd_req_t *req);` | 404 |
| 响应 | `esp_err_t http_reply_method_not_allowed(httpd_req_t *req);` | 405 |
| 响应 | `esp_err_t http_reply_unauthorized(httpd_req_t *req);` | 401 |
| 响应 | `esp_err_t http_reply_unavailable(httpd_req_t *req, const char *msg);` | 503 |
| 响应 | `esp_err_t http_reply_not_implemented(httpd_req_t *req, const char *hint);` | 501，`data` = `{"detail":"not implemented","hint":"..."}` |
| 响应 | `esp_err_t http_reply_esp_error(httpd_req_t *req, esp_err_t err, const char *context);` | 500，`data` = `{"detail":...,"esp_err":esp_err_to_name(err)}` |
| 请求 | `esp_err_t http_read_body(httpd_req_t *req, char **out, size_t *len);` | 读整个 body 到 `malloc` 的 NUL 结尾缓冲（调用方 `free`）；`content_len==0` → `ESP_ERR_NOT_FOUND`，`>16 KB` → `ESP_ERR_INVALID_SIZE` |
| 请求 | `esp_err_t http_get_path(httpd_req_t *req, char *out, size_t len);` | URI 去掉 `?query` |
| 请求 | `bool http_query_get(httpd_req_t *req, const char *key, char *out, size_t len);` | `?key=` 取值 |
| 请求 | `bool http_query_get_i64(httpd_req_t *req, const char *key, int64_t *out);` | 十进制解析 |
| 请求 | `bool http_path_segment(httpd_req_t *req, int index, char *out, size_t len);` | 段号**从 0 开始**：`/api/v1/todos/12/done` → 0=`api` 2=`todos` 3=`12` 4=`done` |
| 请求 | `bool http_path_segment_u32(httpd_req_t *req, int index, uint32_t *out);` | 段解析为无符号整数 |

类型与常量：`typedef esp_err_t (*http_route_handler_t)(httpd_req_t *req);`、
`typedef bool (*http_auth_check_t)(httpd_req_t *req);`、`HTTP_ROUTER_MAX_ROUTES` = **48**（内部路径缓冲 64 B）。

#### 事件

无（本组件不发事件）。

#### 最小测试步骤

按 §0 构建烧录后，PC 与设备同一局域网：

```powershell
curl.exe -i http://todohelper.local/api/v1/todos              # 200，code=0
curl.exe -i -X DELETE http://todohelper.local/api/v1/todos    # 405 method not allowed
curl.exe -i http://todohelper.local/api/v1/nope               # 404 not found
curl.exe -i -X OPTIONS http://todohelper.local/api/v1/todos   # 204 + CORS 头
curl.exe -i http://todohelper.local/ws/events                 # 501 + data.hint
```

串口期望：`I (xxx) TH:todo_http: 7 todo routes registered`，随后
`I (xxx) TH:sync: listening on port 80, N route(s), auth=off, url=http://todohelper.local`；
`N` 应等于 `GET /api/v1/device/status` 的 `routes` 字段（当前为 **29**）。

---

### storage_manager（L1）

- **组件目录**：`components/storage_manager/`
- **头文件**：`components/storage_manager/include/storage_manager.h`
- **依赖**：`sys_utils`、`bsp`（SD 挂载）、`esp_partition`、`fatfs`；私有 `littlefs`

后端选择（`storage_manager_init()` 内一次性完成）：

| 顺序 | 后端 | 根路径 | 条件 |
|---|---|---|---|
| 1 | SD 卡（SDMMC 4-bit） | `/sdcard` | 插卡且挂载成功 |
| 2 | 片内 LittleFS（分区 `storage`，`0x810000`/`0x200000`） | `/littlefs` | SD 不可用（首次自动格式化） |
| 3 | 无 | — | 两者都失败：文件操作返回 `ESP_ERR_INVALID_STATE`，**不崩溃** |

#### 公开函数

| 分组 | 签名 | 说明 |
|---|---|---|
| 生命周期 | `esp_err_t storage_manager_init(void);` | 探测并挂载后端、创建应用目录树；幂等；SD 缺失**不算硬错误** |
| 生命周期 | `esp_err_t storage_manager_deinit(void);` | 释放后端（测试用） |
| 查询 | `const char *storage_root(void);` | `"/sdcard"` / `"/littlefs"`，都不可用时 `NULL` |
| 查询 | `bool storage_sd_available(void);` | 当前后端是否是 SD |
| 查询 | `bool storage_available(void);` | 是否有可用后端 |
| 查询 | `const char *storage_app_dir(void);` | 形如 `/sdcard/todohelper` |
| 路径 | `esp_err_t storage_path(char *buf, size_t len, const char *rel);` | 拼 `<app_dir>/<rel>`；`rel` 为 `NULL`/`""` 时得到应用目录本身 |
| 路径 | `esp_err_t storage_abspath(const char *rel, char *buf, size_t len);` | 同上（附赠形式） |
| 路径 | `esp_err_t storage_mkdir_p(const char *rel);` | 类似 `mkdir -p` |
| 文件 | `esp_err_t storage_read_file(const char *rel, void *buf, size_t *len);` | 进入时 `*len` 是容量，成功时是读到的字节数 |
| 文件 | `esp_err_t storage_write_file(const char *rel, const void *buf, size_t len);` | **原子写**：先写 `.tmp` 再 `rename()` |
| 文件 | `esp_err_t storage_append_file(const char *rel, const void *buf, size_t len);` | 追加，文件不存在则创建 |
| 文件 | `esp_err_t storage_read_text(const char *rel, char **out, size_t *len);` | 读整个文本到堆（NUL 结尾，调用方 `free`） |
| 文件 | `esp_err_t storage_delete_file(const char *rel);` | |
| 文件 | `esp_err_t storage_exists(const char *rel, size_t *size);` | 不存在 → `ESP_ERR_NOT_FOUND` |
| 容量 | `uint64_t storage_free_bytes(void);` | 未知时 0 |
| 容量 | `uint64_t storage_total_bytes(void);` | 未知时 0 |
| 缓存 | `esp_err_t storage_read_cache(const char *name, char **out, size_t *len);` | 读 `cache/<name>`；不存在 → `ESP_ERR_NOT_FOUND` |
| 缓存 | `esp_err_t storage_write_cache(const char *name, const void *buf, size_t len);` | 写 `cache/<name>`；失败只 `SYS_LOGW`，不影响主流程 |

常量：`STORAGE_APP_DIR`=`"todohelper"`、`STORAGE_SD_ROOT`=`"/sdcard"`、`STORAGE_INT_ROOT`=`"/littlefs"`、
`STORAGE_INT_PARTITION`=`"storage"`。

#### 事件

| 基址 `STORAGE_EVENT` | payload |
|---|---|
| `STORAGE_EVENT_MOUNTED`(0) | `const char *root`（**发的是栈上 `s_root` 的地址**，接收方需立即拷贝） |
| `STORAGE_EVENT_UNMOUNTED` | 无 |
| `STORAGE_EVENT_LOW_SPACE` | `uint32_t free_kb` |

挂载成功后额外发 `SYS_EVENT_STORAGE_READY`。

#### 最小测试步骤

按 §0 构建烧录后看串口（三选一）：

| 场景 | 期望串口 |
|---|---|
| 插卡 | `TH:storage: SD card mounted at /sdcard` + `TH:storage: ready: root=/sdcard app=/sdcard/todohelper free=... KB` |
| 不插卡 | `TH:storage: SD card not available (...)` + `internal LittleFS mounted at /littlefs` |
| 都失败 | `no usable storage backend; running without persistence`，UI 仍应正常启动，状态栏显示"无存储" |

HTTP 侧证：`curl.exe http://todohelper.local/api/v1/device/info` 看
`storage_root` / `sd_available` / `storage_available` / `storage_free_bytes`。

---

### bsp（L1）

- **组件目录**：`components/bsp/`
- **头文件**：`components/bsp/include/th_bsp.h`
- **依赖**：`sys_utils`、`esp32_s31_korvo`（工程外，**只读**）、`esp_lvgl_adapter`、`lvgl`；私有 `button` `esp_codec_dev`
- **符号前缀 `th_bsp_`**：工程外 Korvo BSP 已占用 `bsp_display_*` / `bsp_sdcard_*` / `bsp_led_*`，必须避让

#### 公开函数

| 分组 | 签名 | 说明 |
|---|---|---|
| 生命周期 | `esp_err_t th_bsp_init(void);` | 显示 + LVGL + 触摸，然后 LED 与按键；幂等。参数与产测基线一致（800×480、`TEAR_AVOID_MODE_TRIPLE_PARTIAL`、buffer 高 160、显示任务栈 16 KB、PPA 开启） |
| 生命周期 | `lv_display_t *th_bsp_display(void);` | `th_bsp_init()` 前为 `NULL` |
| 锁 | `bool th_bsp_lock(int32_t timeout_ms);` | 包装 `esp_lv_adapter_lock`，**可重入** |
| 锁 | `void th_bsp_unlock(void);` | |
| 亮度 | `esp_err_t th_bsp_set_brightness(uint8_t percent);` | 0..100。本板背光脚为 **NC**，用 `lv_layer_top()` 黑色遮罩做**软件调光**（遮罩去掉 `CLICKABLE`，变暗后触摸仍可用）；`0%` 是"最暗"非全灭 |
| 亮度 | `uint8_t th_bsp_get_brightness(void);` | 最近一次设置值 |
| 亮度 | `esp_err_t th_bsp_restore_brightness(void);` | 恢复 NVS `brightness` |
| 输入 | `esp_err_t th_bsp_buttons_start(void);` | 注册 4 个 ADC 按键，单击 → `SYS_EVENT_BUTTON`(PRESS)，长按 → `SYS_EVENT_BUTTON`(LONG_PRESS) |
| 输入 | `esp_err_t th_bsp_idle_init(void);` | 启动待机看门狗（`esp_timer` 500 ms 轮询），超时发 `SYS_EVENT_IDLE_TIMEOUT`；超时值取 NVS `idle_timeout`（默认 30 s） |
| 输入 | `esp_err_t th_bsp_idle_stop(void);` | |
| 输入 | `void th_bsp_report_activity(void);` | 上报用户交互 |
| 输入 | `uint32_t th_bsp_idle_ms(void);` | 距上次交互的毫秒数 |
| 输入 | `bool th_bsp_is_idle(void);` | 是否已超时 |
| 输入 | `void th_bsp_poll_idle(void);` | 重新求值；进入待机时发 `SYS_EVENT_IDLE_TIMEOUT` |
| LED | `esp_err_t th_bsp_led_set(th_bsp_led_state_t state);` | `TH_BSP_LED_OFF` / `_WIFI` / `_SYNC` / `_ERROR` |
| SD | `esp_err_t th_bsp_sdcard_mount(void);` | 已挂载时返回 `ESP_OK` |
| SD | `esp_err_t th_bsp_sdcard_unmount(void);` | |
| SD | `bool th_bsp_sdcard_mounted(void);` | |
| SD | `const char *th_bsp_sdcard_mount_point(void);` | |
| 音频 | `esp_err_t th_bsp_audio_init(void);` | 首次使用时惰性初始化 I2S + codec；失败不阻塞启动（FR7 降级） |
| 音频 | `void *th_bsp_audio_speaker(void);` | `esp_codec_dev_handle_t`，不可用为 `NULL` |
| 音频 | `void *th_bsp_audio_mic(void);` | 同上 |
| 音频 | `bool th_bsp_audio_available(void);` | 扬声器通路是否建成 |

#### 事件

本组件不声明自己的基址，只往 `SYS_EVENT` 发：
`SYS_EVENT_BUTTON`（`sys_button_event_t`）、`SYS_EVENT_BRIGHTNESS`（`uint8_t`）、
`SYS_EVENT_IDLE_TIMEOUT`（无）、`SYS_EVENT_WAKEUP`（无）。

#### 最小测试步骤

按 §0 构建烧录后，串口期望：

```
I (xxx) TH:bsp: ready: 800x480, tear-avoid=triple-partial, ppa=1, brightness=100%
I (xxx) TH:bsp_input: 4 ADC buttons registered
I (xxx) TH:bsp_input: idle timeout 30 s
```

- 屏幕出现深色背景 + 顶部状态栏（状态栏属 `ui_core`）。
- 按 MODE 键在首页/待机间切换；30 s 无操作屏幕变暗，触摸后恢复。
- 音频为 P2 依赖，验证 `TH:bsp_periph: audio ready (speaker=%d mic=%d)`。

---

### ui_assets（L2）

- **组件目录**：`components/ui_assets/`
- **头文件**：`components/ui_assets/include/ui_assets.h`
- **依赖**：`sys_utils`、`lvgl`、`esp_lvgl_adapter`、`esp_mmap_assets`
- 分区打包在本组件的 `CMakeLists.txt` 完成（`spiffs_create_partition_assets(assets ... MMAP_FILE_SUPPORT_FORMAT ".png")`），
  素材目录 `assets/ui/`，**必须扁平**；当前 8 个 PNG：`w_sunny / w_cloudy / w_overcast / w_rain / w_snow / w_fog / w_thunder / w_night`

#### 公开函数

| 签名 | 说明 |
|---|---|
| `esp_err_t ui_assets_init(void);` | 挂载 `assets` 分区并注册给 LVGL 适配层；失败不致命，之后所有 getter 返回 `NULL`，调用方回退文本 |
| `bool ui_assets_ready(void);` | 分区是否可用 |
| `int ui_assets_count(void);` | 打包文件数（未初始化时为 0） |
| `const char *ui_assets_src(const char *base_name);` | 按**基名**取 LVGL 图像源，如 `ui_assets_src("w_sunny")` → `"A:w_sunny.png"`；未打包返回 `NULL`。指针归本组件所有 |
| `lv_obj_t *ui_assets_image_create(lv_obj_t *parent, const char *base_name, int32_t size);` | 建 `size × size` 图像控件；图标缺失时控件**被隐藏**（保持布局稳定，调用方可放文本兜底） |
| `const char *ui_assets_weather_src(const char *icon_base);` | 天气图标名，回落 `"w_overcast"` |

#### 事件

无。

#### 最小测试步骤

按 §0 构建烧录后，串口期望 `I (xxx) TH:ui_assets: N asset(s) available`（当前 `N=8`）；
若打 `UI assets unavailable, pages will use text fallbacks`（由 `main` 打印），说明分区未挂上，天气页应退化为文本描述。

重新打包验证（**改素材后必须重跑 configure**）：

```powershell
idf.py reconfigure
idf.py build
idf.py -p COM34 flash monitor
```

---

### ui_core（L2）

- **组件目录**：`components/ui_core/`
- **头文件**：`components/ui_core/include/ui_core.h`（页面/锁/事件/控件）、`include/ui_theme.h`（配色/字号/尺寸）
- **依赖**：`sys_utils`、`bsp`、`storage_manager`、`lvgl`、`esp_lvgl_adapter`
- **被谁依赖**：`ui_pages`、`idle_face`

#### 公开函数

| 分组 | 签名 | 说明 |
|---|---|---|
| 生命周期 | `esp_err_t ui_core_init(void);` | 建 UI root：主题、字体、状态栏、LVGL 锁、tick 定时器。必须在 `th_bsp_init()` 与 `sys_events_init()` 之后 |
| 页面表 | `esp_err_t ui_page_register(const ui_page_t *pages, size_t count);` | 表驱动注册，可多次调用；后注册者不得复用已有 `ui_page_id_t` |
| 页面表 | `const ui_page_t *ui_page_get(ui_page_id_t id);` | 未注册返回 `NULL` |
| 导航 | `esp_err_t ui_page_switch(ui_page_id_t id);` | 首次切换时才 `create()`；可从任意任务调用 |
| 导航 | `ui_page_id_t ui_page_current(void);` | 无可见页时 `UI_PAGE_MAX` |
| 导航 | `lv_obj_t *ui_page_parent(ui_page_id_t id);` | 页面容器（`create()` 的父对象）；仅持锁时有效 |
| 导航 | `esp_err_t ui_nav_push(ui_page_id_t id);` | 压栈并切换 |
| 导航 | `esp_err_t ui_nav_back(void);` | 回退；栈空 no-op |
| 导航 | `bool ui_nav_can_back(void);` | |
| 线程 | `bool ui_lock(int32_t timeout_ms);` | 递归锁（包装 `esp_lv_adapter_lock`） |
| 线程 | `void ui_unlock(void);` | |
| 线程 | `esp_err_t ui_run_async(void (*fn)(void *arg), void *arg);` | 到 LVGL 任务执行，持锁运行，`fn` 不得阻塞 |
| 事件 | `esp_err_t ui_event_bind(esp_event_base_t base, int32_t event_id, ui_page_id_t page, size_t data_size);` | 把 `(base,event_id)` 送到某页的 `on_event`；`ui_core` 拷贝 payload 后经 `lv_async_call()` 转 UI 任务，忙时**丢事件** |
| 事件 | `esp_err_t ui_event_bind_all(esp_event_base_t base, ui_page_id_t page, size_t data_size);` | 该 base 的全部 event id |
| 事件 | `esp_err_t ui_event_subscribe(esp_event_base_t base, int32_t event_id, size_t data_size, ui_event_fn_t handler);` | 给非页面 UI 模块（如 `idle_face`）用 |
| 控件 | `lv_obj_t *ui_card_create(lv_obj_t *parent, const char *title, lv_obj_t **out_body);` | 圆角卡片，`out_body` 收回内容容器（可 `NULL`） |
| 控件 | `esp_err_t ui_toast(const char *fmt, ...);` | 底部提示，2 s 自动消失，任意任务可调 |
| 控件 | `lv_obj_t *ui_dialog(const char *title, const char *text, const char *ok_text, lv_event_cb_t on_close, void *user_data);` | 模态框；`on_close` 可 `NULL` |
| 尺寸 | `int32_t ui_screen_width(void);` | 页面**不得**硬编码 800×480 |
| 尺寸 | `int32_t ui_screen_height(void);` | |
| 尺寸 | `int32_t ui_statusbar_height(void);` | |
| 状态栏 | `esp_err_t ui_statusbar_refresh(void);` | 从 `sys_status_get()` 刷；`ui_core` 自动订阅 `SYS_EVENT_STATUS_CHANGED`，页面无需调用 |
| 主题 | `const lv_font_t *ui_font(ui_font_role_t role);` | 见 `ui_theme.h`（**永不返回 `NULL`**） |
| 主题 | `void ui_theme_apply(void);` | 对当前 screen 应用基础主题 |

类型与常量（`ui_core.h`）：

```c
typedef enum { UI_PAGE_SPLASH = 0, UI_PAGE_HOME, UI_PAGE_WIFI, UI_PAGE_TODO, UI_PAGE_TODO_DETAIL,
               UI_PAGE_WEATHER, UI_PAGE_CALENDAR, UI_PAGE_WORD, UI_PAGE_AI_CHAT,
               UI_PAGE_SETTINGS, UI_PAGE_IDLE_FACE, UI_PAGE_MAX } ui_page_id_t;

typedef struct {
    ui_page_id_t id;
    const char  *name;
    esp_err_t (*create)(lv_obj_t *parent);   /* 仅 id/create 必需 */
    esp_err_t (*destroy)(void);
    esp_err_t (*on_show)(void);
    esp_err_t (*on_hide)(void);
    esp_err_t (*on_event)(int32_t event_id, void *data);
    esp_err_t (*on_tick)(void);
    uint32_t   tick_period_ms;               /* 0 = 关闭 */
} ui_page_t;

typedef esp_err_t (*ui_event_fn_t)(int32_t event_id, void *data);
#define UI_STATUS_TIME_UNSYNCED "时间未同步"
```

`ui_theme.h`：`ui_font_role_t`（`UI_FONT_SMALL/BODY/TITLE/HUGE/ROLE_MAX`）、
`UI_COLOR_*` 调色板、`UI_STATUSBAR_HEIGHT=40`、`UI_PAGE_PAD=14`、`UI_PAGE_GAP=12`、
`UI_CARD_RADIUS=14`、`UI_CARD_PAD=14`、`UI_TOUCH_MIN=44`、`UI_BUTTON_HEIGHT=48`、`UI_LIST_ROW_HEIGHT=56`。

字体解析顺序（`src/ui_fonts.c`，保证任何情况下都有中文字形）：

1. FreeType TTF `<storage>/todohelper/fonts/ui.ttf`（NVS 无关，路径来自 `CONFIG_TODOHELPER_FONT_PATH`）；
2. FreeType TTF 内嵌固件的 CJK 子集 `components/ui_core/fonts/ui_cjk.ttf`（`EMBED_FILES`）；
3. LVGL 内置 `lv_font_source_han_sans_sc_{14,16}_cjk`；
4. LVGL 内置 Montserrat（仅拉丁，最后兜底）。

> 第 1/2 条整体受 `CONFIG_TODOHELPER_FONT_FREETYPE`（默认 **n**）门控，该选项又依赖
> `CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE`。默认构建走第 3/4 条，串口打
> `TH:ui_fonts: FreeType disabled, using built-in fonts`。

#### 事件

本组件不发事件；它**订阅** `SYS_EVENT` 的 `IDLE_TIMEOUT` / `WAKEUP`（待机策略）与
`STATUS_CHANGED`（状态栏）。待机策略归属 `ui_core`：超时 → 切 `UI_PAGE_IDLE_FACE` 并把亮度降到
NVS `idle_bright`（默认 20%）；唤醒 → 恢复 NVS `brightness` 并切回待机前的页面。

#### 最小测试步骤

按 §0 构建烧录后，串口期望：`I (xxx) TH:ui_core: ready (800x480)` 与
`I (xxx) TH:ui_core: registered N pages`。

- 上电顺序：启动页 → 首页（6 个入口卡片 + 顶部常驻状态栏）。
- 点卡片进对应页，`返回` 按钮回退；MODE 键在首页/待机间切换。
- 30 s 无操作自动进入待机页并变暗；触摸后恢复原页面。
- 未同步时状态栏时间位应显示 **"时间未同步"**（不是 1970）。

---

### wifi_manager（L3）

- **组件目录**：`components/wifi_manager/`
- **头文件**：`components/wifi_manager/include/wifi_manager.h`
- **依赖**：`sys_utils`、`bsp`（LED 状态灯）、`esp_wifi`、`esp_netif`、`esp_event`、`nvs_flash`
- 状态发布到 `sys_status_*`（`wifi_connected` / `wifi_connecting` / `wifi_ssid` / `wifi_ip` / `wifi_rssi` / `online`），
  **被依赖方向为零**：UI 只订阅事件或读 `sys_status_get()`

#### 公开函数

| 签名 | 说明 |
|---|---|
| `esp_err_t wifi_manager_init(void);` | 建 station netif、初始化 WiFi、注册事件处理器；幂等；**不主动连接**（末尾按 Kconfig 调 `auto_connect`） |
| `esp_err_t wifi_manager_start_scan(void);` | 异步扫描，立即返回；完成发 `WIFI_MGR_EVENT_SCAN_DONE` |
| `esp_err_t wifi_manager_scan(wifi_ap_info_t *aps, size_t max, size_t *count);` | **阻塞**扫描（HTTP handler / 测试用）。⚠️ **禁止**在 WiFi 事件回调或共享事件循环里调用（会自锁） |
| `esp_err_t wifi_manager_get_scan_result(wifi_ap_info_t *aps, size_t max, size_t *count);` | 读上次扫描的缓存结果 |
| `esp_err_t wifi_manager_connect(const char *ssid, const char *password);` | 发起连接后立即返回；成功后**持久化凭据** |
| `esp_err_t wifi_manager_disconnect(void);` | 断开并停止自动重连，直到下次显式 connect |
| `esp_err_t wifi_manager_get_state(wifi_mgr_state_t *state);` | |
| `esp_err_t wifi_manager_get_ip(char *ip, size_t len);` | 未连接时 `"0.0.0.0"` |
| `esp_err_t wifi_manager_get_mac(char *mac, size_t len);` | `"aa:bb:cc:dd:ee:ff"` |
| `esp_err_t wifi_manager_get_ssid(char *ssid, size_t len);` | 未知时 `""` |
| `int8_t wifi_manager_get_rssi(void);` | 未知时 0 |
| `esp_err_t wifi_manager_save_config(const char *ssid, const char *password);` | 只存不连；开放 AP 可传 `NULL` 密码 |
| `esp_err_t wifi_manager_clear_config(void);` | 忘记凭据 |
| `esp_err_t wifi_manager_auto_connect(void);` | 按 `wifi_auto` 用已存凭据连接；无凭据返回 `ESP_ERR_NOT_FOUND`（属正常状态，不算失败） |
| `bool wifi_manager_is_connected(void);` | 是否已拿到 IP |
| `const char *wifi_manager_state_name(wifi_mgr_state_t state);` | 供日志与 JSON |

类型与常量：`wifi_ap_info_t {char ssid[33]; int8_t rssi; wifi_auth_mode_t authmode; uint8_t channel; uint8_t bssid[6];}`、
`wifi_mgr_state_t {WIFI_MGR_DISCONNECTED, WIFI_MGR_CONNECTING, WIFI_MGR_CONNECTED, WIFI_MGR_FAILED}`、
`WIFI_MANAGER_MAX_APS`=24、`WIFI_MANAGER_SSID_MAX`=32、`WIFI_MANAGER_PASS_MAX`=64、`WIFI_MANAGER_HOSTNAME`=`"todohelper"`。

#### 事件

| 基址 `WIFI_MGR_EVENT` | payload |
|---|---|
| `WIFI_MGR_EVENT_SCAN_DONE`(0) | 无 |
| `WIFI_MGR_EVENT_CONNECTING` | 无 |
| `WIFI_MGR_EVENT_CONNECTED` | 无（已关联但可能还没 IP） |
| `WIFI_MGR_EVENT_GOT_IP` | 无（用 `wifi_manager_get_ip()` 取） |
| `WIFI_MGR_EVENT_DISCONNECTED` | `uint8_t reason` |
| `WIFI_MGR_EVENT_FAILED` | `uint8_t` 最后一次断线原因 |

额外向 `SYS_EVENT` 发 `SYS_EVENT_CONNECTIVITY`（payload `bool online`）。

#### 最小测试步骤

按 §0 构建烧录后，串口期望：

```
I (xxx) TH:wifi: ready (hostname 'todohelper', mac 1C:29:04:D0:71:B6)
I (xxx) TH:wifi: no saved network yet            # 首次上电
I (xxx) TH:wifi: auto connecting to stored network 'MyWiFi'
I (xxx) TH:wifi: state -> connecting
I (xxx) TH:wifi: got ip 192.168.1.23 (ssid 'MyWiFi', -52 dBm)
```

- 状态栏 WiFi 由"未连接"→"连接中"→"-52 dBm"（绿）。
- 断开路由器：`disconnected (reason 200)` + `reconnecting (N attempts left)`；恢复后自动重连。
- HTTP 侧证：`curl.exe http://todohelper.local/api/v1/device/info` 的
  `ip` / `mac` / `wifi_state` / 以及 `/api/v1/device/status` 的 `wifi_rssi` / `wifi_connected`。
- ⚠️ 需求 §8.3 的 `/api/v1/wifi/*` 五个接口**当前没有实现**，详见 §2 与 §5。

---

### todo_manager（L3）

- **组件目录**：`components/todo_manager/`
- **头文件**：`components/todo_manager/include/todo_manager.h`（私有 `src/todo_internal.h`）
- **依赖**：`sys_utils`、`storage_manager`、`http_router`、`cjson`
- **权威副本在设备**：清单落 `<storage>/todohelper/cache/todos.json`，断电不丢、离线可用

#### 公开函数

| 分组 | 签名 | 说明 |
|---|---|---|
| 生命周期 | `esp_err_t todo_manager_init(void);` | 载入持久化清单；存储不可用时进入**纯内存模式**。内部会调 `todo_manager_register_http()` |
| 生命周期 | `esp_err_t todo_save(void);` | 强制落盘 |
| 增删改 | `esp_err_t todo_add(const todo_item_t *item, uint32_t *out_id);` | `id` / `updated_at` / `sync_state` 由组件管理（调用方传的忽略）；`out_id` 可 `NULL`；超过 `TODO_MAX_ITEMS`(256) → `ESP_ERR_NO_MEM` |
| 增删改 | `esp_err_t todo_update(uint32_t id, const todo_item_t *item);` | 只替换 `title/desc/due_ts/done/priority` |
| 增删改 | `esp_err_t todo_set_done(uint32_t id, bool done);` | |
| 增删改 | `esp_err_t todo_delete(uint32_t id);` | |
| 增删改 | `esp_err_t todo_clear_done(size_t *removed);` | 删除全部已完成项，返回删除条数 |
| 查询 | `esp_err_t todo_get(uint32_t id, todo_item_t *out);` | 不存在 → `ESP_ERR_NOT_FOUND` |
| 查询 | `esp_err_t todo_list(todo_item_t **items, size_t *count);` | 快照，排序 `(done 升, priority 降, due_ts 升, id 升)`；调用方必须 `todo_free_list()` |
| 查询 | `esp_err_t todo_list_changed_since(int64_t since_ts, todo_item_t **items, size_t *count);` | `updated_at > since_ts` 的项 |
| 查询 | `void todo_free_list(todo_item_t *items);` | |
| 查询 | `esp_err_t todo_stats(todo_stats_t *out);` | `{total, done, pending}`，`pending` = `sync_state != TODO_SYNC_OK` |
| 同步 | `esp_err_t todo_mark_synced(uint32_t id);` | 单项标记为已同步 |
| 同步 | `esp_err_t todo_mark_all_synced(void);` | 整表标记（客户端声明完成全量同步） |
| 同步 | `esp_err_t todo_set_sync_state(uint32_t id, todo_sync_state_t state);` | |
| 同步 | `const char *todo_sync_state_name(todo_sync_state_t state);` | → `none` / `pending` / `ok` / `failed` |
| 同步 | `todo_sync_state_t todo_sync_state_from_name(const char *name);` | |
| JSON | `esp_err_t todo_export_json(char **json, size_t *len);` | 输出 `{"version":1,"next_id":N,"items":[...]}`，调用方 `free()` |
| JSON | `esp_err_t todo_import_json(const char *json);` | 忽略未知字段；`id==0` 补 `max_id+1` |
| HTTP | `esp_err_t todo_manager_register_http(void);` | 向 L1 `http_router` 注册 §2 的 Todo 路由 |

类型与常量：`todo_item_t {uint32_t id; char title[128]; char desc[256]; int64_t due_ts; bool done; uint8_t priority; int64_t updated_at; todo_sync_state_t sync_state;}`、
`todo_stats_t {size_t total; size_t done; size_t pending;}`、
`TODO_TITLE_MAX`=128、`TODO_DESC_MAX`=256、`TODO_MAX_ITEMS`=256。

#### 事件

| 基址 `TODO_EVENT` | payload |
|---|---|
| `TODO_EVENT_ADDED`(0) | `uint32_t id` |
| `TODO_EVENT_UPDATED` | `uint32_t id` |
| `TODO_EVENT_DONE_CHANGED` | `uint32_t id` |
| `TODO_EVENT_DELETED` | `uint32_t id` |
| `TODO_EVENT_SYNC_CHANGED` | `uint32_t id` |
| `TODO_EVENT_LIST_CHANGED` | 无（粗粒度"重画"通知） |

额外：每次变更调 `sys_notify("todo_changed")`（`sync_server` 转 SSE），并用 `sys_status_update()`
刷 `todo_total` / `todo_pending`。

#### 最小测试步骤

按 §0 构建烧录后，串口期望（首次上电）：

```
I (xxx) TH:todo: no todo cache yet, starting empty
I (xxx) TH:todo: ready with 0 item(s)
I (xxx) TH:todo_http: 7 todo routes registered
```

带缓存重启时第一行变成 `imported N items (next id M)`。

```powershell
curl.exe http://todohelper.local/api/v1/todos
curl.exe -X POST http://todohelper.local/api/v1/todos -H "Content-Type: application/json" -d '{\"title\":\"test\"}'
curl.exe -X PATCH http://todohelper.local/api/v1/todos/1/done -H "Content-Type: application/json" -d '{\"done\":true}'
curl.exe -X POST http://todohelper.local/api/v1/sync
```

- 新增返回 **201** + `code:0`，且该项 `sync_state` 立即是 `ok`（客户端推来的版本）。
- 在设备 UI 上勾选后 `GET /api/v1/todos` 应显示 `"sync_state":"pending"`；执行 `/api/v1/sync` 后变 `ok`。
- 断电重启清单仍在（`cache/todos.json`）。存储不可用时应看到
  `storage unavailable: todos will not survive a reboot`，重启后清单为空但不崩溃。

---

### calendar_service（L3）

- **组件目录**：`components/calendar_service/`
- **头文件**：`components/calendar_service/include/calendar_service.h`
- **依赖**：`sys_utils`、`esp_netif`（SNTP）、`lwip`、`http_router`、`cjson`
- **本板无 RTC**：断电即失时，时间只能靠 SNTP 现拿

#### 公开函数

| 分组 | 签名 | 说明 |
|---|---|---|
| 生命周期 | `esp_err_t calendar_service_init(void);` | 应用已存时区并订阅 `SYS_EVENT_CONNECTIVITY` **懒启动** SNTP（`wait_for_sync=false`，不阻塞） |
| 生命周期 | `esp_err_t calendar_sync_ntp(uint32_t timeout_ms);` | 等待校时；`0` 用默认 10 s；超时返回 `ESP_ERR_TIMEOUT` 并发 `SYNC_FAILED` |
| 查询 | `bool calendar_time_synced(void);` | 即 `sys_time_synced()` 转发，**不要**绕过它直接信任 `time(NULL)` |
| 查询 | `esp_err_t calendar_get_now(struct tm *out);` | 从未同步时返回 `ESP_ERR_INVALID_STATE`（**绝不返回 1970**） |
| 查询 | `esp_err_t calendar_format_now(char *out, size_t len);` | `"YYYY-MM-DD HH:MM:SS"`；未同步时写入字面量 `"时间未同步"` |
| 查询 | `esp_err_t calendar_get_month(int year, int month, struct tm *days, size_t *count);` | 每天一项（`tm_year/tm_mon/tm_mday/tm_wday`）；**纯算术，不需要时钟**；`year` 1970–2200、`month` 1–12 |
| 查询 | `esp_err_t calendar_first_weekday(int year, int month, int *out_wday);` | 该月 1 号星期，0=周日 |
| 时区 | `esp_err_t calendar_set_timezone(const char *tz);` | POSIX TZ 串（`CST-8`=UTC+8）；`setenv`+`tzset`、写 NVS `tz`、发事件；超 48 B → `ESP_ERR_INVALID_SIZE` |
| 时区 | `const char *calendar_get_timezone(void);` | |
| 校时 | `esp_err_t calendar_set_time(int64_t unix_seconds);` | 手动设钟，**不持久化**（重启需重新同步） |
| 校时 | `esp_err_t calendar_set_ntp_server(const char *server);` | 写 NVS `ntp_server`；SNTP 已在跑则 deinit 后用新地址重启 |
| 校时 | `const char *calendar_get_ntp_server(void);` | |
| HTTP | `esp_err_t calendar_service_register_http(void);` | 由 `init()` 自动调用 |

常量：`CALENDAR_DEFAULT_TZ`=`"CST-8"`、`CALENDAR_DEFAULT_NTP`=`"ntp.aliyun.com"`。

#### 事件

| 基址 `CALENDAR_EVENT` | payload |
|---|---|
| `CALENDAR_EVENT_TIME_SYNCED`(0) | `int64_t` Unix 秒（SNTP 回调里先 `sys_time_set_ts()` 再发） |
| `CALENDAR_EVENT_SYNC_FAILED` | 无 |
| `CALENDAR_EVENT_TIMEZONE_CHANGED` | `const char *tz`（栈上字符串，需立即拷贝） |

#### 最小测试步骤

按 §0 构建烧录后，WiFi 连上前串口期望：

```
I (xxx) TH:calendar: ready (tz 'CST-8', ntp 'ntp.aliyun.com', synced=0)
```

联网后：

```
I (xxx) TH:calendar: SNTP started (ntp.aliyun.com)
I (xxx) TH:calendar: time synchronised: 1730000000
```

```powershell
curl.exe http://todohelper.local/api/v1/calendar/now
# 未同步: {"code":0,...,"data":{"synced":false,"ts":0,"tz":"CST-8","ntp_server":"ntp.aliyun.com","datetime":"时间未同步"}}
curl.exe "http://todohelper.local/api/v1/calendar/month?year=2026&month=1"   # 未同步也应正常返回
curl.exe -X POST http://todohelper.local/api/v1/calendar/sync                 # 断网时应 503
```

UI 打开"日历"页：未同步时显示"时间未同步"；SNTP 完成后页面（订阅 `CALENDAR_EVENT_TIME_SYNCED`）自动刷新。

---

### weather_service（L3）

- **组件目录**：`components/weather_service/`
- **头文件**：`components/weather_service/include/weather_service.h`（provider 接口在私有 `src/weather_internal.h`）
- **依赖**：`sys_utils`、`storage_manager`、`http_router`、`cjson`、`esp_http_client`、`esp-tls`、`mbedtls`、`esp_timer`
- **无传感器、无 GPS**：温度/体感/湿度/天气现象一律取网络 API；定位只能"手动城市"（P0 唯一路径）；
  断网显示上次缓存（`cache/weather.json`）并用 `stale` 标记
- **默认 provider = Open-Meteo，不需要 API Key**；NVS `weather_key` 是给和风/OpenWeather 预留的，
  这类 provider 接口已就位但**尚未实现**（只有 `weather_provider_open_meteo` 编译进固件）

#### 公开函数

| 分组 | 签名 | 说明 |
|---|---|---|
| 生命周期 | `esp_err_t weather_service_init(void);` | 先载缓存，再起 worker 任务（`th_weather`，栈 6144，优先级 4）+ 周期刷新定时器；网络问题不导致失败 |
| 生命周期 | `esp_err_t weather_refresh(void);` | 同步刷新，最多等 ~20 s。离线 → `ESP_ERR_INVALID_STATE`；未设城市 → `ESP_ERR_NOT_FOUND`；超时 → `ESP_ERR_TIMEOUT` |
| 生命周期 | `esp_err_t weather_refresh_async(void);` | 只往深度 1 的队列丢 token（队列满返回 `ESP_OK`，即**合并**请求） |
| 查询 | `esp_err_t weather_get(weather_data_t *out);` | 最近一次读数（新鲜或缓存） |
| 查询 | `const char *weather_get_city(void);` | 未设为 `""` |
| 查询 | `const char *weather_get_provider(void);` | |
| 查询 | `uint32_t weather_get_interval_s(void);` | |
| 配置 | `esp_err_t weather_set_city(const char *city);` | 清空已缓存经纬度强制重新 geocode、写 NVS `city`，然后异步刷新 |
| 配置 | `esp_err_t weather_set_provider(const char *name);` | 未知 provider → `ESP_ERR_NOT_SUPPORTED` |
| 配置 | `esp_err_t weather_set_interval_s(uint32_t seconds);` | 夹到 **300–86400** 并重启定时器 |
| 配置 | `esp_err_t weather_set_api_key(const char *key);` | 只写 NVS `weather_key`（不写日志、不返回）；空串清除 |
| 配置 | `bool weather_has_api_key(void);` | 对外**唯一**的 Key 查询方式 |
| 缓存 | `esp_err_t weather_load_cache(void);` | 读 `cache/weather.json` |
| 缓存 | `esp_err_t weather_save_cache(void);` | 写 `cache/weather.json`（**绝不写 API Key**） |
| HTTP | `esp_err_t weather_service_register_http(void);` | 由 `init()` 自动调用 |

类型与常量：`weather_data_t {char city[64]; double lat, lon; float temp, feels_like; int humidity; char condition[64]; char icon[24]; int weather_code; int64_t update_ts; bool valid;}`、
`WEATHER_DEFAULT_INTERVAL_S`=900、`WEATHER_STALE_AFTER_S`=1800（=2×900）、`WEATHER_PROVIDER_DEFAULT`=`"open-meteo"`。
`stale` 判定：`now>0 && update_ts>0 && (now-update_ts) > 1800`；**时钟未同步（`now==0`）时 `stale` 恒为 false**（不误报）。

#### 事件

| 基址 `WEATHER_EVENT` | payload |
|---|---|
| `WEATHER_EVENT_UPDATED`(0) | 无（读 `weather_get()`）；同时 `sys_notify("weather_changed")` |
| `WEATHER_EVENT_REFRESH_FAILED` | `esp_err_t` |

#### 最小测试步骤

按 §0 构建烧录后，首次上电（未设城市、无缓存）：

```
I (xxx) TH:weather: no cached weather yet
I (xxx) TH:weather: ready (provider 'open-meteo', city '<unset>', every 900 s)
```

设城市（**不需要任何 API Key**）：

```powershell
curl.exe -X POST http://todohelper.local/api/v1/weather/city -H "Content-Type: application/json" -d '{\"city\":\"Beijing\"}'
curl.exe http://todohelper.local/api/v1/weather
```

期望串口：

```
I (xxx) TH:weather: city set to 'Beijing'
I (xxx) TH:weather_om: 'Beijing' -> 39.9075, 116.3972
I (xxx) TH:weather_om: 4.0 C, feels 2.1 C, 46%, '晴' (code 0)
```

断网验证：`POST /api/v1/weather/refresh` → 503 `"offline, showing the cached reading"`，
串口 `TH:weather: refresh failed: ESP_ERR_INVALID_STATE`，而 `GET /api/v1/weather` 仍回上次读数；
重启后应看到 `TH:weather: cache loaded: Beijing 4.0 C '晴'`。

---

### word_service（L3）

- **组件目录**：`components/word_service/`
- **头文件**：`components/word_service/include/word_service.h`（内置词库在私有 `src/word_default_book.h`）
- **依赖**：`sys_utils`、`storage_manager`、`http_router`、`cjson`
- 词库 `<app_dir>/words/<book>.json`（默认 `book` = `cet4`），进度 `<app_dir>/words/progress.json`；
  无词库文件时把内置起始词库（32 词）写到存储做模板，同时用编译进固件的版本兜底

#### 公开函数

| 分组 | 签名 | 说明 |
|---|---|---|
| 生命周期 | `esp_err_t word_service_init(void);` | 载词库 + 进度；内部会调 `word_service_register_http()` |
| 生命周期 | `esp_err_t word_load_from_sd(const char *path);` | 从显式路径换词库（可为绝对路径或相对应用目录） |
| 生命周期 | `esp_err_t word_reload(void);` | 按 NVS 选择重新载入 |
| 词库 | `const char *word_get_book(void);` | 当前词库名 |
| 词库 | `esp_err_t word_set_book(const char *book);` | 持久化并重新载入 |
| 队列 | `esp_err_t word_get_today(word_item_t *out, size_t max, size_t *count);` | 重建队列并把 `*count` 设为**队列总长**；`out=NULL`/`max=0` 时只查数量 |
| 队列 | `esp_err_t word_next(word_item_t *out);` | 内部游标发牌，走到末尾**回绕** |
| 队列 | `esp_err_t word_get(uint32_t id, word_item_t *out);` | 按 id 查（含当前 level） |
| 队列 | `esp_err_t word_restart_queue(void);` | 游标回到第一张 |
| 队列 | `esp_err_t word_queue_position(size_t *position, size_t *total);` | 1-based 位置与队列长度 |
| 复习 | `esp_err_t word_review(uint32_t id, word_review_result_t result);` | 记分 + 重排期 + **立即落盘**；发 `WORD_EVENT_REVIEWED` 与 `sys_notify("word_reviewed")`；存储不可用时返回 `ESP_ERR_INVALID_STATE` |
| 复习 | `esp_err_t word_save_progress(void);` | 写 `words/progress.json`（原子写） |
| 复习 | `esp_err_t word_reset_progress(void);` | 清空当前词库进度 |
| 复习 | `const char *word_review_name(word_review_result_t result);` | → `known` / `fuzzy` / `unknown` |
| 复习 | `bool word_review_from_name(const char *name, word_review_result_t *out);` | 未知名字返回 `false` |
| 统计 | `esp_err_t word_stats(word_stats_t *out);` | `new_words`=从未复习；`due`=`next_review_ts<=now`；`learned`=`level>=3` |
| 统计 | `uint32_t word_get_daily_count(void);` | |
| 统计 | `esp_err_t word_set_daily_count(uint32_t count);` | 夹到 **1–100**，并清空队列强制重建 |
| HTTP | `esp_err_t word_service_register_http(void);` | 由 `init()` 自动调用 |

类型与常量：`word_item_t {uint32_t id; char spelling[64]; char phonetic[64]; char meaning[256]; char example[256]; uint8_t level; int64_t next_review_ts;}`、
`word_stats_t {total, new_words, due, learned, reviewed_total, known_total, fuzzy_total, unknown_total}`、
`word_review_result_t {WORD_REVIEW_UNKNOWN=0, WORD_REVIEW_FUZZY, WORD_REVIEW_KNOWN}`、
`WORD_SPELLING_MAX`=64、`WORD_PHONETIC_MAX`=64、`WORD_MEANING_MAX`=256、`WORD_EXAMPLE_MAX`=256、
`WORD_MAX_ITEMS`=2000、`WORD_DEFAULT_BOOK`=`"cet4"`、`WORD_DEFAULT_DAILY_COUNT`=10。

复习调度（`schedule()`）：

| 评分 | level | `next_review_ts` |
|---|---|---|
| `WORD_REVIEW_KNOWN` | `level+1`，封顶 5 | `now + 2^level × 86400`（按自增后的 level） |
| `WORD_REVIEW_FUZZY` | 不变 | `now + 86400` |
| `WORD_REVIEW_UNKNOWN` | 归 0 | `now + 600` |

#### 事件

| 基址 `WORD_EVENT` | payload |
|---|---|
| `WORD_EVENT_LOADED`(0) | `size_t` 词数 |
| `WORD_EVENT_REVIEWED` | `uint32_t` 词 id |
| `WORD_EVENT_PROGRESS_SAVED` | 无 |
| `WORD_EVENT_LOAD_FAILED` | `esp_err_t` |

#### 最小测试步骤

按 §0 构建烧录后，首次上电（无词库文件）：

```
I (xxx) TH:word: starter wordbook written to 'words/cet4.json'
I (xxx) TH:word: using the compiled-in starter wordbook
I (xxx) TH:word: ready (book 'cet4', 32 words, 10/day)
```

第二次上电：`loaded 32 words from 'words/cet4.json'` + `progress restored for N word(s)`。

```powershell
curl.exe http://todohelper.local/api/v1/words/today
curl.exe -X POST http://todohelper.local/api/v1/words/1/review -H "Content-Type: application/json" -d '{\"result\":\"known\"}'
curl.exe -X POST http://todohelper.local/api/v1/words/1/review -H "Content-Type: application/json" -d '{\"result\":\"unknown\"}'
curl.exe http://todohelper.local/api/v1/words/stats
```

- `known` 后 `level`=1、`next_review_ts` ≈ now + 2 天；`unknown` 后 `level`=0、`next_review_ts` ≈ now + 600 s。
- `words/progress.json` 被原子更新；重启后 `progress restored` 条数与已复习词数一致。

---

### sync_server（L4）

- **组件目录**：`components/sync_server/`
- **头文件**：`components/sync_server/include/sync_server.h`
- **依赖**：`sys_utils`、`http_router`、`cjson`、`wifi_manager`、`storage_manager`、`esp_app_format`、
  `esp_timer`、`esp_hw_support`、`esp_system`、`heap`；私有 `mdns`；`EMBED_FILES web/index.html`
- **不持有业务逻辑**：只起 HTTP 服务、挂 `http_router`、持有设备 API 与 SSE；业务路由由各组件反向注册

#### 公开函数

| 分组 | 签名 | 说明 |
|---|---|---|
| 生命周期 | `esp_err_t sync_server_init(void);` | 读 NVS 配置并启动；幂等；失败视为可恢复（没有 HTTP 设备照样跑） |
| 生命周期 | `esp_err_t sync_server_start(const sync_server_config_t *cfg);` | 用显式配置启动/重启；`cfg==NULL` 时用 NVS 默认值 |
| 生命周期 | `esp_err_t sync_server_stop(void);` | 停服并撤销 mDNS |
| 生命周期 | `bool sync_server_is_running(void);` | |
| 生命周期 | `const char *sync_server_url(void);` | `"http://todohelper.local"` 或 `"http://<ip>"`；静态存储 |
| 生命周期 | `httpd_handle_t sync_server_handle(void);` | 停止时 `NULL` |
| 路由 | `esp_err_t sync_server_register_route(const char *path, httpd_method_t method, http_route_handler_t handler);` | `http_router_register()` 的薄包装 |
| 路由 | `esp_err_t sync_server_register_not_implemented(const char *path, httpd_method_t method, const char *hint);` | 注册 501 占位 |
| 鉴权 | `esp_err_t sync_server_set_auth_token(const char *token);` | 写 NVS `auth_token` 并开启鉴权；`NULL`/`""` 关闭鉴权；长度上限 63 |
| 鉴权 | `bool sync_server_auth_required(void);` | 写操作当前是否需要 token |
| 鉴权 | `bool sync_server_check_auth(httpd_req_t *req);` | 路由器安装的鉴权 hook，**只读 `X-Auth-Token` 头**（不接受 query 参数） |
| SSE | `esp_err_t sync_server_broadcast_event(const char *event_name, const char *json_payload);` | 推给所有 SSE 客户端；慢客户端丢事件不阻塞发布方 |
| SSE | `size_t sync_server_sse_clients(void);` | 当前 `/api/v1/events` 订阅数 |

类型与常量：`sync_server_config_t {uint16_t port; bool mdns_enabled; bool auth_enabled; char hostname[32];}`、
`SYNC_SERVER_DEFAULT_PORT`=80、`SYNC_SERVER_DEFAULT_HOSTNAME`=`"todohelper"`。

服务端参数（`sync_server_start()` 内）：`uri_match_fn = httpd_uri_match_wildcard`、`lru_purge_enable=true`、
`max_uri_handlers=4`、`max_open_sockets=5`、`stack_size=6144`、`recv/send_wait_timeout=5 s`；
SSE 上限 **3** 个并发客户端（`SYNC_SSE_MAX_CLIENTS`；每个订阅者占 1 个 httpd 会话槽 + 1 个
3 KB 任务，必须给页面与 REST 留槽），每客户端独立队列深度 8，keepalive **15000 ms**
（发 `": keepalive\n\n"`），开场发 `retry: 3000`，超限返回 **503** `"too many event subscribers"`。

> **写新的流式/长连接端点时必须注意**：`esp_http_server` 的 URI handler 在**唯一** httpd 任务里
> **串行**执行，一个"不返回"的 handler 会冻住整台服务器（`ping` 通但任何 TCP 都建不上）。
> 长连接必须 `httpd_req_async_handler_begin()` 把 socket 交给自己的任务，结束时
> `httpd_req_async_handler_complete()`。参考实现：`sync_server.c` 的
> `events_get_handler()` + `sse_client_task()`。背景与实测见 `AGENT.md §9.0.1` 第 4 条。
> （已知同类隐患：`/api/v1/ai/chat/stream` 与阻塞版 `/api/v1/ai/chat`，用户决定本轮不动。）

`lru_purge_enable=true` 时，socket 用满会关掉最久未用的会话，但**异步（SSE）会话不会被回收**
（`esp_httpd_priv.h`：`for_async_req` 的 socket 不参与 LRU purge）。

#### 事件

| 基址 `SYNC_EVENT` | payload | 实际是否发出 |
|---|---|---|
| `SYNC_EVENT_STARTED`(0) | 无 | ✅ |
| `SYNC_EVENT_STOPPED` | 无 | ✅ |
| `SYNC_EVENT_CLIENT_CONNECTED` | 无 | ❌ 已声明但代码里没有任何投递点 |
| `SYNC_EVENT_AUTH_FAILED` | `const char *path` | ✅（强制鉴权路径与 `auth_enabled=true` 时都发） |
| `SYNC_EVENT_BROADCAST` | `const char *event name` | ✅ |

另外订阅 L0 的 `SYS_NOTIFY_EVENT`，把每个通知转成 SSE 推送（这就是它不需要依赖任何业务组件的原因）。

#### 最小测试步骤

按 §0 构建烧录后，串口期望：

```
I (xxx) TH:sync: mDNS published as todohelper.local
I (xxx) TH:sync: listening on port 80, 45 route(s), auth=off, url=http://todohelper.local
```

```powershell
curl.exe http://todohelper.local/api/v1/device/info
curl.exe http://todohelper.local/api/v1/device/status
curl.exe -N http://todohelper.local/api/v1/events          # SSE，Ctrl+C 退出
curl.exe -i http://todohelper.local/ws/events              # 501
```

- 另开一个窗口 `POST /api/v1/todos`，SSE 窗口应收到 `event: todo_changed`。
- 开鉴权后（见 §2）：`curl.exe -X POST http://todohelper.local/api/v1/device/reboot` 应 401；
  带 `-H "X-Auth-Token: <token>"` 才 200 并重启。

---

### idle_face（L4）

- **组件目录**：`components/idle_face/`
- **头文件**：`components/idle_face/include/idle_face.h`
- **依赖**：`sys_utils`、`bsp`、`ui_core`、`lvgl`
- 全部用 LVGL 图元绘制（无位图、无 SD 素材）：所有状态共用一组对象、只改属性做补间，切换表情不分配内存
- **`UI_PAGE_IDLE_FACE` 由本组件自己注册**（`idle_face_init()` → `ui_page_register()`），
  不在 `ui_pages` 的页面表里

#### 公开函数

| 签名 | 说明 |
|---|---|
| `esp_err_t idle_face_init(void);` | 组件入口：注册待机页并启动动画器（由 `main` 模块表调用） |
| `esp_err_t idle_face_attach(lv_obj_t *parent);` | 在 `parent` 内建眼睛并开始绘制；幂等；也可嵌到别的页面（如 AI 聊天页头部） |
| `esp_err_t idle_face_set_state(idle_face_state_t state);` | 切换表情，任意任务可调 |
| `idle_face_state_t idle_face_get_state(void);` | 当前表情 |
| `esp_err_t idle_face_express(idle_face_state_t state, uint32_t duration_ms);` | 临时显示某表情 `duration_ms` 后回到原表情 |
| `esp_err_t idle_face_standby_enter(void);` | 待机页 `on_show` 调用的钩子。**刻意不闭眼入睡**：待机期间脸部保持 `NORMAL` 并重启整套行为时间线（眨眼/注视/自发表情/瞳孔），然后靠"困倦度"自己慢慢沉下去（见下）。活动表情在显示时返回 `ESP_ERR_INVALID_STATE`（不打断它），否则 `ESP_OK` |
| `esp_err_t idle_face_standby_exit(void);` | 待机页 `on_hide` 调用的钩子：一次约 420 ms 的睁眼过冲（峰值在 ~200 ms），并清掉困倦度与当前自发表情。**可见**是因为 `ui_core` 现在让离场页面淡出 240 ms（`UI_PAGE_ANIM_OUT_MS`）；表情占用脸部时返回 `ESP_ERR_INVALID_STATE` |
| `esp_err_t idle_face_tick(uint32_t now_ms);` | 推进动画（内部 33 ms 定时器调用；暴露给自建循环的宿主） |
| `esp_err_t idle_face_set_brightness(uint8_t percent);` | 调暗/恢复；**何时调由 `ui_core` 决定**，本组件只转发 |
| `bool idle_face_is_attached(void);` | 眼睛是否已建好 |
| `extern const void *idle_face_page_descriptor(void);` | 待机页描述符（不透明指针），供宿主组合而不必包含 `ui_core` 内部结构 |

状态枚举（11 个业务状态 + `IDLE_FACE_MAX`）：
`IDLE_FACE_NORMAL`(0)、`BLINK`、`LOOK_LEFT`、`LOOK_RIGHT`、`HAPPY`、`SLEEPY`、`ERROR`、`SYNCING`、
`THINKING`、`LISTENING`(P2)、`SPEAKING`(P2)。

**动画行为**（纯程序化，无位图/SD 依赖）：

- **表情之间是交叉淡入**：目标状态变化时把当前视觉值冻结为起点，用 `smoothstep` 在
  180 ms（`SPEED_FAST`，眨眼）或 420 ms（其余）内走到目标参数并吸附，**不会跳变**；
- **随机化**：眨眼 2.6–7.5 s / 110–165 ms 且 18% 连眨、左右看 4–12 s（22% 会立刻向另一侧再看一眼）、
  微扫视 1.8–5.2 s、呼吸周期 12–19 s（每周期重抽）；
- **1.1 新增的四层"慢动作"**（这是"待机时不能看起来像静止图"的关键）：
  1. **自发表情**：每 13–34 s 随机做一个 320–1100 ms 的表情（SLEEPY/HAPPY/LISTENING/THINKING/SPEAKING），
     通过正常交叉淡入进出，绝不超过自己的时长；
  2. **困倦度** 0→1：无人交互时约 2.5 分钟累积（眨眼更密更慢、眼睛缩短 10%、视线下垂 5 px），
     任何真实交互（表情/唤醒）瞬间清零；自发表情**不清零**（那只是发呆，不是注意）；
  3. **瞳孔缩放**：5–11 s 重抽一次 0.91–1.14 的缓慢随机游走（LISTENING 再额外放大 6%）；
  4. **整脸微漂移**：两条不相干频率的正弦（±3 px / ±2 px）合成"轻微转头"，表情占用时缓动归零
     （这样 ERROR 的 X 线与眼睛始终对齐）；
- **待机过场**：进入待机**不闭眼**（保持 NORMAL + 上述全部动作），离开待机时睁眼过冲一次再回 `NORMAL`；
- 过场与切换都只改已有对象的属性，**不分配内存**；动画器是 `lv_timer`，回调天然在 LVGL 任务里，无需加锁。

> 状态定义在三处必须同步：本头文件、`idle_face.c` 的效果表、需求文档 FR6 / §11.2。
> 详细设计见 `components/idle_face/README.md`。

#### 事件

| 基址 `IDLE_FACE_EVENT` | payload |
|---|---|
| `IDLE_FACE_EVENT_STATE_CHANGED`(0) | `idle_face_state_t` |

表情来源有两条：L0 `sys_notify()` 通知经 `s_triggers[]` 触发表映射（`wifi_connected`→HAPPY、
`wifi_failed`→ERROR、`net_offline`→SLEEPY、`net_online`/`todo_done`/`sync_done`/`widget_done`→HAPPY、
`sync_started`→SYNCING），以及 `ai_service` 直接调 `idle_face_express()`
（聆听 4 s、思考/说话 20 s、出错 2.5 s）。

#### 最小测试步骤

按 §0 构建烧录后，串口期望：

```
I (xxx) TH:idle_face: ready (11 states, 8 triggers)
I (xxx) TH:idle_face: idle face attached (WxH eyes, gap G)      # 待机页首次创建时
```

- 30 s 无触摸 → 自动切到待机页：全黑背景 + 白色线条双眼，眼睛在约 1 s 内**闭合成半闭睡眼**
  并保持（不再眨眼），亮度降到 NVS `idle_bright`（默认 20%）；
- 触摸任意位置 → 眼睛**睁一下**再恢复，回到待机前页面、亮度恢复。

---

### ui_pages（L4）

- **组件目录**：`components/ui_pages/`
- **头文件**：`components/ui_pages/include/ui_pages.h`（对外只有 `ui_pages_init()`；页面描述符在私有 `src/pages_internal.h`）
- **依赖**：`ui_core`、`ui_assets`、`sys_utils`、`storage_manager`、`wifi_manager`、`todo_manager`、
  `calendar_service`、`weather_service`、`word_service`、`sync_server`、`lvgl`

#### 公开函数

| 签名 | 说明 |
|---|---|
| `esp_err_t ui_pages_init(void);` | 在 `ui_core_init()` 之后调用，把页面表注册给 `ui_core` |

页面表（`src/ui_pages.c` 的 `s_pages[]`，**唯一的组合点**）：

| 页面 | 枚举 | 文件 |
|---|---|---|
| 启动页 | `UI_PAGE_SPLASH` | `src/page_splash.c` |
| 主菜单 / 桌面 | `UI_PAGE_HOME` | `src/page_home.c` |
| WiFi（扫描/密码/状态子视图） | `UI_PAGE_WIFI` | `src/page_wifi.c` |
| 待办列表 | `UI_PAGE_TODO` | `src/page_todo.c` |
| 待办详情 / 同步状态 | `UI_PAGE_TODO_DETAIL` | `src/page_todo.c`（`page_todo_detail`，同一文件两个描述符） |
| 天气 | `UI_PAGE_WEATHER` | `src/page_weather.c` |
| 日历 | `UI_PAGE_CALENDAR` | `src/page_calendar.c` |
| 单词卡 | `UI_PAGE_WORD` | `src/page_word.c` |
| AI 对话 | `UI_PAGE_AI_CHAT` | `src/page_ai_chat.c` |
| 设置 | `UI_PAGE_SETTINGS` | `src/page_settings.c` |
| 待机眼睛 | `UI_PAGE_IDLE_FACE` | 由 `idle_face` 组件注册（**不在此表**） |

页面规范（`README.md` 与 `pages_internal.h`）：

- 只用 `ui_theme.h` 的配色/字号/间距，不写字面色值与绝对坐标；触摸控件 ≥ `UI_TOUCH_MIN`(44 px)；
- 布局用 flex（`LV_FLEX_FLOW_COLUMN` / `ROW_WRAP`），不用 `lv_obj_set_pos()`；
- 只在 `create(parent)` 传入的容器里建控件，**不得**碰屏幕或其他页面、不得自建 LVGL screen；
- 需要业务数据时订阅事件（`ui_event_bind`）或读 `sys_status_get()`，不直接调 service 的阻塞接口；
- 时间相关 UI 必须先判 `sys_time_synced()`，未同步显示 `UI_STATUS_TIME_UNSYNCED`。

#### 事件

不发事件；通过 `ui_event_bind(base, id, page, payload_size)` 订阅各 service 事件
（`WIFI_MGR_EVENT`、`TODO_EVENT`、`WEATHER_EVENT`、`CALENDAR_EVENT`、`WORD_EVENT`、`SYS_EVENT`）。

#### 最小测试步骤

按 §0 构建烧录后，逐页点开：每页应显示自己的标题、左侧"返回"按钮，顶部状态栏保持不动。
串口侧证：`I (xxx) TH:ui_core: registered 10 pages`（`ui_pages` 注册 10 个，`idle_face` 再注册 1 个）。

---

### main（编排层，非组件）

- **目录 / 入口**：`main/app_main.c`（`void app_main(void)`）；没有公开头文件
- **依赖**：`main/CMakeLists.txt` 的 `REQUIRES` = `sys_utils` `storage_manager` `bsp` `ui_assets`
  `http_router` `ui_core` `ui_pages` `wifi_manager` `todo_manager` `calendar_service`
  `weather_service` `word_service` `sync_server` `idle_face`

启动顺序（`app_main()` 实际代码）：

1. `SYS_LOGI("ToDoHelper starting")`；`ESP_ERROR_CHECK(sys_config_init(NULL))`、`ESP_ERROR_CHECK(sys_events_init())`；
2. `log_reset_reason()`（打印复位原因）；
3. `ESP_ERROR_CHECK(th_bsp_init())`（显示必须最先）；
4. `ESP_ERROR_CHECK(storage_manager_init())`；
5. `ui_assets_init()`（失败只告警："pages will use text fallbacks"）；
6. `ESP_ERROR_CHECK(ui_core_init())`、`ESP_ERROR_CHECK(ui_pages_init())`，切到 `UI_PAGE_SPLASH`；
7. 跑 `after_ui=false` 的模块（当前表里没有），延时 400 ms 后切 `UI_PAGE_HOME`；
8. 跑 `after_ui=true` 的模块表（顺序）：`wifi_manager` → `todo_manager` → `weather_service` →
   `calendar_service` → `word_service` → `sync_server` → `idle_face`（**全部 `optional=true`**）；
9. `ui_statusbar_refresh()`，发 `SYS_EVENT_READY`，打印 `ready (storage=<root>)`。

模块表结构（新增功能组件 = 加一行）：

```c
typedef struct {
    const char *name;
    esp_err_t (*init)(void);
    bool optional;      /* true: 失败只告警不中断启动 */
    bool after_ui;      /* true: UI 起来之后再初始化 */
} app_module_t;
```

**任何 optional 模块失败都不得阻止 UI 起来**（需求 §15 健壮性验收）。

#### 事件

`main` 只发一个：`SYS_EVENT_READY`（无 payload）。

#### 最小测试步骤

按 §0 构建烧录后，串口关键行：

```
I (xxx) TH:app: ToDoHelper starting
I (xxx) TH:app: reset reason: power-on (1)
I (xxx) TH:bsp: ready: 800x480, tear-avoid=triple-partial, ppa=1, brightness=100%
I (xxx) TH:storage: ready: root=... app=... free=... KB
I (xxx) TH:ui_core: ready (800x480)
I (xxx) TH:app: module 'wifi_manager' ready
...（每个模块一行）
I (xxx) TH:app: ready (storage=/sdcard)
```

- 若某可选模块不可用，应看到 `TH:app: module 'xxx' unavailable: ESP_ERR_...`，而**不是**启动中断。
- 端到端冒烟：`curl.exe http://todohelper.local/api/v1/device/status` 的 `routes`(29) / `time_synced` / `free_heap`。

---

## 2. HTTP API

基础地址：`http://todohelper.local`（mDNS，主机名 `todohelper`）或设备 IP；
把示例里的 `todohelper.local` 换成 IP 也可以（IP 见 `GET /api/v1/device/info` 的 `ip` 字段）。
所有 JSON 响应统一信封，`OPTIONS` 统一 204 + CORS。

### 2.1 端点总表

下表是**实际注册**的全部路由（来源：`components/*/src/*.c` 中的
`http_router_register()` / `http_router_register_not_implemented()` 路由表）。
当前注册 **45** 条：`sync_server` 8 + `todo_manager` 7 + `weather_service` 5 +
`calendar_service` 5 + `word_service` 5 + `wifi_manager` 7 + `ai_service` 6 + `voice_service` 2，
与 `GET /api/v1/device/status` 的 `routes` 字段一致（1.0.0 真机实测 45）。

> **1.0.0 更正**：旧版这张表把 `/api/v1/wifi/*` 记为"组件里没有任何 `http_router_register()`
> 调用 → 404"、把 `ai_service` 记为"不在 `app_main` 模块表里 → 运行时 404"。**两条都是错的**，
> 来源是子 agent 未经核实的自述。真机实测：`GET /api/v1/wifi/status` → 200（connected/ssid/ip/rssi）、
> `POST /api/v1/wifi/scan` → 200 `{"scanning":true}`、随后 `GET /api/v1/wifi/scan` → 22 个 AP；
> `ai_service_init()` 就在 `main/app_main.c` 的模块表里（开机日志 `module 'ai_service' ready`）。
> 教训见 `docs/HANDOVER.md §5` 第 1 条：**验收必须自己读代码、上机验证**。

| 方法 | 路径 | 组件 | 说明 | 状态 |
|---|---|---|---|---|
| GET | `/` | sync_server | 内嵌浏览器待办页（`EMBED_FILES web/index.html`，`text/html; charset=utf-8`，`Cache-Control: no-cache`） | ✅ 200 |
| GET | `/index.html` | sync_server | 同上 | ✅ 200 |
| GET | `/api/v1/device/info` | sync_server | 芯片/固件版本/IDF/`mac`/`ip`/`hostname`/`wifi_state`/存储状态/`auth_enabled`；**不含任何密钥或 token**。`version` 来自工程根目录 `version.txt`（规则见 `AGENT.md §8.1`），1.0.0 实测 `"version":"1.0.0"` | ✅ 200 |
| GET | `/api/v1/device/status` | sync_server | `uptime_s`/`free_heap`/`min_free_heap`/`free_psram`/`routes`/`sse_clients`/`wifi_rssi`/`wifi_connected`/`time_synced`/`server_ts`/`url`；1.0.0 实测 `routes:45`、`sse_clients` 随订阅数变化 | ✅ 200 |
| POST | `/api/v1/device/reboot` | sync_server | 先回 `{"rebooting":true}`，再 700 ms 后 `esp_restart()`。**强制鉴权** | ✅ 200（需 token） |
| POST | `/api/v1/device/auth` | sync_server | body `{"token":"..."}`；空串/`null` 关闭鉴权、开启鉴权时长度 ≤63。**强制鉴权** | ✅ 200（需 token） |
| GET | `/api/v1/events` | sync_server | **SSE** 推送（`text/event-stream`，**异步 handler + 每客户端任务**）；最多 3 客户端，15 s keepalive，开场 `retry: 3000`；超限 → 503 | ✅ 200 |
| GET | `/ws/events` | sync_server | WebSocket 占位，`data` = `{"detail":"not implemented","hint":"WebSocket is not available in this build; use /api/v1/events (SSE) instead"}` | ❌ **501 未实现** |
| GET | `/api/v1/todos` | todo_manager | 全量列表；带 `?updated_since=<unix秒>` 时只回 `updated_at > ts` 的项（`incremental:true`）。`data` 含 `items`/`count`/`incremental`/`server_ts` | ✅ 200 |
| POST | `/api/v1/todos` | todo_manager | body `{title,desc,due_ts,priority}`，`title` 必填；handler 随后 `todo_mark_synced()` | ✅ **201**（`code:0`） |
| GET | `/api/v1/todos/{id}` | todo_manager | 单项；不存在 → 404 | ✅ 200 |
| PUT | `/api/v1/todos/{id}` | todo_manager | 全量更新，`title` 必填 | ✅ 200 |
| PATCH | `/api/v1/todos/{id}/done` | todo_manager | body `{"done":true}`（必须是布尔，否则 400） | ✅ 200 |
| DELETE | `/api/v1/todos/{id}` | todo_manager | 删除，成功返回 `{"id":N,"deleted":true}` | ✅ 200 |
| POST | `/api/v1/sync` | todo_manager | 客户端声明整表已同步 → `todo_mark_all_synced()` | ✅ 200 |
| GET | `/api/v1/weather` | weather_service | 当前读数；无读数时仍 **200** + `valid:false`（让页面提示选城市）。含 `stale` | ✅ 200 |
| POST | `/api/v1/weather/refresh` | weather_service | 同步刷新；未设城市 → 400；离线/超时 → 503 | ✅ 200/400/503 |
| POST | `/api/v1/weather/city` | weather_service | body `{"city":"Beijing"}`，持久化后立即刷新一次 | ✅ 200 |
| POST | `/api/v1/weather/locate` | weather_service | 纯 IP 定位；`geo_url` 可经 NVS 换服务商；本机网络实测到 `ipapi.co` 不通 → 502（语义正确） | ✅ 200/502 |
| POST | `/api/v1/weather/config` | weather_service | body 可含 `provider`/`api_key`/`interval_s`；响应只回 `provider`/`has_api_key`/`interval_s`（**不回显 Key**） | ✅ 200 |
| GET | `/api/v1/calendar/now` | calendar_service | `synced`/`ts`/`tz`/`ntp_server`；已同步时另有 `datetime`/`year`/`month`/`day`/`weekday`，未同步 `datetime` = `"时间未同步"` | ✅ 200 |
| GET | `/api/v1/calendar/month` | calendar_service | **必须**带 `?year=&month=`（缺 → 400；`year` 1970–2200、`month` 1–12 越界 → 400）。含 `days`/`first_weekday`/`weekdays[]`，本月带 `today` | ✅ 200 |
| POST | `/api/v1/calendar/sync` | calendar_service | 主动 `calendar_sync_ntp(0)`；失败 → 503 `"NTP sync failed (is the network up?)"` | ✅ 200/503 |
| POST | `/api/v1/calendar/time` | calendar_service | body `{"ts":<unix秒>}`，手动设钟（**不持久化**） | ✅ 200 |
| POST | `/api/v1/calendar/tz` | calendar_service | body `{"tz":"CST-8"}`，改时区并持久化 | ✅ 200 |
| GET | `/api/v1/words/today` | word_service | 今日队列 + `count`/`book`/`server_ts`/嵌套 `stats` | ✅ 200 |
| GET | `/api/v1/words/stats` | word_service | 统计（含 `book`/`daily_count`） | ✅ 200 |
| GET | `/api/v1/words/next` | word_service | 下一张卡 + `position`/`queue_total`；队空 → **404** | ✅ 200/404 |
| POST | `/api/v1/words/{id}/review` | word_service | body `{"result":"known"|"fuzzy"|"unknown"}`（其它值 → 400），返回该词最新状态 | ✅ 200 |
| POST | `/api/v1/words/config` | word_service | body 可含 `daily_count`/`book`/`reset:true`，返回统计 | ✅ 200 |
| GET | `/api/v1/wifi/status` | wifi_manager | `state`/`connected`/`ssid`/`ip`/`rssi`/`mac`/`hostname`。**1.0.0 实测 200** | ✅ 200 |
| GET | `/api/v1/wifi/scan` | wifi_manager | **上一次**扫描的缓存结果 `{count,items[]}`；刚开机未扫过时 `count:0`（要新结果先 POST） | ✅ 200 |
| POST | `/api/v1/wifi/scan` | wifi_manager | 触发异步扫描，立即返回 `{"scanning":true}`；结果靠随后 GET 轮询 | ✅ 200 |
| POST | `/api/v1/wifi/connect` | wifi_manager | body `{"ssid","password"}`，异步发起；凭据写 NVS | ✅ 200 |
| POST | `/api/v1/wifi/disconnect` | wifi_manager | 断开并停止自动重连 | ✅ 200 |
| POST | `/api/v1/wifi/config` | wifi_manager | body `{"ssid","password","auto_connect"}`：只存凭据不连接 | ✅ 200 |
| POST | `/api/v1/wifi/forget` | wifi_manager | 清除 NVS 里的 SSID/密码（不主动断开） | ✅ 200 |
| POST | `/api/v1/ai/chat` | ai_service | 阻塞式对话（一次 AI 回复期间会占住 httpd 单任务，见上文长连接警告） | ✅ 200 |
| POST | `/api/v1/ai/chat/stream` | ai_service | 流式对话（`text/event-stream`）；**同类长连接隐患，用户决定 1.0.0 暂不修** | ✅ 200 |
| GET | `/api/v1/ai/history` | ai_service | 对话历史。**强制鉴权** | ✅ 200（需 token） |
| DELETE | `/api/v1/ai/history` | ai_service | 清空历史。**强制鉴权** | ✅ 200（需 token） |
| POST | `/api/v1/ai/config` | ai_service | 设置 API Key / 模型。**强制鉴权**，响应不回显 Key | ✅ 200（需 token） |
| POST | `/api/v1/ai/abort` | ai_service | 中断正在进行的流式回答 | ✅ 200 |
| GET | `/api/v1/voice/status` | voice_service | 录音/回放可用性与 ASR/TTS 端点配置状态（音频按需打开） | ✅ 200 |
| POST | `/api/v1/voice/config` | voice_service | 配置 ASR/TTS HTTP 端点（选型未定，见需求 FR7） | ✅ 200 |

> **路径参数写法**：路由器只支持**一个** `*`，所以单项接口注册为 `/api/v1/todos/*`、动作为 `/api/v1/todos/*/done`。
> `*` 必须至少吞掉一个字符，因此 `/api/v1/todos/`（尾斜杠）**不**匹配 `/api/v1/todos/*`，会落到 404。

### 2.2 响应信封

所有 JSON 响应（`OPTIONS` 除外）都是：

```json
{ "code": 0, "msg": "ok", "data": {} }
```

- `code` **就是 HTTP 状态码的副本**，唯一例外是成功时为 `0`；`msg` 经 `json_escape()` 转义；
  `data` 是**预序列化**的 JSON，传 `NULL` 时输出 `null`。
- 响应头固定带 `Access-Control-Allow-Origin: *` 与 `Cache-Control: no-store`（`text/html` 页用 `no-cache`）。
- 除 `OPTIONS` 外**不要**手写 `httpd_resp_send()`——绕过 `http_reply_*` 就破坏了全局契约。

| HTTP 状态码 | body `code` | 含义 | 典型场景 |
|---|---|---|---|
| 200 / 201 | 0 | 成功 | 正常返回 / 新建待办 |
| 204 | —（无 body） | 预检/无内容 | `OPTIONS` |
| 400 | 400 | 参数错误 | JSON 解析失败、缺字段、字段越界 |
| 401 | 401 | 未授权 | 需要 token 的操作未携带或 token 错误 |
| 404 | 404 | 不存在 | id 不存在、**路径未注册** |
| 405 | 405 | 方法不允许 | 路径存在但没有该方法的路由 |
| 500 | 500 | 内部错误 | 未预期失败（`data.esp_err` 带 IDF 错误名） |
| 501 | 501 | 未实现 | **路径已注册但功能未开发**（本项目唯一现例：`/ws/events`） |
| 503 | 503 | 不可用 | 网络离线、天气 API 超时、NTP 失败、SSE 客户端超限 |

错误响应同样是上述信封，`data` 为 `null` 或含 `detail`（501 还带 `hint`）。
所有时间戳字段（`due_ts`/`updated_at`/`update_ts`/`ts`/`server_ts`）都是 **Unix 秒级 `int64`**。

### 2.3 鉴权（`X-Auth-Token`）

| 项 | 实际行为（`http_router.c` + `sync_server.c`） |
|---|---|
| 传递方式 | **只有请求头 `X-Auth-Token: <token>`**；不接受 URL query 传 token（会进日志）。头读取用 `httpd_req_get_hdr_value_str()`，缓冲 80 B |
| 存储 | NVS 键 `auth_token`（明文，局域网内传输；**不提供 HTTPS**） |
| 默认状态 | 未设置 token 时 `auth_enabled=false`：局域网内大多接口可无鉴权访问（便于电脑端直接改待办） |
| 开启方式 | `POST /api/v1/device/auth` body `{"token":"..."}`；长度 >63 → `ESP_ERR_INVALID_ARG`（500）；空串/`null` 则删除键并关闭鉴权 |
| **写方法无条件鉴权** | `POST` / `PUT` / `PATCH` / `DELETE` **一律**过 hook（`is_write_method()`） |
| **强制鉴权路径** | `/api/v1/device/reboot`、`/api/v1/device/auth` 被 `http_router_require_auth()` 显式标记，**即使 `auth_enabled=false` 也必须带正确 token**，否则 401 |
| 只读接口 | `GET` 默认不鉴权（当前没有任何 `GET` 路由被标记 `require_auth`） |
| hook 缺失 | `http_router_check_auth()` **fail-open 返回 `true`**（只在没装 `sync_server` 的测试环境出现） |
| 失败响应 | HTTP **401** + `{"code":401,"msg":"unauthorized","data":null}`；同时发 `SYNC_EVENT_AUTH_FAILED`（payload = 路径）并打 `TH:sync: auth failed for <path>` |
| 副作用名单 | `sync_server.c` 的 `s_always_auth[]` 还列了 `/api/v1/ai/config`、`/api/v1/ai/history`（这两条**已注册**，强制鉴权：无 token → 401，不是 404） |

`curl.exe` 两个例子：

```powershell
# 设置 token（首次必须通过其它方式打开，或先在设备上通过设置页设置）
curl.exe -X POST http://todohelper.local/api/v1/device/auth -H "Content-Type: application/json" -H "X-Auth-Token: old-token" -d '{\"token\":\"my-secret\"}'
# 之后写操作带 token
curl.exe -X POST http://todohelper.local/api/v1/device/reboot -H "X-Auth-Token: my-secret"
```

### 2.4 404 / 405 / 501 的实际判定（`http_router.c`）

分发顺序（`router_dispatch()`）：

1. `OPTIONS` **最先**短路 → `204 No Content` + CORS 头，不进路由表、不过鉴权；
2. **第一趟**：在表里找"路径命中 **且** 方法匹配"的项，取**得分最高**者：
   精确匹配得分 `1000 + 路径长度`，通配匹配得分 `前缀长 + 后缀长`（最具体者胜）；
3. 若第一趟没找到，但**第一趟期间**有任意一条路由路径命中过（不看方法）→ **405**；
   一条都没路径命中 → **404**；
4. 命中的路由若 `hint != NULL` 或 `handler == NULL`（`http_router_register_not_implemented()`）
   → **501**（`data` = `{"detail":"not implemented","hint":"..."}`）。**501 在鉴权之前返回**，
   所以对未实现路由发无 token 的 POST 得到的是 501 而不是 401；
5. 需要鉴权 = 该路由被 `http_router_require_auth()` 标记 **或** 方法是 POST/PUT/PATCH/DELETE；
   鉴权不过 → **401**；
6. 通过后执行 handler。

其它实现细节：路由表为定长静态数组（`HTTP_ROUTER_MAX_ROUTES`=48，当前用 29），路径缓冲 64 B；
同 `path+method` 重复注册 = **替换 handler**（因此各组件 `init()` 是幂等的）；
表满返回 `ESP_ERR_NO_MEM` 并 `SYS_LOGE("route table full (48)")`；
`http_router_attach()` 重复调用遇到 `ESP_ERR_HTTPD_HANDLER_EXISTS` 返回 `ESP_OK`。

自检命令：

```powershell
curl.exe -i http://todohelper.local/api/v1/todos              # 200，code=0
curl.exe -i -X DELETE http://todohelper.local/api/v1/todos    # 405 method not allowed
curl.exe -i http://todohelper.local/api/v1/nope               # 404 not found
curl.exe -i -X OPTIONS http://todohelper.local/api/v1/todos   # 204 + CORS 头
curl.exe -i http://todohelper.local/ws/events                 # 501 + data.hint
curl.exe -i http://todohelper.local/api/v1/wifi/status        # 404（WiFi API 未实现）
```

---

## 3. NVS 键登记表

- **命名空间**：统一 **`todohelper`**（`SYS_NVS_NAMESPACE` = `CONFIG_TODOHELPER_NVS_NAMESPACE`，默认 `"todohelper"`）。
- **唯一入口**：组件**不得**调用 `nvs_open()` / `nvs_set_*()`，一律经 `sys_config_*`（需求 §7.13 / §9.1）。
  键名字符串**只**在 `components/sys_utils/include/sys_keys.h` 里定义，禁止在组件里写裸键名。
- NVS 键名长度上限 **15 字符**（`NVS_KEY_NAME_MAX_SIZE - 1`）。
- 🔒 标记的键属敏感信息，**不得**写日志、不得进备份文件、任何 HTTP 接口都不得回读。
- "读写者"列为当前代码里实际的调用方（`grep SYS_KEY_*`）。

| 宏 | 键名 | 类型 | 含义 / 默认值 | 读写者 |
|---|---|---|---|---|
| `SYS_NVS_NAMESPACE` | `todohelper` | 命名空间 | 全项目共用 | `sys_config_*` |
| `SYS_KEY_WIFI_SSID` | `wifi_ssid` | str | 上次成功配置的 SSID | `wifi_manager` |
| `SYS_KEY_WIFI_PASS` | `wifi_pass` | str | 密码 🔒（**永不写日志**） | `wifi_manager` |
| `SYS_KEY_WIFI_AUTO` | `wifi_auto` | bool | 开机自动重连；默认 `true`。⚠️ 需求 §9.1 记为 i32，实现按 `sys_config_get_bool_or()` 读写 | `wifi_manager` |
| `SYS_KEY_TZ` | `tz` | str | POSIX 时区串；缺省用 `CONFIG_TODOHELPER_TZ_DEFAULT` = `"CST-8"` | `calendar_service` |
| `SYS_KEY_CITY` | `city` | str | 手动设置的城市（P0 唯一定位路径）；缺省 `""` | `weather_service` |
| `SYS_KEY_CITY_LAT` | `city_lat` | str | 解析出的纬度（**字符串存小数**，仅诊断用） | `weather_service` |
| `SYS_KEY_CITY_LON` | `city_lon` | str | 解析出的经度（字符串存小数） | `weather_service` |
| `SYS_KEY_NTP_SERVER` | `ntp_server` | str | NTP 服务器；缺省用 `CONFIG_TODOHELPER_NTP_SERVER` = `"ntp.aliyun.com"` | `calendar_service` |
| `SYS_KEY_WEATHER_KEY` | `weather_key` | str | 天气 provider 的 API Key 🔒（默认 provider 不需要） | `weather_service` |
| `SYS_KEY_WEATHER_PROV` | `weather_prov` | str | provider 名；缺省 `WEATHER_PROVIDER_DEFAULT` = `"open-meteo"` | `weather_service` |
| `SYS_KEY_WEATHER_INT` | `weather_int` | i32 | 刷新周期秒；缺省 `CONFIG_TODOHELPER_WEATHER_INTERVAL_S` = 900（夹到 300–86400） | `weather_service` |
| `SYS_KEY_BRIGHTNESS` | `brightness` | i32 | 正常亮度百分比；缺省 100 | `bsp`、`ui_core`、`page_settings` |
| `SYS_KEY_IDLE_TIMEOUT` | `idle_timeout` | i32 | 待机超时秒；缺省 30 | `bsp`、`page_settings` |
| `SYS_KEY_IDLE_BRIGHT` | `idle_bright` | i32 | 待机亮度百分比；缺省 20 | `ui_core`、`page_settings` |
| `SYS_KEY_FIRST_BOOT` | `first_boot` | i32 | 首次启动标志（引导流程用） | ⚠️ 已登记，**当前无组件读写** |
| `SYS_KEY_LANGUAGE` | `language` | str | 界面语言 | ⚠️ 已登记，**当前无组件读写** |
| `SYS_KEY_WORD_DAILY` | `word_daily` | i32 | 每日单词数；缺省 `CONFIG_TODOHELPER_WORD_DAILY_COUNT` = 10（夹到 1–100） | `word_service` |
| `SYS_KEY_WORD_BOOK` | `word_book` | str | 当前词库名；缺省 `WORD_DEFAULT_BOOK` = `"cet4"` | `word_service` |
| `SYS_KEY_SYNC_ENABLED` | `sync_enabled` | i32 | 同步开关（0/1） | ⚠️ 已登记，**当前无组件读写** |
| `SYS_KEY_AUTH_TOKEN` | `auth_token` | str | HTTP 鉴权 token 🔒（≤63 字符；空/缺省 = 关闭鉴权） | `sync_server` |
| `SYS_KEY_HTTP_PORT` | `http_port` | i32 | HTTP 端口；缺省 `SYNC_SERVER_DEFAULT_PORT` = 80 | `sync_server` |
| `SYS_KEY_MDNS_ENABLED` | `mdns_enabled` | bool | 是否发布 `todohelper.local`；缺省 `true` | `sync_server` |
| `SYS_KEY_AI_API_KEY` | `ai_api_key` | str | DeepSeek API Key 🔒（P2） | `ai_service`（未接入） |
| `SYS_KEY_AI_MODEL` | `ai_model` | str | `deepseek-chat` / `deepseek-reasoner`（P2） | `ai_service`（未接入） |
| `SYS_KEY_AI_PROMPT` | `ai_prompt` | str | 角色设定 system prompt（P2） | `ai_service`（未接入） |
| `SYS_KEY_AI_ROUNDS` | `ai_rounds` | i32 | 保留对话轮数，默认 10（P2） | `ai_service`（未接入） |
| `SYS_KEY_AI_SAVE_HIST` | `ai_save_hist` | bool | 是否保存聊天记录（P2；Kconfig 默认 `n`） | `ai_service`（未接入） |
| `SYS_KEY_VOICE_WAKE` | `voice_wake` | i32 | 语音唤醒方式（P2） | ⚠️ 已登记，**当前无组件读写**（无 `voice_service`） |
| `SYS_KEY_TTS_ENABLED` | `tts_enabled` | i32 | TTS 播报开关（P2） | ⚠️ 已登记，**当前无组件读写** |

验证命令：

```powershell
curl.exe http://todohelper.local/api/v1/device/info     # auth_enabled / storage_root（不回读任何密钥）
idf.py -p COM34 erase-flash                             # 清空 NVS + 文件系统，回到首次上电状态
```

---

## 4. 文件与目录布局

### 4.1 运行时存储树（需求 §9.2）

根路径以 `storage_root()` 为准：SD 可用时 `/sdcard`，否则回退 `/littlefs`。
`storage_manager_init()` 会自动创建下表全部目录（`s_app_subdirs[]`）。
**上层不得硬编码 `/sdcard`**，一律用 `storage_path(buf, len, "…")` 拼。

```text
<storage_root>/todohelper/          # = storage_app_dir()
  fonts/                            # ui.ttf 等字体（ui_core 字体链第 1 顺位）
  images/weather/                   # 预留：SD 天气图标（当前图标走 assets 分区）
  anim/idle_face/                   # 预留：SD 帧动画（当前为程序化绘制，不用）
  words/                            # <book>.json 词库 + progress.json 进度
  ai/                               # 预留：chat_history.json（P2）
  tts/                              # 预留：reply_*.pcm（P2）
  cache/                            # todos.json / weather.json 等缓存
  backup/                           # 预留：todos_YYYYMMDD.json 每日备份
  logs/                             # 预留：app.log
```

### 4.2 文件归属

| 相对路径 | 组件 | 写入方式 / 说明 |
|---|---|---|
| `cache/todos.json` | `todo_manager` | 文件名可配 `CONFIG_TODOHELPER_TODO_CACHE_FILE`（默认 `todos.json`），实际路径由 `storage_write_cache()` 拼成 `cache/<名字>`；`storage_write_file()` **原子写**（tmp + rename），每两把锁（`s_lock` 保护数组、`s_io_lock` 串行化写）；缓存写失败只 `SYS_LOGW` |
| `cache/weather.json` | `weather_service` | 字段 `city/lat/lon/temp/feels_like/humidity/condition/icon/weather_code/update_ts/provider`；**绝不写入 API Key**；断网时页面靠它显示上次读数 |
| `words/progress.json` | `word_service` | 结构 `{"items":{"w<id>":{level,n,last,next,at}},"version":1,"book","daily_count","known_total","fuzzy_total","unknown_total"}`；`review_count == 0` 的词不写入；原子写 |
| `words/<book>.json` | `word_service` | 当前词库（默认 `cet4.json`）。缺失时：若 `CONFIG_TODOHELPER_WORD_EXPORT_TEMPLATE`（默认 y）把内置 32 词示例写到该路径，然后无论写成功与否都用固件内置词库兜底 |
| `fonts/ui.ttf` | `ui_core`（字体链） | 路径来自 `CONFIG_TODOHELPER_FONT_PATH`（默认 `fonts/ui.ttf`，相对 `storage_app_dir()`）。放这里即可**免重烧**换字体；整条 FreeType 通路受 `CONFIG_TODOHELPER_FONT_FREETYPE`（默认 n）门控，关闭时回落到 LVGL 内置 Source Han Sans SC CJK |
| `backup/todos_*.json` | — | 需求 §9.2 规划项。⚠️ 目录会被创建，Kconfig `CONFIG_TODOHELPER_TODO_BACKUP_KEEP`（默认 7）已声明，但**当前没有任何代码写备份** |
| `logs/app.log` | — | 需求 §9.2 规划项，**当前未实现**（日志只在串口） |
| `ai/chat_history.json` | `ai_service`（未接入） | 定义在 `ai_service.c`（`AI_HISTORY_FILE`），因组件未接入 `main`，当前不会产生 |
| `tts/*.pcm` | — | P2 规划项，当前不存在 |
| `images/weather/`、`anim/idle_face/` | — | 目录会被创建但**当前不使用**：天气图标在 `assets` 分区（`ui_assets`），待机眼睛为程序化绘制 |
| `cache/<name>`（通用） | 任意组件 | `storage_read_cache()` / `storage_write_cache()` 的固定前缀，见上两条 |

### 4.3 片内分区与素材

| 分区 | 偏移 / 大小 | 用途 | 归属 |
|---|---|---|---|
| `nvs` | `0x9000` / `0x6000` | 全部配置（§3） | `sys_utils` |
| `phy_init` | `0xf000` / `0x1000` | 射频校准 | IDF |
| `factory` | `0x10000` / `0x800000` | App（含内嵌 CJK 子集字体的位置） | — |
| `storage` | `0x810000` / `0x200000` | 片内 LittleFS（SD 不可用时的回退，subtype `littlefs`） | `storage_manager` |
| `assets` | `0xa10000` / `0x5f0000` | UI 资源 PNG（`assets/ui/`，`esp_mmap_assets`） | `ui_assets` |

**硬约束**：GPIO20~25 与 SPI NAND flash 脚位复用（需求 §4），两者互斥；
因此"SD 卡可用"与"片内 `storage`/`assets` 分区可用"两条路径都必须能独立工作，不能互相假设。

---

## 5. 已知限制与待办

### 5.1 硬件限制（不是 bug）

| 限制 | 影响 | 现状 / 规避 |
|---|---|---|
| **无 RTC** | 断电即失时，每次上电都必须重新 SNTP | `sys_time_ts()` 在同步前返回 0；`calendar_get_now()` 返回 `ESP_ERR_INVALID_STATE`；UI 显示 `UI_STATUS_TIME_UNSYNCED`（"时间未同步"）。`POST /api/v1/calendar/time` 手动设钟也**不持久化** |
| **无 GPS / 无温度传感器** | FR3 的定位与温度只能来自网络 API | 城市**手动设置**是 P0 唯一路径（`POST /api/v1/weather/city`）；IP 定位列为 P1 且未实现 |
| **无独立电池/电量计** | 状态栏不显示电量 | 需求 §10.2 已剔除该项 |
| **GPIO20~25 与 SPI NAND flash 复用** | SD 卡与片内 NAND 只能二选一 | `storage_manager` 的"SD → 内部 LittleFS"两级回退必须两条路都能独立跑 |
| **本板背光脚为 NC** | 无法用 LEDC/PWM 真调光 | `th_bsp_set_brightness()` 用 `lv_layer_top()` 黑色遮罩做软件调光；`0%` 是"最暗"而非全灭 |
| **麦克风型号 / 扬声器是否已装未确认** | 阻塞 FR7 的离线 ASR 与 TTS 播报结论 | `th_bsp_audio_init()` 惰性初始化，失败即走"无麦/无喇叭"降级路径（需求 §17.2） |

### 5.2 构建与工具链

| 限制 | 说明 |
|---|---|
| **`CONFIG_LOG_MAXIMUM_LEVEL_VERBOSE=y` 在本目标上是强制的** | `sdkconfig.defaults` 第 27–43 行：若 `CONFIG_LOG_MAXIMUM_LEVEL == CONFIG_LOG_DEFAULT_LEVEL`，IDF 会把 `ESP_LOGD/V` 的格式串放进 `.rodata_wlog_debug/.rodata_wlog_verbose`，而 `sections.ld` 在 `esp32s31` + `SPIRAM_XIP_FROM_PSRAM` 下会把这两个段排在 `_rodata_reserved_end` 之后，导致 `cannot move location counter backwards` + `collect2.exe: error: ld returned 1 exit status`。**不要"优化"掉这一项**；它只改编译期保留，不改运行期输出（`CONFIG_LOG_DEFAULT_LEVEL` 仍是 INFO） |
| 工程外 Korvo BSP 依赖补丁 | `patches/esp_lvgl_adapter_idf61_support.patch` 是编译前提；`managed_components/espressif__esp_lvgl_adapter` 被重新解压/下载后会再次出现 `fatal error: esp_async_fbcpy.h: No such file or directory`（`AGENT.md` §7） |
| `assets/ui/` **必须保持扁平** | `esp_mmap_assets` 的打包脚本用 `os.listdir()` **只扫一层**，不支持子目录，且只打包 `MMAP_FILE_SUPPORT_FORMAT` 列出的后缀（当前仅 `.png`）。因此：① 用文件名前缀分类（`w_rain.png` = 天气-雨）；② 字体（`.ttf`）**不能**放这里，要走 `EMBED_FILES` 内嵌（`ui_core`）或放 SD/内部存储。同时 **该自定义命令的 `DEPENDS` 依赖 configure 期求值的 `GLOB_RECURSE`，增删素材后必须 `idf.py reconfigure`**，否则镜像不更新（`docs/TODOHELPER_FRAMEWORK.md` §8） |
| 构建耗时 | 全量冷启动约数分钟（约 2200 个编译目标）；建议后台跑并重定向 `> build/dsh_build.log 2>&1`，或设 `$env:CCACHE_DIR` 到工程内 |

### 5.3 未实现 / 未接入的能力

| 项 | 现状 |
|---|---|
| **`/ws/events`（WebSocket）** | 本 IDF 构建**没有 WebSocket 组件**，故用 **SSE**：`GET /api/v1/events`（`text/event-stream`，最多 3 客户端，15 s keepalive）。`/ws/events` 仍**注册为 501 占位**并给出 `hint`，让客户端能区分"没做"与"URL 写错"（需求 §8.7） |
| **WiFi HTTP API（5 条）** | `GET /api/v1/wifi/status`、`POST\|GET /api/v1/wifi/scan`、`POST /api/v1/wifi/connect`、`POST /api/v1/wifi/disconnect` 在需求 §8.3 里要求，但 `components/wifi_manager/` 里**没有任何 `http_router_register()` 调用**，也没有注册 501 占位 → 客户端拿到的是 **404**。这是 §2 表格里唯一"既没做也没占位"的一组接口，**建议补 `http_router_register_not_implemented()`** |
| **FreeType CJK 字体通路** | 代码路径完整（`ui_fonts.c` 的三级回退），但默认构建**不启用**：`CONFIG_TODOHELPER_FONT_FREETYPE` 默认 `n` 且依赖 `CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE`（`sdkconfig.defaults` 里未开）。此外**子集 TTF 尚未生成**：`components/ui_core/fonts/ui_cjk.ttf` 当前**不存在**，`CMakeLists.txt` 的 `if(EXISTS ...)` 因此跳过 `EMBED_FILES`。要用需先跑 `tools/gen_cjk_font.py` 生成子集，再打开上述 Kconfig（`ui_core/README.md`）。**默认路径靠 LVGL 内置 Source Han Sans SC 16/14 CJK，串口打 `TH:ui_fonts: FreeType disabled, using built-in fonts`** |
| `ai_service`（P2，FR7） | 组件目录、头文件、`ai_service_http.c`（6 条 `/api/v1/ai/*` 路由）都在，但：① `main/CMakeLists.txt` 未 `REQUIRES ai_service`；② `main/app_main.c` 的模块表里**没有** `ai_service` 一行，`ai_service_init()` 从不被调用 → 运行时那 6 条路由是 **404**，`ai_api_key` 等 NVS 键也不会被读写。另注意其 `REQUIRES` 含 `idle_face`（L4），若接入需一并核对分层方向 |
| `voice_service`（P2，FR7） | **组件目录不存在**（`components/voice_service/` 缺失）。`SYS_KEY_VOICE_WAKE` / `SYS_KEY_TTS_ENABLED` 已登记但无人读写 |
| 待机表情联动 | `idle_face` **不订阅任何系统事件**：`HAPPY/SLEEPY/ERROR/SYNCING/THINKING/LOOK_*` 都只是"支持"，没有任何组件在 WiFi 失败、Todo 完成、同步中等场景下调用 `idle_face_express()`。当前唯一调用方是 `ai_service`（未接入），所以实际只出现 `NORMAL` / `BLINK`（`ui_core` 的待机策略会切到待机页并调暗） |
| `backup/todos_*.json` 每日备份 | 目录已建、`CONFIG_TODOHELPER_TODO_BACKUP_KEEP` 已声明，**无实现**（需求 §16/§9.2 的滚动备份策略待定） |
| `logs/app.log` | **无实现**，日志只走串口（`SYS_LOGx`） |
| IP 定位（P1） | 未实现；`city_lat` / `city_lon` 只由 geocode 结果回填（诊断用） |
| AI/语音/TTS/MQTT/OTA/多用户 | P2 及以后，均未实现 |

### 5.4 文档与代码不一致处（**请维护者核对**）

这几处是本次写文档时对照代码发现的，**不属于功能缺陷**，但会误导后续开发者：

1. **`components/ui_pages/CMakeLists.txt` 的 `SRCS` 与目录内容不匹配**：
   列出了 `src/page_placeholders.c`，但该文件**已不存在**（其 `page_placeholder_create()` 现位于 `src/page_common.c`）；
   同时**新增的 `src/page_ai_chat.c` 未列入 `SRCS`**。按现状 configure 期应会报
   "Cannot find source file: src/page_placeholders.c"。请核对并修正（本文件只做记录，未改动任何代码）。
2. **`components/ui_pages/README.md` 的页面表已过期**：它写 WiFi/待办/天气/日历/单词/AI/设置均为"占位"，
   实际 `page_wifi.c` / `page_todo.c` / `page_weather.c` / `page_calendar.c` / `page_word.c` /
   `page_settings.c` / `page_ai_chat.c` **都已实现**（各自导出 `const ui_page_t`）。
3. **`docs/TODOHELPER_FRAMEWORK.md` §6 的事件名**：写的是 `SYS_EVENT_BRIGHTNESS_CHANGED`，
   实际枚举是 `SYS_EVENT_BRIGHTNESS`，且实际还多了 `STORAGE_READY` / `CONNECTIVITY` / `STATUS_CHANGED`。
4. **`SYNC_EVENT_CLIENT_CONNECTED`** 在头文件里声明了，但代码里**没有任何投递点**；
   客户端连接数只能看 `/api/v1/device/status` 的 `sse_clients`（SSE，不是 WebSocket 连接数）。
5. **`SYS_KEY_WIFI_AUTO` 的类型**：需求 §9.1 记为 i32，实现按 **bool** 读写（`sys_config_get_bool_or`）。
6. **`docs/TODOHELPER_REQUIREMENTS.md` 里 `docs/API.md` 的"尚不存在"**（§16 末与 §6 目录注释）在本文件创建后即失效。

---

> 变更本文件时请遵守：新组件 → 加一节（含头文件、函数签名表、事件、最小测试步骤）；
> 新路由 → 更新 §2 并标注状态；新 NVS 键 → 更新 §3；新文件路径 → 更新 §4。
> 修改任何公共接口后，同步回改 `docs/TODOHELPER_REQUIREMENTS.md` §7（需求文档）与
> `docs/TODOHELPER_FRAMEWORK.md` §5（框架文档）。
