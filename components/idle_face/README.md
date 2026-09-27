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
| 待机过场 | `idle_face_standby_enter()`（交给待机页：**不闭眼**，重启行为时间线）、`idle_face_standby_exit()`（睁眼过冲一次 + 清困倦度） |
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
| 眨眼间隔 | `BLINK_MIN/MAX_MS` = 2600 / 7500 | 每次**重新抽**，不是固定 3–8 s；**困倦时最多缩短 35%** |
| 眨眼时长 | `BLINK_DUR_MIN/MAX_MS` = 110 / 165 | 快眨和慢眨都有；9% 概率抽成 240–380 ms 的"慢眨"；**困倦时最多加长 60%** |
| 眨眼深度 | `BLINK_PARTIAL_PCT` = 28% | 28% 的眨眼只闭到 40%–62%（眯一下），不是每次都全闭 |
| 连眨 | `BLINK_DOUBLE_PCT` = 18%，`BLINK_DOUBLE_MS` = 300 | 真人眨眼常成对出现 |
| 左右看间隔 | `LOOK_MIN/MAX_MS` = 4000 / 12000 | 保持时间 `LOOK_HOLD_MIN/MAX_MS` = 900–2000 ms 也随机 |
| 二连看 | `LOOK_DOUBLE_PCT` = 22% | 一次注视结束后，22% 概率立刻向**另一侧**再看一眼（幅度 55%–80%），像"扫视房间" |
| 看的幅度 | — | 目标 `0.75…0.95` 随机，且**用 `s_look_dx` 缓动**而不是切状态 |
| 微扫视 | `SACCADE_MIN/MAX_MS` = 1800 / 5200 | 瞳孔 ±0.1 的随机漂移，保持 220–700 ms 后释放 |
| 呼吸周期 | `BREATH_MIN/MAX_MS` = 12000 / 19000，幅度 `BREATH_AMP` = 3% | **每个周期结束时重抽**，还在挂载时随机起始相位 |
| 同步转圈 | `SYNC_PERIOD_MS` = 1800 | `IDLE_FACE_SYNCING` 时瞳孔绕小圆 |
| 说话节奏 | `TALK_PERIOD_MS` = 620 | `IDLE_FACE_SPEAKING` 时上弧轻微起伏（**仍无嘴巴**） |
| **自发表情** | `ANTIC_MIN/MAX_MS` = 13000 / 34000，时长 320–1100 ms | 见下"慢动作三层" |
| **瞳孔缩放** | `DILATE_MIN/MAX_MS` = 5000 / 11000，范围 0.91–1.14 | 缓慢随机游走（`DILATE_GAIN` = 0.05/帧） |
| **整脸漂移** | `DRIFT_X_PX` = 3.0、`DRIFT_Y_PX` = 2.0 | 两条不相干频率正弦合成，表情占用时缓动归零 |
| **困倦度** | `DROWSY_RISE_MS` = 150000（2.5 min 满） | 见下 |

左右看**不再切换 `IDLE_FACE_LOOK_LEFT/RIGHT` 状态**，而是在表情参数之上叠一个
`dx` 偏移量（`s_look_dx`，每帧 0.16 收敛 ≈180 ms）。好处是：看到一半被打断（眨眼、通知表情）
时瞳孔从**当前位置**继续走，不会瞬移回中心；两个枚举值保留给外部 API/触发表使用。

## 慢动作三层（1.1 新增：把"待机时看起来像静止图"彻底解决）

前四层（眨眼/注视/扫视/呼吸）周期都在几秒内，看一分钟就会觉得"就这几下"。所以在其上又加了
三层**秒到分钟级**的行为，全部是加法、全部有界、全部走同一条交叉淡入通道：

1. **自发表情 `s_antic_*`**：每 13–34 s 随机挑一个 320–1100 ms 的表情演一下，候选表就在
   `start_antic()` 里（HAPPY ×2 / SLEEPY / LISTENING / THINKING / SPEAKING —— 重复即权重）。
   - 它走 `effective_state()` 里**第二优先**的通道：`眨眼 > 外部表情(通知/AI) > 自发表情 > 基础状态`，
     所以任何真实表情都能立刻抢占，绝不打架；
   - 不会超过自己的时长（`tick_behaviour()` 到点就撤），也不会在注视/扫视飞行途中启动；
   - **自发表情期间不眨眼**（把睫毛砍在表情中间看起来是坏的），结束后重排眨眼计划；
   - 每个表情都会打一条 DEBUG 日志：`idle mood N for M ms (drowsy P%)`。
2. **困倦度 `s_drowsy`（0→1）**：无人交互时约 **2.5 分钟**累积到 1，效果是
   眨眼更密（间隔 ×0.65）、更慢（时长 ×1.6）、眼睛缩短最多 10%、视线下垂最多 5 px。
   - **自发表情不清零它**（那只是发呆，不是注意）——这条很关键，否则攒不起来（第一版就是每
     25 s 被清零一次，实测日志里 `drowsy` 一直是 0–3%）；
   - **任何真实交互清零**：`idle_face_express()`（通知/AI）、`idle_face_set_state()`、待机唤醒；
   - 待机中途不交互约 2.5 分钟后，脸会明显"困"下来；碰一下屏幕立刻清醒（配合退出过场）。
3. **瞳孔缩放 + 整脸漂移**：瞳孔 5–11 s 重抽一次 0.91–1.14 的目标值缓慢游走（LISTENING 再放大
   6%，像"注意了"）；两只眼睛**一起**做 ±3 px / ±2 px 的缓慢漂移，读起来像轻微转头。
   表情占用时漂移目标缓动归零，这样 ERROR 的 X 线（在 `apply_params()` 里和眼睛用同一组
   偏移对齐）不会和眼睛错位。

> 三层都只改已有对象的属性：**没有新对象、没有新定时器、没有分配**（除了 DEBUG 日志）。

## 连续/反射性动作四层（1.2 新增：把"事件之间完全静止"解决掉）

上面所有层都是**离散事件**——眨眼发生、注视发生、表情发生；事件之间眼睛是**完全静止**的，
所以盯久了仍能看出"在摆姿势"。真实的眼睛从不静止。这四层补上连续或反射性的动作，
全部是**加法**、全部**有界**、全部**零分配**（只扰动 `apply_params()` 已经算出的值）：

| 层 | 参数（`src/idle_face.c` 顶部宏） | 说明 |
|---|---|---|
| **连续视线游移** `s_wander_*` | `WANDER_X` = 0.16、`WANDER_Y` = 0.10（瞳孔行程比例） | 两轴 Lissajous 缓慢漂移叠加在瞳孔上。频率**故意不可通约**（0.26/0.41 与 0.19/0.33 rad/s），路径永不闭合。用缓动（gain 0.06）而非直接写入，所以表情占用时收拢、放开时也不跳 |
| **眼睑回弹** `s_settle_from_ms` | `SETTLE_OPEN` = 5.5%、`SETTLE_MS` = 260 | 眨眼睁开后眼睑**过冲再回落**。没有它眼睑到位即停，看着像机械快门。`sin(πt)` 鼓包，两端为 0 |
| **注意力提振** `s_perk_from_ms` | 间隔 `PERK_MIN/MAX_MS` = 17/46 s，时长 520 ms，`PERK_OPEN` = +18%、`PERK_PUPIL` = −10% | 眼睛放大 **同时** 瞳孔收缩，像"注意到什么"。不改表情状态，所以读起来是**反射**而不是情绪（这是它和自发表情的区别） |
| **眨眼不对称** `s_eye_lid[2]` | `BLINK_ASYM_PCT` = 30%，`BLINK_ASYM_MAX` = 0.30 | 30% 的眨眼两片眼睑深浅不同；**哪只眼更深是随机的**（否则不对称本身变成规律）。两只眼都夹在同一个 full/partial 档内，不会把全眨变成半眨 |

四层都在 `idle_face_set_state()` / `idle_face_express()` / `standby_enter()` / `standby_exit()`
里复位 —— 否则一个进行中的 perk 或 settle 会**把 ERROR 脸拉宽**。

> 实现上 `apply_params()` 现在按**每只眼**算高度（`eye_h_eye[2]`），瞳孔尺寸/行程也跟着
> 各自的眼睛走，所以一只眼半闭时瞳孔不会跑出眼框。

### 第二批：打破"两只眼一起动"（1.2）

上面所有层都是**对称**的（两眼同步）。这三层补上对称骨架做不到的事：

| 层 | 参数（`src/idle_face.c` 顶部宏） | 说明 |
|---|---|---|
| **呼吸联动** | `BREATH_EYE_DY` = 1.6 px | 呼吸的正弦除了缩放眼高，**同时**把整对眼上下抬降。复用已算好的 `breath_phase`，所以**不存在第二个时钟**，两者永远不会走散。幅度故意很小：再大就像整张脸在上下抖 |
| **双眼扫视错峰** | `SACCADE_SKEW` = 0.35、`SKEW_MIN/MAX_MS` = 25/90 | 微扫视触发时一只眼带一个**衰减的额外偏移**，约 60 ms 后跟上，所以两眼从不完全同步。**哪只眼迟到每次重抽**，否则同一只眼总慢半拍，这个规律本身就成了破绽 |
| **困倦点头** | 门闩 `NOD_MIN_DROWSY` = 0.55，间隔 8–26 s，概率 `NOD_PCT` = 45%，时长 520–900 ms | 困倦度够高后偶尔眼睑**慢慢合上再慢慢抬起**。这是困倦层的兑现，也是眨眼表达不了的东西（眨眼快、这个慢）。曲线**故意不对称**：前 35% 合上、后 65% 抬起，才像"抗不住睡意"而不是一次慢眨眼 |

**困倦点头为什么要三道门闩**：只按时间触发它会变成新的节拍（正是整个组件在避免的东西），
所以它同时要求 ① 困倦度 ≥ 0.55、② 上一轮之后过了 8–26 s、③ 本轮 45% 概率命中。
实测确认它**会等困倦度积累**：第一次触发在 `drowsy 70%`，第二次在 `86%`。

> 点头用 `nod_env()` 返回"额外闭合量"，在 `apply_params()` 里**最后**以乘法施加 ——
> 因为它是"眼睑赢了"的最强信号，perk 或呼吸都不该能把正在合上的眼睑重新撑开。

## 进入 / 退出待机的过场

由待机页自己的生命周期钩子驱动，`ui_core` 不需要改动，也**不需要新增 UI 事件**：

| 时机 | 钩子 | 动画 |
|---|---|---|
| 进入待机（30 s 无触摸 → 切到 `UI_PAGE_IDLE_FACE`） | `idle_page_on_show()` → `idle_face_standby_enter()` | **刻意不闭眼**：脸部保持 `NORMAL`，把眨眼/注视/扫视/呼吸/自发表情/瞳孔的时间线全部**从现在重排**，困倦度从 0 开始。于是待机画面是"活的"，然后随困倦度慢慢沉下去 |
| 唤醒（触摸/按键 → 离开待机页） | `idle_page_on_hide()` → `idle_face_standby_exit()` | 状态回 `NORMAL`，叠加一个 **420 ms** 的"睁眼过度"包络（峰值 +12% 开合度，峰在 ~200 ms），并清掉困倦度与当前自发表情 |

**为什么"进入不闭眼"**：第一版让眼睛闭合成 SLEEPY 姿态并整个待机期保持，那会**抑制眨眼/扫视**
—— 即让待机画面动画变少，与"丰富待机动画"的要求相反。想要"睡觉的脸"，按 `standby_enter()`
注释里那三行一起改（`s_state` / `begin_motion` / `s_current`）。

**退出动画的可见性（v1.2.0 起的变化，重要）**：曾经 `ui_core` 在 `on_hide()` 的同一帧就给
页面容器加 `LV_OBJ_FLAG_HIDDEN`，所以这段过场一帧都渲染不出来（旧版还因为"待机不再进入
SLEEPY"而恒返回 `ESP_ERR_INVALID_STATE`，属于死代码）；后来改成切页交叉淡入，靠
`UI_PAGE_ANIM_OUT_MS`（240 ms）的离场窗口把过场露出来。

**现在 `ui_core` 已经把整页动画全部删掉，切页改为同帧原子替换**（原因是那个动画会造成
上一页重影，详见 `AGENT.md §9.0.1.1` 第 5 条与 `CHANGELOG.md` 1.2.0）。因此：

- `wake_env()` 的睁眼过冲**仍然会执行**，但待机页在同一帧就消失了，所以它**在线下已经看不见**；
  这是为了切页正确性付出的代价，不是 bug。
- 唤醒时的"用户回来了"反馈现在由**回到的那个页面**承担。
- 若将来要让过冲重新可见，需要一个**不依赖整页遮挡**的做法（例如把过冲画在
  `lv_layer_top()` 的独立小对象上，或让待机页延迟一个短窗口再隐藏），
  **不要**重新引入整页 alpha 动画。

实现上是**一个有界包络**（`motion_env()` / `wake_env()`），加在表参数之上：

```
open_render = table.open × breath × motion_env(now) + wake_env(now)
```

`motion_env()` 只在入睡时从 1→0（并在闭眼期间把呼吸幅度也压掉，避免呼吸把睡着的眼睛又撑开），
`wake_env()` 是一个 `sin(πt)` 鼓包，两端为 0——所以**过场开始和结束都不会有跳变**。

`standby_enter()` 只在"中性脸"（`NORMAL` 且没有通知表情在生效）时才动作；
`standby_exit()` 要求没有**仍然有效**的表情在占用。两者都会先调 `override_expire_due()`
把**已经超时但还没被 tick 回收**的临时表情清掉 —— 动画器定时器是页面创建时才起的，
所以第一次进待机时可能看到一个几分钟前就该过期的 override（实测：t=7.7 s 的 2.5 s
`wifi_connected` 表情在 t=82 s 仍被当成"有效"，导致首次进待机被静默拒绝）。

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
