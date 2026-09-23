# ToDoHelper 框架设计 v1.0

> 配套文档：`docs/TODOHELPER_REQUIREMENTS.md`（需求，下称"需求文档"）、根目录 `AGENT.md`（环境与实测基线）。
> 本文档定义**骨架、分层、契约与扩展点**，是并行开发（多 Agent）时的接口基准。
> 优先级：代码实际行为 > `AGENT.md` > 本文档 > 需求文档。

---

## 1. 框架目标

| 目标 | 落实方式 |
|---|---|
| 低耦合 | 严格单向分层 + 只依赖公开头文件（`include/`） |
| 易扩展 | 三个"表驱动"注册点：**模块表**、**页面表**、**HTTP 路由 provider 表** |
| 可降级 | 每个可选能力（SD / WiFi / 网络 / 字体 / 音频 / API Key）都有明确回退路径 |
| 可并行开发 | 组件接口先冻结（本文档 §5），实现各自独立、可独立编译 |
| 线程安全 | LVGL 只在 UI 任务访问；组件间只经事件通信 |

**贯穿原则**

1. `main` 只做编排，不含业务逻辑；
2. UI 不发 HTTP、不读文件；只调 service API + 订阅事件；
3. service 不碰 LVGL；只发事件；
4. 驱动只由 `bsp` 门面暴露；业务层不直接调工程外 Korvo BSP；
5. 组件间异步通信统一走**一个共享事件循环**（`sys_events_get_loop()`）；
6. 配置只经 `sys_config_*`，禁止组件直接 `nvs_open`。

---

## 2. 目录与分层

```text
components/
  sys_utils/           L0  基础设施：日志 / 事件循环 / 通知 / 时间 / 状态 / NVS 配置 / 错误码
  http_router/         L1  HTTP 路由表 + 统一响应封装 + 鉴权钩子（无业务逻辑）
  storage_manager/     L1  存储根路径抽象（SD → LittleFS 回退）+ 文件与缓存 API
  bsp/                 L1  板级门面：显示启动、亮度、按键→事件、LED、音频
  ui_core/             L2  页面注册表 / 导航 / 状态栏 / 锁 / 主题 / 字体 / 提示
  ui_assets/           L2  assets 分区打包（本组件拥有）+ 图标按基名解析
  wifi_manager/        L3  FR1
  todo_manager/        L3  FR2（含 HTTP API 注册）
  weather_service/     L3  FR3
  calendar_service/    L3  FR4（NTP + 时区）
  word_service/        L3  FR5
  sync_server/         L4  HTTP 服务生命周期 + 设备 API + SSE + Web 页面（需求 §8）
  idle_face/           L4  FR6 待机眼睛（程序化绘制）
  ui_pages/            L4  全部页面（需求 §10.1）
  ai_service/          L5  FR7（P2）
  voice_service/       L5  FR7（P2）
main/
  app_main.c           模块编排器（表驱动）
docs/                  TODOHELPER_REQUIREMENTS.md / TODOHELPER_FRAMEWORK.md / API.md
assets/ui/             扁平目录，打包进 assets 分区（见 §8 的扁平约束）
tools/                 开发辅助脚本（如 gen_cjk_font.py）
```

### 2.1 允许的依赖方向

```text
L5  ai_service, voice_service
L4  sync_server, idle_face, ui_pages
L3  wifi_manager, todo_manager, weather_service, calendar_service, word_service
L2  ui_assets, ui_core
L1  http_router, storage_manager, bsp
L0  sys_utils
--------------------------------------------------> 依赖只能向下，绝不向上或横向
```

**规则**

- L3 组件之间**不得**互相 `REQUIRES`：需要对方数据时，订阅对方事件或由 `main` 注入回调；
- `ui_core` / `ui_assets` / `idle_face` **不得被任何业务组件依赖**；
- **`ui_pages` 是组合层**：它可以依赖任意非 UI 组件（`wifi_manager`、`sync_server`、
  `ai_service` …），因为没人依赖它，不会成环；
- **HTTP 路由反向注册**：业务组件向 L1 的 `http_router` 注册路由，
  `sync_server`（L4）只负责起服务并把 `http_router` 挂上去。
  这样 `todo_manager`（L3）不需要依赖 `sync_server`（L4），也没有组件被它依赖；
- **跨层通知**：L3 想通知 UI/Web 时调用 L0 的 `sys_notify()`，而不是调用 L4 的
  `sync_server_broadcast_event()`（见 §5.8）；
- 出现"必须横向依赖"时，把公共部分下沉到 L0/L1，或在 `main` 里编排。

---

## 3. 命名规范

| 项 | 约定 |
|---|---|
| 组件目录 / 组件名 | 小写下划线，如 `wifi_manager` |
| 公开头文件 | `include/<组件名>.h` |
| C 符号前缀 | `<组件名>_`，如 `wifi_manager_scan()` |
| 板级门面前缀 | **`th_bsp_`**（`include/th_bsp.h`） |
| 事件基址 | **每个组件一个** `ESP_EVENT_DECLARE_BASE`，如 `WIFI_MGR_EVENT` |
| Kconfig | `CONFIG_TODOHELPER_*` |
| NVS 命名空间 | 统一 `todohelper`，键名登记在需求文档 §9.1 |
| 日志 TAG | 组件名，统一经 `SYS_LOGI/W/E`（自动加 `TH:` 前缀） |

> **为什么 `bsp` 门面用 `th_bsp_` 前缀**：工程外 Korvo BSP 已经占用了 `bsp_display_*`、
> `bsp_sdcard_*`、`bsp_led_*` 等同名符号。门面若沿用 `bsp_` 会与之冲突，故改用 `th_bsp_`。
> 需求文档 §6 的"暴露 `bsp_display_*` 给业务层"意图不变，只是符号名加 `th_`。

---

## 4. 组件契约

### 4.1 每个组件的必需文件

```text
components/<name>/
  CMakeLists.txt
  idf_component.yml     # 仅当需要托管组件时
  Kconfig               # 仅当有可配置项时
  README.md             # 职责 / API 摘要 / 依赖 / 测试步骤
  include/<name>.h      # 唯一对外接口
  src/<name>.c          # 实现（可再拆私有 .c/.h）
```

### 4.2 生命周期约定

组件对外只提供幂等的 `xxx_init()`（可重复调用返回 `ESP_OK`）与可选的 `xxx_start()/xxx_stop()`。
`init()` 内**只做资源准备，不做阻塞网络/存储 IO**；需要后台工作的组件自建任务或用 `esp_timer`。

### 4.3 错误码

公共 API 返回 **IDF 标准 `esp_err_t`**（`ESP_OK` / `ESP_ERR_INVALID_ARG` / `ESP_ERR_NOT_FOUND` /
`ESP_ERR_TIMEOUT` / `ESP_ERR_INVALID_STATE` / `ESP_ERR_NOT_SUPPORTED` / `ESP_ERR_NO_MEM`）。
需要细分原因时用 `sys_utils` 的 `sys_err_t` **只在组件内部**传递，出组件前经 `sys_err_to_esp()` 转换。
禁止把枚举强转成 `esp_err_t`（需求文档 §7.1）。

### 4.4 HTTP 状态码约定（需求文档 §8.1）

`{ "code": <status>, "msg": "...", "data": {...} }`；**成功时 `code = 0`**，其余 `code == HTTP 状态码`。
路径已注册但功能未实现 → **501**；路径不存在 → **404**。由 `sync_server` 的公共回复函数统一产生。

---

## 5. 基础组件接口（冻结）

### 5.1 `sys_utils`（L0）

```c
#include "sys_utils.h"

/* 日志：TAG 自动加 "TH:" 前缀，级别可运行时调 */
void        sys_log_set_level(int level);        /* SYS_LOG_ERROR/WARN/INFO/DEBUG */
#define SYS_LOGE(tag, fmt, ...)  ...

/* 事件：全项目唯一事件循环 */
esp_err_t              sys_events_init(void);
esp_event_loop_handle_t sys_events_get_loop(void);
esp_err_t              sys_events_post(int32_t event_id, const void *data, size_t size);
/* 常用：向 ESP_EVENT_ANY_BASE 之外的指定 base 投递（语法糖） */
esp_err_t              sys_event_post_to(esp_event_base_t base, int32_t id, const void *data, size_t size);

/* 时间 */
int64_t sys_time_ms(void);      /* 单调，动画/超时 */
int64_t sys_time_ts(void);      /* Unix 秒；NTP 未同步返回 0 */
bool    sys_time_synced(void);
void    sys_time_mark_synced(int64_t ts);   /* 由 calendar_service 在 NTP 成功后调用 */
void    sys_time_set_ts(int64_t ts);        /* 组件用系统时钟推算的时间回填 */

/* NVS 配置 */
esp_err_t sys_config_init(const char *nvs_namespace);   /* 默认 "todohelper"，幂等 */
esp_err_t sys_config_get_str(const char *key, char *out, size_t len);
esp_err_t sys_config_set_str(const char *key, const char *val);
esp_err_t sys_config_get_i32(const char *key, int32_t *out);
esp_err_t sys_config_set_i32(const char *key, int32_t val);
esp_err_t sys_config_get_bool(const char *key, bool *out);
esp_err_t sys_config_set_bool(const char *key, bool val);
esp_err_t sys_config_erase(const char *key);
```

约定：
- 所有 get 在键不存在时返回 `ESP_ERR_NOT_FOUND`，调用方给默认值；
- 敏感键（`wifi_pass` / `ai_api_key` / `auth_token` / `weather_api_key`）**不得**进日志。

### 5.2 `storage_manager`（L1）

```c
#include "storage_manager.h"

esp_err_t    storage_manager_init(void);        /* 尝试 SD，失败回退 LittleFS */
const char  *storage_root(void);                /* "/sdcard" 或 "/littlefs"，永不为 NULL */
bool         storage_sd_available(void);
bool         storage_available(void);

/* 路径拼接：storage_path(buf, len, "words/cet4.json") -> "<root>/todohelper/words/cet4.json" */
esp_err_t    storage_path(char *buf, size_t len, const char *rel);
esp_err_t    storage_mkdir_p(const char *rel);
esp_err_t    storage_read_file(const char *rel, void *buf, size_t *len);
esp_err_t    storage_write_file(const char *rel, const void *buf, size_t len);   /* 原子写：tmp+rename */
esp_err_t    storage_delete_file(const char *rel);
esp_err_t    storage_exists(const char *rel, size_t *size);
/* 便捷：整文件读入堆（调用方 free），适合 JSON/词库 */
esp_err_t    storage_read_text(const char *rel, char **out, size_t *len);
```

- 所有 `rel` 相对于 `<root>/todohelper/`；上层**不得**硬编码 `/sdcard`；
- 写失败只 `SYS_LOGW`，不影响主流程（断网/无卡降级）。

### 5.3 `bsp` 门面（L1）

```c
#include "th_bsp.h"

esp_err_t        th_bsp_init(void);                  /* 显示 + LVGL + 触摸 + 按键 + LED，幂等 */
lv_display_t    *th_bsp_display(void);
esp_err_t        th_bsp_lock(int32_t timeout_ms);    /* 包装 esp_lv_adapter_lock */
void             th_bsp_unlock(void);

/* 亮度：本板背光脚为 NC，无法用 LEDC 调光；用顶层遮罩（overlay）做软件调光 */
esp_err_t        th_bsp_set_brightness(uint8_t percent);   /* 0..100，0=最暗（非全灭） */
uint8_t          th_bsp_get_brightness(void);

/* 按键 -> TODOHELPER 事件（见 §6），业务不直接碰 iot_button */
esp_err_t        th_bsp_buttons_start(void);

/* LED 状态灯：按系统状态着色 */
typedef enum { TH_BSP_LED_OFF, TH_BSP_LED_WIFI, TH_BSP_LED_SYNC, TH_BSP_LED_ERROR } th_bsp_led_state_t;
esp_err_t        th_bsp_led_set(th_bsp_led_state_t state);

/* 音频（P2 用；惰性初始化，失败不阻塞启动） */
esp_err_t        th_bsp_audio_init(void);
void            *th_bsp_audio_speaker(void);   /* esp_codec_dev_handle_t */
void            *th_bsp_audio_mic(void);
```

### 5.4 `ui_core`（L2）

```c
#include "ui_core.h"

/* ---- 页面 ---- */
typedef enum { UI_PAGE_SPLASH, UI_PAGE_HOME, UI_PAGE_WIFI, UI_PAGE_TODO, UI_PAGE_TODO_DETAIL,
               UI_PAGE_WEATHER, UI_PAGE_CALENDAR, UI_PAGE_WORD, UI_PAGE_AI_CHAT, UI_PAGE_SETTINGS,
               UI_PAGE_IDLE_FACE, UI_PAGE_MAX } ui_page_id_t;

typedef struct {
    ui_page_id_t id;
    const char  *name;
    esp_err_t (*create)(lv_obj_t *parent);   /* 首次进入时创建（延迟创建） */
    esp_err_t (*destroy)(void);              /* 可选，NULL = 常驻 */
    esp_err_t (*on_show)(void);
    esp_err_t (*on_hide)(void);
    esp_err_t (*on_event)(int32_t event_id, void *data);   /* 只收自己关心的事件 */
} ui_page_t;

esp_err_t ui_core_init(void);
esp_err_t ui_page_register(const ui_page_t *pages, size_t count);  /* 表驱动 */
esp_err_t ui_page_switch(ui_page_id_t id);
ui_page_id_t ui_page_current(void);
lv_obj_t *ui_page_parent(ui_page_id_t id);   /* 页面内容父容器（状态栏之下） */

/* ---- 线程安全 ---- */
bool ui_lock(int32_t timeout_ms);
void ui_unlock(void);
/* 从任意任务安全执行（内部加锁；UI 任务内请用 ui_run_sync_forced） */
esp_err_t ui_run_async(void (*fn)(void *arg), void *arg);

/* ---- 通用 UI 组件 ---- */
esp_err_t ui_statusbar_create(lv_obj_t *parent);   /* ui_core_init 内部已创建 */
esp_err_t ui_toast(const char *fmt, ...);          /* 2 秒自动消失 */
lv_obj_t *ui_card_create(lv_obj_t *parent, const char *title);
const lv_font_t *ui_font(ui_font_role_t role);     /* 见 §5.5 */
esp_err_t ui_nav_push(ui_page_id_t id);            /* 记录返回栈 */
esp_err_t ui_nav_back(void);
```

页面实现者只提供 `ui_page_t` 并把它登记进 `ui_pages` 的表；**不得**自建 LVGL screen、
不得调用 `lv_disp_load_scr()`。`ui_core` 在单个 `lv_obj_t` 屏幕下为每个页面建一个全屏容器，
切换即 `lv_obj_add_flag(HIDDEN)` 切换，状态栏常驻不重建。

### 5.5 `ui_core/ui_fonts`（字体，扩展点）

```c
typedef enum { UI_FONT_SMALL, UI_FONT_BODY, UI_FONT_TITLE, UI_FONT_HUGE, UI_FONT_ROLE_MAX } ui_font_role_t;
const lv_font_t *ui_font(ui_font_role_t role);
void ui_fonts_reload(void);   /* 运行中替换了 SD 上的字体时调用 */
```

解析顺序（首个可用者胜出），保证**任何情况下都有中文字形**：

1. **FreeType TTF（SD 卡）**：`<storage>/todohelper/fonts/ui.ttf`——换字体不用重新烧录；
2. **FreeType TTF（内嵌）**：`components/ui_core/fonts/ui_cjk.ttf`，由
   `tools/gen_cjk_font.py` 生成（Noto Sans SC 可变字体钉到 wght=400 + GB2312 一级
   3755 常用字 + ASCII + 标点，约 **1.2 MB**），经 `EMBED_FILES` 内嵌；
3. **LVGL 内置 CJK**：`lv_font_source_han_sans_sc_16_cjk`——FreeType 或字体文件不可用时的兜底；
4. **Montserrat**（内置）：仅拉丁字形，最后兜底。

实测要点：

- 字号按角色固定为 16/20/28/48 px，都由**同一个** TTF 渲染，因此中英文粗细一致；
- `CONFIG_ESP_LVGL_ADAPTER_FREETYPE_SMALL_RENDER_POOL=y` 把渲染池压到 4 KB 并去掉 LVGL
  的 32 KB 绘制栈构建期检查；`..._MINIMAL_BUILD=y` 去掉用不到的字体驱动与压缩流支持；
- 关掉 `CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_16_CJK` 就没有中文兜底，**不要关**；
- 字体**不能**走 `assets` 素材分区（`esp_mmap_assets` 只打包
  `MMAP_FILE_SUPPORT_FORMAT` 里的后缀），所以用 `EMBED_FILES`。

### 5.6 `http_router`（L1）

```c
#include "http_router.h"

/* 路由注册（模式里允许一个 '*'，按"最具体优先"匹配） */
esp_err_t http_router_register(const char *path, httpd_method_t method,
                               http_route_handler_t handler);
esp_err_t http_router_register_not_implemented(const char *path, httpd_method_t method,
                                               const char *hint);   /* 回 501 */
esp_err_t http_router_require_auth(const char *path, httpd_method_t method, bool required);
esp_err_t http_router_attach(httpd_handle_t server);   /* 装 catch-all 分发器 */

/* 统一响应（需求 §8.1） */
esp_err_t http_reply_ok(httpd_req_t *req, const char *data_json);
esp_err_t http_reply_json(httpd_req_t *req, int status, const char *msg, const char *data_json);
esp_err_t http_reply_err(httpd_req_t *req, int status, const char *msg);
esp_err_t http_reply_bad_request/not_found/method_not_allowed/unauthorized(req, ...);
esp_err_t http_reply_not_implemented(httpd_req_t *req, const char *hint);
esp_err_t http_reply_esp_error(httpd_req_t *req, esp_err_t err, const char *context);

/* 请求解析 */
esp_err_t http_read_body(httpd_req_t *req, char **out, size_t *len);
esp_err_t http_get_path(httpd_req_t *req, char *out, size_t len);
bool      http_query_get(httpd_req_t *req, const char *key, char *out, size_t len);
bool      http_query_get_i64(httpd_req_t *req, const char *key, int64_t *out);
bool      http_path_segment(httpd_req_t *req, int index, char *out, size_t len);
bool      http_path_segment_u32(httpd_req_t *req, int index, uint32_t *out);
```

要点：

- 服务端只注册**一个** `"/*"` + `HTTP_ANY` 的 catch-all，其余全在表里分发；
- 匹配顺序：先找"路径+方法都命中"的最具体模式，找不到再判断是否只是方法不匹配
  （→ 405），最后才 404；
- 已注册但未实现的路由（`..._not_implemented`）返回 **501**，这是需求 §8.1 的硬要求；
- 写方法（POST/PUT/PATCH/DELETE）一律经过鉴权钩子；只读接口默认不鉴权，
  敏感接口用 `http_router_require_auth()` 单独打开（如 `/api/v1/device/reboot`）；
- `OPTIONS` 统一回 204 + CORS 头，方便浏览器直接调用。

### 5.7 `sync_server`（L4）

```c
typedef struct { uint16_t port; bool mdns_enabled; bool auth_enabled; char hostname[32]; } sync_server_config_t;

esp_err_t sync_server_init(void);          /* 读 NVS → start；失败不阻塞启动 */
esp_err_t sync_server_start(const sync_server_config_t *cfg);
esp_err_t sync_server_stop(void);
bool      sync_server_is_running(void);
httpd_handle_t sync_server_handle(void);
const char   *sync_server_url(void);       /* "http://todohelper.local" */

esp_err_t sync_server_register_route(const char *path, httpd_method_t method,
                                     http_route_handler_t handler);
esp_err_t sync_server_register_not_implemented(const char *path, httpd_method_t method,
                                               const char *hint);

esp_err_t sync_server_set_auth_token(const char *token);   /* 存 NVS，不硬编码 */
bool      sync_server_auth_required(void);
bool      sync_server_check_auth(httpd_req_t *req);        /* 读 X-Auth-Token */

esp_err_t sync_server_broadcast_event(const char *event_name, const char *json_payload);
size_t    sync_server_sse_clients(void);
```

`sync_server` 自己持有的路由（其余全部由业务组件反向注册）：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/`、`/index.html` | 内嵌的浏览器待办页（`web/index.html`，经 `EMBED_FILES`） |
| GET | `/api/v1/device/info` | 芯片/固件/IP/MAC/存储状态（**不返回任何密钥**） |
| GET | `/api/v1/device/status` | 运行时长/堆/路由数/SSE 客户端数 |
| POST | `/api/v1/device/reboot` | **强制鉴权**；先回包再用 700 ms 定时器重启 |
| POST | `/api/v1/device/auth` | **强制鉴权**；设置 token 并开启鉴权 |
| GET | `/api/v1/events` | SSE 推送（每客户端独立队列 + 15 s keepalive，最多 4 个） |
| GET | `/ws/events` | **501 占位**：本 IDF 无 WebSocket 组件，提示改用 SSE（需求 §8.7） |

`sync_server` **不依赖任何业务组件**：它订阅 L0 的 `SYS_NOTIFY_EVENT`（见 §5.8）
把变更转成 SSE 推送。

### 5.8 `sys_notify`（L0，跨层通知）

```c
#include "sys_notify.h"

typedef struct { char name[25]; char payload[193]; } sys_notify_t;

esp_err_t sys_notify(const char *name, const char *payload_json_or_null);
esp_err_t sys_notify_subscribe(esp_event_handler_t handler, void *arg,
                               esp_event_handler_instance_t *instance);
```

用途：L3 服务想通知 UI/Web"数据变了"时，调 `sys_notify("todo_changed", NULL)`，
**不需要**知道 `ui_core` 或 `sync_server` 的存在。载荷按值拷贝进事件队列，
接收方永远不会拿到悬空指针（因此限制在 216 B 内；大块数据请接收方回查 API）。

**已落地的通知及其消费者**：

| 通知名 | 发送方 | 消费者 / 效果 |
|---|---|---|
| `todo_changed` | `todo_manager`（每次变更） | `sync_server` → SSE `todo_changed`；网页重新拉列表 |
| `todo_done` | `todo_manager`（勾选为完成） | `idle_face` → HAPPY 2 s |
| `sync_started` / `sync_done` | `todo_manager::todo_mark_all_synced()` | `idle_face` → SYNCING 9 s / HAPPY 1.5 s |
| `wifi_connected` | `wifi_manager`（GOT_IP） | `idle_face` → HAPPY 2.5 s |
| `wifi_failed` | `wifi_manager`（重试耗尽） | `idle_face` → ERROR 3.5 s |
| `net_online` / `net_offline` | `sync_server`（订阅 `SYS_EVENT_CONNECTIVITY`） | SSE + `idle_face` → HAPPY / SLEEPY |
| `weather_changed` / `weather_located` | `weather_service` | SSE（网页刷新） |
| `word_reviewed` | `word_service` | SSE |

**`idle_face` 的表情触发就是这张表的子集**（`idle_face.c` 的 `s_triggers[]`）：
新增一种反应 = 在触发表加一行 + 让生产者 `sys_notify()`，**两处，无其它改动**。
唯一例外是 `ai_service`：它有自己的状态机，直接调 `idle_face_express()`
（LISTENING / THINKING / SPEAKING / ERROR），因为"说话中"这种状态需要持续时长而非一次性触发。

### 5.9 业务组件

签名与事件严格按需求文档 §7.2 ~ §7.11 实现，本文档不重复。
实现时若确需偏离，**先改需求文档**再改代码。

---

## 6. 事件清单（冻结）

一个共享事件循环；每个组件自己的基址：

| 基址 | 事件 |
|---|---|
| `WIFI_MGR_EVENT` | `SCAN_DONE` `CONNECTING` `CONNECTED` `GOT_IP` `DISCONNECTED` `FAILED` |
| `TODO_EVENT` | `ADDED` `UPDATED` `DONE_CHANGED` `DELETED` `SYNC_CHANGED` `LIST_CHANGED` |
| `WEATHER_EVENT` | `UPDATED` `REFRESH_FAILED` |
| `CALENDAR_EVENT` | `TIME_SYNCED` `SYNC_FAILED` |
| `WORD_EVENT` | `LOADED` `REVIEWED` `PROGRESS_SAVED` |
| `SYNC_EVENT` | `STARTED` `STOPPED` `CLIENT_CONNECTED` `AUTH_FAILED` `BROADCAST` |
| `IDLE_FACE_EVENT` | `STATE_CHANGED` |
| `STORAGE_EVENT` | `MOUNTED` `UNMOUNTED` `LOW_SPACE` |
| `SYS_EVENT` | `BUTTON` `BRIGHTNESS_CHANGED` `IDLE_TIMEOUT` `WAKEUP` `READY` |
| `AI_EVENT` | `STATE_CHANGED` `REPLY_CHUNK` `REPLY_DONE` `ERROR` |
| `VOICE_EVENT` | `RECORD_DONE` `ASR_DONE` `PLAY_DONE` `WAKE` |

约定：事件 `data` 只在回调期内有效，接收方自行拷贝；**不要**在事件里传大结构（>256B 传指针+长度）。

---

## 7. 扩展点：新增一个功能要改哪些地方

### 7.1 新增一个业务 service（例：`note_service`）

1. 建 `components/note_service/`（§4.1 的文件集，`include/note_service.h`）；
2. `CMakeLists.txt` 里 `REQUIRES sys_utils storage_manager`（**只允许向下**）；
3. 事件在 `include/note_service.h` 里 `ESP_EVENT_DECLARE_BASE(NOTE_EVENT)`；
4. 若需 HTTP：实现 `note_register_http(void)`，在 `note_service_init()` 里调
   `sync_server_add_route_provider("note", note_register_http)`；
5. 在 `main/app_main.c` 的**模块表**里加一行 `APP_MODULE(note_service)`；
6. 若需页面：在 `components/ui_pages/` 加文件并在 `ui_pages_table[]` 加一行；
7. 在 `docs/API.md` 追加一节。

> 第 5、6 步就是全部"接线"工作——**没有其他地方需要改**。

### 7.2 新增一个页面

在 `components/ui_pages/` 新建 `page_xxx.c`，导出 `const ui_page_t page_xxx`，
在 `ui_pages_table[]`（`ui_pages.c`）里加一行，并在 `ui_core.h` 的 `ui_page_id_t` 里加枚举值
（枚举顺序必须与页面表一致，需求文档 §10.1 的对应表同步更新）。

### 7.3 新增一个待机表情

1. `idle_face.h` 的 `idle_face_state_t` 加值；
2. `idle_face_effects[]` 表加一行参数（瞳孔位移 / 眼睁度 / 眼形 / 颜色 / 动效）；
3. 需求文档 FR6 与 §11.2 同步补一行。

### 7.4 新增一个 HTTP 接口

在**拥有该资源的组件**里加 handler，再向 L1 的 `http_router` 注册：

```c
static const http_route_t routes[] = {
    { "/api/v1/notes",      HTTP_GET,  notes_list_handler },
    { "/api/v1/notes",      HTTP_POST, notes_create_handler },
    { "/api/v1/notes/*",    HTTP_DELETE, notes_delete_handler },
};
for (...) http_router_register(routes[i].path, routes[i].method, routes[i].handler);
```

- **不要**改 `sync_server`；它只负责起服务并挂 catch-all 分发器；
- 路径前缀归属：`/api/v1/todos*`→todo、`/wifi*`→wifi、`/weather*`→weather、
  `/words*`→words、`/calendar*`→calendar、`/ai*`→ai、`/device*`→sync_server 自身；
- 预留但未实现的接口**也要注册**（`http_router_register_not_implemented`）并返回 **501**，
  客户端必须能区分"没做"和"路径不存在"；
- 入参用 `http_read_body()` + cJSON 解析，出参用 `http_reply_ok()` 等统一封装，
  这样 `code`/HTTP 状态码/鉴权/404/405 语义全局一致；
- 路径段用 `http_path_segment_u32(req, 3, &id)` 取，不要自己 `sscanf(req->uri)`。

### 7.5 新增一个 NVS 键

在 `sys_utils/include/sys_keys.h` 里加 `#define`，并登记到需求文档 §9.1 与 `docs/API.md`。
禁止在组件里写裸字符串键名。

---

## 8. 存储与分区布局

新的 `partitions.csv`（16 MB flash）：

| 分区 | 偏移 | 大小 | 用途 |
|---|---|---|---|
| `nvs` | 0x9000 | 0x6000 | 全部配置（需求 §9.1） |
| `phy_init` | 0xf000 | 0x1000 | 射频校准 |
| `factory` | 0x10000 | 0x800000 | App（8 MB，含内嵌 CJK 子集字体） |
| `storage` | 0x810000 | 0x200000 | 片内 LittleFS（SD 不可用时的回退，subtype `littlefs`） |
| `assets` | 0xa10000 | 0x5f0000 | UI 资源（PNG 图标），复用 `spiffs_create_partition_assets` + `esp_mmap_assets` |

> **实测约束（踩过的坑）**：`esp_mmap_assets` 的打包脚本用
> `os.listdir(assets_dir)` **只扫描一层**，不支持子目录，也不会把非
> `MMAP_FILE_SUPPORT_FORMAT` 后缀的文件打进镜像。因此：
> 1. `assets/ui/` 必须是**扁平目录**，用文件名前缀分类（如 `w_rain.png` = 天气-雨）；
> 2. 字体（`.ttf`）**不能**放这里，改用 `idf_component_register(EMBED_FILES ...)`
>    内嵌进 app（见 `ui_core`），或放 SD 卡。
>
> 另外该自定义命令的 `DEPENDS` 依赖 configure 期求值的 `GLOB_RECURSE`，
> 增删素材后若镜像没更新，执行一次 `idf.py reconfigure`。

运行时根路径：

```text
storage_root() = "/sdcard"      (SD 挂载成功)
               | "/littlefs"    (回退)
<root>/todohelper/...           需求文档 §9.2 的目录树
```

**硬约束**：GPIO20~25 与 SPI NAND flash 复用（需求 §4），两者互斥；因此
"SD 卡可用" 与 "片内 assets 分区可用" 两条路径都必须能独立工作，不能互相假设。

---

## 9. 模块编排（`main/app_main.c`）

```c
typedef struct {
    const char *name;
    esp_err_t (*init)(void);
    bool        optional;    /* true: 失败只告警不中断启动 */
} app_module_t;

static const app_module_t s_modules[] = {
    { "bsp",             th_bsp_init,            false },  /* 显示必须最先 */
    { "storage",         storage_manager_init,   true  },
    { "wifi",            wifi_manager_init,      true  },
    { "todo",            todo_manager_init,      true  },
    { "weather",         weather_service_init,   true  },
    { "calendar",        calendar_service_init,  true  },
    { "word",            word_service_init,      true  },
    { "sync_server",     sync_server_init,       true  },
    { "idle_face",       idle_face_init,         true  },
    { "ui_pages",        ui_pages_init,          false },
    ...
};
```

启动顺序：`nvs_flash_init` → `sys_config_init` → `sys_events_init` → 模块表逐项 init →
`ui_core_init` + 页面表注册 → 切到 SPLASH → 发 `SYS_EVENT_READY` → 启动待机计时。
**任何 optional 模块失败都不得阻止 UI 起来**（需求 §15 健壮性验收）。

---

## 10. 里程碑

| 阶段 | 内容 | 状态 |
|---|---|---|
| **A 框架** | 分区表/工程骨架 + 摘除咖啡机 + `sys_utils`/`http_router`/`storage_manager`/`bsp`/`ui_assets`/`ui_core` + 新 `app_main.c` | ✅ `idf.py build` 通过，**未上机** |
| **B P0 业务** | wifi/todo/weather/calendar/word + sync_server + idle_face(NORMAL/BLINK) + 全部页面 | ✅ 代码完成，**未上机** |
| **C P1** | 设置页/鉴权/mDNS/缓存降级/SSE/内嵌 CJK 字体/表情自动触发/IP 定位 | ✅ 代码完成，**未上机** |
| **D P2** | ai_service + voice_service + 小眼睛联动 + AI 对话页 | ✅ 代码完成；ASR/TTS 仅定义协议，**未上机** |

> ⚠️ **全部四个阶段的共同前提是"编译通过"而非"真机验证"**。首次上机的逐项清单、
> 最可能出问题的位置、以及开放决策见 `docs/HANDOVER.md`。

### 并行分工（B 阶段，历史记录）

`sys_utils`/`http_router`/`storage_manager`/`ui_core` 冻结后，B 阶段可 5 路并行：

| 路 | 负责 | 依赖 |
|---|---|---|
| B1 | `wifi_manager` | sys_utils |
| B2 | `todo_manager` + `sync_server` | sys_utils, storage_manager, http_router |
| B3 | `weather_service` + `calendar_service` | sys_utils, storage_manager, wifi 事件 |
| B4 | `word_service` + `idle_face` | sys_utils, storage_manager, ui_core |
| B5 | `ui_pages`（全部页面） | ui_core + 各 service 的**头文件**（不依赖其实现） |
