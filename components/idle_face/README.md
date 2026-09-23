# idle_face

**L4 表现组件（FR6）** — 纯程序化绘制的待机"小眼睛"：黑底白线，11 种表情，零位图、零素材。

- 头文件：`include/idle_face.h`
- 依赖：`sys_utils`、`bsp`（`th_bsp_set_brightness`）、`ui_core`（页面注册、配色）、`lvgl`
- 被谁依赖：`ai_service`（用 `idle_face_express()` 表达聆听/思考/说话/出错）、`main`

## 为什么"纯程序化"是设计决策

需求 §11.1 要求"无复杂贴图优先，程序化绘制优先"，§11.5 进一步规定 **P0/P1 不引入 SD 帧素材**。
所以本组件**没有任何位图、不读 SD、不依赖 `ui_assets`**：所有状态都是同一批 LVGL 对象的属性组合，
`anim/idle_face/` 目录可以不创建。这也顺带满足了 §11.3 的"所有对象初始化时创建，状态切换只更新属性"。

## API 摘要

| 分组 | 函数 |
|---|---|
| 生命周期 | `idle_face_init()`（注册页面 + 启动动画）、`idle_face_attach()`（在给定容器里建眼睛，幂等） |
| 状态 | `idle_face_set_state()`、`idle_face_get_state()`、`idle_face_express()` |
| 驱动 | `idle_face_tick()`（33 ms 步进，通常由内部 `lv_timer` 调） |
| 其他 | `idle_face_set_brightness()`、`idle_face_is_attached()`、`idle_face_page_descriptor()` |

## 11 个状态与唯一参数表

状态枚举 `idle_face_state_t`（`IDLE_FACE_NORMAL` … `IDLE_FACE_SPEAKING`）与需求 §7.9 / §11.2 一一对应。
**表驱动**是这里的核心：每个状态在 `src/idle_face.c` 的 `s_effects[IDLE_FACE_MAX]` 里只有一行参数，
没有 `switch (state)` 分散在绘制代码里：

| 字段 | 含义 |
|---|---|
| `open` | 眼睛开合度，缩放椭圆高度（`0.04`–`1.6` 截断） |
| `dx` / `dy` | 瞳孔在眼内的归一化位移（−1…+1，会乘上可用的活动余量） |
| `pupil` | 瞳孔可见度（0 = 无瞳孔） |
| `arc` | 眼睛轮廓弯成上弧（开心），同时略微加宽 |
| `cross` | 切成 X 形（错误） |
| `eye_dy` | 整只眼睛的垂直偏移（困倦下垂、思考上抬） |
| `width_scale` | 线宽倍数 |
| `speed` | 每帧插值增益（0…1），取 `SPEED_FAST 0.55` / `SPEED_NORMAL 0.22` / `SPEED_SLOW 0.10` |

> 状态定义有**三处**必须同步修改：`idle_face.h` 的枚举、`idle_face.c` 的 `s_effects[]`、
> `docs/TODOHELPER_REQUIREMENTS.md` 的 FR6 / §11.2。头文件里对此有明确注释。

**只有一个 33 ms 动画器**（`lv_timer` → `idle_face_tick()`）：每帧把当前的
`open/dx/dy/pupil/arc/cross/eye_dy/width_scale` 朝目标参数 `lerp()` 一步。
因此**切换表情不会创建/销毁任何对象**，也就不会分配内存——这正是需求 §11.3 想要的。

## 每种视觉是怎么画出来的

| 视觉 | 实现 |
|---|---|
| 椭圆眼框 | 一个**只画边框**的圆角对象（`lv_obj_remove_style_all()` + `border_color` + `border_width` + `LV_RADIUS_CIRCLE`），背景透明、内部无子对象 |
| 开心上弧 | 同一个眼框对象把 `border_side` 改成 `LV_BORDER_SIDE_TOP`，高度取一半——下半弧不画即为"弯眼" |
| 瞳孔 | 眼框的**子对象**（所以跟着眼移动），圆形 + `bg_opa` 控制可见度，靠 `lv_obj_align(LV_ALIGN_CENTER, dx·travel_x, dy·travel_y)` 移动 |
| 错误 X 眼 | 每只眼两条 `lv_line`（画布上的两条对角线），靠 `line_opa` 显示/隐藏；此时眼框边框设为透明 |
| 同步中 | 不用 `dx/dy`，而是让相位走圆：`dx = 0.75·cos(2πφ)`、`dy = 0.75·sin(2πφ)`，φ 周期 1.8 s |
| 呼吸 | 在目标 `open` 上叠一层 `1 + 0.035·sin(2πφ)`（周期约 15 s） |

前景色用 `ui_theme.h` 的 `UI_COLOR_IDLE_FG`，背景是 `lv_color_black()`，所有对象都关掉
`LV_OBJ_FLAG_SCROLLABLE` / `LV_OBJ_FLAG_CLICKABLE`。

## 自动行为

- **随机眨眼**：间隔 `rand(IDLE_FACE_BLINK_MIN_MS, IDLE_FACE_BLINK_MAX_MS)` = **3–8 秒**，每次持续 130 ms；
- **随机左右看**：间隔 **4–12 秒**，保持 1400 ms 后回到 `NORMAL`；
- 两者只在"普通类"状态下生效（`state_allows_idle_antics()`：`NORMAL` / `LOOK_LEFT` / `LOOK_RIGHT` / `BLINK`），
  否则 `ERROR`/`THINKING` 会被眨眼"眨回中性脸"；
- 眨眼优先级高于左右看；随机数来自 `esp_random()`，时间基是 `esp_timer_get_time()`。

## 对外钩子与职责边界

- **`idle_face_express(state, ms)`** 是给其他组件用的钩子：临时显示 `state` 共 `ms` 毫秒
  （传 0 用默认 1600 ms），到点后自动恢复**之前**的状态。再次调用会刷新截止时间；
  `ai_service` 就是这么用的（聆听 4 s、思考/说话 20 s、出错 2.5 s）。
  本组件**不订阅** `sys_notify`，也没有内置的通知→表情映射表：谁想表达谁自己调。
- **策略不属于本组件**。什么时候进入待机（30 秒无触摸）、亮度降到多少，都由 **`ui_core`** 决定
  （见 `ui_core` README 的"待机策略"）：`ui_core` 收到 `SYS_EVENT_IDLE_TIMEOUT` 后切到
  `UI_PAGE_IDLE_FACE` 并调 `idle_face_set_brightness()`。本组件只负责**画**，
  `idle_face_set_brightness()` 也只是转发给 `th_bsp_set_brightness()`。
- **页面归属**：`UI_PAGE_IDLE_FACE` 由**本组件**通过 `idle_face_init()` → `ui_page_register()` 注册
  （`ui_pages` 的页面表里刻意没有它）；`idle_face` 页面显示时回到 `IDLE_FACE_NORMAL`。
  `idle_face_page_descriptor()` 用 `const void *` 暴露描述符，宿主不必包含 `ui_core` 的页面内部结构。
  `idle_face_attach()` 是幂等的，理论上也能把同一张脸嵌进别的页面。

## Kconfig

- `CONFIG_TODOHELPER_IDLE_EYE_WIDTH`（默认 150，60–300）
- `CONFIG_TODOHELPER_IDLE_EYE_HEIGHT`（默认 150，40–300）
- `CONFIG_TODOHELPER_IDLE_EYE_GAP`（默认 96，20–400）
- `CONFIG_TODOHELPER_IDLE_LINE_WIDTH`（默认 8，2–20）— 同时是 X 线的基准线宽

## 事件

`IDLE_FACE_EVENT_STATE_CHANGED`，payload 为 `idle_face_state_t`
（状态切换、临时表情开始/结束时都会发）。

## 测试方法

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build
idf.py -p COM34 flash monitor
```

期望串口输出：

```
I (xxx) TH:idle_face: ready (11 states)
I (xxx) TH:idle_face: idle face attached (150x150 eyes, gap 96)
```

（`state -> N` 是 DEBUG 级日志。）

手工验证：

1. 30 秒不触摸 → 进入全黑待机页，两只白线椭圆眼；亮度降到 NVS `idle_bright`（默认 20%）；
2. 观察 **3–8 秒**随机眨眼、**4–12 秒**随机左右看、缓慢呼吸（约 15 s 一个周期）；
3. 触碰屏幕 → `ui_core` 唤醒、亮度恢复，眼睛回到 `NORMAL`；
4. 触发 AI 对话（`ai_service`）→ 依次看到 LISTENING（眼睛放大）→ THINKING（单眼上移）→
   SPEAKING（节奏感）→ ERROR（X 眼）的过渡；**注意切换是渐变的，不应有跳变或内存抖动**
   （可对比切换前后的 `free_heap`）。
