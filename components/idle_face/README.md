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
| 待机过场 | `idle_face_standby_enter()`（闭眼入睡）、`idle_face_standby_exit()`（醒来睁眼） |
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
| `pupil` | 瞳孔可见度（0 = 无瞳孔；实际不透明度还会乘 `sqrt(open)`，闭眼时瞳孔自然消失） |
| `arc` | 眼睛轮廓弯成上弧（开心），同时略微加宽 |
| `cross` | 切成 X 形（错误） |
| `eye_dy` | 整只眼睛的垂直偏移（困倦下垂、思考上抬） |
| `width_scale` | 线宽倍数 |
| `speed` | **过渡档位**：`SPEED_FAST` → 交叉淡入 180 ms（眨眼要快）；其余 → 420 ms |

> 状态定义有**三处**必须同步修改：`idle_face.h` 的枚举、`idle_face.c` 的 `s_effects[]`、
> `docs/TODOHELPER_REQUIREMENTS.md` 的 FR6 / §11.2。头文件里对此有明确注释。

**只有一个 33 ms 动画器**（`lv_timer` → `idle_face_tick()`），全程不创建/销毁对象，因此
**切换表情与过场动画都不分配内存**。

## 表情切换是"交叉淡入"，不是瞬间跳变

旧实现只有**单个表情内部**的插值：`s_current` 每帧朝目标 `lerp()` 固定比例，切换表情时第一帧
就已经跳掉一大截。现在改为**有起止时间的交叉淡入**（`face_transition_t`）：

1. 每帧先算 `effective_state()`（眨眼 > 通知表情 > 基础状态）；一旦它变化，
   `begin_xfade()` 把**当前视觉值**冻结成 `from`，记下目标状态和时长；
2. 每帧用 `t = smoothstep(elapsed / duration)` 把 8 个参数从 `from` 重算到**目标表行**，
   而不是从"上一帧的值"再走一步——所以任何时刻开始过渡，起点都是屏幕上的真实画面；
3. 参数接近目标时直接吸附（`xfade_value()` 的 `snap`），确保过渡**有限时间结束**，
   不会永远停在 99%。

`smoothstep` 两端斜率为 0，所以既不会"起步猛冲"，也不会"结束卡顿"。
时长由目标状态的 `speed` 列决定（眨眼 180 ms，其余 420 ms）。

## 随机化：不让人看出周期

| 行为 | 参数（`src/idle_face.c` 顶部宏） | 说明 |
|---|---|---|
| 眨眼间隔 | `BLINK_MIN/MAX_MS` = 2600 / 7500 | 每次**重新抽**，不是固定 3–8 s |
| 眨眼时长 | `BLINK_DUR_MIN/MAX_MS` = 110 / 165 | 快眨和慢眨都有 |
| 连眨 | `BLINK_DOUBLE_PCT` = 18%，`BLINK_DOUBLE_MS` = 300 | 真人眨眼常成对出现 |
| 左右看间隔 | `LOOK_MIN/MAX_MS` = 4000 / 12000 | 保持时间 `LOOK_HOLD_MIN/MAX_MS` = 900–2000 ms 也随机 |
| 看的幅度 | — | 目标 `0.75…0.95` 随机，且**用 `s_look_dx` 缓动**而不是切状态 |
| 微扫视 | `SACCADE_MIN/MAX_MS` = 1800 / 5200 | 瞳孔 ±0.1 的随机漂移，保持 220–700 ms 后释放 |
| 呼吸周期 | `BREATH_MIN/MAX_MS` = 12000 / 19000，幅度 `BREATH_AMP` = 3% | **每个周期结束时重抽**，还在挂载时随机起始相位 |
| 同步转圈 | `SYNC_PERIOD_MS` = 1800 | `IDLE_FACE_SYNCING` 时瞳孔绕小圆 |
| 说话节奏 | `TALK_PERIOD_MS` = 620 | `IDLE_FACE_SPEAKING` 时上弧轻微起伏（**仍无嘴巴**） |

左右看**不再切换 `IDLE_FACE_LOOK_LEFT/RIGHT` 状态**，而是在表情参数之上叠一个
`dx` 偏移量（`s_look_dx`，每帧 0.16 收敛 ≈180 ms）。好处是：看到一半被打断（眨眼、通知表情）
时瞳孔从**当前位置**继续走，不会瞬移回中心；两个枚举值保留给外部 API/触发表使用。

## 进入 / 退出待机的过场

由待机页自己的生命周期钩子驱动，`ui_core` 不需要改动，也**不需要新增 UI 事件**：

| 时机 | 钩子 | 动画 |
|---|---|---|
| 进入待机（30 s 无触摸 → 切到 `UI_PAGE_IDLE_FACE`） | `idle_page_on_show()` → `idle_face_standby_enter()` | 眼睛在 **950 ms** 内以 `smoothstep` 闭合成 `SLEEPY` 半闭眼（同时整眼微微下垂、瞳孔淡出），然后**保持这个"睡着"的样子** |
| 唤醒（触摸/按键 → 离开待机页） | `idle_page_on_hide()` → `idle_face_standby_exit()` | 状态回 `NORMAL`，叠加一个 **700 ms** 的"睁眼过度"包络（峰值 +12% 开合度），像刚睡醒睁眼；随后继续随机眨眼 |

实现上是**一个有界包络**（`motion_env()` / `wake_env()`），加在表参数之上：

```
open_render = table.open × breath × motion_env(now) + wake_env(now)
```

`motion_env()` 只在入睡时从 1→0（并在闭眼期间把呼吸幅度也压掉，避免呼吸把睡着的眼睛又撑开），
`wake_env()` 是一个 `sin(πt)` 鼓包，两端为 0——所以**过场开始和结束都不会有跳变**。

两个函数都是**幂等且自保护**的：只有在"中性脸"（`NORMAL` 且没有通知表情在生效）时才会入睡；
`standby_exit()` 只在真的处于 `SLEEPY` 时才动。因此 AI 对话中突然待机、或表情覆盖期间进入待机，
都不会被这段过场打断。

> 注意：`ui_core` 在 `on_hide()` 之后立刻把页面容器隐藏，所以**退出动画的正常可见部分只有最初
> 几帧**（随后页面被隐藏）。这正是它被设计成"短促睁眼"而不是一整段序列的原因。若以后想让
> 整段唤醒动画可见，需要 `ui_core` 支持延迟隐藏——那是 `ui_core` 侧的事，本组件不依赖它。

## 每种视觉是怎么画出来的

| 视觉 | 实现 |
|---|---|
| 椭圆眼框 | 一个**只画边框**的圆角对象（`lv_obj_remove_style_all()` + `border_color` + `border_width` + `LV_RADIUS_CIRCLE`），背景透明、内部无子对象 |
| 开心上弧 | 同一个眼框对象把 `border_side` 改成 `LV_BORDER_SIDE_TOP`，高度取一半——下半弧不画即为"弯眼" |
| 瞳孔 | 眼框的**子对象**（所以跟着眼移动），圆形 + `bg_opa` 控制可见度，靠 `lv_obj_align(LV_ALIGN_CENTER, dx·travel_x, dy·travel_y)` 移动；`dx = 表.dx + 看的偏移 + 微扫视`，再截断到 ±1 |
| 错误 X 眼 | 每只眼两条 `lv_line`（画布上的两条对角线），靠 `line_opa` 显示/隐藏；此时眼框边框设为透明 |
| 同步中 | 不用 `dx/dy`，而是让相位走圆：`dx = 0.75·cos(2πφ)`、`dy = 0.75·sin(2πφ)`，φ 周期 1.8 s |
| 呼吸 | 在目标 `open` 上叠一层 `1 + 0.03·sin(2πφ)`，周期每个循环在 12–19 s 之间重抽 |

前景色用 `ui_theme.h` 的 `UI_COLOR_IDLE_FG`，背景是 `lv_color_black()`，所有对象都关掉
`LV_OBJ_FLAG_SCROLLABLE` / `LV_OBJ_FLAG_CLICKABLE`。

## 自动行为

- **随机眨眼**：每次间隔在 2.6–7.5 s 之间重抽，持续 110–165 ms，约 18% 概率立刻补一次（连眨）；
- **随机左右看**：间隔 4–12 s，保持 0.9–2.0 s 后瞳孔缓动回中；
- **微扫视**：1.8–5.2 s 一次，瞳孔 ±0.1 的漂移，保持 220–700 ms；
- 以上只在"普通脸"（`state_allows_idle_antics()`：仅 `NORMAL`）时生效，否则 `ERROR`/`THINKING`
  会被眨眼"眨回中性脸"。`SLEEPY` 被**故意排除**：那是睡着的样子，要一直保持到唤醒；
- 眨眼优先于左右看；随机数来自 `esp_random()`，时间基是 `esp_timer_get_time()`；
- 帧间 `dt` 取两次 tick 的差值并限制在 250 ms 内，计时器被拖慢也不会让动画卡住。

## 对外钩子与职责边界

- **`idle_face_express(state, ms)`** 是给其他组件用的钩子：临时显示 `state` 共 `ms` 毫秒
  （传 0 用默认 1600 ms），到点后自动恢复**之前**的状态。再次调用会刷新截止时间；
  `ai_service` 就是这么用的（聆听 4 s、思考/说话 20 s、出错 2.5 s）。
- **通知触发表**：本组件订阅 L0 `sys_notify()`，`s_triggers[]` 把通知名映射到表情与时长
  （`wifi_connected`→HAPPY、`wifi_failed`→ERROR、`sync_started`→SYNCING …）。
  生产者不需要知道小眼睛存在，加一条反应就是加一行表。
- **策略不属于本组件**。什么时候进入待机（30 秒无触摸）、亮度降到多少，都由 **`ui_core`** 决定
  （见 `ui_core` README 的"待机策略"）：`ui_core` 收到 `SYS_EVENT_IDLE_TIMEOUT` 后切到
  `UI_PAGE_IDLE_FACE` 并调 `idle_face_set_brightness()`——切页动作本身就会触发本组件的
  入睡/醒来过场。本组件只负责**画**。
- **页面归属**：`UI_PAGE_IDLE_FACE` 由**本组件**通过 `idle_face_init()` → `ui_page_register()` 注册
  （`ui_pages` 的页面表里刻意没有它）。
  `idle_face_page_descriptor()` 用 `const void *` 暴露描述符，宿主不必包含 `ui_core` 的页面内部结构。
  `idle_face_attach()` 是幂等的，理论上也能把同一张脸嵌进别的页面。

## 线程与锁定

动画器是 `lv_timer_create()` 创建的，回调本来就在 **LVGL 任务**里跑，所以**不需要加锁**，
也**不用** `esp_lv_adapter_lock(0)` 那种"试着拿一下"的写法（拿不到会打 ERROR 日志，见 `AGENT.md` §9.0.1）。
`idle_face_set_state()` / `idle_face_express()` 任意任务可调：它们只改静态变量并发事件，
真正的 LVGL 调用都发生在下一帧的 tick 里（tick 来自 LVGL 任务）。

## Kconfig

- `CONFIG_TODOHELPER_IDLE_EYE_WIDTH`（默认 150，60–300）
- `CONFIG_TODOHELPER_IDLE_EYE_HEIGHT`（默认 150，40–300）
- `CONFIG_TODOHELPER_IDLE_EYE_GAP`（默认 96，20–400）
- `CONFIG_TODOHELPER_IDLE_LINE_WIDTH`（默认 8，2–20）— 同时是 X 线的基准线宽

> 上面那批**动画时序**（眨眼/左右看/微扫视/呼吸/交叉淡入/入睡醒来）目前是 `src/idle_face.c`
> 顶部的 `#define` 常量，没有做成 Kconfig：它们属于调试时才会动的表现调参，
> 每加一个 Kconfig 都会进 `sdkconfig` 与 `-Werror` 的维护面。

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
I (xxx) TH:idle_face: ready (11 states, 8 triggers)
I (xxx) TH:idle_face: idle face attached (150x150 eyes, gap 96)
```

（`state -> N`、`standby enter/exit` 是 DEBUG 级日志。）

手工验证：

1. 30 秒不触摸 → 切到待机页：眼睛在约 1 s 内**缓缓闭上**并保持半闭（不再睁着眨眼），
   亮度降到 NVS `idle_bright`（默认 20%）；
2. 触碰屏幕 → 眼睛**睁一下**再恢复，同时 `ui_core` 唤醒、亮度恢复、回到待机前页面；
3. 在普通脸下观察：眨眼间隔不规律（偶尔连眨两下）、左右看幅度/时长不同、瞳孔偶有微抖动、
   呼吸节奏约 12–19 s 且每次略有差异；
4. 触发 AI 对话（`ai_service`）→ 依次看到 LISTENING（眼睛放大）→ THINKING（双眼上移）→
   SPEAKING（上弧起伏）→ ERROR（X 眼）的**渐变**切换，**不应有跳变**；
5. 触发 `sys_notify("sync_started")` → SYNCING 瞳孔绕圈 9 s，结束后自动回原表情；
6. 内存：整个过程 `free_heap` 不应有波动（切换表情与过场都不分配内存）。
