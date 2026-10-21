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
串口 **COM34**（Silicon Labs CP210x，板子 MAC `1c:29:04:xx:xx:xx`）；`COM26`(CH340) 不是本板。

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
- 未加载 profile 时报 `idf.py: command not found`；Git Bash 下直接跑 `idf.py` 会被**拒绝执行**
  （`MSys/Mingw is no longer supported`）。**Git-Bash / 自动化里请用 `bash tools/build.sh build`**
  （1.3.0 新增的包装器，会把活交给原生 powershell 并原样回传退出码；`flash` / `-v build` 同理）。
  三个会**静默产出空日志 + 退出码 0** 的坑见 `AGENT.md` §4.2.1。

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
| L3 | `word_service` | `components/word_service/` | `sys_utils` `storage_manager` `http_router` `cjson` `maimemo_client` |
| L4 | `sync_server` | `components/sync_server/` | `sys_utils` `http_router` `cjson` `wifi_manager` `storage_manager` `esp_app_format` `esp_timer` `esp_hw_support` `esp_system` `heap`（私有 `mdns`） |
| L4 | `idle_face` | `components/idle_face/` | `sys_utils` `bsp` `ui_core` `lvgl` |
| L4 | `ui_pages` | `components/ui_pages/` | `ui_core` `ui_assets` `sys_utils` `storage_manager` `wifi_manager` `todo_manager` `calendar_service` `weather_service` `word_service` `sync_server` `maimemo_client` `lvgl` |
| L5 | `ai_service` ⚠️ | `components/ai_service/` | `sys_utils` `storage_manager` `http_router` `idle_face` `cjson` `esp_http_client` `esp-tls` `mbedtls` `esp_timer` |
| L5 | `voice_service` | `components/voice_service/` | `sys_utils` `bsp` `storage_manager` `http_router` `cjson` `esp_http_client` `esp-tls` `mbedtls` |
| L5 | `maimemo_client` | `components/maimemo_client/` | `sys_utils` `storage_manager` `http_router` `cjson` `esp_http_client` `esp-tls` `mbedtls` |
| — | `main`（编排，非组件） | `main/` | `REQUIRES` 上述除 `ai_service` 外的全部组件 |

**规则（框架文档 §2.1）**

- 依赖只能向下，**绝不向上或横向**；L3 组件之间不得互相 `REQUIRES`，需要对方数据时订阅事件或由 `main` 注入回调。
- **HTTP 路由反向注册**：业务组件向 L1 的 `http_router` 注册路由，`sync_server`（L4）只负责
  `httpd_start()` 后调用一次 `http_router_attach()`。因此 `todo_manager`(L3) 不必依赖 `sync_server`(L4)。
- **跨层通知**走 L0 `sys_notify()`，而不是调用 L4 的 `sync_server_broadcast_event()`。
- `ui_pages` / `idle_face` 不被任何组件依赖，只有 `main` 依赖它们。
- ⚠️ `ai_service`(L5) 的 `REQUIRES` 里含 `idle_face`(L4)（**由下向上的依赖**，与上面第一条规则相悖）。
  它**已经接入** `main/app_main.c` 的模块表与 `main/CMakeLists.txt`（1.0.0 那句「未接入」是错的）——详见 §5。
- ⚠️ **`word_service`(L3) 的 `REQUIRES` 里含 `maimemo_client`(L5)** —— 这是**由下向上的依赖**，
  与上面第一条规则相悖。1.3.0 的实现选择是"`word_service` 直接调用 `maimemo_fetch_today()`"
  （`word_cloud_refresh()`），而不是让 `maimemo_client` 订阅事件后回注，理由是云端队列要在
  `word_service_init()` 之前就绪、且 `maimemo_client` 因此被排到模块表里 `word_service`
  **之前**。**这是已知的架构债**：若以后 `maimemo_client` 再依赖别的 L4 组件，
  就会形成真正的循环。要走"干净"路线需把云端队列的上游改成事件 + `main` 注入回调
  （真要做，改动面在 `word_service.c` 的 `word_cloud_refresh()` 与 `app_main.c` 的模块顺序）。
  `ui_pages`(L4) 也 `REQUIRES maimemo_client`(L5)，同样属向下例外。

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
| `sys_utils.h` | `void sys_json_use_psram(void);` | 把 cJSON 的分配钩子装成「PSRAM 优先」（开机必须调一次，见 AGENT.md §9.0.5） |
| `sys_utils.h` | `cJSON *sys_json_parse(const char *json, int max_depth);` | **v1.5.0 新增**。**解析外部输入一律用它，不要用 `cJSON_Parse()`**：cJSON 自带 1000 层上限，而每层递归约 96 字节栈帧（objdump 实测），开机主任务栈只有 12 KB → 约 128 层就爆栈，一个坏文件就能让设备每次开机都崩。本函数在**解析前**先扫文本统计嵌套深度（跳过字符串字面量里的括号），超限返回 `NULL`；调用方负责 `cJSON_Delete()` |

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
| 缓存 | `esp_err_t storage_write_cache_pinned(const char *name, const void *buf, size_t len);` | **v1.5.0 新增**。写**片内 LittleFS** 的 `cache/<name>`，**不随存储根切换**（插拔 SD 卡不影响待办） |
| 缓存 | `esp_err_t storage_read_cache_pinned(const char *name, char **out, size_t *len);` | **v1.5.0 新增**。读片内 LittleFS 的同名文件。**不回退到存储根**：两份都在时哪份更新由 `todo_manager` 按清单自己的 `updated_at`/`next_id` 判断（文件时间戳在 LittleFS 上不可靠，实测判错过） |

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
| 控件 | `lv_obj_t *ui_dialog(const char *title, const char *text, const char *ok_text, lv_event_cb_t on_close, void *user_data);` | 模态框；`on_close` 可 `NULL`。⚠️ **v1.5.0 起它的 `on_close` 不可靠**：关闭回调是同步 `lv_obj_delete(mbox)`，LVGL 会因此中止同一次点击里的后续回调。**破坏性操作用 `ui_dialog_confirm()`** |
| 控件 | `lv_obj_t *ui_dialog_confirm(const char *title, const char *text, const char *ok_text, const char *cancel_text, lv_event_cb_t on_ok, void *user_data);` | **v1.5.0 新增**。带「取消 + 确认」两个按钮的确认框（确认键用危险色）；`on_ok` 收到的 `user_data` 是**弹窗对象本身**（回调里可以 `lv_msgbox_close(mbox)`） |
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
I (xxx) TH:wifi: ready (hostname 'todohelper', mac 1c:29:04:xx:xx:xx)
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
| JSON | `esp_err_t todo_import_json(const char *json);` | 忽略未知字段；`id==0` 或与已有 id 重复 → 分配下一个空闲 id（v1.5.0） |
| 落盘 | `void todo_mark_dirty(void);` | **v1.5.0 新增**。标记清单已变更，交给后台去抖落盘（安静期 400 ms）。**LVGL 回调里必须用它**，不要直接写盘 |
| 落盘 | `esp_err_t todo_save(void);` | 立即同步落盘，绕过去抖。导入后、网页端改完、关机前用 |
| 视图 | `todo_due_bucket_t todo_due_bucket(int64_t due_ts);` | **v1.5.0 新增**。按**本地时区**给截止时间分档：`NONE`/`OVERDUE`/`TODAY`/`TOMORROW`/`LATER`；**未校时时除 `NONE` 外一律返回 `NONE`**（不编造日期） |
| 视图 | `esp_err_t todo_list_filtered(todo_filter_t filter, todo_item_t **items, size_t *count);` | **v1.5.0 新增**。按 `ALL`/`OPEN`/`DUE_TODAY`/`DONE` 取快照，排序与 `todo_list()` 完全一致；数组用 `todo_free_list()` 释放 |
| 视图 | `uint32_t todo_version(void);` | **v1.5.0 新增**。清单版本号，任何内容或结构变化都自增；页面拿它做脏检查 |
| 视图 | `size_t todo_count_due_today(void);` | **v1.5.0 新增**。今天到期且未完成的条数；未校时时返回 0 |
| HTTP | `esp_err_t todo_manager_register_http(void);` | 向 L1 `http_router` 注册 §2 的 Todo 路由（v1.5.0 起 10 条） |

类型与常量：`todo_item_t {uint32_t id; char title[128]; char desc[256]; int64_t due_ts; bool done; uint8_t priority; int64_t updated_at; todo_sync_state_t sync_state;}`、
`todo_stats_t {size_t total; size_t done; size_t pending;}`、
`todo_due_bucket_t {NONE, OVERDUE, TODAY, TOMORROW, LATER}`、`todo_filter_t {ALL, OPEN, DUE_TODAY, DONE}`（v1.5.0）、
`TODO_TITLE_MAX`=128、`TODO_DESC_MAX`=256、`TODO_MAX_ITEMS`=256。

> **落盘语义（v1.5.0 起）**：普通增删改只改内存并置脏，由后台线程去抖写盘（400 ms 安静期），
> 所以**设备端点一下复选框不会同步写卡**；一次变更只写一次盘。
> 需要立刻落盘的地方（导入后、网页端改完）调 `todo_save()`。
> `todo_save()` 等写锁的上限是 3 s，超时返回 `ESP_ERR_TIMEOUT`（不再可能无限期卡住调用方）。

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
| 词库 | `size_t word_count(void);` | **1.4.0** 当前词库载入的词数 |
| 云端 | `bool word_cloud_mode(void);` | 需 `mm_mode=cloud` **且**有令牌 |
| 云端 | `esp_err_t word_cloud_refresh(void);` | **阻塞**。拉今日词表 + **给每词填满内容**（本地书优先，缺的查云端）。见 §2.5 与 `components/word_service/README.md` |
| 云端 | `size_t word_cloud_count(void);` | 当前云端队列词数 |
| 云端 | `esp_err_t word_cloud_export(bool force);` | **1.4.0** 把队列写成 `words/maimemo.json` |
| 记录 | `esp_err_t word_history_append(uint32_t id, const char *spelling, word_review_result_t result, bool cloud);` | **1.4.0** 追加一条学习记录（JSONL 追加写） |
| 记录 | `esp_err_t word_history_read(word_review_log_entry_t *out, size_t max, size_t *count, bool today_only);` | **1.4.0** 读回最近记录（追加顺序=旧→新） |
| 记录 | `esp_err_t word_history_clear(void);` | **1.4.0** 删除记录文件 |
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
> （`/api/v1/ai/chat` 与 `/api/v1/ai/chat/stream` **v1.5.0 起也改成了异步 handler**，
> 不再是同类隐患；参考实现：`ai_service_http.c` 的 `ai_http_dispatch()` +
> `th_ai_http` 任务。**注意**：异步 handler 结束时必须
> `httpd_req_async_handler_complete()`，漏了就永久占住一个会话槽，整机不再 accept。）

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

### maimemo_client（L5，1.3.0 新增）

- **组件目录**：`components/maimemo_client/`
- **头文件**：`components/maimemo_client/include/maimemo_client.h`
- **依赖**：`sys_utils`、`storage_manager`、`http_router`、`cjson`、`esp_http_client`、`esp-tls`、`mbedtls`
- **用途**：墨墨背单词（MaiMemo）开放 API 客户端。三件事（用户选定的三个方案）：
  **A 学习同步**（今日队列与账号真实进度）、**B 助记/例句预取**（缓存到 SD 卡，离线可看）、
  **C 反向推送**（把「不认识」的词加回学习计划）。
- **可选组件**：默认**不配令牌就静默降级** —— 云端模式关闭，`word_service` 继续用本地词库，
  UI 与 HTTP 都不报错。这也是它只做 `SYS_LOGW` 不做 `ESP_LOGE` 的原因。
- **不变量**：`maimemo_client` 只与墨墨通信；本地词库/进度仍归 `word_service`
  （云端词由 `word_service` 用 `voc_spelling` 匹配回本地条目，见 §2.1 与 `word_service` 一节）。

外部端点基址 `https://open.maimemo.com/open/api/v1`（`MM_BASE`）；响应上限 **24 KB**
（`MM_MAX_RESPONSE`，64 词的今日队列约 8 KB）；单次请求超时 **15 s**（`MM_TIMEOUT_MS`）。

#### 公开函数

| 分组 | 签名 | 说明 |
|---|---|---|
| 生命周期 | `esp_err_t maimemo_client_init(void);` | 初始化配置/缓存/同步任务；内部会调 `maimemo_register_http()` |
| 令牌 | `bool maimemo_has_token(void);` | **只读 NVS，不碰网络，可从 UI 任务调用** |
| 令牌 | `esp_err_t maimemo_set_token(const char *token);` | 写 NVS `mm_token`（`NULL`/`""` 清除）；**永不写日志** |
| 令牌 | `esp_err_t maimemo_verify_token(void);` | 用一次 `/study/get_study_progress` 做连通性校验；会阻塞 |
| 方案 A | `esp_err_t maimemo_fetch_today(maimemo_item_t *out, size_t max, size_t *count);` | 今日词表；阻塞 |
| 方案 A | `esp_err_t maimemo_fetch_progress(maimemo_progress_t *out);` | 账号进度（finished/total/study_time_ms/due_count）；阻塞 |
| 方案 A | `esp_err_t maimemo_sync_start(void);` | **异步**刷新今日队列（worker 任务 + 事件）；**唯一对 UI 安全的入口** |
| 方案 A | `esp_err_t maimemo_get_cached_progress(maimemo_progress_t *out);` | 上次同步结果，**不碰网络** |
| 方案 B | `esp_err_t maimemo_enrich(const char *spelling, maimemo_enrich_t *out);` | 助记/例句/**释义**；命中缓存则不联网；**只有三者的作用域都不足**才返回 `ESP_ERR_NOT_SUPPORTED` 并按词缓存 |
| 方案 B | `esp_err_t maimemo_fetch_meaning(const char *spelling, char *out, size_t len);` | **1.4.0** 单独取释义（`GET /interpretations`）。`/vocabulary` **只返回 `{id,spelling}`**，所以这是云端词唯一的释义来源 |
| 方案 C | `esp_err_t maimemo_push_words(const char **spellings, size_t count, bool advance, int *accepted);` | 批量加词（≤1000/次）；`accepted` = **服务器自报**条数 |
| 方案 C | `esp_err_t maimemo_push_unknown(const char *spelling);` | 单张卡评分路径用；未开启方案 C 时**不碰网络**直接返回 `ESP_ERR_NOT_SUPPORTED` |
| 方案 C | `bool maimemo_push_enabled(void);` | 方案 C 是否开启 |
| 方案 C | `esp_err_t maimemo_set_push_enabled(bool enabled);` | 持久化到 NVS `mm_push` |
| 方案 D | `esp_err_t maimemo_advance_spellings(const char **spellings, size_t count, int *advanced);` | **1.4.0** 把学过的词（按拼写）排回复习计划（`POST /study/advance_study`）；**阻塞**，调用方应批量 |
| 方案 D | `esp_err_t maimemo_advance_words(const char **voc_ids, size_t count, int *advanced);` | **1.4.0** 同上但直接给 voc_id；`advanced` = 服务器自报的 `advanced_count` |
| 方案 D | `bool maimemo_advance_enabled(void);` | **1.4.0** 复用 `mm_push`：加词与推进都改账号数据，一个开关管两者 |
| 方案 D | `esp_err_t maimemo_probe(maimemo_probe_result_t *out, size_t max, size_t *count);` | **1.4.0** 逐条探测 6 个端点作用域；**一个失败不会中断其余**（作用域互相独立）|
| HTTP | `esp_err_t maimemo_register_http(void);` | 注册 **8** 条 `/api/v1/maimemo/*` 路由；由 `init()` 自动调用 |

类型与常量：`maimemo_item_t {char voc_id[80]; char spelling[64]; bool is_new; bool is_finished;
char first_response[24];}`（`first_response` = `"FORGET"` / `"FAMILIAR"` / `"UNSPECIFIED"`）、
`maimemo_progress_t {int finished; int total; int study_time_ms; int due_count;}`、
`maimemo_enrich_t {char mnemonic[256]; char example[256]; char meaning[192]; bool from_cache;}`、
`maimemo_probe_result_t {const char *path; esp_err_t err; int detail;}`、`MAIMEMO_PROBE_MAX`=6、
`MAIMEMO_SPELLING_MAX`=64、`MAIMEMO_VOC_ID_MAX`=80、`MAIMEMO_MAX_TODAY`=64、
`MM_MAX_RESPONSE`=24 KB、`MM_MAX_CACHE_FILE`=256 KB、`MM_TOKEN_MAX`=192。

#### ⚠️ 线程规则（**最重要的一条**）

**本组件所有会做 TLS 的函数都是阻塞的，绝不允许从 LVGL 任务调用。**
LVGL 的控件事件回调与 `lv_timer` 回调都在**适配器锁内**运行（`AGENT.md` §9.0.1.1），
在回调里调 `maimemo_fetch_today()` 就是**持锁等一次 HTTPS 往返 → 整个 UI 冻结**。

对 UI 安全的只有三个：`maimemo_sync_start()`（起 worker、立刻返回、结果走事件）、
`maimemo_has_token()` 与 `maimemo_get_cached_progress()`（只读 NVS/内存）。
`page_word.c` 的助记预取用 `lv_async_call` 起一次性任务，结果再回到 LVGL 任务里写标签。

#### 令牌与作用域模型

- 令牌存 NVS **`mm_token`**，经 `sys_config_set_str()` 写入，由网页控制台录入。
  **与 AI Key 同规则（需求 §8.8）：不硬编码、不写日志、任何接口都不回读。**
  `GET /api/v1/maimemo/status` 只回 `configured` 与掩码提示（`token_hint`，形如 `g_zV****`）。
- **令牌作用域决定哪些方案能用，且这是正常情况不是错误。** 常见的学习作用域令牌：
  `study` 与 `/vocabulary` 可用，`/notes`、`/phrases` 返回 `common_permission_denied`。
  **1.4.0 起必须知道 `/interpretations`（释义）是独立于这两者的作用域** ——
  它常常**可用**，而释义正是"本地词库没有的词"唯一的内容来源。所以不要再从
  "助记读不到"推断"释义也读不到"；用 `POST /api/v1/maimemo/probe` 实测。
  处理方式：映射为 **`ESP_ERR_NOT_SUPPORTED`**（语义 "这个令牌不许做这件事，别再问了"）
  并**按端点、按词缓存**（`unavailable` / `meaning_unavailable`），所以每个词只会被问一次。
  理由：一个"能同步队列但不显示助记"的
  设备仍是有用的，一个"报错"的设备不是。

#### 错误模型（**HTTP 200 也可能是失败**）

墨墨的接口**即使权限错误也返回 HTTP 200**，错误写在信封里：

```json
{ "success": false, "data": null,
  "errors": [ { "code": "common_permission_denied", "msg": "..." } ] }
```

所以**只看状态码会漏掉全部权限/参数错误**。`mm_classify()` 统一检查 `success` 与
`errors[].code`：含 `permission_denied` → `ESP_ERR_NOT_SUPPORTED`，其余 → 其他错误。
HTTP 层的映射单独一层：`401`/`403` → `ESP_ERR_INVALID_STATE`（令牌过期/不被允许）、
非 200 → `ESP_ERR_INVALID_RESPONSE`、响应超限 → `ESP_ERR_INVALID_SIZE`。
无令牌时 `mm_token()` 失败 → `ESP_ERR_NOT_FOUND`（调用方据此**安静降级**）。

**两个已实测的接口坑**（都写进了代码注释，改这里前先读）：

1. **`advance` 是 `POST /study/add_words` 的必需属性**，不是可选：
   漏掉它**即使 `words` 数组完全合法**也会失败 ——
   `common_invalid_param: {base} must have required property 'advance'`。
   因此**永远发送**（`false` 也发）；且回报**服务器自己的 `added_count`**而不是发送条数
   （服务器会拒掉已在计划里的词）。
   `add_words` 的请求体是 `{"words":[{"id":<voc_id>},...],"advance":bool}` ——
   **按 id 加词，不是按拼写**，所以设备侧先用 `GET /vocabulary?spelling=` 把拼写解析成
   `voc_id`（`mm_resolve_voc_id()`），解析不到的词跳过并 `SYS_LOGD`。
2. **助记/例句的读取端点是 `GET`，不是 `POST`** ——
   `GET /api/v1/notes?voc_id=...` 与 `GET /api/v1/phrases?voc_id=...`。
   `POST /api/v1/notes` **也存在，但它是"创建笔记"**（body `{"note":{voc_id,note_type,note}}`），
   拿 POST 去读会得到完全不同的东西。已对照官方 CLI 源码
   （`@maimemo/memo-api` v1.2.0：`api("/api/v1/notes", {method:"GET", query:{voc_id}})`，
   响应按 `res.notes.map(...)` 取）核实。
   解析出的结构是 `{"data":{"notes":[{"note":"..."}]}}` 与
   `{"data":{"phrases":[{"phrase":"..."}]}}`（`mm_fetch_field()` 取数组第 0 项的文本字段）。

#### 事件

| 基址 `MAIMEMO_EVENT` | payload |
|---|---|
| `MAIMEMO_EVENT_SYNC_STARTED`(0) | 无 |
| `MAIMEMO_EVENT_SYNC_DONE` | `maimemo_progress_t` |
| `MAIMEMO_EVENT_SYNC_FAILED` | `esp_err_t` |
| `MAIMEMO_EVENT_PUSH_DONE` | `int`（服务器接受的词数） |

#### 最小测试步骤

```powershell
# 1) 存入令牌并当场校验（verify 会真的访问墨墨，5~15 s）
curl.exe -X POST http://todohelper.local/api/v1/maimemo/config -H "Content-Type: application/json" -d "{\"token\":\"g_zV...\",\"verify\":true}"
# 2) 状态（不含明文令牌）
curl.exe http://todohelper.local/api/v1/maimemo/status
# 3) 同步今日队列，再回读
curl.exe -X POST http://todohelper.local/api/v1/maimemo/sync
curl.exe http://todohelper.local/api/v1/maimemo/today
# 4) 助记/例句（学习作用域令牌会得到 unavailable:true）
curl.exe "http://todohelper.local/api/v1/maimemo/enrich?spelling=abrupt"
```

期望串口日志：

```
I (xxx) TH:maimemo: ready (token=set, push=off)
I (xxx) TH:maimemo: token OK: 10/20 finished today
I (xxx) TH:word: cloud queue: 20 words (12 also in the local book)
I (xxx) TH:maimemo: pushed to the study plan: 1 accepted of 1 resolved (advance=0)
```

- **无令牌时 `ready (token=unset, push=off)` 也算成功**（`optional=true`，
  不得因为没有令牌而中断启动）。
- **验证状态（诚实说明）**：以上端点**已用真实令牌对线上 API 实测**（`get_study_progress`
  返回 10/20、`get_today_items` 返回 20 词、`add_words` 成功并回报 `added_count`；
  `/notes`、`/phrases` 确认返回 `common_permission_denied` 并被正确映射）。
  **但这些都没有在真机上跑过** —— 1.3.0 只构建、未烧录。
  **方案 B 的"快乐路径"（真的在卡片上显示出一条助记）尚未被观察到**，
  因为当前令牌读不了 `/notes`。

---

### ai_service（L5，DeepSeek 对话；v1.9.0 补文档）

- **目录 / 入口**：`components/ai_service/src/ai_service.c`（`ai_service_init()`）、
  `ai_service_http.c`（`ai_service_register_http()`）
- **对话接口**：`POST https://api.deepseek.com/chat/completions`（OpenAI 兼容格式），
  鉴权用 `Authorization: Bearer <key>`，TLS 走系统 CA bundle
- **模型白名单**：`deepseek-chat`（默认）/ `deepseek-reasoner`

**路由（7 条）**

| 方法 | 路径 | 请求 / 响应 |
|---|---|---|
| POST | `/api/v1/ai/chat` | `{"text":"问题"}` → `{"reply":"回答"}`。**v1.5.0 起异步**：handler 立刻把会话交给自有任务，AI 跑着的时候设备其它接口照常可用 |
| POST | `/api/v1/ai/chat/stream` | `{"text":"问题"}` → `text/event-stream`，事件：`chunk`（`data: {"delta":"..."}`）→ `done` / `error` |
| GET | `/api/v1/ai/config` | **v1.9.0 新增**（读状态）：`{"model","has_api_key","history_rounds","state"}` |
| POST | `/api/v1/ai/config` | 字段：`api_key`（空串 = 清除）、`system_prompt`、`model`、`history_rounds`、`save_history` → 同上的状态 JSON。**响应绝不回显 Key** |
| GET | `/api/v1/ai/history` | 对话历史。**强制鉴权** |
| DELETE | `/api/v1/ai/history` | 清空历史。**强制鉴权** |
| POST | `/api/v1/ai/abort` | 中断正在进行的回答 |

```bash
# 上位机配 Key（网页端「AI 对话与语音」分区也是这个接口）
curl -X POST http://todohelper.local/api/v1/ai/config \
     -H "Content-Type: application/json" -d '{"api_key":"sk-..."}'
# 问一句
curl -X POST http://todohelper.local/api/v1/ai/chat \
     -H "Content-Type: application/json" -d '{"text":"用一句话打个招呼"}'
```

**令牌模型**：Key 存在 NVS `ai_api_key`，**只写不读** —— `ai_service_has_api_key()`、
`POST /ai/config` 的响应、调试 shell 的 `cfg get ai_api_key` 三处都只回布尔。
⚠️ NVS 没开加密，**Key 在 flash 上是明文**（`AGENT.md` §9.0.1.4 第 14 条）。

**错误模型**：无 Key → 400 `no API key configured`；离线 → 503；Key 被拒（401/403）→
503；超时 → 503。**没有重试** —— 一次网络抖动就是一次失败。

**⚠️ 两个容易踩的点**：
- `ai_service_set_system_prompt()` **会清空对话历史**（换角色设定 = 重新开始）。
- 多轮历史是**内存态**（上限 24 条、按轮成对裁剪）；要落盘得打开 NVS `ai_save_hist`。

### voice_service（L5，录音/回放与云端语音；v1.9.0 补文档）

- **目录 / 入口**：`components/voice_service/src/voice_service.c`（`voice_service_init()`）
- **硬件**：I2S-STD + ES8389 codec（`esp_codec_dev`），模拟麦走 codec 的 ADC，
  功放由 `pa_pin = GPIO7` 控制。**音频是惰性起起来的**（省内部 RAM）

**路由（2 条）**

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/voice/status` | `{"microphone","speaker","recording","asr_configured","tts_configured","wake_mode","last_ms"}`。**v1.9.0 起会真的把音频外设起起来再回答**（否则「没起」看起来就像「没麦克风」） |
| POST | `/api/v1/voice/config` | 字段：`asr_url`、`tts_url`、`wake_mode`（`button` / `none`）→ 返回状态 JSON |

**协议（两个端点都是自己定的，设备是客户端）**

| | 请求 | 期望响应 |
|---|---|---|
| ASR（识别） | `POST` 端点，body 是**原始 PCM**（16 kHz / 16 bit / 单声道） | `{"text":"识别结果"}` |
| TTS（合成） | `POST` 端点，body 是 `{"text":"要念的内容"}` | `audio/pcm`（16 kHz / 16 bit / 单声道） |

**录音**：48 kHz / 16 bit / 2ch 采集 → 软件下混 + 3 点抽取 → 16 kHz / 16 bit / 单声道，
缓冲在 PSRAM（10 s = 320 KB），落盘 `ai/rec_<uptime_ms>.pcm`（无 WAV 头）。
**回放**：16 kHz 单声道 → 最近邻升采样 → 48 kHz 立体声 → codec。

**硬件现状（v1.9.3 真机实测）**：codec 能起来（`mic=1 speaker=1`），
**麦克风也真的能录到东西**（1.5 秒录到 1466 ms、峰值 3124/32767）。
（v1.9.0 那句「采集通路一个样本都收不到」是本工程的 bug ——
`esp_codec_dev_read()` 返回错误码而不是字节数，见 `docs/ROADMAP_v1.9.0.md` §2。）
调试 shell 的 `audio [ms]` 命令可以随时复测。

---

### main（编排层，非组件）

- **目录 / 入口**：`main/app_main.c`（`void app_main(void)`）；没有公开头文件
- **依赖**：`main/CMakeLists.txt` 的 `REQUIRES` = `sys_utils` `storage_manager` `bsp` `ui_assets`
  `http_router` `ui_core` `ui_pages` `wifi_manager` `todo_manager` `calendar_service`
  `weather_service` `maimemo_client` `word_service` `sync_server` `idle_face`

启动顺序（`app_main()` 实际代码）：

1. `SYS_LOGI("ToDoHelper starting")`；`ESP_ERROR_CHECK(sys_config_init(NULL))`、`ESP_ERROR_CHECK(sys_events_init())`；
2. `log_reset_reason()`（打印复位原因）；
3. `ESP_ERROR_CHECK(th_bsp_init())`（显示必须最先）；
4. `ESP_ERROR_CHECK(storage_manager_init())`；
5. `ui_assets_init()`（失败只告警："pages will use text fallbacks"）；
6. `ESP_ERROR_CHECK(ui_core_init())`、`ESP_ERROR_CHECK(ui_pages_init())`，切到 `UI_PAGE_SPLASH`；
7. 跑 `after_ui=false` 的模块（当前表里没有），延时 400 ms 后切 `UI_PAGE_HOME`；
8. 跑 `after_ui=true` 的模块表（顺序）：`wifi_manager` → `todo_manager` → `weather_service` →
   `calendar_service` → `maimemo_client` → `word_service` → `sync_server` → `idle_face` →
   `ai_service` → `voice_service`（**全部 `optional=true`**）；
9. `ui_statusbar_refresh()`，发 `SYS_EVENT_READY`，打印 `ready (storage=<root>)`；
   若处于云端模式（`word_cloud_mode()`）则顺手发起一次 `maimemo_sync_start()`。

> **`maimemo_client` 必须排在 `word_service` 之前**（1.3.0）：`word_service` 的云端队列
> （`word_cloud_refresh()`）初始化时就要读令牌与同步结果。模块表当前共 **10** 项。

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
- 端到端冒烟：`curl.exe http://todohelper.local/api/v1/device/status` 的 `routes`(51) / `time_synced` / `free_heap`。

---

## 2. HTTP API

基础地址：`http://todohelper.local`（mDNS，主机名 `todohelper`）或设备 IP；
把示例里的 `todohelper.local` 换成 IP 也可以（IP 见 `GET /api/v1/device/info` 的 `ip` 字段）。
所有 JSON 响应统一信封，`OPTIONS` 统一 204 + CORS。

### 2.1 端点总表

下表是**实际注册**的全部路由（来源：`components/*/src/*.c` 中的
`http_router_register()` / `http_router_register_not_implemented()` 路由表）。
**v1.9.0 真机复核**：`GET /api/v1/device/status` 返回 `"routes":62`。
按注册项（`path + method`）拆开正好是 62：
`sync_server` 8（含 `/ws/events` 的 501 占位）+ `todo_manager` 10 + `weather_service` 5 +
`calendar_service` 5 + `word_service` 8 + `wifi_manager` 7 + `ai_service` 7 +
`voice_service` 2 + `maimemo_client` 8 + `debug_shell` 2 = 62。
（**注意**：`debug_shell` 的 2 条**在**这 62 条里 —— 本工程
`CONFIG_TODOHELPER_DEBUG_SHELL=y`，调试 shell 是开着的。1.3.0 那句「51 条」
是按**唯一路径**数的，而且当时 `todo_manager` / `word_service` /
`maimemo_client` 还没长到现在这么多。）

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
| POST | `/api/v1/ota/upload` | ota_service | **请求体就是固件镜像**（`application/octet-stream`）。写进另一个槽位 → 校验 → **上膛**（下次启动就跑它）。**不自动重启**（见下面「为什么重启是单独一步」）。升级期间设备其它接口照常可用（异步 handler）；**同一时间只允许一次上传**，第二次回 503 | ✅ 200 |
| POST | `/api/v1/ota/reboot` | ota_service | **v1.12.0 新增**。重启进刚写好的固件；没有待启动的镜像时回 503 | ✅ 200 |
| GET | `/api/v1/ota/status` | ota_service | `{state:"idle"/"writing"/"verifying"/"ready"/"rebooting"/"failed", written, total, error, running, pending, boot, busy}`。**`running`** = 现在跑的是哪个槽位；**`boot`** = 下次启动会跑哪个槽位（= `running` 表示没有待启动的）；**`pending`** = 下一个会被写进去的槽位（**永远等于「另一个」**，别拿它判断「有没有升级等着启动」）；**`busy`** = 现在正忙（擦除/写入/校验/上膛期间都是 true，这时再来一次上传会回 503） | ✅ 200 |
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
| POST | `/api/v1/todos/ack` | todo_manager | **v1.5.0 新增**。body `{"ids":[1,2]}`，设备对这些 id 调 `todo_mark_synced()`；响应 `{"acked":[1,2],"count":2,"missing":[9]}`。一次最多 64 个 id | ✅ 200 |
| GET | `/api/v1/todos/export` | todo_manager | **v1.5.0 新增**。返回整份清单 JSON，带 `Content-Disposition: attachment`（浏览器直接下载）；`?envelope=1` 走项目统一信封 | ✅ 200 |
| POST | `/api/v1/todos/import` | todo_manager | **v1.5.0 新增**。body 就是 export 的那份 JSON，**整体替换**现有清单；非法 JSON / 缺 `items` → 400 且**不动现有清单**。响应 `{"imported":N,"total":M,"pending":K}` | ✅ 200 |
| GET | `/api/v1/weather` | weather_service | 当前读数；无读数时仍 **200** + `valid:false`（让页面提示选城市）。含 `stale` | ✅ 200 |
| POST | `/api/v1/weather/refresh` | weather_service | 同步刷新；未设城市 → 400；离线/超时 → 503 | ✅ 200/400/503 |
| POST | `/api/v1/weather/city` | weather_service | body `{"city":"Beijing"}`，持久化后立即刷新一次 | ✅ 200 |
| POST | `/api/v1/weather/locate` | weather_service | 纯 IP 定位；**v1.5.0 起异步**（handler 立刻返回，定位在自有任务里跑，期间设备其它 HTTP 接口照常可用）；`geo_url` 可经 NVS 换服务商；本机网络实测到 `ipapi.co` 不通 → 502（语义正确） | ✅ 200/502 |
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
| GET | `/api/v1/words/history` | word_service | **1.4.0** 设备端学习记录；`?today=1&limit=N`（≤200，默认 50）。**无记录返回空数组而非错误** | ✅ 200 |
| POST | `/api/v1/words/history` | word_service | **1.4.0** body `{"clear":true}` 清空记录；缺该字段 → 400 | ✅ 200/400 |
| POST | `/api/v1/words/export` | word_service | **1.4.0** 把当前云端队列写成 `words/maimemo.json`；无云端队列 → 400 | ✅ 200/400 |
| GET | `/api/v1/wifi/status` | wifi_manager | `state`/`connected`/`ssid`/`ip`/`rssi`/`mac`/`hostname`。**1.0.0 实测 200** | ✅ 200 |
| GET | `/api/v1/wifi/scan` | wifi_manager | **上一次**扫描的缓存结果 `{count,items[]}`；刚开机未扫过时 `count:0`（要新结果先 POST） | ✅ 200 |
| POST | `/api/v1/wifi/scan` | wifi_manager | 触发异步扫描，立即返回 `{"scanning":true}`；结果靠随后 GET 轮询 | ✅ 200 |
| POST | `/api/v1/wifi/connect` | wifi_manager | body `{"ssid","password"}`，异步发起；凭据写 NVS | ✅ 200 |
| POST | `/api/v1/wifi/disconnect` | wifi_manager | 断开并停止自动重连 | ✅ 200 |
| POST | `/api/v1/wifi/config` | wifi_manager | body `{"ssid","password","auto_connect"}`：只存凭据不连接。**v1.10.0 追加 `ip`**：固定地址（空串 = 改回 DHCP）；网关和子网掩码用设备上次租约里的值 | ✅ 200 |
| POST | `/api/v1/wifi/forget` | wifi_manager | 清除 NVS 里的 SSID/密码（不主动断开） | ✅ 200 |
| POST | `/api/v1/ai/chat` | ai_service | **v1.5.0 起异步**：handler 立刻把会话交给自有任务，AI 请求期间**设备其它 HTTP 接口照常可用**；对外响应格式不变 | ✅ 200 |
| POST | `/api/v1/ai/chat/stream` | ai_service | **v1.5.0 起异步**（`text/event-stream`，异步 handler + 自有任务）；chunk/done/error 事件格式不变 | ✅ 200 |
| GET | `/api/v1/ai/history` | ai_service | 对话历史。**强制鉴权** | ✅ 200（需 token） |
| DELETE | `/api/v1/ai/history` | ai_service | 清空历史。**强制鉴权** | ✅ 200（需 token） |
| POST | `/api/v1/ai/config` | ai_service | 设置 API Key / 模型 / 轮数。**强制鉴权**，响应不回显 Key | ✅ 200（需 token） |
| GET | `/api/v1/ai/config` | ai_service | **v1.9.0 新增**：读当前配置状态（`model` / `has_api_key` / `history_rounds` / `save_history` / `state`），**不回显 Key**。**强制鉴权**（v1.9.7 起，与 `/ai/history` 同列） | ✅ 200（需 token） |
| POST | `/api/v1/ai/abort` | ai_service | 中断正在进行的流式回答 | ✅ 200 |
| GET | `/api/v1/voice/status` | voice_service | 板子有没有麦克风/扬声器（`microphone`/`speaker` 是**能力**，不占用内存）、音频外设是否已启动（`audio_ready`）、ASR/TTS 端点是否已配 | ✅ 200 |
| POST | `/api/v1/voice/config` | voice_service | 配置 ASR/TTS HTTP 端点（协议见 §ai_service / §voice_service） | ✅ 200 |
| GET | `/api/v1/maimemo/status` | maimemo_client | 墨墨令牌**掩码**与上次同步进度；**不碰网络**（可安全轮询）。见 §2.5 | ✅ 200 |
| POST | `/api/v1/maimemo/config` | maimemo_client | body `{"token","push","verify","clear"}`；**令牌只写不读**。见 §2.5 | ✅ 200（写方法→需 token） |
| POST | `/api/v1/maimemo/sync` | maimemo_client | 异步发起一次方案 A 同步；已有同步在跑 → 400。见 §2.5 | ✅ 200/400 |
| GET | `/api/v1/maimemo/today` | maimemo_client | **实时**拉取今日词表（阻塞一次 HTTPS 往返）。见 §2.5 | ✅ 200/400 |
| GET | `/api/v1/maimemo/enrich` | maimemo_client | `?spelling=` 取助记/例句；作用域不足 → `unavailable:true` 而非报错。见 §2.5 | ✅ 200/400 |
| POST | `/api/v1/maimemo/push` | maimemo_client | body `{"spellings":[...],"advance":bool}`，把词加回学习计划。见 §2.5 | ✅ 200/400 |
| POST | `/api/v1/maimemo/probe` | maimemo_client | **1.4.0** 逐条探测令牌能力（6 个端点），返回 `endpoints[]`/`usable`/`total`。异步，最坏 30 秒。见 §2.5 | ✅ 200/400 |
| POST | `/api/v1/maimemo/advance` | maimemo_client | **1.4.0** body `{"spellings":[...]}`（1–64），把学过的词排回复习计划。未开启回传 → 400。见 §2.5 | ✅ 200/400 |

> **路径参数写法**：路由器只支持**一个** `*`，所以单项接口注册为 `/api/v1/todos/*`、动作为 `/api/v1/todos/*/done`。
> `*` 必须至少吞掉一个字符，因此 `/api/v1/todos/`（尾斜杠）**不**匹配 `/api/v1/todos/*`，会落到 404。

### 2.1.1 OTA 的两步流程（v1.12.0 起）

```
POST /api/v1/ota/upload   （请求体 = 固件镜像）   → 200 {"ok":true,"state":"ready",...}
POST /api/v1/ota/reboot                             → 200 {"ok":true,"state":"rebooting"}
```

**为什么重启是单独一步**：上传的响应还没送到浏览器的时候重启，
芯片复位会把 WiFi 协议栈一起带走 —— 那个响应就丢了，
网页上看到的是「上传失败」，而固件其实已经写好了。
（v1.11.0 就是这样：上传成功但网页显示失败，还查了很久。）
所以上传只负责「写好并上膛」，重启由调用方显式发起。

**上膛是什么意思**：`esp_ota_set_boot_partition()` 之后，
**下次**启动就会跑新固件（`GET /api/v1/ota/status` 的 `boot` 字段会显示它）。
所以即使设备没有立刻重启，新固件也已经「上膛」了；
`POST /api/v1/ota/reboot` 只是让这件事现在发生。

**三条安全线**（真机都验过，见 `docs/ROADMAP_v1.11.0.md`）：
写进另一个槽位 / 写完校验（截断镜像当场被拒，设备不重启）/ 开机回滚
（新固件启动后调 `esp_ota_mark_app_valid_cancel_rollback()` 认下自己）。

**一次只能有一个升级在进行**：从请求被接受到重启之前，
`busy` 都是 true，这期间再来一次上传会回 **503**。
（这个护栏不是多余的：第二次上传会写**同一个**槽位 ——
还没重启，`esp_ota_get_next_update_partition()` 返回的就是另一个 ——
于是刚校验通过的镜像被擦掉，甚至让设备从一个写了一半的槽位启动。）

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
| 开启方式 | `POST /api/v1/device/auth` body `{"token":"..."}`；长度 >63 → `ESP_ERR_INVALID_ARG`（500）；空串/`null` 则删除键并关闭鉴权。**设备没设令牌时这个接口不需要旧令牌**（否则「设置第一个令牌」这条路会被自己堵死）；设备已经有令牌时，这个请求自己要带**当前**令牌 |
| **模型一：没设令牌** | `auth_enabled=false` → **所有路由都不校验**（局域网默认）。这是设备出厂与常用的状态，也让「写第一个令牌」「配 AI Key」「用调试 shell」这三条路走得通 |
| **模型二：设了令牌** | **所有路由都要带 token，包括 GET**。`POST`/`PUT`/`PATCH`/`DELETE` 由 `is_write_method()` 强制过 hook；**只读路由**由 `http_router_require_auth()` 标记后同样过 hook：<br>`GET /api/v1/todos`、`GET /api/v1/todos/*`（含 `{id}`、`export`）、`GET /api/v1/ai/history` |
| 标记时机 | 路由由各组件在 `init()` 里注册，而 `require_auth()` 只能在**注册之后**调用；因此 `sync_server` 在**服务器启动时**与 **`SYS_EVENT_READY`（所有模块初始化完毕）**各标记一次 |
| 令牌缓存 | token 在内存里缓存（`s_auth_token`），设置/清除时刷新；**不再每个请求都读一遍 NVS**（那会让 httpd 任务排在 flash 写后面）。令牌本身**不写日志**、不通过任何接口回读 |
| 失败关闭 | 缓存里的令牌放不下（只可能发生在有人绕过本组件直接改 NVS 时）→ **鉴权按开启处理**，宁可谁也进不来，也不把设备敞开 |
| hook 缺失 | `http_router_check_auth()` **fail-open 返回 `true`**（只在没装 `sync_server` 的测试环境出现） |
| 失败响应 | HTTP **401** + `{"code":401,"msg":"unauthorized","data":null}`；同时发 `SYNC_EVENT_AUTH_FAILED`（payload = 路径）并打 `TH:sync: auth failed for <path>` |
| 真机实测 | 2026-10-20 板子（`192.168.137.95`）：设令牌后 `GET /api/v1/todos` 无令牌 → **401**、错令牌 → **401**、正确令牌 → **200**；`GET /api/v1/ai/history` 无令牌 → **401**（这一条在 v1.4.0 是 200，**是本轮修掉的那个洞**） |

> **网页上位机**始终带 `X-Auth-Token`（用户在「连接设置」里填的那个），
> 所以开了鉴权后网页照常可用；SSE 订阅也带同一个头。
> 网页的「连接设置」里还有两个按钮：**「让设备用这个令牌」**（把令牌写进设备）与
> **「关闭设备鉴权」**——因为没设令牌时设备不校验，这两个操作不需要旧令牌。

`curl.exe` 两个例子：

```powershell
# 设置 token（设备还没设过令牌时不需要带旧令牌；已经有令牌时要带当前的）
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

其它实现细节：路由表为定长静态数组（`HTTP_ROUTER_MAX_ROUTES`=**64**，见
`components/http_router/include/http_router.h` —— 本文档此前写 48 与"当前用 29"**都是过期的**，
1.4.0 已核对源码更正）；
**1.4.0 后的用量是 51 个唯一路径 / 55 条 `path+method` 注册项**（表按注册项计数），
所以**还剩 9 个位置**，加路由前请重新核对；
路径缓冲 64 B；同 `path+method` 重复注册 = **替换 handler**（因此各组件 `init()` 是幂等的）；
表满返回 `ESP_ERR_NO_MEM` 并 `SYS_LOGE("route table full (64)")`；
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

### 2.5 墨墨背单词接口（`/api/v1/maimemo/*`，1.3.0 起；1.4.0 增 2 条）

**八条端点封装的是设备侧对墨墨开放 API 的代理**，不是墨墨 API 本身。
设备不把令牌交给浏览器；浏览器只能通过这些路由使用令牌，且**永远读不回令牌明文**。

> **状态码语义（重要，别按常见约定猜）**：
> **HTTP 200 只表示"设备这一层传输成功"**，它**不代表墨墨那边成功**。
> 墨墨的响应始终是信封 `{"success":bool,"data":{...},"errors":[{"code","msg"}]}`，
> 而且**权限/参数错误时墨墨仍然返回 HTTP 200**。
> 所以真正的结果要看 `success:false` + `errors[].code`；
> 其中 `common_permission_denied` 在设备侧被映射为 **`ESP_ERR_NOT_SUPPORTED`**，
> 语义是"这个令牌没这个权限，别再重试了"（与"网络抖动，稍后再试"区分开）。
> 设备**自己的**错误（无令牌、body 非法、上游网络失败）才用 4xx/5xx。

| 方法 | 路径 | 鉴权 | 请求 | 响应 |
|---|---|---|---|---|
| GET | `/api/v1/maimemo/status` | 无（只读） | — | `configured`(bool)、`token_hint`(掩码，如 `g_zV****`)、`push_enabled`(bool)、`synced`(bool)；已同步时另有嵌套 `progress`：`finished`/`total`/`remaining`/`study_time_ms`/`study_time_min`/`plan_count`。**不碰网络**（用上次同步结果），可安全轮询 |
| POST | `/api/v1/maimemo/config` | **需要 `X-Auth-Token`**（写方法） | body `{"token":"...","push":bool,"verify":bool,"clear":bool}`，全部可选 | 与 `status` 同结构；另在请求了 `verify` 时多一个 `verify` 字段（`"ok"` 或 `esp_err_to_name()` 值）。**校验失败不等于令牌坏** |
| POST | `/api/v1/maimemo/sync` | **需要 `X-Auth-Token`**（写方法） | — | `{"started":true}`；已有同步在跑 → **400** `"a sync is already running"`；无令牌 → **400**。结果靠轮询 `status` |
| GET | `/api/v1/maimemo/today` | 无（只读） | — | `items[]`（每项 `spelling`/`is_new`/`is_finished`/`first_response`）+ `count`；无令牌 → **400**。**实时拉取，阻塞一次 HTTPS 往返** |
| GET | `/api/v1/maimemo/enrich` | 无（只读） | query **`?spelling=`**（必填，缺失/空 → **400**） | `spelling`、`mnemonic`、`example`、**`meaning`**（1.4.0）、`from_cache`(bool)、`unavailable`(bool)。作用域不足 → `unavailable:true` 且**仍返回 200**（不是错误） |
| POST | `/api/v1/maimemo/push` | **需要 `X-Auth-Token`**（写方法） | body `{"spellings":["a","b"],"advance":bool}`；`spellings` 必须是数组、1–1000 条，否则 **400** | `{"accepted":N}`，N 是**服务器自报**的接受条数。无令牌 → **400** |
| POST | `/api/v1/maimemo/probe` | **需要 `X-Auth-Token`**（写方法） | —（无 body） | **1.4.0** `{"endpoints":[{"endpoint","ok","status","detail"?}...],"usable":N,"total":6}`。无令牌 → **400**。**异步 handler**（最坏 6 次串行 HTTPS，30+ 秒），绝不阻塞 httpd |
| POST | `/api/v1/maimemo/advance` | **需要 `X-Auth-Token`**（写方法） | **1.4.0** body `{"spellings":[...]}`，1–64 条 | `{"advanced":N,"count":M}`，N 是服务器自报的 `advanced_count`。未开启回传（`mm_push`）→ **400**；无令牌 → **400** |

**`/probe` 的 `status` 字段就是给人看的**，它靠 `esp_err_to_name()` 区分三种"不 ok"
（这正是以前让用户以为令牌坏了的根源）：

| `status` | 含义 | 用户该做什么 |
|---|---|---|
| `ESP_OK`（`ok:true`）| 可用，`detail` 是该端点的条数（词数 / 记录数）| — |
| `ESP_ERR_NOT_SUPPORTED` | **令牌作用域不够**（正常，尤其只有学习权限的令牌）| 想要这个功能就换权限更宽的令牌 |
| `ESP_ERR_INVALID_STATE` | **令牌被拒绝**（过期 / 无效）| 重新生成令牌 |
| `ESP_ERR_NOT_FOUND` | 端点可用，只是这个词/账号没数据 | — |

**`/probe` 的意义**：`interpretations` 那一行决定"**本地词库没有的墨墨词能否显示中文释义**"。
它是独立于 `notes`/`phrases` 的作用域，所以常见情况是 `interpretations: ok` 而
`notes`/`phrases: ESP_ERR_NOT_SUPPORTED` —— 这时云端词**仍然有释义**，只是没有助记。

**关于 `X-Auth-Token`**：这些路由**都不在** `sync_server.c` 的强制鉴权名单里
（那份名单叫 `s_protected_routes[]`，只列了 `todos` 与 `ai/history` 这几条 GET）。
因此生效的是 §2.3 的**通用规则**：**`POST` 无条件鉴权**（`is_write_method()`），
`GET` 默认不鉴权。即 `auth_enabled=true` 时，**五个 POST**
（`config`、`sync`、`push`、**`probe`**、**`advance`**）必须带 `X-Auth-Token`，
三个 GET（`status`、`today`、`enrich`）不需要。
（`verify` 会真的访问墨墨，5~15 s，所以它是**显式请求**而不是保存就校验。）

```powershell
# 写操作带 token（设备鉴权开启时）
curl.exe -X POST http://todohelper.local/api/v1/maimemo/config -H "Content-Type: application/json" -H "X-Auth-Token: my-secret" -d "{\"token\":\"g_zV...\",\"verify\":true}"
curl.exe -X POST http://todohelper.local/api/v1/maimemo/sync -H "X-Auth-Token: my-secret"
# 只读操作不需要 token
curl.exe http://todohelper.local/api/v1/maimemo/status
curl.exe "http://todohelper.local/api/v1/maimemo/enrich?spelling=abrupt"
# 1.4.0：先看令牌能做什么，再看设备端学习记录
curl.exe -X POST http://todohelper.local/api/v1/maimemo/probe -H "X-Auth-Token: my-secret"
curl.exe "http://todohelper.local/api/v1/words/history?limit=20"
```

**`enrich` 的 `unavailable` 与"没有助记"是两件事**：
`unavailable:true` = 令牌作用域不够（不重试）；`unavailable:false` + 空 `mnemonic` = 问过了、
这个词确实没有助记。UI 只在前者/后者都为空时**整块隐藏**，不显示空标签。

**1.4.0 起 `unavailable` 只管助记/例句**：释义（`meaning`）的作用域是独立的，
由 `enrich.json` 里单独的 `meaning_unavailable` 跟踪。所以**可能**看到
`unavailable:true` 但 `meaning` 有值 —— 那是正常且期望的组合（云端独占词因此有中文可看）。

#### ⛔ 墨墨开放 API 没有"提交复习评分"的接口

`/study/` 下只有 `get_study_progress`、`get_today_items`、`query_study_records`、
`add_words`、`advance_study` 五个操作。所以：

- 设备上背完的词**不会**让墨墨 App 的「今日 N/M」增加；
- "回传设备端学习情况"在平台能力内**只能**用 `advance_study`（`POST /api/v1/maimemo/advance`）
  把学过的词排回复习计划；
- 设备自己的记录在 `GET /api/v1/words/history`（见 §2.1）。

依据：[官方开放 API 功能页](http://memodocs.maimemo.com/docs/PNvOw2E1AivPtlkRlQgce1W1n2c)、
[官方 CLI `memo-api-cli`](https://github.com/maimemo/memo-api-cli) 的
`src/commands/study.ts` 与 `src/types/study_model.ts`。

**组件细节、三个方案的完整语义、线程规则与实测记录见
`components/maimemo_client/README.md` 与 `AGENT.md` §9.0.6。**

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
| `SYS_KEY_AI_API_KEY` | `ai_api_key` | str | DeepSeek API Key 🔒（P2） | `ai_service` |
| `SYS_KEY_AI_MODEL` | `ai_model` | str | `deepseek-chat` / `deepseek-reasoner`（P2） | `ai_service` |
| `SYS_KEY_AI_PROMPT` | `ai_prompt` | str | 角色设定 system prompt（P2） | `ai_service` |
| `SYS_KEY_AI_ROUNDS` | `ai_rounds` | i32 | 保留对话轮数，默认 10（P2） | `ai_service` |
| `SYS_KEY_AI_SAVE_HIST` | `ai_save_hist` | bool | 是否保存聊天记录（P2；Kconfig 默认 `n`） | `ai_service` |
| `SYS_KEY_VOICE_WAKE` | `voice_wake` | i32 | 语音唤醒方式（P2） | ⚠️ 已登记，**当前无组件读写**（无 `voice_service`） |
| `SYS_KEY_TTS_ENABLED` | `tts_enabled` | i32 | TTS 播报开关（P2） | ⚠️ 已登记，**当前无组件读写** |
| `SYS_KEY_MAIMEMO_TOKEN` | `mm_token` | str | 墨墨开放 API 令牌 🔒（≤191 字符；空/缺省 = 云端模式关闭）。**与 `ai_api_key` 同规则：不写日志、任何接口都不回读（只回掩码）** | `maimemo_client` |
| `SYS_KEY_MAIMEMO_MODE` | `mm_mode` | str | 单词队列来源：`"cloud"` / `"local"`；缺省 `"local"`。`word_cloud_mode()` 要求**它等于 `cloud` 且有令牌** | `maimemo_client`、`word_service` |
| `SYS_KEY_MAIMEMO_PUSH` | `mm_push` | str | 方案 C 反向推词开关：`"1"` / `"0"`；缺省关（`"1"` 才开启）。**会改动用户在云端的学习计划，所以刻意默认关闭** | `maimemo_client` |

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
  words/                            # <book>.json 词库 + progress.json 进度 + enrich.json 墨墨缓存
  ai/                               # 预留：chat_history.json（P2）
  tts/                              # 预留：reply_*.pcm（P2）
  cache/                            # todos.json / weather.json 等缓存
  backup/                           # 预留：todos_YYYYMMDD.json 每日备份
  logs/                             # 预留：app.log
```

> **v1.5.0 的落盘协议变了**：`storage_write_file()` 不再是「tmp + rename」，
> 而是**双槽**——写 `X.new` → 正式文件 `X` 改名成 `X.bak` → `X.new` 改名成 `X`。
> 起因是 FatFs 的 `f_rename()` 在**目标已存在时返回 `FR_EXIST` 而不是覆盖**，
> 原来的「先 `remove(X)` 再 `rename(tmp,X)`」在两步之间掉电，
> **会把上一份有效清单和新的临时文件一起丢掉**（README 里宣称的「原子写」当时并不成立）。
> 现在掉电最多损失最新一次写入，启动时 `storage_recover_tree()` 会把
> `.new` / `.bak` 收敛回正式文件（正式文件在且 `.new` 在 → 丢 `.new`；
> 正式文件不在且 `.new` 在 → 提 `.new`；都不在 → 用 `.bak` 顶上）。

### 4.2 文件归属

| 相对路径 | 组件 | 写入方式 / 说明 |
|---|---|---|
| `cache/todos.json` | `todo_manager` | 文件名可配 `CONFIG_TODOHELPER_TODO_CACHE_FILE`（默认 `todos.json`），实际路径由 `storage_write_cache_pinned()` 拼成 `cache/<名字>`（**v1.5.0 起固定存片内 LittleFS**，插拔 SD 卡不再让待办「消失」）；`storage_write_file()` 走**双槽协议**（`X.new` → `X` 改名 `X.bak` → `X.new` 改名 `X`），掉电后由启动恢复收敛；两把锁（`s_lock` 保护数组、`s_io_lock` 串行化写）；缓存写失败只 `SYS_LOGW` |
| `cache/weather.json` | `weather_service` | 字段 `city/lat/lon/temp/feels_like/humidity/condition/icon/weather_code/update_ts/provider`；**绝不写入 API Key**；断网时页面靠它显示上次读数 |
| `words/progress.json` | `word_service` | 结构 `{"items":{"w<id>":{level,n,last,next,at}},"version":1,"book","daily_count","known_total","fuzzy_total","unknown_total"}`；`review_count == 0` 的词不写入；原子写 |
| `words/<book>.json` | `word_service` | 当前词库（默认 `cet4.json`）。缺失时：若 `CONFIG_TODOHELPER_WORD_EXPORT_TEMPLATE`（默认 y）把内置 32 词示例写到该路径，然后无论写成功与否都用固件内置词库兜底 |
| `words/enrich.json` | `maimemo_client` | **1.3.0 新增，1.4.0 扩字段**。墨墨助记/例句/释义缓存（方案 B），路径常量 `MM_CACHE_REL` = `"words/enrich.json"`（**相对**应用目录，经 `storage_read_text()` / `storage_write_file()` 读写，**原子写**）。结构是 `{"entries":[{"spelling":"...","mnemonic":"...","example":"...","meaning":"...","unavailable":true?,"meaning_unavailable":true?}]}` —— 注意是**数组**（`entries`），不是按拼写做键的对象；空/false 的字段**整体不写**（省体积）。**1.4.0 把释义的"不可用"拆成独立字段**：`unavailable` 只表示 `/notes`+`/phrases` 被拒，`meaning_unavailable` 表示 `/interpretations` 被拒，因为两者作用域独立；只有两者都为 true 才整体判为不可用。**整个文件一次性读进 PSRAM（上限 `MM_MAX_CACHE_FILE` = 256 KB）、改动后整文件重写**，不做逐词小文件 —— 内部 RAM 只有约 310 KB，批量数据必须显式走 PSRAM（`AGENT.md` §9.0.5）。文件不存在 / JSON 非法时只 `SYS_LOGW` 并**从空缓存开始**，功能照常（只是每次都要联网）|
| `words/maimemo.json` | `word_service` | **1.4.0 新增**。当日墨墨词表落成的**普通词库文件**（与 `<book>.json` 同格式：`{"name","version","words":[{id,spelling,phonetic?,meaning?,example?}]}`），由 `word_cloud_export()` 在每次云端同步成功后写出，也可用 `POST /api/v1/words/export` 手动重写。`id` 用**队列序号**（云端独占词没有本地 id）。**目的**：让墨墨那天的词表在重启后仍在、断网也能背，并能在上位机/SD 卡里查看。写失败只 `SYS_LOGD`，不影响同步结果 |
| `words/review_log.jsonl` | `word_service` | **1.4.0 新增**。设备端学习记录，**JSONL（一行一条，追加写）**：`{"id":<num>,"w":"<拼写>","r":"known\|fuzzy\|unknown","t":<unix秒>,"cloud":true?}`。**为什么是 JSONL 不是 JSON 数组**：数组要整文件重写，而 `word_history_append()` 是从 LVGL 回调链（`word_review`）上来的，一次 O(n) FAT 写会卡住 UI；追加是 O(1)，且断电只损失最后一行（读取端跳过解析失败的行）。超过 `WORD_HISTORY_MAX_READ`（128 KB）时**只 WARN 不在线压缩**（同样理由）。`cloud:true` 表示这张卡来自墨墨队列。清空用 `POST /api/v1/words/history` |
| `fonts/ui.ttf` | `ui_core`（字体链） | 路径来自 `CONFIG_TODOHELPER_FONT_PATH`（默认 `fonts/ui.ttf`，相对 `storage_app_dir()`）。放这里即可**免重烧**换字体；整条 FreeType 通路受 `CONFIG_TODOHELPER_FONT_FREETYPE`（默认 n）门控，关闭时回落到 LVGL 内置 Source Han Sans SC CJK |
| `backup/todos_*.json` | — | 需求 §9.2 规划项。⚠️ 目录会被创建，Kconfig `CONFIG_TODOHELPER_TODO_BACKUP_KEEP`（默认 7）已声明，但**当前没有任何代码写备份** |
| `logs/app.log` | — | 需求 §9.2 规划项，**当前未实现**（日志只在串口） |
| `ai/chat_history.json` | `ai_service` | 定义在 `ai_service.c`（`AI_HISTORY_FILE`）。**只有把 NVS `ai_save_hist` 打开才会写**（Kconfig `TODOHELPER_AI_SAVE_HISTORY` 默认 `n`），所以默认不会产生这个文件 |
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

> **本节 1.0.0 版本的若干条目已过期**（`voice_service` 存在、WiFi API 已注册、`ai_service`
> 已接入、FreeType 与子集字体都已启用）。下表已按 1.3.0 的实际状态更正；
> 保留的"⚠️ 1.0.0 旧记录"行是为了让读者知道曾经的判断是怎么错的。

| 项 | 现状 |
|---|---|
| **`/ws/events`（WebSocket）** | 本 IDF 构建**没有 WebSocket 组件**，故用 **SSE**：`GET /api/v1/events`（`text/event-stream`，最多 3 客户端，15 s keepalive）。`/ws/events` 仍**注册为 501 占位**并给出 `hint`，让客户端能区分"没做"与"URL 写错"（需求 §8.7） |
| ~~**WiFi HTTP API（5 条）**~~ | ⚠️ **1.0.0 旧记录已作废**：`wifi_manager_http.c` **有** 7 条路由且真机实测 200（见 §2.1 与 §2.3 的更正说明）。原文"没有任何 `http_router_register()` 调用 → 404"来自未核实的子 agent 自述 |
| **FreeType CJK 字体通路** | **已启用**：`components/ui_core/fonts/ui_cjk.ttf` 已生成（含 **1.3.0 合并的 203 个 IPA 字形**，`tools/gen_cjk_font.py` 生成后会跑 `validate_font()`），运行期从 `<storage>/todohelper/fonts/ui_cjk.ttf` 加载。**字体没有烘进 app**，所以**换字体是拷文件、不是重新烧录**（`AGENT.md` §4.4 / §9.2） |
| ~~`ai_service`（P2，FR7）~~ | ⚠️ **1.0.0 旧记录已作废**：`ai_service_init()` **就在** `main/app_main.c` 的模块表里（开机日志 `module 'ai_service' ready`），`/api/v1/ai/*` 路由运行时**可达**。**v1.5.0 起 `/ai/chat` 与 `/ai/chat/stream` 已改成异步 handler**（`httpd_req_async_handler_begin()` 把会话交给自有任务），一次 AI 回复期间**设备其它 HTTP 接口照常可用** —— 1.0.0 那句「会堵住整台设备的 HTTP」早已不成立。**v1.9.0** 又补了 `GET /api/v1/ai/config`（读状态）与网页端的「AI 对话与语音」配置区 |
| **`voice_service`（P2，FR7）** | ⚠️ **1.0.0 旧记录"组件目录不存在"已作废**：组件在 `components/voice_service/`，2 条路由已注册（`GET /api/v1/voice/status`、`POST /api/v1/voice/config`）。录音回放真实可用；**ASR/TTS 端点选型未定**（走可配置 HTTP 端点），属半成品 |
| **待机表情联动** | ⚠️ **1.0.0 旧记录"不订阅任何系统事件"已改善但仍有缺口**：`ai_service` 已接入会驱动表情。1.1/1.2 又加了自发表情 / 困倦度 / 瞳孔缩放 / 慢动作三层（`idle_face`）。**但 WiFi 失败、待办完成、同步中等业务场景仍未触发 `idle_face_express()`** |
| `backup/todos_*.json` 每日备份 | 目录已建、`CONFIG_TODOHELPER_TODO_BACKUP_KEEP` 已声明，**无实现**（需求 §16/§9.2 的滚动备份策略待定） |
| `logs/app.log` | **无实现**，日志只走串口（`SYS_LOGx`）。⚠️ 开机页的滚动日志是内存环形缓冲（`sys_log.c`），**不落盘** |
| IP 定位（P1） | ⚠️ **1.0.0 旧记录"未实现"已作废**：`POST /api/v1/weather/locate` 已实现（纯 IP 定位，`geo_url` 可经 NVS 换服务商）；但**本机网络实测到 `ipapi.co` 不通** → 502（语义正确，见 §2.1） |
| **墨墨方案 B 的"快乐路径"** | **1.3.0 已知缺口**：代码路径完整、权限拒绝的处理已验证，但当前令牌是**学习作用域**（`/notes`、`/phrases` 恒返回 `common_permission_denied`），所以"卡片上真的显示出一条助记"**从未在设备上被观察到**。要验证需换一个能读 `/notes` 的令牌 |
| **墨墨云端与本地是两套记账** | **1.3.0 刻意取舍**：云端词按 `voc_spelling` 匹配回本地词库后，本地 level/复习间隔照常生效；匹配不上的词（`WORD_ID_CLOUD`）在设备上评分只留痕在账号侧，**本地不记进度**。换回本地书时两边数字不会合并 |
| **墨墨方案 C 没有反向删除** | `/study/add_words` 只负责加词，推上去的词**撤不回来**——这也是它默认关闭的原因 |
| **1.3.0 全部未上机** | 本轮**只构建、未烧录**。墨墨 6 条路由、音标修复、网页面板的实机行为均待验证（清单见 `docs/HANDOVER.md` §4 第 7 组） |
| AI/语音/TTS/MQTT/OTA/多用户 | P2 及以后，多数未实现（`ai_service`/`voice_service` 已接入，其余未做） |

### 5.4 文档与代码不一致处（**请维护者核对**）

这几处是写文档时对照代码发现的，**不属于功能缺陷**，但会误导后续开发者。
**1.3.0 复核结论**：第 1、2 条**已修复**；第 3~6 条仍待处理。

1. ~~**`components/ui_pages/CMakeLists.txt` 的 `SRCS` 与目录内容不匹配**~~ ——
   ✅ **已修复**：`page_placeholders.c` 已移出 `SRCS`，`page_ai_chat.c` 已列入；
   当前 `SRCS` 与实际文件一一对应。
2. ~~**`components/ui_pages/README.md` 的页面表已过期**~~ —— ✅ **已修复**：页面表已更新为
   实现状态（`page_wifi` / `page_todo` / `page_weather` / `page_calendar` / `page_word` /
   `page_settings` / `page_ai_chat` 均已实现并各自导出 `const ui_page_t`）。
3. **`docs/TODOHELPER_FRAMEWORK.md` §6 的事件名**：写的是 `SYS_EVENT_BRIGHTNESS_CHANGED`，
   实际枚举是 `SYS_EVENT_BRIGHTNESS`，且实际还多了 `STORAGE_READY` / `CONNECTIVITY` / `STATUS_CHANGED`。
   **仍待处理。**
4. **`SYNC_EVENT_CLIENT_CONNECTED`** 在头文件里声明了，但代码里**没有任何投递点**；
   客户端连接数只能看 `/api/v1/device/status` 的 `sse_clients`（SSE，不是 WebSocket 连接数）。
   **仍待处理。**
5. **`SYS_KEY_WIFI_AUTO` 的类型**：需求 §9.1 记为 i32，实现按 **bool** 读写（`sys_config_get_bool_or`）。
   **仍待处理**（§3 表内已标注）。
6. **`docs/TODOHELPER_REQUIREMENTS.md` 里 `docs/API.md` 的"尚不存在"**（§16 末与 §6 目录注释）
   在本文件创建后即失效。**仍待处理。**
7. **本文件 §2.1 的"45 条路由"曾被 1.0.0 记录为与 `device/status.routes` 一致，但同文件
   §2.4 又写"当前用 29"** —— 两处自相矛盾（29 是更早的中间状态）。
   1.3.0 已把两处统一为**按注册表推算的 51 条**，并注明 **51 尚未上机复核**；
   **1.4.0 更正为：51 个唯一路径 / 55 条 `path+method` 注册项，上限 64**（已核对源码）。
   下次上机时请用 `GET /api/v1/device/status` 的 `routes` 字段校准这个数字。

---

> 变更本文件时请遵守：新组件 → 加一节（含头文件、函数签名表、事件、最小测试步骤）；
> 新路由 → 更新 §2 并标注状态；新 NVS 键 → 更新 §3；新文件路径 → 更新 §4。
> 修改任何公共接口后，同步回改 `docs/TODOHELPER_REQUIREMENTS.md` §7（需求文档）与
> `docs/TODOHELPER_FRAMEWORK.md` §5（框架文档）。
>
> **1.3.0 已补**：`maimemo_client` 组件一节、§2.1 的 6 条路由、§2.5 接口详解、
> §3 的 `mm_token`/`mm_mode`/`mm_push`、§4.2 的 `words/enrich.json`，
> 并更正 §1/§2.4/§5.3/§5.4 若干 1.0.0 时代的过期记录。
