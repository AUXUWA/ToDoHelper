# ui_pages

**L4 页面集合** — 按需求 §10.1 实现全部页面，并维护**页面表**。

- 头文件：`include/ui_pages.h`（只有 `ui_pages_init()`）
- 依赖：`ui_core`、`sys_utils`（后续按需增加各业务组件的**公开头文件**）
- 被谁依赖：`main`

## 页面表

`src/ui_pages.c` 的 `s_pages[]` 是唯一的组合点：

| 页面 | 枚举 | 状态 | 文件 |
|---|---|---|---|
| 启动页 | `UI_PAGE_SPLASH` | ✅ 已实现 | `page_splash.c` |
| 主菜单 | `UI_PAGE_HOME` | ✅ 已实现 | `page_home.c` |
| WiFi | `UI_PAGE_WIFI` | ✅ 已实现 | `page_wifi.c` |
| 待办列表 | `UI_PAGE_TODO` | ✅ 已实现（**只读**，见下） | `page_todo.c` |
| 待办详情 | `UI_PAGE_TODO_DETAIL` | ✅ 已实现（**只读**，见下） | `page_todo.c`（与列表页同文件，导出 `page_todo_detail`） |
| 天气 | `UI_PAGE_WEATHER` | ✅ 已实现 | `page_weather.c` |
| 日历 | `UI_PAGE_CALENDAR` | ✅ 已实现 | `page_calendar.c` |
| 单词 | `UI_PAGE_WORD` | ✅ 已实现 | `page_word.c` |
| AI 对话 | `UI_PAGE_AI_CHAT` | ✅ 已实现 | `page_ai_chat.c` |
| 设置 | `UI_PAGE_SETTINGS` | ✅ 已实现 | `page_settings.c` |
| 待机眼睛 | `UI_PAGE_IDLE_FACE` | ✅ 已实现，但**由 `idle_face` 组件自己注册**，不在 `s_pages[]` 里 | `components/idle_face/src/idle_face.c`（`idle_face_init()` → `ui_page_register()`） |

> **全部 11 个页面均已实现**，`page_placeholders.c` 已删除。
> 原有的"先注册占位页、再逐个替换"策略已完成使命：新增页面时按下面 5 步直接加即可。

## 待办页只读：设备端不编辑待办

设备端待办 UI（`src/page_todo.c`，列表页 + 详情页）**只读**，待办的增 / 删 / 改 / 完成
全部只在**电脑网页端**（`sync_server` 提供的 HTTP API + 网页）完成。

已移除的设备端编辑入口：

- 列表页底部工具条的「新增」按钮，以及它弹出的文字输入浮层 + `ui_keyboard_create()` 软键盘；
- 每行的「删除」按钮；
- 行首勾选框的完成状态切换（`row_toggle_cb`）——勾选框改成**纯状态指示**（去掉
  `LV_OBJ_FLAG_CLICKABLE`，点击穿透到整行，只打开详情页）；
- 批量按钮「标记已同步」（`todo_mark_all_synced()`）与「清除已完成」（`todo_clear_done()`）；
- 详情页底部的「完成/取消」「优先级」「删除」按钮。

保留的查看能力（页面对用户仍然有用）：

- 列表：标题、完成状态（勾选指示 + `✓` 前缀 + 颜色）、同步状态徽标（失败 / 待同步）、
  统计行「共 N 项 · 已完成 N · 待同步 N」；点击整行进入详情页；
- 详情：标题、备注、完成状态、优先级、到期时间、同步状态；
- 两页仍各自 `ui_event_bind(TODO_EVENT, TODO_EVENT_LIST_CHANGED, ...)`，
  网页端改动后自动重绘，无需手动刷新；
- 页面底部固定显示提示 **「设备端只读 | 请在电脑网页端编辑」**；列表为空时提示
  「请在电脑网页端新增」。

为什么这样定：

1. 800x480 触摸屏 + 内置 CJK 子集字库 + 软键盘的中文输入体验很差（原先的输入框本来就
   只能可靠输入英文/数字），实际可用性远不如网页；
2. **单一写入口**：`todo_manager` 原来同时被网页端（`sync_server`）和设备端 UI 写入，
   两边的改动会互相覆盖、同步状态难以推理；只读把"谁在写"收敛成一处；
3. 设备端仍保留 FR2 的展示价值，只是不再承担编辑职责。设备端 UI 现在**没有任何**
   调用 `todo_add / todo_update / todo_delete / todo_set_done /
   todo_mark_all_synced / todo_clear_done` 的代码——全工程只剩 `todo_manager` 自己
   （实现 + `sync_server` 用的 HTTP 接口）在写。

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

对待办两页额外确认**只读**：列表/详情底部只应看到提示文字「设备端只读 | 请在电脑网页端编辑」，
不应出现任何按钮；点行首勾选框和整行都只跳到详情页，不改变待办；在电脑网页端改一条待办，
设备端应在不触摸屏幕的情况下自动刷新（`TODO_EVENT_LIST_CHANGED`）。
