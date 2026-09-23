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
| WiFi | `UI_PAGE_WIFI` | 占位 | `page_placeholders.c` |
| 待办列表 | `UI_PAGE_TODO` | 占位 | 同上 |
| 待办详情 | `UI_PAGE_TODO_DETAIL` | 占位 | 同上 |
| 天气 | `UI_PAGE_WEATHER` | 占位 | 同上 |
| 日历 | `UI_PAGE_CALENDAR` | 占位 | 同上 |
| 单词 | `UI_PAGE_WORD` | 占位 | 同上 |
| AI 对话 | `UI_PAGE_AI_CHAT` | 占位 | 同上 |
| 设置 | `UI_PAGE_SETTINGS` | 占位 | 同上 |
| 待机眼睛 | `UI_PAGE_IDLE_FACE` | 占位（将由 `idle_face` 接管） | 同上 |

> 所有页面 id 从第一天就注册，因此导航、状态栏、待机策略可以立刻端到端联调；
> 每实现一个功能，就删掉 `page_placeholders.c` 里的对应块，新建 `page_xxx.c`
> 并替换 `ui_pages.c` 表格中的指针。

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
