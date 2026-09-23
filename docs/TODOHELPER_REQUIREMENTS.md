# ToDoHelper 项目需求与组件化设计 v0.5

> **v0.5 变更（框架与 P0/P1/P2 代码已落地后回改，供接手者对照）**
>
> 本文档原为**设计意图**；实际实现完成后按"代码实际行为 > `AGENT.md` > 本文档"的原则回改。
> **完整的实现状态、未完成项与风险清单见 `docs/HANDOVER.md`；组件与 HTTP 接口清单见 `docs/API.md`。**
> 以下为**实现与 v0.4 设计的全部偏离**，正文未逐条改写的地方以本节为准：
>
> 1. **新增 L1 组件 `http_router`**（原 §7.12 让业务组件向 `sync_server` 反向注册路由）。
>    原因：把注册表放在 L4 会迫使 L3 业务组件向上依赖。现在业务组件向 L1 的 `http_router`
>    注册，`sync_server` 只负责起 httpd 并把 catch-all 分发器挂上。统一响应封装、
>    404/405/501 语义、鉴权钩子也一并集中在这里。
> 2. **新增 L2 组件 `ui_assets`**（原 §12 "复用 `spiffs_create_partition_assets` 流程"）。
>    `assets` 分区的打包调用放在该组件的 `CMakeLists.txt` 里，才能保证生成的
>    `mmap_generate_assets.h` 在被编译前就绪。同时新增**实测约束**：`esp_mmap_assets`
>    的打包脚本用 `os.listdir()` **只扫一层**，所以 `assets/ui/` 必须是**扁平目录**
>    （天气图标命名为 `w_sunny.png` 这种前缀形式），且**字体不能放这里**（改用 `EMBED_FILES`）。
> 3. **`UI_PAGE_IDLE_FACE` 由 `idle_face` 组件自己注册**，不在 `ui_pages` 的 `s_pages[]` 里
>    （§10.1 原表说"由 idle_face 组件接管"，这样 `ui_pages` 不必依赖 `idle_face`）。
> 4. **新增 L0 通知通道 `sys_notify()`**（原文档没有）。没有它，L3 想通知 UI/Web 就必须向上
>    依赖 L4。`idle_face` 的**表情自动触发**也建在它上面（通知名 → 表情 → 时长，一张表）。
> 5. **天气默认 provider = Open-Meteo（免 API Key）**，§17.2 #4 的选型未定；
>    provider 可插拔（`weather_provider_t` 两个函数指针），加和风/OpenWeather 只需加一个文件
>    加一行表项。**定位优先级第 2 档（IP 定位）已实现**，默认端点 `https://ipapi.co/json/`，
>    可用 NVS 键 `geo_url` 覆盖。
> 6. **WebSocket 用 SSE 代替**：推送端点是 `/api/v1/events`（Server-Sent Events），
>    `/ws/events` 保留为 **501 占位**。原因：本地组件镜像中**没有** `esp_websocket_client`。
>    §8.7 允许 SSE，且要求未实现返回 501——两者都满足。
> 7. **`sdkconfig.defaults` 强制 `CONFIG_LOG_MAXIMUM_LEVEL_VERBOSE=y`**：这不是偏好而是**必需**，
>    否则 esp32s31 + `SPIRAM_XIP_FROM_PSRAM` 下链接失败
>    （`sections.ld:1280 cannot move location counter backwards`）。原因与完整说明见
>    `AGENT.md` §9.1。运行时日志级别不变（`LOG_DEFAULT_LEVEL` 仍为 INFO）。
> 8. **`bsp` 门面符号前缀为 `th_bsp_`** 而非 §6 的 `bsp_display_*`：工程外 Korvo BSP 已占用
>    这些符号。设计意图（业务层不直接调 BSP）不变。
> 9. **设备端不支持中文输入**：屏幕键盘只有拉丁字母（无 IME）。中文待办 / AI 提问 / 城市名
>    请通过电脑浏览器或 HTTP API。这是系统限制而非遗漏，已在相关页面文案中说明。
> 10. **`sys_utils` 统一日志用 `esp_log_writev` + 运行时级别**，因此串口前缀固定为
>     `TH:<tag>`，且 `sys_log_set_level()` 才能运行时生效。
> 11. **分区表已重排**（§12 原表作废）：
>     `nvs 0x9000/0x6000` · `phy_init 0xf000/0x1000` · `factory 0x10000/0x800000` ·
>     `storage(data,littlefs) 0x810000/0x200000` · `assets(data,spiffs) 0xa10000/0x5f0000`。
>     应用名由 `esp32_s31_korvo_factory_demo` 改为 **`todohelper`**。
> 12. **中文字体方案**：FreeType + 内嵌 GB2312 一级子集（`components/ui_core/fonts/ui_cjk.ttf`，
>     约 1.2 MB，`tools/gen_cjk_font.py` 生成），回退链为
>     SD 字体 → 内嵌子集 → LVGL 内置 Source Han Sans SC 16px → Montserrat。§9.2/§12 中
>     "字体放 SD 或资源分区"的取舍现在是"**内嵌进 app**"，因为字体无法走 mmap 素材管线。
> 13. **FR6 的 11 个状态全部实现**（不止 P0 的 NORMAL/BLINK），且由**一张参数表**驱动；
>     §11.3 建议的 `lv_canvas` 未采用，改用"仅描边的圆角对象 + `LV_BORDER_SIDE_TOP` 弧 +
>     两条 `lv_line` 组成 X 眼 + 子对象瞳孔"，好处是零位图缓冲、可被 LVGL 动画平滑插值。
> 14. **FR7 的 ASR/TTS 只做到"可配置 HTTP 端点"**：录音与回放是真实可用的，
>     但 ASR/TTS 选型（§17.2 #6/#7）未定，故只定义了协议（见 `docs/API.md`）。
>     三条降级路径（无麦克风→键盘/HTTP，无喇叭→仅上屏，无网络→提示）全部实现。
> 15. **实现顺序**：P0 → P1 → P2 的代码均已完成；**但从未烧录到真机**，
>     运行期行为零验证。首次上机清单见 `docs/HANDOVER.md` §5。

> v0.4 变更（对齐 `AGENT.md` 实测基线）：
> 1. 已阅读根目录 `AGENT.md`，§4 硬件基线由"全部待确认"改为**实测值回填**（仅剩天气 API、词库来源、屏幕驱动 IC 型号、MIC/扬声器型号待定）；
> 2. 修正 v0.3 中的入口文件名错误（`AGENT.MD` → `AGENT.md`）并删除"未读取 AGENT.MD"的过期声明；
> 3. 补齐 §7 缺失的 `sync_server.h` / `sys_utils.h` 两节 API，并说明错误码与 `esp_err_t` 的转换关系；
> 4. §8 增加鉴权方案与"HTTP 状态码 vs 响应体 code"语义说明；
> 5. FR3/FR4/§12 明确标注 GPS、RTC **当前硬件不存在**；新增 **GPIO20~25 与 SPI NAND flash 复用**约束；
> 6. 明确"程序化绘制优先 / SD 素材可选"的取舍规则、复用现有 `coffee_pjpg` 资源分区方案；
> 7. 对齐 `ui_page_id_t` 与 §10.1 页面列表、`idle_face_state_t` 与 FR6 状态列表；
> 8. §16 约定产物 `docs/API.md` 由"隐含"改为显式要求（当前尚未创建）。

> 文档路径：`docs/TODOHELPER_REQUIREMENTS.md`。本文件可作为其他 Agent 的入口文档。
> **已阅读并核对根目录 `AGENT.md`（项目现有驱动与环境的权威描述）。**
> **⚠️ 实现已完成但未经真机验证；接手前请先读 `docs/HANDOVER.md`。**
> 本文档中涉及已有驱动、引脚、屏幕参数的描述来自 AGENT.md 的实测结论；若与代码实际不符，以**代码与 AGENT.md** 为准并回改本文档。

---

## 0. 文档说明

- 项目名：ToDoHelper
- 目标平台：**ESP32-S31（IDF 目标名 `esp32s31`，RISC-V）**，开发板为 ESP32-S31-Korvo
- 开发框架：ESP-IDF（本机 **v6.1.0**）
- UI：LVGL **9.5.0**（`managed_components/lvgl__lvgl`）+ `espressif__esp_lvgl_adapter` 0.5.3（已打本地补丁）
- 显示/触摸：800×480 RGB 16-bit 并口 + GT1151 电容触摸（详见 §4）
- 存储：NVS + SD 卡（SDMMC 4-bit，`/sdcard`）；SD 不可用时回退内置文件系统（LittleFS）
- 音频：已在硬件上具备（I2S + Codec 录制/回放），FR7 语音输入输出具备基础条件
- 同步：局域网 HTTP Server + Web 页面，后续可扩展 MQTT

> **工程现状（务必先理解）**：本仓库当前内容是 **ESP32-S31-Korvo 产测固件（factory_demo）**，
> 即"LVGL 咖啡机 UI + 摄像头预览 + 音频录制回放 + 按键/WS2812/USB HID + SD 卡探测"，
> 而不是一个待改造的 ToDoHelper 雏形。因此本项目的实际起点是：
>
> - **可复用**：板级 BSP（工程外 `..\common_components\esp32_s31_korvo`）、LVGL 适配层与显示骨架
>   （`esp_lvgl_adapter` 注册显示/触摸、撕裂控制、PPA 加速、显示任务栈 16KB）、
>   分区表与资源打包流程（`coffee_pjpg` + `spiffs_create_partition_assets` + `esp_mmap_assets`）、
>   各驱动的初始化范例（音频、SD、按键、LED、摄像头）。
> - **需重建**：`ui/`（SquareLine 导出的咖啡机 12 屏）与 `main/` 的业务编排，全部替换为 ToDoHelper 页面与业务组件。
> - **禁止修改**：工程外的 Korvo BSP（见 §17 与 `AGENT.md` 第 9 节）。
>
> 结论：§14 的 P0 工作量**不包含**底层驱动开发，但**包含**从咖啡机 UI 切换到 ToDoHelper 业务框架的重构。

本文档目标：
1. 明确功能范围与验收标准；
2. 明确组件划分、依赖关系与 API；
3. 方便其他 Agent 按模块并行开发；
4. 记录待确认硬件信息与需要用户物理协助的事项；
5. 作为需求侧与实现侧（`AGENT.md`）的对照基准，避免两份文档再次分叉。

**重要**：开始任何代码修改前，请先阅读项目根目录 **`AGENT.md`** 和本文档。若两者冲突，以 `AGENT.md` 中的已有驱动与硬件约束为准，并更新本文档。

**文档优先级**：代码实际行为 > `AGENT.md`（已实测确认） > 本文档（需求与设计意图）。发现差异时以左侧为准，并回改本文档。

**不可丢失的前置条件**（详见 `AGENT.md` 第 7 节）：
`patches/esp_lvgl_adapter_idf61_support.patch` 是本工程编译的前提。只要 `managed_components/espressif__esp_lvgl_adapter` 被重新解压或下载（删除该目录、删除整个 `managed_components/`、改动组件版本约束），就会重新出现
`fatal error: esp_async_fbcpy.h: No such file or directory`。此时需重新 `patch -p1 < patches/esp_lvgl_adapter_idf61_support.patch` 并 `idf.py reconfigure && idf.py build`。

---

## 1. 项目概述

基于乐鑫官方 demo 工程与已有驱动，摒弃原 app 业务代码，保留底层驱动，实现一个运行在 ESP32 上的桌面助手：

> 上述"摒弃原 app 业务代码、保留底层驱动"已与 `AGENT.md` 核对：原 app 业务代码即 factory_demo 的工厂自检界面与咖啡机 UI，
> 底层驱动位于工程外 Korvo BSP（只读）。详细现状与复用清单见 §0 末尾的"工程现状"。

1. WiFi 扫描与连接；
2. 待办清单，支持电脑端更新、设备端勾选；
3. 天气、温度、地理位置；
4. 日历；
5. 单词背诵；
6. 全黑背景白色线条眼睛待机动画；
7. UI 适配当前屏幕与触摸；
8. 代码组件化、模块化、低耦合；
9. 提供内部 C API 与 HTTP API 文档，方便多 Agent 协作。

---

## 2. 功能需求

### FR1 WiFi 扫描与连接

- 扫描当前环境 WiFi 与热点；
- 显示 SSID、RSSI、加密方式；
- 触摸选择 WiFi；
- 输入密码并连接；
- 状态：未连接 / 连接中 / 已连接 / 失败；
- 保存已连接 WiFi，开机自动重连；
- 支持断开、重新扫描、切换网络；
- 显示 IP、RSSI、MAC；
- 支持 mDNS：`todohelper.local`。

### FR2 待办清单 Todo

- 设备端显示待办列表；
- 支持勾选完成 / 取消完成；
- P0：显示、勾选、本地持久化；
- P1：设备端新增、删除、编辑；
- 电脑通过浏览器或 API 更新待办；
- 设备端勾选后电脑端可见；
- 同步状态：已同步 / 待同步 / 同步失败；
- 断网可查看和勾选，联网后自动同步；
- 推荐局域网 HTTP Server + Web 页面；
- 后续可扩展 MQTT / 云同步。

### FR3 天气、温度、地理位置

- 获取天气、温度、天气状况；
- 获取城市、经纬度；
- 显示城市、温度、体感温度、湿度、天气图标、更新时间；
- 定时刷新，默认 15 分钟；
- 断网显示上次缓存；
- 定位优先级：
  1. 手动设置城市（**P0 唯一可用路径**）；
  2. IP 定位（P1）；
  3. ~~GPS / 模块定位~~ —— **当前硬件不存在 GPS 模块，本项不支持**；若后续更换硬件再扩展；
- 天气 API 待选型：和风、OpenWeather、心知等（见 §17）。
- **硬件说明**：本节所有信息均来自网络 API，**不依赖任何本地传感器**；设备无温度传感器，温度一律取天气 API 的返回值。

### FR4 日历

- 显示当前日期、时间、星期；
- 月历显示，支持上一月 / 下一月；
- NTP 网络时间同步；
- 时区设置；
- 可选显示当天待办；
- ~~若有 RTC，可断电保持~~ —— **当前硬件无独立 RTC，本项不支持**。
- **时间来源**：开机后依赖 NTP 同步；在 NTP 同步成功前
  `sys_time_now_ts()` 返回 0（见 §7.13），UI 必须显示"时间未同步"而不是显示 1970 年；
  断电后时间**不保持**，每次上电需重新同步。
- 时区与 NTP 服务器地址存 NVS（键名见 §9.1），支持手动设置时区偏移。

### FR5 单词背诵

- 显示单词、音标、释义、例句；
- 翻卡：先单词，后释义；
- 标记：认识 / 模糊 / 不认识；
- 记录学习进度与熟悉度；
- 每日背诵数量可设置；
- 词库存于 SD 卡或 LittleFS；
- 可选发音、复习算法、错词本。

### FR6 待机眼睛动画

- 全黑背景；
- 白色纯色简约线条眼睛；
- 类似表情动画；
- 状态包括：普通、眨眼、左右看、开心、困倦、错误、同步中、思考、聆听、**说话**（P2）；
  > 状态的权威定义见 §7.9 的 `idle_face_state_t`（11 个值），触发条件与表现见 §11.2 状态机表，三处必须保持一致。
- **表情之间要平滑过渡**（交叉淡入，不跳变）；眨眼/左右看/呼吸的节奏要随机化，不能有固定周期；
- 进入待机和唤醒时都要有短过场动画（闭眼入睡 / 睁眼醒来）；
- **只画眼睛，不要嘴巴**；**全程程序化绘制，零位图、不依赖 SD 卡素材**（用户明确要求）；
- 无触摸 30 秒进入待机；
- 触摸或系统事件唤醒；
- 待机时降低亮度，但 WiFi / Todo 同步继续；
- 素材可放 SD 卡，也支持程序化矢量绘制。

### FR7 AI 语音对话助手（P2，其他功能完成后引入）

- ESP32 联网后调用 DeepSeek API（deepseek-chat / deepseek-reasoner）进行多轮对话；
- 语音输入：麦克风采集语音，支持按键唤醒或触摸唤醒（语音识别方案待选型：本地离线 ASR、云端 ASR、或 DeepSeek 直接处理文本）；
- 输入方式降级：无麦克风时支持屏幕键盘 / HTTP API 文本输入；
- 输出方式：
  1. 音频播报（TTS 方案待选型：本地 TTS 或云端 TTS）；
  2. 屏幕上以聊天气泡形式显示文字；
  3. 待机小眼睛联动：聆听时眼睛放大（LISTENING）、思考时单眼向上看（THINKING）、回答时开心（HAPPY）、出错时错误眼（ERROR）；
- 上下文管理：保留最近 N 轮对话（默认 10 轮），支持"清除对话"与单次问答模式；
- API Key 保存于 NVS，不硬编码，支持通过设置页或 HTTP API 配置；
- 对话记录可选保存到 SD 卡（/sdcard/todohelper/ai/chat_history.json），可设置不保存；
- 支持自定义 system prompt（角色设定），如"你是一个桌面学习助手"；
- 支持联网查询类回答与设备控制类指令的扩展：后续可解析"帮我添加待办 XXX"等指令联动 todo_manager；
- 断网时 AI 功能不可用并给出提示，不影响其他功能；
- 流式输出：优先流式接收并逐字上屏，超时或非流式时一次性显示；
- 隐私说明：语音与文本仅在用户触发时上传。

**FR7 硬件可行性（v0.4 补充）**：

| 环节 | 硬件/驱动现状（来自 `AGENT.md`） | 结论 |
|---|---|---|
| 录音 | **已具备**：I2S + `esp_codec_dev` 麦克风句柄，现有工程已实现 48 kHz/16 bit/2ch 录制 | 可直接复用，无需新驱动 |
| 回放 | **已具备**：Codec 扬声器句柄，现有工程已实现回放；`PA_CTRL=GPIO7` | 可直接复用 |
| MIC 型号 | **未知** | 阻塞**离线 ASR** 可行性判断（见 §17.2） |
| 喇叭/功放 | **未确认是否已装** | 阻塞 TTS 播报，若未装则降级为"仅屏幕上屏" |
| 算力/内存 | ESP32-S31 + PSRAM（250 MHz，XIP_FROM_PSRAM） | 具备云端 ASR/TTS 的缓冲余量；离线 ASR 需实测 |

**降级路径（必须实现）**：麦克风不可用 → 屏幕键盘 / HTTP API 文本输入；
喇叭不可用 → 仅屏幕气泡显示，跳过 SPEAKING 状态的音频部分；
网络不可用 → AI 功能给出提示，不影响其他功能。

---

## 3. 非功能需求

- UI 必须适配屏幕分辨率、方向、触摸区域；
- LVGL 使用 flex / grid，避免大量绝对坐标；
- 中文字体按屏幕裁剪，避免 Flash / RAM 过大；
- 业务层不直接操作底层寄存器；
- 驱动统一由 BSP 封装；
- 网络、存储、UI、业务逻辑解耦；
- 支持断网、重连、错误提示；
- 任务优先级合理，避免看门狗超时；
- 配置保存到 NVS；
- 日志分级；
- 所有组件提供 README.md 与 API 头文件；
- 修改公共接口必须更新本文档。

---

## 4. 硬件与软件基线

本节已按 `AGENT.md`（第 2、3、5 节）的**实测结论**回填。仅剩 4 项待确认，已在下表显式标出。

| 项目 | 状态 / 实测值 |
|---|---|
| 芯片型号 | **esp32s31**（ESP32-S31，RISC-V，IDF preview 目标）。工具链 `riscv32-esp-elf esp-15.2.0_20251204` |
| 开发板 | **ESP32-S31-Korvo**（来源 `espressif/esp-dev-kits` 的 `examples/esp32-s31-korvo/examples/factory_demo`） |
| 屏幕分辨率 | **800×480** |
| 屏幕驱动 IC | **待确认**：走 `esp_lcd_new_rgb_panel` RGB 并口直驱，未使用 SPI 控制器 IC；面板 IC 型号未在 AGENT.md 中记录 |
| 屏幕接口 | **16-bit RGB 并口**；PCLK 26 MHz；H: BP40 / FP20；V: BP10 / FP5；背光/使能脚为 NC |
| 触摸 IC | **GT1151**（`esp_lcd_touch_new_i2c_gt1151`） |
| 触摸接口 | **I2C**，SDA=**GPIO0** / SCL=**GPIO1**；RST/INT 为 NC |
| LVGL 版本 | **9.5.0**；适配层 `espressif__esp_lvgl_adapter` 0.5.3（**已打本地补丁，见 §0**） |
| PSRAM | **有**：250 MHz + `SPIRAM_XIP_FROM_PSRAM` |
| SD 卡 | **有**：**SDMMC 4-bit**，D0-3=GPIO20~23、CLK=GPIO24、CMD=GPIO25；电源开关 GPIO39（低有效）；挂载点 `/sdcard`、HIGHSPEED、16KB alloc |
| 音频 | **有**：I2S，默认 48 kHz / 16 bit / 2ch，`esp_codec_dev` 双句柄（MIC + Speaker 均已实现录制/回放）；MCLK=GPIO2、SCLK=GPIO3、LRCLK=GPIO4、DSIN=GPIO6、SDOUT=GPIO5、PA_CTRL=GPIO7 |
| MIC / 扬声器型号 | **待确认**：硬件与驱动能力已具备，但麦克风型号（数字/模拟）与是否带功放未记录，无法判定离线 ASR 可行性（影响 FR7，见 §17） |
| 按键 | **有**：ADC 单线梯形 **4 键**（GPIO42），VOLUP 0.38 V / VOLDOWN 0.82 V / MODE 1.34 V / SET 1.87 V，使用 `espressif/button` |
| LED | **有**：WS2812 单颗状态灯，**GPIO37**（RMT / `led_strip`） |
| RTC | **无**（当前硬件不存在独立 RTC，FR4 的断电保持不可实现） |
| GPS / 定位模块 | **无**（当前硬件不存在，FR3 的定位只能走"手动城市 / IP 定位"，见 FR3） |
| 摄像头 | **有**（本阶段不使用）：DVP + SCCB，8-bit（D0-7=GPIO46~53、PCLK=54、XCLK=55、VSYNC=56、HSYNC=57），XCLK 20 MHz，OV3660 / SC101 |
| 已有驱动接口 | **见 `AGENT.md` 第 2.1 / 2.2 节**（RGB 屏、触摸、摄像头、音频、SD、按键、WS2812 均已实现并有 BSP 封装） |
| 天气 API | **待选型**：和风 / OpenWeather / 心知等 |
| 词库来源 | **待确认** |
| 板级 BSP | 工程外 `D:\project\ESP-IDFv6.1\common_components\esp32_s31_korvo`，**只读，禁止修改** |

**确认来源与时效**：以上实测值来自 `AGENT.md` 第 2、3 节的记录，未在本轮逐条复核代码；
烧录/调试串口为 **COM34**（Silicon Labs CP210x），`COM26`(CH340) 不是本板。

**必须遵守的硬件约束**：

1. **GPIO20~25 与 SPI NAND flash 脚位复用，同一块板只能装其中一个。**
   即：使用 SD 卡（占用 GPIO20~25）的板子不能同时使用 SPI NAND flash，反之亦然。
   这直接决定 §12 的存储方案与 §9.2 的素材存放位置，故任何"把字体/词库/动画放进 flash 分区"的方案都必须先确认该板是否装了 SD 卡。
2. 屏幕与触摸为 RGB 并口 + I2C，显示链路已由 `esp_lvgl_adapter` 接管（撕裂控制 `TEAR_AVOID_MODE_TRIPLE_PARTIAL`、PPA 加速、LVGL buffer 高 160、显示任务栈 16 KB），ToDoHelper 的 UI 必须走同一链路，不得自建显示初始化。
3. 摄像头可暂不使用，但注意其占用共享 I2C，原 `main.c` 的初始化顺序为先占用共享 I2C 再启动显示。

---

## 5. 总体架构

```text
+------------------------------------------------------+
|                      main/app_main.c                 |
|               初始化、组件编排、事件分发              |
+------------------------------------------------------+
| ui_core | ui_pages | idle_face | bsp                  |
+------------------------------------------------------+
| wifi_manager | todo_manager | sync_server             |
| weather_service | calendar_service | word_service     |
| ai_service | voice_service |                         |
+------------------------------------------------------+
| storage_manager | sys_utils | nvs | lv_fs             |
+------------------------------------------------------+
| ESP-IDF: WiFi, HTTP, NTP, FATFS, LittleFS, LVGL 9.5  |
| esp_lvgl_adapter（显示/触摸适配，已打补丁）           |
| Korvo BSP（工程外，只读）：LCD / GT1151 / audio / SD  |
|            / button / led_strip / video              |
+------------------------------------------------------+
| 硬件：800x480 RGB 屏 + GT1151 触摸 + SD(SDMMC)       |
|       + I2S Codec + ADC 按键 + WS2812 + 摄像头       |
+------------------------------------------------------+
```

原则：

- main 只做初始化和协调；
- UI 不直接发 HTTP，调用 service；
- service 不直接操作 LVGL，通过事件通知 UI；
- 驱动只由 BSP 暴露（本项目的 `bsp` 是工程外 Korvo BSP 的**薄封装**，见 §4/§6）；
- 组件间通过公开头文件和 esp_event 通信；
- 显示初始化与 LVGL 集成已由 `esp_lvgl_adapter` 完成，业务层**不得重复初始化**显示/触摸。

---

## 6. 组件划分与依赖

建议目录：

```text
components/
  bsp/                 # 薄封装层，包装工程外 Korvo BSP（只读）
  ui_core/
  ui_pages/
  idle_face/
  wifi_manager/
  todo_manager/
  sync_server/
  weather_service/
  calendar_service/
  word_service/
  ai_service/          # P2
  voice_service/       # P2
  storage_manager/
  sys_utils/
main/
  app_main.c
docs/
  TODOHELPER_REQUIREMENTS.md   # 本文档
  API.md                       # 各组件交付后追加接口说明与测试方法（见 §16，当前尚未创建）
```

依赖关系：

| 组件 | 依赖 | 说明 |
|---|---|---|
| bsp | ESP-IDF 驱动、LVGL port | **现状**：板级 BSP 位于工程外 `..\common_components\esp32_s31_korvo`（只读）。本工程的 `components/bsp/` 只做**薄封装/门面**（暴露 `bsp_display_*`、`bsp_audio_*`、`bsp_sdcard_*`、`bsp_led_*`、`bsp_button_*` 给业务层），**不得复制或修改** BSP 本体 |
| ui_core | bsp、LVGL | 复用现有 `esp_lvgl_adapter` 显示/触摸注册与撕裂控制，不自建显示初始化 |
| ui_pages | ui_core、各 service | 需替换现有 SquareLine 咖啡机 UI（`ui/`） |
| idle_face | ui_core、LVGL | 程序化绘制优先（见 §11.1） |
| wifi_manager | sys_utils、NVS | |
| todo_manager | storage_manager、sys_utils | |
| sync_server | todo_manager、wifi_manager、storage_manager | API 见 §7.12 |
| weather_service | wifi_manager、storage_manager、HTTP | |
| calendar_service | wifi_manager、NTP | **注意**：无 RTC 硬件，断电不保持 |
| word_service | storage_manager | |
| ai_service | wifi_manager、storage_manager、HTTP、JSON、sys_utils | P2 |
| voice_service | bsp（音频）、storage_manager、ai_service（可选） | P2；音频硬件已具备 |
| storage_manager | NVS、FATFS、LittleFS、SD | 挂载点 `/sdcard`；注意 GPIO20~25 复用约束（§4） |
| sys_utils | 日志、事件、时间、配置 | API 见 §7.13 |

---

## 7. 内部 C API 文档

所有组件公开头文件放 `include/`，实现放 `src/`。以下为建议接口，其他 Agent 可基于此并行开发。

约定：

- 所有 API 返回 `esp_err_t`；`todohelper_err_t`（§7.1）只用于**内部细分原因**，通过下述映射转成 `esp_err_t` 返回；
- 组件对外只暴露 `include/` 下的头文件，跨组件不得调用 `static` 函数；
- 组件间异步通信统一走 `esp_event`（事件基址见 §7.1，各组件事件见对应小节）；
- 本节的接口是**设计提案**；组件实现完成后，实际签名与测试方法以 `docs/API.md` 为准（见 §16）。

### 7.1 错误码与事件基址

```c
#define TODOHELPER_ERR_BASE 0x10000

typedef enum {
    TODOHELPER_OK = 0,
    TODOHELPER_ERR_INVALID_ARG,
    TODOHELPER_ERR_NO_MEM,
    TODOHELPER_ERR_NOT_FOUND,
    TODOHELPER_ERR_STORAGE,
    TODOHELPER_ERR_NETWORK,
    TODOHELPER_ERR_TIMEOUT,
    TODOHELPER_ERR_BUSY,
    TODOHELPER_ERR_NOT_SUPPORTED,
} todohelper_err_t;

ESP_EVENT_DECLARE_BASE(TODOHELPER_EVENT);
```

**错误码与 `esp_err_t` 的关系（重要）**：

ESP-IDF 的 `esp_err_t` 是 `int`，不是枚举，因此上面这个枚举**不能直接当作 `esp_err_t` 返回**，
必须做一次显式映射；同时 `0x10000` 这类值已超出 `esp_err_t` 的宏语义范围
（IDF 的 `esp_err_t` 宏只覆盖 `0x101~0x10F` 与 `ESP_FAIL=-1`）。

推荐做法：**公共组件 API 一律复用 IDF 标准错误码，本枚举只在需要细分原因时使用**。

| `todohelper_err_t` | 对外返回的 `esp_err_t` |
|---|---|
| `TODOHELPER_OK` | `ESP_OK` |
| `TODOHELPER_ERR_INVALID_ARG` | `ESP_ERR_INVALID_ARG` |
| `TODOHELPER_ERR_NO_MEM` | `ESP_ERR_NO_MEM` |
| `TODOHELPER_ERR_NOT_FOUND` | `ESP_ERR_NOT_FOUND` |
| `TODOHELPER_ERR_TIMEOUT` | `ESP_ERR_TIMEOUT` |
| `TODOHELPER_ERR_STORAGE` | `ESP_ERR_INVALID_STATE`（并 `ESP_LOGE` 说明存储原因） |
| `TODOHELPER_ERR_NETWORK` | `ESP_ERR_INVALID_RESPONSE`（或 `ESP_ERR_HTTP_*`） |
| `TODOHELPER_ERR_BUSY` | `ESP_ERR_INVALID_STATE`（或 `ESP_ERR_TIMEOUT`） |
| `TODOHELPER_ERR_NOT_SUPPORTED` | `ESP_ERR_NOT_SUPPORTED` |

若确需对外区分本项目特有的错误原因，应基于 IDF 允许的自定义方式定义，例如：

```c
#include "esp_err.h"

/* IDF 允许 >= 0x10000 的自定义错误码（esp_err_t 为 int，只看高字节不为 0 即视为错误） */
#define TODOHELPER_ERR_BASE          0x10000
#define TODOHELPER_ERR_STORAGE_IO    (TODOHELPER_ERR_BASE + 1)
#define TODOHELPER_ERR_NET_OFFLINE   (TODOHELPER_ERR_BASE + 2)
```

并在本文档与 `docs/API.md` 中登记每个自定义值，避免不同组件抢用同一编号。

### 7.2 wifi_manager.h

```c
typedef struct {
    char ssid[33];
    int8_t rssi;
    wifi_auth_mode_t authmode;
    uint8_t channel;
} wifi_ap_info_t;

typedef enum {
    WIFI_MGR_DISCONNECTED,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,
    WIFI_MGR_FAILED,
} wifi_mgr_state_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_scan(wifi_ap_info_t *aps, size_t max, size_t *count);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_disconnect(void);
esp_err_t wifi_manager_get_state(wifi_mgr_state_t *state);
esp_err_t wifi_manager_get_ip(char *ip, size_t len);
esp_err_t wifi_manager_save_config(const char *ssid, const char *password);
esp_err_t wifi_manager_auto_connect(void);
```

事件：

- `WIFI_MGR_EVENT_SCAN_DONE`
- `WIFI_MGR_EVENT_CONNECTED`
- `WIFI_MGR_EVENT_DISCONNECTED`
- `WIFI_MGR_EVENT_GOT_IP`
- `WIFI_MGR_EVENT_FAILED`

### 7.3 todo_manager.h

```c
typedef enum {
    TODO_SYNC_NONE,
    TODO_SYNC_PENDING,
    TODO_SYNC_OK,
    TODO_SYNC_FAILED,
} todo_sync_state_t;

typedef struct {
    uint32_t id;
    char title[128];
    char desc[256];
    int64_t due_ts;
    bool done;
    uint8_t priority;
    int64_t updated_at;
    todo_sync_state_t sync_state;
} todo_item_t;

esp_err_t todo_manager_init(void);
esp_err_t todo_add(const todo_item_t *item, uint32_t *out_id);
esp_err_t todo_update(uint32_t id, const todo_item_t *item);
esp_err_t todo_set_done(uint32_t id, bool done);
esp_err_t todo_delete(uint32_t id);
esp_err_t todo_get(uint32_t id, todo_item_t *out);
esp_err_t todo_list(todo_item_t **items, size_t *count);
void todo_free_list(todo_item_t *items);
esp_err_t todo_export_json(char **json, size_t *len);
esp_err_t todo_import_json(const char *json);
```

事件：

- `TODO_EVENT_ADDED`
- `TODO_EVENT_UPDATED`
- `TODO_EVENT_DONE_CHANGED`
- `TODO_EVENT_DELETED`
- `TODO_EVENT_SYNC_CHANGED`

### 7.4 weather_service.h

```c
typedef struct {
    char city[64];
    double lat;
    double lon;
    float temp;
    float feels_like;
    int humidity;
    char condition[64];
    int64_t update_ts;
    bool valid;
} weather_data_t;

esp_err_t weather_service_init(void);
esp_err_t weather_refresh(void);
esp_err_t weather_get(weather_data_t *out);
esp_err_t weather_set_city(const char *city);
esp_err_t weather_load_cache(void);
esp_err_t weather_save_cache(void);
```

### 7.5 calendar_service.h

```c
esp_err_t calendar_service_init(void);
esp_err_t calendar_sync_ntp(void);
esp_err_t calendar_get_now(struct tm *out);
esp_err_t calendar_get_month(int year, int month, struct tm *days, size_t *count);
esp_err_t calendar_set_timezone(const char *tz);
```

### 7.6 word_service.h

```c
typedef enum {
    WORD_REVIEW_UNKNOWN,
    WORD_REVIEW_FUZZY,
    WORD_REVIEW_KNOWN,
} word_review_result_t;

typedef struct {
    uint32_t id;
    char spelling[64];
    char phonetic[64];
    char meaning[256];
    char example[256];
    uint8_t level;
    int64_t next_review_ts;
} word_item_t;

esp_err_t word_service_init(void);
esp_err_t word_load_from_sd(const char *path);
esp_err_t word_get_today(word_item_t *items, size_t max, size_t *count);
esp_err_t word_next(word_item_t *out);
esp_err_t word_review(uint32_t id, word_review_result_t result);
esp_err_t word_save_progress(void);
```

### 7.7 storage_manager.h

```c
esp_err_t storage_manager_init(void);
esp_err_t storage_mount_sd(void);
esp_err_t storage_mount_internal_fs(void);
const char *storage_get_sd_mount(void);
esp_err_t storage_read_file(const char *path, void *buf, size_t *len);
esp_err_t storage_write_file(const char *path, const void *buf, size_t len);
esp_err_t storage_delete_file(const char *path);
bool storage_sd_available(void);
```

### 7.8 ui_core.h

```c
typedef enum {
    UI_PAGE_SPLASH,        // 启动页
    UI_PAGE_HOME,          // 主菜单 / 桌面
    UI_PAGE_WIFI,          // WiFi 扫描列表 / 密码输入 / 状态（同一页内的子视图）
    UI_PAGE_TODO,          // 待办列表页
    UI_PAGE_TODO_DETAIL,   // 待办详情 / 同步状态
    UI_PAGE_WEATHER,       // 天气页
    UI_PAGE_CALENDAR,      // 日历页
    UI_PAGE_WORD,          // 单词卡片页
    UI_PAGE_AI_CHAT,       // AI 对话页（P2）
    UI_PAGE_SETTINGS,      // 设置页
    UI_PAGE_IDLE_FACE,     // 待机眼睛动画页
    UI_PAGE_MAX,
} ui_page_id_t;

typedef struct {
    ui_page_id_t id;
    const char *name;
    esp_err_t (*create)(void);
    esp_err_t (*destroy)(void);
    esp_err_t (*on_show)(void);
    esp_err_t (*on_hide)(void);
    esp_err_t (*on_event)(int32_t event_id, void *data);
} ui_page_t;

esp_err_t ui_core_init(void);
esp_err_t ui_page_register(const ui_page_t *page);
esp_err_t ui_page_switch(ui_page_id_t id);
esp_err_t ui_lock(void);
esp_err_t ui_unlock(void);
```

### 7.9 idle_face.h

```c
typedef enum {
    IDLE_FACE_NORMAL,      // 普通：随机呼吸（周期 12~19 s 每次重抽）
    IDLE_FACE_BLINK,       // 眨眼：间隔 2.6~7.5 s、时长 110~165 ms，约 18% 连眨
    IDLE_FACE_LOOK_LEFT,   // 向左看（瞳孔偏移，不再切状态；枚举保留给 API/触发表）
    IDLE_FACE_LOOK_RIGHT,  // 向右看（同上）
    IDLE_FACE_HAPPY,       // 开心（Todo 完成、WiFi 成功）
    IDLE_FACE_SLEEPY,      // 困倦（夜间、长时间无操作）；待机时**不**使用——待机保持 NORMAL 继续动画
    IDLE_FACE_ERROR,       // 错误（WiFi / 同步失败）
    IDLE_FACE_SYNCING,     // 同步中
    IDLE_FACE_THINKING,    // 思考（网络请求中）
    IDLE_FACE_LISTENING,   // 聆听（语音唤醒、AI 录音中）—— P2
    IDLE_FACE_SPEAKING,    // 说话（AI 播报回复中）—— P2
    IDLE_FACE_MAX,
} idle_face_state_t;

esp_err_t idle_face_init(lv_obj_t *parent);
esp_err_t idle_face_set_state(idle_face_state_t state);
esp_err_t idle_face_standby_enter(void);   // 待机页 on_show：钩子（待机保持清醒，不闭眼）
esp_err_t idle_face_standby_exit(void);    // 待机页 on_hide：预留钩子（当前无副作用）
esp_err_t idle_face_tick(uint32_t now_ms);
esp_err_t idle_face_set_brightness(uint8_t percent);
```

### 7.10 ai_service.h

```c
typedef enum {
    AI_STATE_IDLE,
    AI_STATE_LISTENING,   // 录音/等待输入
    AI_STATE_RECOGNIZING, // 语音转文字中
    AI_STATE_THINKING,    // 等待 DeepSeek 响应
    AI_STATE_SPEAKING,    // 音频/文字输出中
    AI_STATE_ERROR,
} ai_state_t;

typedef struct {
    char role[16];    // "system" / "user" / "assistant"
    char content[1024];
} ai_message_t;

esp_err_t ai_service_init(void);
esp_err_t ai_service_set_api_key(const char *key);      // 保存到 NVS
esp_err_t ai_service_set_system_prompt(const char *prompt);
esp_err_t ai_service_chat(const char *text, char *reply, size_t len);        // 单次问答（阻塞，网络任务内调用）
esp_err_t ai_service_chat_stream(const char *text);                          // 流式：通过事件逐段推送
esp_err_t ai_service_clear_history(void);
esp_err_t ai_service_get_state(ai_state_t *state);
esp_err_t ai_service_abort(void);                                            // 中断当前请求
```

事件：

- `AI_EVENT_STATE_CHANGED`（参数：ai_state_t）
- `AI_EVENT_REPLY_CHUNK`（流式片段，参数：文本指针，需调用方立即拷贝）
- `AI_EVENT_REPLY_DONE`（完整回复可通过 `ai_service_get_last_reply()` 获取）
- `AI_EVENT_ERROR`

### 7.11 voice_service.h

```c
typedef enum {
    VOICE_WAKE_NONE,
    VOICE_WAKE_BUTTON,     // 按键/触摸唤醒
    VOICE_WAKE_KEYWORD,    // 离线关键词唤醒（若硬件算力允许）
} voice_wake_mode_t;

esp_err_t voice_service_init(void);
esp_err_t voice_service_start_record(void);          // 开始录音，完成后触发事件
esp_err_t voice_service_stop_record(void);
esp_err_t voice_service_play(const char *pcm_path);  // 播放 TTS 结果
esp_err_t voice_service_set_wake_mode(voice_wake_mode_t mode);
```

事件：

- `VOICE_EVENT_RECORD_DONE`（参数：音频数据/路径）
- `VOICE_EVENT_ASR_DONE`（参数：识别文本）
- `VOICE_EVENT_PLAY_DONE`
- `VOICE_EVENT_WAKE`

### 7.12 sync_server.h

局域网 HTTP Server：为 §8 的全部接口提供宿主，并把请求转成 `todo_manager` 等的调用。
**本组件不实现业务逻辑，只做路由、鉴权、JSON 编解码与错误码转换。**

```c
typedef struct {
    uint16_t port;              // 默认 80
    bool mdns_enabled;          // 是否发布 todohelper.local（默认 true）
    bool auth_enabled;          // 写操作是否要求 token
    char hostname[32];          // mDNS 主机名，默认 "todohelper"
} sync_server_config_t;

typedef void (*sync_server_handler_t)(httpd_req_t *req);

esp_err_t sync_server_init(void);
esp_err_t sync_server_start(const sync_server_config_t *cfg);   // 启动时自动尝试 mDNS
esp_err_t sync_server_stop(void);
esp_err_t sync_server_set_port(uint16_t port);
const char *sync_server_get_url(void);                          // 形如 "http://todohelper.local"

/* 供其他 service 注册自己的 /api/v1/* 路由（避免 sync_server 直接依赖所有组件） */
esp_err_t sync_server_register_route(const char *path, httpd_method_t method,
                                     sync_server_handler_t handler, void *user_ctx);

/* 写操作鉴权：token 存 NVS，未设置时默认仅允许局域网无鉴权（见 §8.8） */
esp_err_t sync_server_set_auth_token(const char *token);
bool sync_server_check_auth(httpd_req_t *req);

/* 变更通知：业务组件通过它触发事件广播（WebSocket / SSE） */
esp_err_t sync_server_broadcast_event(const char *event_name, const char *json_payload);
```

事件：

- `SYNC_EVENT_CLIENT_CONNECTED`（参数：客户端 IP）
- `SYNC_EVENT_CLIENT_DISCONNECTED`
- `SYNC_EVENT_REQUEST_HANDLED`（参数：路径 + 状态码，用于调试）
- `SYNC_EVENT_AUTH_FAILED`

实现要点：

- 路由按"前缀 → 组件"分发：`/api/v1/todos*` 归 `todo_manager`，`/api/v1/wifi*` 归 `wifi_manager`，
  `/api/v1/weather*` 归 `weather_service`，`/api/v1/words*` 归 `word_service`，
  `/api/v1/ai*` 归 `ai_service`，`/api/v1/device*` 归 `sync_server` 自身；
- 未实现的路由返回 **501**（见 §8），路径不存在返回 **404**；
- 只能由 `todo_manager` 的 API 变更数据，`sync_server` 不得直接写文件；
- 网络处理在独立任务中，**不得**直接调用 LVGL。

### 7.13 sys_utils.h

公共基础设施：日志分级、统一事件循环、时间工具、NVS 配置封装。**被几乎所有组件依赖，需最先落地。**

```c
/* ---- 日志 ---- */
typedef enum {
    SYS_LOG_ERROR,
    SYS_LOG_WARN,
    SYS_LOG_INFO,
    SYS_LOG_DEBUG,
} sys_log_level_t;

void sys_log_set_level(sys_log_level_t level);   // 运行时可调，也可由 Kconfig 给默认值
const char *sys_log_get_tag_prefix(void);        // 统一加 "TODOHELPER:" 前缀

/* ---- 事件 ---- */
esp_err_t sys_events_init(void);
esp_event_loop_handle_t sys_events_get_loop(void);   // 全项目共用的事件循环
esp_err_t sys_events_post(int32_t event_id, const void *data, size_t data_size);

/* ---- 时间 ---- */
int64_t sys_time_now_ms(void);                  // 单调时钟，用于超时/动画
int64_t sys_time_now_ts(void);                  // Unix 时间戳（秒），NTP 未同步时返回 0
bool sys_time_is_synced(void);

/* ---- 配置（NVS 封装，键名见 §9.1） ---- */
esp_err_t sys_config_init(const char *nvs_namespace);   // 默认 "todohelper"
esp_err_t sys_config_get_str(const char *key, char *out, size_t len);
esp_err_t sys_config_set_str(const char *key, const char *val);
esp_err_t sys_config_get_i32(const char *key, int32_t *out);
esp_err_t sys_config_set_i32(const char *key, int32_t val);
esp_err_t sys_config_erase(const char *key);
```

约定：

- 其他组件**不得**直接调用 `nvs_open`/`nvs_set_*`，统一经 `sys_config_*`，便于集中处理键名与迁移；
- 事件数据只在事件循环内有效，接收方需自行拷贝；
- `sys_time_now_ts()` 在 NTP 同步前返回 0，调用方必须先判断 `sys_time_is_synced()`。

---

## 8. HTTP API 文档

电脑端更新待办、调试、同步使用。基础地址：`http://todohelper.local`（mDNS，见 §FR1）或设备 IP。所有接口以 `/api/v1` 开头。
**路径存在但功能未实现返回 501；路径不存在返回 404。** 鉴权方案见 §8.8。

### 8.1 通用响应

所有响应统一为：

```json
{ "code": 0, "msg": "ok", "data": {} }
```

**`code` 语义（与 HTTP 状态码的关系）**：

`code` **就是 HTTP 状态码的副本**（成功为 `0` 是唯一例外），二者必须一致，客户端以 HTTP 状态码为准，`code` 仅便于只解析 body 的场景：

| HTTP 状态码 | body `code` | 含义 | 典型场景 |
|---|---|---|---|
| 200 / 201 | 0 | 成功 | 正常返回 |
| 400 | 400 | 参数错误 | JSON 解析失败、缺字段、字段越界 |
| 401 | 401 | 未授权 | 需要 token 的写操作未携带或 token 错误 |
| 404 | 404 | 不存在 | id 不存在、路径未注册 |
| 409 | 409 | 冲突 | 版本/时间戳冲突（增量同步用） |
| 500 | 500 | 内部错误 | 未预期的失败 |
| 501 | 501 | 未实现 | **路径已注册但功能未开发**（本项目大量接口初期为此状态，客户端需能优雅处理） |
| 503 | 503 | 不可用 | 网络离线、存储（SD/NVS）不可用 |

约定：

- 错误响应同样使用上述统一结构，`data` 为 `null` 或含 `detail` 字段说明原因；
- 所有时间戳字段（`due_ts`、`updated_at`、`start_ts` 等）均为 **Unix 秒级时间戳（int64）**；
- `GET` 接口不做鉴权；`POST` / `PUT` / `PATCH` / `DELETE` 是否鉴权由 §8.8 决定。

### 8.2 Todo API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | /api/v1/todos | 获取全部待办 |
| GET | /api/v1/todos?updated_since=ts | 增量同步 |
| POST | /api/v1/todos | 新增待办 |
| GET | /api/v1/todos/{id} | 获取单个 |
| PUT | /api/v1/todos/{id} | 全量更新 |
| PATCH | /api/v1/todos/{id}/done | 勾选完成 |
| DELETE | /api/v1/todos/{id} | 删除 |
| POST | /api/v1/sync | 触发设备端同步 |

新增请求：

```json
{ "title": "买牛奶", "desc": "两盒", "due_ts": 1735689600, "priority": 1 }
```

勾选请求：

```json
{ "done": true }
```

返回：

```json
{
  "code": 0,
  "msg": "ok",
  "data": {
    "id": 12,
    "title": "买牛奶",
    "done": true,
    "updated_at": 1735689700,
    "sync_state": "ok"
  }
}
```

### 8.3 WiFi API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | /api/v1/wifi/status | 当前状态 |
| POST | /api/v1/wifi/scan | 触发扫描 |
| GET | /api/v1/wifi/scan | 获取扫描结果 |
| POST | /api/v1/wifi/connect | 连接 |
| POST | /api/v1/wifi/disconnect | 断开 |

连接请求：

```json
{ "ssid": "MyWiFi", "password": "12345678" }
```

### 8.4 天气 / 日历 / 单词

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | /api/v1/weather | 当前天气缓存 |
| POST | /api/v1/weather/refresh | 强制刷新 |
| POST | /api/v1/weather/city | 设置城市 |
| GET | /api/v1/calendar/now | 当前时间 |
| GET | /api/v1/calendar/month?year=2026&month=9 | 月历 |
| GET | /api/v1/words/today | 今日单词 |
| POST | /api/v1/words/{id}/review | 提交复习结果 |

复习请求：

```json
{ "result": "known" }
```

### 8.5 AI 对话 API

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | /api/v1/ai/chat | 文本对话，返回 AI 回复 |
| POST | /api/v1/ai/chat/stream | 流式对话（SSE / WebSocket） |
| DELETE | /api/v1/ai/history | 清空对话记录 |
| POST | /api/v1/ai/config | 设置 API Key / system prompt |

对话请求：

```json
{ "text": "今天的待办有哪些？" }
```

回复：

```json
{
  "code": 0,
  "msg": "ok",
  "data": {
    "reply": "你今天有 3 项待办……",
    "state": "speaking"
  }
}
```

### 8.6 设备信息

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | /api/v1/device/info | 芯片、固件、IP、MAC、SD 状态 |
| GET | /api/v1/device/status | 运行状态、内存、任务 |
| POST | /api/v1/device/reboot | 重启，**必须鉴权**（见 §8.8） |

### 8.7 WebSocket（可选）

`/ws/events` 推送（服务端通过 `sync_server_broadcast_event()` 广播，见 §7.12）：

- Todo 变化；
- WiFi 状态；
- 天气更新；
- 同步状态；
- 待机表情状态；
- AI 对话状态与流式回复（P2）。

优先级：**P1**（P0 阶段可不实现；未实现时 `/ws/events` 返回 501，Web 页面需退化为轮询）。

### 8.8 鉴权方案

**结论：局域网内默认不做鉴权，但提供可开启的 Token 机制；重启等敏感操作强制鉴权。**

| 项 | 方案 |
|---|---|
| 默认状态 | `auth_enabled = false`：所有接口在局域网内可无鉴权访问（便于电脑端直接改待办） |
| 开启方式 | 通过设置页或 `POST /api/v1/device/auth` 设置 token；开启后**写操作**（POST/PUT/PATCH/DELETE）必须携带 |
| 传递方式 | 请求头 `X-Auth-Token: <token>`；**不接受** URL query 传 token（会进日志） |
| 存储 | token 存 NVS（键名 `auth_token`，见 §9.1），**不硬编码** |
| 强制鉴权接口 | `POST /api/v1/device/reboot`、`POST /api/v1/ai/config`（涉及 API Key）、`DELETE /api/v1/ai/history` |
| 失败响应 | HTTP 401 + `{"code":401,"msg":"unauthorized","data":null}` |
| 传输安全 | 明文本地局域网传输；**不提供 HTTPS**（ESP32 上代价过高）。故 token 不得与公网暴露的端口复用 |
| 实现位置 | `sync_server_check_auth()`（§7.12），各路由 handler 在入口处调用 |

安全提示（写入 §12/§9 的日志与备份需注意）：

- 备份文件 `backup/todos_*.json` 与日志 `logs/app.log` **不得包含** WiFi 密码、DeepSeek API Key、auth token；
- `GET /api/v1/device/info` 返回 MAC/IP 属正常，但**不得**返回 token 或 API Key（任何接口都不得回读 API Key）。

---

## 9. 数据模型与存储

### 9.1 NVS

命名空间统一为 **`todohelper`**，所有读写经 `sys_config_*`（§7.13），组件不得直接 `nvs_open`。
建议键名（**新增键必须在 `docs/API.md` 登记**）：

| 键名 | 类型 | 说明 | 引入阶段 |
|---|---|---|---|
| `wifi_ssid` / `wifi_pass` | str | 上次成功连接的 WiFi（开机自动重连） | P0 |
| `wifi_auto` | i32 | 是否自动重连（0/1） | P1 |
| `tz` | str | 时区，如 `CST-8` | P1 |
| `city` | str | 手动设置的城市 | P0 |
| `city_lat` / `city_lon` | str | 经纬度（IP 定位或手动设置的结果） | P1 |
| `weather_api_key` | str | 天气 API Key（选型后确定键名） | P1 |
| `brightness` | i32 | 背光亮度百分比 | P1 |
| `word_daily_count` | i32 | 每日单词数量 | P1 |
| `sync_enabled` | i32 | 同步开关（0/1） | P0 |
| `auth_token` | str | HTTP API 鉴权 token（§8.8） | P1 |
| `ai_api_key` | str | DeepSeek API Key，**不硬编码** | P2 |
| `ai_model` | str | `deepseek-chat` / `deepseek-reasoner` | P2 |
| `ai_system_prompt` | str | 角色设定 | P2 |
| `ai_history_rounds` | i32 | 保留对话轮数，默认 10 | P2 |
| `ai_save_history` | i32 | 是否保存聊天记录到 SD（0/1） | P2 |
| `voice_wake_mode` | i32 | 语音唤醒方式（§7.11） | P2 |
| `tts_enabled` | i32 | TTS 播报开关（0/1） | P2 |
| `idle_timeout_s` | i32 | 进入待机的时间，默认 30 秒 | P1 |
| `idle_brightness` | i32 | 待机亮度，默认 20% | P1 |
| `first_boot` | i32 | 是否首次启动（用于引导流程） | P0 |

注意：

- **不得**把天气 API Key、DeepSeek API Key、auth token 写入日志或备份文件（§8.8 安全提示）；
- NVS 分区当前为 `0x9000` / `0x6000`（24 KB），键值均为小字符串/整型，容量充足；
  若后续需要存储大量文本（如长对话），应改存 SD 卡而非 NVS。

### 9.2 文件存储

```text
/sdcard/todohelper/
  fonts/
    ui_16.bin
    ui_24.bin
  images/
    weather/
      sunny.png
      cloudy.png
  anim/
    idle_face/
      normal.json
      blink.json
  words/
    cet4.json
    progress.json
  ai/
    chat_history.json
    system_prompt.txt
  tts/
    reply_0001.pcm
  cache/
    weather.json
    todos.json
  backup/
    todos_20260923.json
  logs/
    app.log
```

说明：

- 根目录以 `storage_get_sd_mount()` 返回值为准：SD 可用时为 `/sdcard`，回退内部文件系统时为内置挂载点，
  上层代码**不得硬编码** `/sdcard`；
- `anim/idle_face/` 为**可选路径**：按 §11.5 规则，P0/P1 采用程序化绘制，该目录可暂不创建；
- `tts/` 与 `ai/` 属 P2 产物；P2 之前可不存在；
- `cache/weather.json`、`cache/todos.json` 是**断网降级**与**快速启动**的关键（FR2/FR3 要求断网可查看），
  需在每次数据变更后写入，且写入失败不得影响主流程；
- `backup/todos_*.json` 建议按日期滚动，保留最近 N 份，避免涨满 SD 卡。

### 9.3 数据结构

- Todo：id, title, desc, due_ts, done, priority, updated_at, sync_state
- WiFiConfig：ssid, password, auto_connect, last_ok
- Weather：city, lat, lon, temp, feels_like, humidity, condition, update_ts
- CalendarEvent：id, title, start_ts, end_ts, remind, todo_id
- Word：id, spelling, phonetic, meaning, example, level, next_review_ts

---

## 10. UI / UX 规范

### 10.1 页面列表

- 启动页 / 状态栏；
- 主菜单 / 桌面；
- WiFi 扫描列表页；
- WiFi 密码输入页；
- WiFi 状态页；
- 待办列表页；
- 待办详情 / 同步状态；
- 天气页；
- 日历页；
- 单词卡片页；
- 设置页；
- AI 对话页（气泡式聊天 / 录音按钮 / 状态联动小眼睛）；
- 待机眼睛动画页。

**与 `ui_page_id_t` 的对应关系**（枚举见 §7.8，页面与枚举必须一一对应）：

| 页面 | 枚举值 | 优先级 | 实现方式建议 |
|---|---|---|---|
| 启动页 | `UI_PAGE_SPLASH` | P0 | 显示启动横幅 + 初始化进度；替换现有 Splash_screen |
| 主菜单 / 桌面 | `UI_PAGE_HOME` | P0 | 卡片式入口 + 状态栏 |
| WiFi 扫描列表页 | `UI_PAGE_WIFI` | P0 | 与密码输入、状态页**共用一个 page**，内部用子视图切换（避免枚举膨胀） |
| WiFi 密码输入页 | （同上，`UI_PAGE_WIFI` 的子视图） | P0 | LVGL 键盘，注意 800×480 下的键盘高度 |
| WiFi 状态页 | （同上，`UI_PAGE_WIFI` 的子视图） | P1 | 显示 IP / RSSI / MAC |
| 待办列表页 | `UI_PAGE_TODO` | P0 | flex 列表 + 勾选框（触摸目标 ≥44×44） |
| 待办详情 / 同步状态 | `UI_PAGE_TODO_DETAIL` | P1 | |
| 天气页 | `UI_PAGE_WEATHER` | P0 | |
| 日历页 | `UI_PAGE_CALENDAR` | P0 | |
| 单词卡片页 | `UI_PAGE_WORD` | P0 | 翻卡动画 |
| 设置页 | `UI_PAGE_SETTINGS` | P1 | 亮度、时区、城市、API Key、鉴权 token |
| AI 对话页 | `UI_PAGE_AI_CHAT` | P2 | 气泡列表 + 录音按钮 |
| 待机眼睛动画页 | `UI_PAGE_IDLE_FACE` | P0 | 由 `idle_face` 组件接管，超时自动进入（§11.4） |

### 10.2 状态栏

常驻显示：

- WiFi 状态；
- 时间（**NTP 未同步时显示"时间未同步"，不得显示 1970**，见 FR4）；
- ~~电量~~ —— **当前硬件无电池与电量计，本项不显示**；若后续加装电池再启用；
- 同步状态；
- SD 卡状态（未插入 / 已挂载 / 读取失败）。

> 状态栏由 `ui_core` 统一维护，页面切换不重建；其数据全部来自 `esp_event` 订阅，不得直接轮询 service。

### 10.3 屏幕适配

- 使用 `BSP_LCD_H_RES`、`BSP_LCD_V_RES`；
- 页面使用 flex / grid；
- 触摸控件最小 44x44 px；
- 字体按分辨率选择；
- 横竖屏通过配置或运行时切换；
- 避免硬编码坐标。

### 10.4 LVGL 线程安全

- LVGL 只在 UI 任务调用；
- 其他任务通过 `ui_lock()` / `ui_unlock()` 或事件通知；
- 禁止在 WiFi 回调直接操作 LVGL。

---

## 11. 待机眼睛动画设计

### 11.1 视觉要求

- 全黑背景；
- 白色纯色线条；
- 两只简约眼睛；
- 类似表情；
- 无复杂贴图优先，程序化绘制优先；
- 支持 SD 卡帧动画作为备选。

### 11.2 状态机

| 状态（枚举值） | 触发 | 表现 | 优先级 |
|---|---|---|---|
| NORMAL | 默认 | 两只椭圆眼睛，随机呼吸（周期 12~19 s 重抽） | P0 |
| BLINK | 随机 2.6~7.5 s | 眼睛闭合再打开（110~165 ms，约 18% 连眨） | P0 |
| LOOK_LEFT | 随机 | 瞳孔左移（用偏移量缓动，不切状态） | P1 |
| LOOK_RIGHT | 随机 | 瞳孔右移（同上） | P1 |
| HAPPY | Todo 完成、WiFi 成功 | 眼睛弯成弧线 | P1 |
| SLEEPY | 夜间、长时间无操作 | 半闭眼 | P1 |
| ERROR | WiFi 失败、同步失败 | X 眼或下垂眼 | P1 |
| SYNCING | 同步中 | 瞳孔绕小圆转圈（1.8 s 一圈） | P1 |
| THINKING | 网络请求 | 双眼看向上方 | P1 |
| LISTENING | 语音唤醒、AI 聆听 | 眼睛放大 | P2 |
| SPEAKING | AI 播报回复 | 眼睛随语音节奏起伏（上弧脉动，**无嘴巴**） | P2 |
| （过场，无独立枚举） | 进入/离开待机 | 950 ms 闭眼入睡 / 700 ms 睁眼醒来，均为有界包络 | P1 |

> 枚举定义见 §7.9（`IDLE_FACE_*`），需求描述见 FR6。**三者必须同步修改。**
> 状态切换只更新已创建对象的属性（见 §11.3），并在切换时通过 `TODOHELPER_EVENT` 广播，供状态栏与 WebSocket 订阅。
> **表情之间必须交叉淡入**（180 ms 快档 / 420 ms 普通档，`smoothstep` 起止），不允许瞬间跳变。

### 11.3 LVGL 实现建议

- 使用 lv_canvas 绘制白色线条；或使用 lv_line + lv_arc；
- 每帧 30~60 FPS，待机可降至 15 FPS；
- 眨眼通过改变眼睛高度或重绘实现；
- 开心用 lv_arc 画上弧线；
- 错误用两条交叉线；
- 所有对象初始化时创建，状态切换只更新属性，避免频繁创建删除。

### 11.4 待机策略

- 无触摸 30 秒进入待机；
- 亮度降至 20%；
- 触摸或按键唤醒；
- 系统事件可临时覆盖表情；
- 待机时 WiFi、同步、天气继续运行。

### 11.5 绘制方式与素材取舍规则

§11.1 的"程序化绘制优先"与 §9.2 的 `anim/idle_face/*.json` 帧素材是**两条可选路径**，取舍规则如下：

| 路径 | 使用条件 | 代价 |
|---|---|---|
| **程序化绘制（默认，P0–P1 采用）** | 所有状态都优先用 `lv_canvas` / `lv_line` / `lv_arc` 实现 | 零额外存储，可与状态机直接联动参数化（如瞳孔位移、眼睁度） |
| **SD 卡帧动画（备选，仅在需要时启用）** | 出现程序化难以表达的效果（如复杂过渡、手绘风格帧） | 占用 SD 空间与 RAM，需解码；受 §12 的 SD 可用性与 GPIO20~25 约束影响 |

规则：

1. **P0/P1 不引入 SD 帧素材**，`anim/idle_face/` 目录可暂不创建；
2. 若启用帧动画，素材缺失、SD 未挂载或解码失败时**必须回退到程序化绘制**，不得黑屏或卡死（对应 §12 降级要求）；
3. 同一状态不得同时存在程序化与帧动画两套实现而产生视觉不一致；切换需在 §11.2 表中登记。

---

## 12. SD 卡方案

- 挂载点：`/sdcard`（**SDMMC 4-bit**，引脚 D0-3=GPIO20~23、CLK=24、CMD=25、电源开关 GPIO39 低有效，见 §4）
- 若 SD 不可用，回退内置 LittleFS；`storage_manager` 需对上层屏蔽该差异（`storage_get_sd_mount()` 返回实际可用根路径）
- SD 卡用于：中文字体、天气图标、待机动画帧、单词词库、AI 对话记录与 TTS 音频缓存、日志、备份
- LVGL 需注册 `lv_fs` 驱动（现有工程已启用 `ESP_LVGL_ADAPTER_ENABLE_FS`）

**硬约束（必须遵守）**：

1. **GPIO20~25 与 SPI NAND flash 脚位复用，同一块板只能装其中一个**（见 §4）。
   因此"把字体/词库/动画放进片内 flash 分区"与"使用 SD 卡"是**互斥的硬件选择**，
   不能假设两者同时可用。方案设计必须以"两者取其一"为前提，并在两条路径下都能工作。
2. 屏幕与触摸不占用 GPIO20~25（屏幕为 RGB 并口、触摸为 GPIO0/1），
   故 v0.3 中"注意 SD 卡与屏幕、触摸引脚冲突"的担心**已排除**，真正的冲突对象是 SPI NAND flash。
3. 文件读取失败要有降级策略（缺失字体 → 内置字体；缺失词库 → 提示未装载；缺失图标 → 文本替代）。

**可复用的现有资产**：本工程已有 `coffee_pjpg` 资源分区（`partitions.csv`：起始 `0x810000`、大小 `0x600000`）、
`spiffs_create_partition_assets` 打包流程与 `esp_mmap_assets` mmap 加载机制（见 `AGENT.md` 第 2.2/5 节）。
若最终决定将字体/图标放入片内分区而非 SD 卡，**应复用这套流程**（重命名分区与资源目录即可），不要另造一套。

**分区表现状（`partitions.csv`，改动前需评估）**：

| 分区 | 偏移 | 大小 | 用途 |
|---|---|---|---|
| nvs | 0x9000 | 0x6000 | NVS（§9.1 全部配置） |
| phy_init | 0xf000 | 0x1000 | 射频校准 |
| factory | 0x10000 | 0x800000 | App（当前约 1.58 MB，余量约 80%） |
| coffee_pjpg | 0x810000 | 0x600000 | UI 资源（当前为咖啡机素材，可复用/替换） |

---

## 13. 事件与数据流

```text
触摸事件 -> ui_pages -> service API -> storage/network
service 事件 -> esp_event -> ui_pages 更新
sync_server 收到 HTTP -> todo_manager -> 事件 -> UI 刷新
weather_service 定时刷新 -> 事件 -> 天气页刷新
idle_face 监听系统事件 -> 表情切换
```

关键规则：

- 事件只传必要数据，避免大结构；
- UI 刷新在 UI 任务；
- 网络请求不阻塞 UI；
- 存储操作加锁。

---

## 14. 开发优先级与里程碑

### P0：最小闭环

前提：**先把咖啡机业务与 UI 摘除**（保留 BSP、LVGL 适配层、显示骨架、分区与资源打包流程），建立 `sys_utils` + `storage_manager` + `ui_core` + `bsp` 四个基础组件。

- BSP 封装层 + LVGL UI 框架（复用现有 `esp_lvgl_adapter` 链路，不自建显示初始化）；
- WiFi 扫描连接（含 NVS 保存与自动重连）；
- Todo 本地显示与勾选，**含 `cache/todos.json` 本地持久化**；
- 电脑 HTTP 同步（`sync_server` 框架 + Todo API，未实现接口返回 501）；
- 天气基础获取（手动城市 + 天气 API）；
- 日历基础显示（NTP + 时区，显示"未同步"状态）；
- 单词卡片基础功能（翻卡 + 标记，词库来源待定）；
- 待机眼睛 NORMAL + BLINK（程序化绘制）。

### P1：体验完善

- 断线重连、缓存、自动同步；
- NTP 校时完善、时区设置（**无 RTC，不含断电保持**）；
- 设置页、亮度、mDNS、HTTP 鉴权 token；
- 单词复习进度与复习算法；
- 待机表情丰富（LOOK/HAPPY/SLEEPY/ERROR/SYNCING/THINKING）；
- 待机超时与亮度降低（§11.4）；
- IP 定位、天气定时刷新与降级；
- `/ws/events` 事件推送；
- SD 卡素材加载（字体/图标；动画帧按 §11.5 视需要启用）。

### P2：扩展

- MQTT / 云同步；
- 语音发音；
- 提醒、日程联动；
- 多用户、低功耗；
- OTA；
- AI 语音对话助手（FR7）：DeepSeek 接入、语音输入、TTS 播报、小眼睛联动（LISTENING / THINKING / SPEAKING / ERROR）；
- 语音控制联动："帮我添加待办 XXX" 等指令解析。

### 阶段与验收的对应

| 阶段 | 完成的判据 |
|---|---|
| P0 | §15 前 5 条验收通过 + 全工程 `idf.py build` 成功（判据见 §15 末尾） |
| P1 | §15 全部验收通过 |
| P2 | FR7 可完成一次"语音提问 → 文字上屏"的完整链路 |

---

## 15. 测试与验收

功能验收：

- WiFi 可扫描、连接、保存、自动重连；
- 电脑可新增待办，设备可显示；
- 设备勾选后电脑可见；
- 天气页显示城市、温度、天气、更新时间，断网有缓存；
- 日历页显示当前月，支持翻月，NTP 同步（**断电后需重新同步，此为预期行为**）；
- 单词页可翻卡、标记、记录进度；
- 待机页全黑背景，白色线条眼睛动画正常；
- UI 不溢出、不卡死，触摸区域适配（控件 ≥44×44 px）；
- 各组件可独立编译，依赖清晰；
- SD 卡可挂载，素材可读取，失败可降级。

健壮性验收（v0.4 新增）：

- 未实现的 `/api/v1/*` 返回 **501** 而非 500 或空响应；不存在的路径返回 404；
- 未插入 SD 卡时，字体/图标/词库缺失均有降级路径，不崩溃、不黑屏；
- 未同步 NTP 时，时间相关 UI 显示"未同步"而非 1970；
- 断网时：待办可查看与勾选、天气显示缓存、AI 功能给出明确提示且不影响其他功能；
- 写操作在 `auth_enabled=true` 且缺失/错误 token 时返回 401；
- 日志与备份文件中**不出现** WiFi 密码、天气 API Key、DeepSeek API Key、auth token。

编译与烧录基线（**判定"构建成功"的依据，摘自 `AGENT.md` 第 8 节**）：

- 构建日志出现 `Project build complete.`；
- 烧录成功判据：每个镜像后出现 `Hash of data verified.`，结尾 `Hard resetting via RTS pin...` + `Done`；
- 参考数值（**咖啡机固件基线，替换业务后需重新记录**）：
  app bin 约 1.58 MB（`0x193a60`），factory 分区余量约 80%，bootloader 余 6%，编译警告 0；
- 排障入口见 `AGENT.md` 第 10 节；预先提醒：补丁被覆盖会导致 `fatal error: esp_async_fbcpy.h: No such file`
  （处理方式见 §0）。

---

## 16. 给其他 Agent 的工作约定

1. 每个组件必须有：CMakeLists.txt、include/xxx.h、src/xxx.c、README.md（api.md 可选）；
2. 公共接口只放 include/；
3. 禁止跨组件调用 static 函数；
4. 组件间通信用 esp_event；
5. 配置用 Kconfig + NVS，**NVS 读写统一走 `sys_config_*`**（§7.13）；
6. 日志统一用 ESP_LOGI/W/E；
7. 错误码统一（映射规则见 §7.1，**不得直接把 `todohelper_err_t` 强转成 `esp_err_t`**）；
8. LVGL 操作必须加锁；
9. 修改 API 必须更新本文档；
10. 每个 Agent 完成任务后，在 `docs/API.md` 追加接口说明与测试方法。

**开工前必读**（v0.4 新增）：

- 根目录 `AGENT.md`（现有驱动、引脚、环境、补丁、验证基线）；本文档；改动前确认 §4 的硬件约束；
- **不要修改**工程外的 `..\common_components\esp32_s31_korvo`；
- **不要手改** `sdkconfig`（生成物），要改默认值改 `sdkconfig.defaults`；
- 改动 `managed_components/` 下任何文件，必须把 diff 追加进 `patches/` 并在 `AGENT.md` 第 9 节登记；
- **不要删除或重建** `managed_components/espressif__esp_lvgl_adapter` 而不重新打补丁（见 §0）。

**`docs/API.md` 当前尚不存在**（`docs/` 目录目前只有本文档），由各 Agent 按第 10 条创建并追加；
建议每个组件交付时追加一节，含：头文件路径、函数签名、错误码、事件、最小测试步骤（含 `idf.py` 命令）。

### 建议任务分工

| Agent | 负责 | 输出 | 依赖 | 对应阶段 |
|---|---|---|---|---|
| A | BSP 封装层、ui_core、屏幕适配 | `bsp`、`ui_core` | 工程外 Korvo BSP（只读） | P0 |
| B | WiFi、sync_server、HTTP API | `wifi_manager`、`sync_server` | sys_utils、storage_manager | P0（Todo API）→ P1（鉴权/WS） |
| C | Todo、storage、NVS | `todo_manager`、`storage_manager`、`sys_utils` | 无（**建议最先开工**） | P0 → P1 |
| D | 天气、日历、NTP | `weather_service`、`calendar_service` | wifi_manager、storage_manager | P0 → P1 |
| E | 单词、词库 | `word_service` | storage_manager | P0 → P1 |
| F | 待机眼睛动画、UI 页面 | `idle_face`、`ui_pages` | ui_core、各 service 的事件 | P0（NORMAL/BLINK）→ P1 |
| G（P2） | AI 对话、语音 | `ai_service`、`voice_service` | wifi_manager、bsp 音频、FR7 选型结论 | P2 |

协作提示：

- **C 是其他多数 Agent 的前置**（`sys_utils`、`storage_manager` 是公共底座），应优先交付最小可用版本；
- A 需要先摘除咖啡机 UI 并把 `ui_core` 骨架跑通，F 才能并行开发页面；
- 所有 Agent 的页面注册统一走 `ui_page_register()`，不得各自创建 LVGL 顶层 screen。

---

## 17. 待确认信息与需要用户物理协助

### 17.1 已确认（v0.4，来源 `AGENT.md`）

| 原问题 | 结论 |
|---|---|
| 提供 AGENT.md 内容 | **已读取并核对**：根目录 `AGENT.md`（注意扩展名是小写 `.md`） |
| 芯片型号 | **esp32s31**（ESP32-S31，RISC-V，preview 目标） |
| 屏幕分辨率 | **800×480** |
| 屏幕接口 | **16-bit RGB 并口**，PCLK 26 MHz |
| 触摸 IC / 接口 | **GT1151 / I2C**（SDA=GPIO0、SCL=GPIO1） |
| LVGL 版本 | **9.5.0**（+ esp_lvgl_adapter 0.5.3，已打补丁） |
| 已有驱动接口 | **见 `AGENT.md` 第 2.1/2.2 节**（屏/触摸/摄像头/音频/SD/按键/WS2812 全部已实现） |
| PSRAM | **有**（250 MHz + XIP_FROM_PSRAM） |
| SD 卡 | **有**，SDMMC 4-bit，D0-3=GPIO20~23、CLK24、CMD25、电源 GPIO39，挂载点 `/sdcard` |
| RTC | **无**（不支持断电保持） |
| 音频 | **有**：I2S 48 kHz/16 bit/2ch + `esp_codec_dev`，录制与回放均已实现 |
| 按键 | **有**：ADC 单线 4 键（GPIO42） |
| LED | **有**：WS2812（GPIO37） |
| 电脑更新待办的方式 | **局域网 HTTP**（本机已有 WiFi/HTTP 能力，MQTT 列为 P2 扩展） |

### 17.2 仍需用户确认（阻塞项）

1. **屏幕驱动 IC 型号**：走 RGB 并口直驱，`AGENT.md` 未记录面板 IC（影响时序微调与背光方案）；
2. **麦克风型号**（数字 MIC / 模拟 MIC + ADC）：决定 FR7 能否做离线 ASR；
3. **扬声器 / 音频功放是否已装**：`AGENT.md` 记录了 Codec 录制回放与 `PA_CTRL=GPIO7`，但未说明是否带喇叭/功放；
4. **天气 API 选型与 Key**：和风 / OpenWeather / 心知（影响 `weather_service` 实现与 NVS 键名）；
5. **词库来源**：CET4/CET6？格式？是否需要中文释义、音标、例句、发音？
6. **语音识别方案**：本地离线 ASR（`esp-sr`）还是云端 ASR（决定 voice_service 架构与是否需 PSRAM 余量）；
7. **TTS 方案**：本地 TTS / 云端 TTS / 跳过音频仅屏幕显示（若跳过，FR7 的 SPEAKING 状态可简化）；
8. **DeepSeek API Key、使用模型**（`deepseek-chat` / `deepseek-reasoner`）、是否流式输出；
9. **待机眼睛风格**：圆眼 / 椭圆眼 / 线条眼，是否需要瞳孔；
10. **字体与动画素材存放位置**：SD 卡（`/sdcard/todohelper/fonts`）还是片内资源分区（复用 `coffee_pjpg` 流程）——
    **注意此选择受 GPIO20~25 复用约束**（§4/§12）；
11. **屏幕方向**（横屏/竖屏）与触摸校准参数；
12. **FR2 P1 范围确认**：设备端是否必须支持新增/删除/编辑（P0 只做显示与勾选）；
13. **备份策略确认**：`backup/todos_*.json` 保留份数与频率（默认建议保留最近 7 份，每日一次）。

### 17.3 需要用户物理协助

1. 拍摄接线图（尤其是 SD 卡槽、音频、按键部分）；
2. 拍摄屏幕显示 demo 照片（用于确认色彩、方向、有无撕裂）；
3. 确认板上是否已焊接 SD 卡槽 / SPI NAND flash（**二者互斥**，见 §4）；
4. 确认摄像头是否保留（当前未被 ToDoHelper 使用，若拆除可释放引脚与共享 I2C）；
5. 在设备上实测：烧录后提供串口日志（`idf.py -p COM34 monitor`），用于核对时间同步与 WiFi 行为。

---

## 18. 结论

ToDoHelper 可拆为：

```text
BSP 封装层 + sys_utils(底座) + storage_manager
  + UI 框架(ui_core/ui_pages/idle_face)
  + 5 个业务组件(wifi/todo/weather/calendar/word)
  + sync_server + 待机表情组件

P2 阶段追加：ai_service + voice_service，与待机表情深度联动
```

**v0.4 明确的起点**：本仓库当前是 ESP32-S31-Korvo 产测固件（咖啡机 UI + 工厂自检），
因此 P0 的第一步不是写业务，而是**摘除咖啡机业务与 UI、保留 BSP 与显示骨架**，并把 §6 的组件目录建起来。

推荐先完成 P0 闭环：

0. **摘除原业务与 UI，建立 `sys_utils` + `storage_manager` + `bsp` + `ui_core` 四个基础组件**（前置）；
1. WiFi 扫描连接；
2. Todo 本地显示与勾选（含本地缓存持久化）；
3. 电脑 HTTP 同步；
4. 天气基础获取；
5. 日历基础显示；
6. 单词卡片；
7. 待机眼睛普通与眨眼。

确认 §17.2 的信息后，下一步可继续输出：

- 每个组件的具体目录与 CMakeLists.txt（含与工程外 BSP 的引用方式）；
- 头文件完整定义（基于 §7，含 §7.12/§7.13 新增两节）；
- LVGL 页面原型（基于 §10.1 与 §7.8 的枚举对应表）；
- HTTP Web 页面（基于 §8，含 501 未实现接口的占位处理）；
- 待机眼睛动画代码框架（基于 §11，程序化绘制）；
- 多 Agent 任务卡（基于 §16 分工表，含依赖顺序）。
