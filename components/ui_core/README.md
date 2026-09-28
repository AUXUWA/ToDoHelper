# ui_core

**L2 UI 框架** — 页面注册表、导航、状态栏、LVGL 线程安全、主题、字体、通用控件。

- 头文件：`include/ui_core.h`（页面/锁/事件/控件）、`include/ui_theme.h`（配色/字号/尺寸）
- 依赖：`sys_utils`、`bsp`、`storage_manager`、`lvgl`、`esp_lvgl_adapter`
- 被谁依赖：`ui_pages`、`idle_face`

## API 摘要

| 分组 | 函数 |
|---|---|
| 生命周期 | `ui_core_init()` |
| 页面表 | `ui_page_register()`、`ui_page_get()` |
| 导航 | `ui_page_switch()`、`ui_page_current()`、`ui_page_parent()`、`ui_nav_push()`、`ui_nav_back()`、`ui_nav_can_back()` |
| 线程 | `ui_lock()`、`ui_unlock()`、`ui_run_async()` |
| 事件 | `ui_event_bind()`、`ui_event_bind_all()`、`ui_event_subscribe()` |
| 控件 | `ui_card_create()`、`ui_toast()`、`ui_dialog()` |
| 状态栏 | `ui_statusbar_refresh()`、`UI_STATUS_TIME_UNSYNCED` |
| 尺寸 | `ui_screen_width/height()`、`ui_statusbar_height()` |

## 页面模型

`ui_page_t` 六个可选钩子：`create(parent)` / `destroy` / `on_show` / `on_hide` /
`on_event(event_id, data)` / `on_tick`（+ `tick_period_ms`）。

- **一个屏幕**：`ui_core` 独自持有 `lv_screen_active()`；每个页面是内容区下的一个全屏容器，
  页面**不得**调用 `lv_screen_load()` / 自己建 screen（需求 §16 第 10 条）。
- **延迟创建**：页面首次 `ui_page_switch()` 时才 `create()`；之后只切 `LV_OBJ_FLAG_HIDDEN`。
- **切页是交叉淡入，不是硬切**（1.1 起）：新页面先 `lv_obj_move_foreground()`，然后
  `translate_x 28→0` + 透明度 0→255（220 ms ease-out）滑入；旧页面同时 `translate_y 0→-16`
  + 透明度 255→0（240 ms ease-in）淡出，**动画结束才加 `HIDDEN`**。
  两个后果：① 全屏页面之间的交接（开机日志 → 主界面）不再是一瞬间的跳变；
  ② 页面**第一次**有了可用的"离场动画窗口"——`idle_face_standby_exit()` 的睁眼过冲
  正好落在 240 ms 里，以前在同一帧就被隐藏掉，一帧都看不到。
  离开的页面在窗口内仍会渲染最后一帧（它的 `on_tick` 已经暂停，内容是静止的）。
- **`on_tick` 只在页面可见时运行**，周期由 `tick_period_ms` 决定。
- 未注册的页面 id：`ui_page_switch()` 返回 `ESP_ERR_NOT_FOUND` 并告警，不崩溃。

## 线程模型（重要）

- LVGL 只在适配器的 UI 任务里访问；`ui_lock()` / `ui_unlock()` 包装
  `esp_lv_adapter_lock()`，**是递归锁**，所以在 UI 回调里再 `ui_lock()` 是安全的。
- 非 UI 任务改 UI：
  - 一次性动作 → `ui_run_async(fn, arg)`；
  - 事件驱动 → `ui_event_bind(base, event_id, page, payload_size)`，
    `ui_core` 会**拷贝 payload**、经 `lv_async_call()` 转到 UI 任务，再调页面的 `on_event`。
- `payload_size` 必须与发送方一致（无 payload 传 0）。超过 256 B 会被截断并告警。
- 事件是**通知**不是可靠传输：UI 忙时可能丢事件，需要最终一致的地方请配合 `on_tick`。

## 通用控件

- `ui_card_create(parent, title, &body)` — 圆角卡片 + 可选标题，返回 `body` 供放内容。
- `ui_toast(fmt, ...)` — 底部提示，2 秒自动消失，可从任意任务调用。
- `ui_dialog(title, text, ok_text, on_close, user_data)` — 模态框。
- `ui_dialog_secret(title, lead_in, secret, note, ok_text)` — 弹一个**长机密串**
  （HTTP 鉴权 token）。**两列横排**：左边 `lead_in`、右边高亮 `secret` 框。

### 对话框的三条硬规则（v1.3.1 修 bug 时定下）

1. **关闭回调不要把"按钮"当成"消息框"。**
   `lv_msgbox_add_footer_button()` 返回的是 **footer 里的按钮**，回调也注册在它上面，
   所以 `lv_event_get_current_target()` 拿到的是按钮 —— `lv_msgbox_close(按钮)`
   是**静默 no-op**（这正是"点「知道了」关不掉"的原因）。
   `ui_dialog_secret()` / `ui_dialog()` 现在把消息框作为 **event user data** 显式传入。
2. **面板是 800×480 横屏，内容该横着排。**
   `lv_msgbox` 默认 `LV_FLEX_FLOW_COLUMN`（标题/正文/footer 一路往下），480 px 高很快用完
   就被"挤压"。需要并排的内容自己放进一个 `LV_FLEX_FLOW_ROW` 容器。
   实测几何：对话框 `94%×800 = 752 px`，内宽 720 px，两列各 ~353 px，
   32 字符 token 一行约需 320 px → **正好一行**。
3. **样式必须走 `dialog_style()` / `dialog_style_button()`。**
   LVGL 默认 msgbox 是浅色皮肤，与本工程深色卡片完全不是一个东西。这两个函数套上
   `UI_COLOR_SURFACE`/`_ALT`/`_PRIMARY`、`UI_CARD_RADIUS` 与正文/标题字体。
   **新弹窗直接复用它们**，不要手写色值。

另外：**`lv_msgbox_create(lv_layer_top())` 不会创建独立遮罩**（传 `NULL` 才会）。
所以 `lv_obj_get_parent(mbox)` 是 `lv_layer_top()` 本身，而 `ui_toast()` 也挂在那里——
给它上色会**连 toast 一起压暗**。现在靠消息框自己的 shadow + 不透明底色区分。

## 主题与字体

配色/尺寸集中在 `include/ui_theme.h`（`UI_COLOR_*`、`UI_TOUCH_MIN=44` 等），
页面**不得**写字面色值或绝对坐标。

字体解析顺序（`src/ui_fonts.c`，任何情况下都保证有中文字形）：

1. FreeType TTF：`<storage>/todohelper/fonts/ui.ttf`（换字体不用重新烧录）；
2. FreeType TTF：烧进固件的 CJK 子集（`components/ui_core/fonts/ui_cjk.ttf`）；
3. LVGL 内置 `lv_font_source_han_sans_sc_16_cjk`；
4. LVGL 内置 Montserrat（仅拉丁，最后兜底）。

开启方式：`CONFIG_LV_USE_FREETYPE` + `CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE`
+ `CONFIG_TODOHELPER_FONT_FREETYPE`。

## 待机策略（FR6 §11.4）

`ui_core` 是策略唯一归属地：收到 `SYS_EVENT_IDLE_TIMEOUT` → 切到 `UI_PAGE_IDLE_FACE`
并把亮度降到 NVS `idle_bright`（默认 20%）；收到 `SYS_EVENT_WAKEUP` → 恢复
NVS `brightness` 并切回待机前的页面。`idle_face` 组件只负责画眼睛，不参与决策。

## 测试方法

```powershell
idf.py build && idf.py -p COM34 flash monitor
```

期望：启动页 → 首页（6 个入口卡片 + 顶部状态栏）。
点击卡片进入对应页面，`返回` 按钮回退；MODE 键在首页/待机之间切换；
30 秒无操作自动进入待机页并变暗，触摸后恢复。
