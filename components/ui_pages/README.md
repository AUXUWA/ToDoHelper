# ui_pages

**L4 页面集合** — 按需求 §10.1 实现全部页面，并维护**页面表**。

- 头文件：`include/ui_pages.h`（只有 `ui_pages_init()`）
- 依赖：`ui_core`、`sys_utils`（后续按需增加各业务组件的**公开头文件**）
- 被谁依赖：`main`

## 页面表

`src/ui_pages.c` 的 `s_pages[]` 是唯一的组合点：

| 页面 | 枚举 | 状态 | 文件 |
|---|---|---|---|
| 启动页 | `UI_PAGE_SPLASH` | ✅ 已实现（滚动启动日志 + 动画省略号 + 真实固件版本） | `page_splash.c` |
| 主菜单 | `UI_PAGE_HOME` | ✅ 已实现 | `page_home.c` |
| WiFi | `UI_PAGE_WIFI` | ✅ 已实现 | `page_wifi.c` |
| 待办列表 | `UI_PAGE_TODO` | ✅ 已实现（**可勾选完成 / 删除**，见下） | `page_todo.c` |
| 待办详情 | `UI_PAGE_TODO_DETAIL` | ✅ 已实现（**只读**，见下） | `page_todo.c`（与列表页同文件，导出 `page_todo_detail`） |
| 天气 | `UI_PAGE_WEATHER` | ✅ 已实现 | `page_weather.c` |
| 日历 | `UI_PAGE_CALENDAR` | ✅ 已实现 | `page_calendar.c` |
| 单词 | `UI_PAGE_WORD` | ✅ 已实现 | `page_word.c` |
| AI 对话 | `UI_PAGE_AI_CHAT` | ✅ 已实现 | `page_ai_chat.c` |
| 设置 | `UI_PAGE_SETTINGS` | ✅ 已实现 | `page_settings.c` |
| 待机眼睛 | `UI_PAGE_IDLE_FACE` | ✅ 已实现，但**由 `idle_face` 组件自己注册**，不在 `s_pages[]` 里 | `components/idle_face/src/idle_face.c`（`idle_face_init()` → `ui_page_register()`） |

> **全部 11 个页面均已实现**，`page_placeholders.c` 已删除。
> 原有的"先注册占位页、再逐个替换"策略已完成使命：新增页面时按下面 5 步直接加即可。

> **设置页显示固件版本**（1.0.0 起）：`page_settings.c` 的 `refresh_info()` 用
> `esp_app_get_description()` 输出「固件 1.1.0 · IDF v6.1」—— 这是判断"板子上到底烧的是哪个
> 版本"最快的方式。版本来自工程根目录 `version.txt`（规则见 `AGENT.md §8.1`）。

> **启动页（`page_splash.c`）的两处细节**（1.1）：
> 1. 副标题的固件版本同样来自 `esp_app_get_description()->version`。以前它读
>    `CONFIG_APP_PROJECT_VER`，而那个符号只在 `CONFIG_APP_PROJECT_VER_FROM_CONFIG` 打开时
>    才存在，所以屏幕上**一直显示 `0.1.0`**；
> 2. 状态行有**动画省略号**（每 `SPLASH_DOTS_MS` = 420 ms 多一个点）。它由页面自己的
>    100 ms `lv_timer` 驱动，**与日志序号无关**——否则两次日志之间（模块初始化慢的时候可能
>    好几秒）状态行会完全不动，看起来像死机。
> 离场时的淡出+上移由 `ui_core` 的交叉淡入负责，本页不需要写任何退出动画。

## 待办页：设备端可「勾选完成 + 删除」，新增/编辑只在网页端

设备端待办 UI（`src/page_todo.c`，列表页 + 只读详情页）保留**两个**写操作：
**行首勾选框切换完成状态**（`row_toggle_cb` → `todo_set_done()`，**失败会回滚勾选状态**，
不让 UI 撒谎）与**行尾 52×34「删除」按钮**（`todo_delete()`）。
**新增与编辑只由电脑网页端完成** —— 本板无中文 IME，软键盘输入中文不现实。

- 列表页提示语：`TODO_READONLY_HINT = "此页可勾选完成 / 删除 | 新增请到电脑网页端"`；
- 已移除的入口：列表页底部「新增」按钮及其文字输入浮层 + `ui_keyboard_create()` 软键盘、
  批量按钮「标记已同步」/「清除已完成」、详情页底部的「完成/取消 / 优先级 / 删除」条
  （详情页因此是**只读**的：标题、备注、状态、优先级、到期、同步状态）；
- 保留的查看能力：列表的完成指示 + `✓` 前缀 + 颜色、同步状态徽标、统计行
  「共 N 项 · 已完成 N · 待同步 N」，点击整行进详情页；两页都
  `ui_event_bind(TODO_EVENT, TODO_EVENT_LIST_CHANGED, ...)`，网页端改动后自动重绘。

为什么这样定：

1. 800×480 触摸屏 + 软键盘的中文输入体验很差，实际可用性远不如网页；
2. **单一新增/编辑入口**：新增与编辑只走网页，避免两端同时改写同一项导致互相覆盖；
3. 勾选与删除留在设备上是**用户明确要求**的（"在设备上看到就想直接划掉"），
   而且这两个操作不需要输入；失败时回滚状态，保证设备端显示与 NVS/卡上的数据一致。

> 历史：曾经整页做成"完全只读"（连勾选和删除都移除），用户实测后澄清要保留这两个操作，
> 于是按上述语义收敛。详见 `docs/HANDOVER.md §0.5.2` 与 §3 需求 4。

## 新增一个页面（扩展点）

1. 建 `src/page_xxx.c`，导出 `const ui_page_t page_xxx = { .id = UI_PAGE_XXX, ... };`
2. 在 `src/pages_internal.h` 加 `extern const ui_page_t page_xxx;`
3. 在 `src/ui_pages.c` 的 `s_pages[]` 加一行；
4. 在 `CMakeLists.txt` 的 `SRCS` 加文件；
5. 若需要新的页面 id，在 `ui_core.h` 的 `ui_page_id_t` 加枚举
   （**顺序必须与需求 §10.1 对应表一致**）。

## 页面实现规范

- 只用 `ui_theme.h` 的配色/字号/间距，不写字面值与绝对坐标（需求 §10.3）；
- 触摸控件高度 ≥ `UI_TOUCH_MIN`（44 px）；
- 布局用 flex（`LV_FLEX_FLOW_COLUMN` / `ROW_WRAP`），不要 `lv_obj_set_pos()`；
- 只通过 `create(parent)` 传入的容器建控件，不要碰屏幕或其他页面；
- 需要业务数据时**订阅事件**（`ui_event_bind`）或读 `sys_status_get()`，
  不要直接调用 service 的阻塞接口；
- 时间相关 UI 必须先判断 `sys_time_synced()`，未同步时显示 `UI_STATUS_TIME_UNSYNCED`。

## 测试方法

```powershell
idf.py build && idf.py -p COM34 flash monitor
```

逐页点开：每页都应显示自己的标题、左侧"返回"按钮，且顶部状态栏保持不动。

对待办页额外确认（1.0.0 语义）：列表页底部应看到提示文字
**「此页可勾选完成 / 删除 | 新增请到电脑网页端」**，行尾有 52×34「删除」按钮；
点行首勾选框应切换完成状态（**失败会回滚**，可故意拔卡验证），点整行进只读详情页；
**不应出现**新增按钮 / 软键盘 / 批量按钮。在电脑网页端改一条待办，设备端应在不触摸屏幕的
情况下自动刷新（`TODO_EVENT_LIST_CHANGED`）。
