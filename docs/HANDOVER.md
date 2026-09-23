# ToDoHelper 交接说明（HANDOVER）

> 面向**接手的下一个 Agent / 开发者**。本文档描述「当前真实状态、阻塞问题、下一步做什么、哪里最容易踩坑」。
>
> 配套阅读顺序：**本文档 → `AGENT.md`（环境/命令/全部实测坑，尤其 §9.0~§9.0.5、§9.7）→
> `docs/TODOHELPER_REQUIREMENTS.md`（需求）→ `docs/TODOHELPER_FRAMEWORK.md`（框架）→ `docs/API.md`（组件与 HTTP 接口）**。
>
> 最后更新：第二轮收尾（提交 `2dfa7b7` / `e365448` 及其后一个小改动）。

---

## 0. 一句话现状

**8 项需求全部实现并提交；构建 0 错误 0 警告；上一个阻塞问题「插 SD 卡即崩溃重启循环」
已定位根因、修复并在真机上 3/3 冷启动验证通过。目前没有已知阻塞问题。**

> **关键更正**：交接时判断根因是「crypto 锁惰性初始化竞态」，**那是错的**（两处归因都是
> panic 栈的误读）。真实根因是**批量 JSON 解析把内部 RAM 抽干**，使同一时刻正在连网的
> WiFi 驱动 `ESP_ERROR_CHECK` 失败并 abort —— 只有插卡时才会去解析卡上的 546 KB 词库。
> 详见 §2；`AGENT.md §9.0.5` 已改写为实测结论并保留了两处误读作为教训。

---

## 0.5 现阶段的目标与需求

### 0.5.1 项目总目标（缘起，长期有效）

在 **ESP32-S31-Korvo** 上实现 ToDoHelper：以 `docs/TODOHELPER_REQUIREMENTS.md` 为需求书，
**先规划可扩展的框架（分层 L0~L5、三个表驱动注册点），再逐条完成需求**；允许参考乐鑫官方工程
或优秀开源项目的代码与 UI。框架设计见 `docs/TODOHELPER_FRAMEWORK.md`，接口见 `docs/API.md`。

### 0.5.2 本轮目标（8 项，即当前正在收尾的这一轮）

> 原文（用户下达）：**「ToDoHelper 第二轮收尾（8 项）：(1) 修复待办无法同步；(2) 优先完成两项
> 未完成功能——待机眼睛动画（只要眼睛不要嘴巴、零位图）与单词书 cet4→cet6；(3) 在 SD 卡
> （当前在 PC 的 F:\）准备所需资源：六级词汇、字体/字库等（动画为程序化绘制，无需图片）；
> (4) 待办逻辑改为设备端只读、仅由网页上位机编辑；(5) 优化网页上位机；(6) 开机在屏幕上滚动
> 显示启动日志；(7) 时刻更新 harness 待办清单；(8) 部分任务使用子 agent，完成后更新文档并
> 提交 git。每步烧录 COM34 验证。」**

逐条状态见 **§3**。特别提示两条**已被用户中途修正**的需求，接手时别按字面理解：

| 需求原文 | 中途修正与最终结论 |
|---|---|
| (2) 「**优先**完成两项未完成功能」 | 优先级确实最高，已先做；但其中的**待机动画有一个隐藏 bug**（脸不会眨眼、临时表情永不过期），见 §3 的 2a |
| (4) 「设备端只读、仅网页编辑」 | 用户实测后澄清：**设备端要保留"勾选完成 + 删除"**，只有"新增/编辑"留给网页（本板无 IME，打字不现实）。**最终按修正后的语义实现**（§3 的 4）。注意这不是"完全只读" |

### 0.5.3 更早各轮的需求（历史，供理解上下文）

按时间顺序，除上面 8 项外用户还提过并已实现的要求：

1. **需求书全量实现 + P1 三项 + 文档回改**（`docs/TODOHELPER_REQUIREMENTS.md` v0.5，含 15 处实现偏差记录）
2. **硬件 bring-up 与优化**：内存管理（大量数据放 SD 卡）、UI 参考官方工程写法、待机动画参考
   esp-vocat、字体放 SD 卡解决中文方框、烧录 COM34 实测
3. **一批现场 bug**（用户实际使用中发现，均已修并上机验证）：WiFi 扫描崩溃、连接崩溃、
   日志可读性、键盘样式统一、日历排版、天气页显示 "Text"、待机/唤醒行为、待机后设备失联
4. **天气纯 IP 定位**：去掉手输城市、修 `lat/lon=0` 解析 bug、默认城市设为**南昌**
5. **性能与体验**：烧录提速（147.9s → 31.6s，4.7×）、丰富待机动画、单词书 cet4→cet6

### 0.5.4 明确"不做/未做"的项（避免重复劳动）

- **网页 WiFi 配置面板**：不是原始 8 项需求里的条目，是我在核实子 agent 报告时**自己追加**的想法。
  子 agent 被中断，**只留下页头注释，没有实现**。若要做得先读
  `components/wifi_manager/src/wifi_manager_http.c:266-272` 的路由表（7 条路由，均可用）。
- **L0 `SYS_EVENT_TIME_SYNCED` 事件**：已在 `AGENT.md §9.7` 降级为**可选** —— `weather_task`
  现在会等时钟最多 75 秒，已覆盖原场景。

---

## 1. 环境与命令（原文档漏了标题）

- 目标芯片 `esp32s31`（ESP32-S31-Korvo，800×480 RGB 屏 + GT1151 触摸 + ES8389 + SDMMC 4-bit）
- ESP-IDF **v6.1**，路径 `D:\ESP-IDF\v6.1\esp-idf`；工程 `D:\project\ESP-IDFv6.1\ToDoHelper`
- 串口 **COM34**（115200）；设备 IP 固定 **192.168.137.110**，mDNS `todohelper.local`

```powershell
# 0) 关键前置：先杀掉占用串口的 monitor，否则烧录/抓串口都会失败
Get-CimInstance Win32_Process -Filter "Name like '%python%'" |
  Where-Object { $_.CommandLine -match 'idf_monitor|esp_idf_monitor' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force }

# 1) 构建（约 40s）
idf.py build

# 2) 快速烧录（约 32s；脚本封装了 921600 波特 + stub 压缩，别用 idf.py flash）
#    注意：必须在"已加载 IDF 环境"的 pwsh 里运行，否则会静默什么都不做
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
```

烧录命令本体（`tools/flash.ps1` 也是这个）：

```powershell
python -m esptool --chip esp32s31 -p COM34 -b 921600 --before=default-reset --after=hard-reset `
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB `
  0x2000  build\bootloader\bootloader.bin `
  0x8000  build\partition_table\partition-table.bin `
  0xa10000 build\mmap_build\ui\assets\assets.bin `
  0x10000 build\todohelper.bin
```

抓串口（`idf.py monitor` 无法脚本化，用这个）：

```powershell
python tools\monitor_capture.py --port COM34 --seconds 16 --out build\boot.log   # 会做一次复位
python tools\monitor_capture.py --port COM34 --seconds 16 --no-reset --out build\x.log
```

---

## 2. ✅ 已修复：插 SD 卡启动崩溃重启循环（根因与当初的判断不同）

### 当初的现象（100% 可复现）

| 条件 | 结果 |
|---|---|
| **拔掉 SD 卡** | 启动干净：0 故障、9 模块 ready、`storage=/littlefs` |
| **插上 SD 卡** | **3/3 崩溃**，从未到达 `TH:app: ready`；16 秒内崩 3 次 = 重启循环 |

崩点固定：`module 'calendar_service' ready` 之后、`word_service` 打出任何日志之前。
`reset reason: panic (4)`。

### 真实根因：JSON 解析掏空内部 RAM → WiFi 驱动的 `ESP_ERROR_CHECK` 失败

**答案一直写在 panic 文本里，只是被两处误读盖住了**：

```
I (5910) TH:app: module 'calendar_service' ready
I (5915) TH:heap: after calendar_service internal 107 KB (largest 61 KB) psram 11183 KB
ESP_ERROR_CHECK failed: esp_err_t 0x101 (ESP_ERR_NO_MEM) at 0x50127178
file: ".../esp_timer/src/ets_timer_legacy.c" line 58
func: ets_timer_setfn
expression: esp_timer_create(&create_args, (esp_timer_handle_t*) & (ptimer->timer_arg))
abort() was called at PC 0x2f00f08f on core 0
```

1. **`locks.c:77` 不是断言**。本机 IDF v6.1 的 `esp_libc/src/locks.c:77` 恰恰是
   `lock_init_generic()` 里的 `abort(); /* No more semaphores available or OOM */`；
   而那段惰性初始化全程由 `lock_init_spinlock` 保护，**不存在"两个上下文同时首次 acquire"
   的竞态**。所以"crypto 锁竞态"这个结论不成立 —— 两条"线索"其实都指向**内存不够**。
   （`esp_crypto_lock.c:39` 那个地址是数据段里静态锁变量的地址，根本不是返回地址。）
2. **`RA = 0x2f00f098` 也不是"上下文来自 tick 钩子"**。`nm --numeric-sort` 显示该地址就是
   `esp_vApplicationTickHook` 的**函数入口**，它紧跟在 `esp_system_abort` 之后；而
   `esp_system_abort` 的 `sw ra,12(sp)` 保存的正是 `2f00f096: jal panic_abort` 的下一条指令。
   **纯函数相邻的巧合。**
3. `abort() was called at PC 0x...` 打印的是 `return_address - 3`（`esp_libc/src/abort.c:27`），
   所以真实调用者是 `0x2f00f092`，即 `_esp_error_check_failed` 的末尾
   （反汇编 `2f00f08e: jal 2f00c898 <abort>`）→ **就是 `ESP_ERROR_CHECK` 失败**。

**因果链**：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` 使 **≤4096 B** 的分配优先落在内部 RAM；
cJSON 每个 JSON 元素 ≈56 B 节点（外加键与字符串值的拷贝），833 词的 cet6 词库约 7000 个节点，
于是 `cJSON_Parse()` 把 ~107 KB 的内部空闲抽到接近 0。此刻 WiFi 正在连网，其
`ets_timer_setfn()` → `esp_timer_create()` 要的是 `MALLOC_CAP_INTERNAL`
（**不允许回退 PSRAM**）的 ~100 B，拿不到即 `ESP_ERR_NO_MEM`，外面包着 `ESP_ERROR_CHECK`
→ abort → 重启循环。拔卡时解析的是内置 32 词的小词库，内部 RAM 不会被抽干 ——
**这就是"只有插卡才崩"**；而"间歇/时序相关"只是因为崩溃取决于那次 WiFi 内部分配是否正好
落进解析窗口，本机 3/3 必崩只是这个 ~5.9 s 窗口每次都重合。

### 修复（已实现，已上机验证）

- **`components/sys_utils/src/sys_json.c`（新增）**：`sys_json_use_psram()` 用
  `cJSON_InitHooks()` 把 cJSON 的分配改成"**先 PSRAM、失败再回退默认堆**"；
  `main/app_main.c` 在最早期（`sys_log_init()` 之后、任何模块之前）调用一次。
- `word_service`：`items[]` / `progress[]` 改为显式
  `heap_caps_malloc_prefer / calloc_prefer(..., MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)`；
  并在 `cJSON_Parse()` 返回后打印 `book tree alive: internal N KB free`（判据行）。
- **删除**了 `app_main.c` 里那段"启动期预初始化 crypto 锁"的代码：它出自被推翻的归因，
  实测从未起作用，留着只会误导后来人。
- `sys_heap_log()` 末列由 `esp_get_minimum_free_heap_size()`（被 12 MB PSRAM 主导、
  永远不动）改为 **`int-low`：内部 RAM 的历史最低水位** —— 这一类 bug 的判据就是它。

### 验证证据（真机，插卡 = 7.4 GB SDHC，`root=/sdcard`）

| 项 | 修复前 | 修复后 |
|---|---|---|
| 冷启动 ×3 | `abort=3/3`、`TH:app: ready 0/3` | **`abort=0/3`、`TH:app: ready 3/3`** |
| 词库 | 解析途中 abort，从未加载成功 | `loaded 833 words from 'words/cet6.json' (120311 B)` 3/3 |
| 解析中（JSON 树存活）内部 RAM | 被抽到接近 0 | **86 KB 空闲 / 最大连续块 40 KB** |
| 启动结束 | — | 9 模块全 ready、`storage=/sdcard`、`int-low 55 KB` |
| 端到端 | — | `words/stats` → `book=cet6 total=833`；`wifi/status` → connected；`POST+GET wifi/scan` → 22 个 AP 且设备存活 |

> 回归复现命令：`python tools\monitor_capture.py --port COM34 --seconds 18 --out build\boot.log`，
> 然后统计 `abort\(\) was called` 与 `TH:app: ready` 的出现次数。
> **注意**：PC 侧偶发 `http 000` / ping 丢包是 Windows 热点的老问题（实测丢包 33%），
> 不是设备问题 —— 判断设备死活请看串口的 `TH:wifi: link:` 行。

---

## 3. 八项需求的状态

| # | 需求 | 状态 | 说明 |
|---|---|---|---|
| 1 | 修复待办无法同步 | ✅ **结论：不是同步的问题** | 端到端实测通的：`POST /api/v1/todos`→201→设备串口 `TH:todo: added #2`；`todo_manager.c:183-190` 同时发 `TODO_EVENT_LIST_CHANGED`（设备页刷新）与 `sys_notify("todo_changed")`（SSE 推网页）。用户遇到的"无法同步"是**设备从 PC 不可达** —— Windows 移动热点的间歇性丢包（实测：设备自称 `connected rssi=-33` 而 PC 同时 100% 丢包，本地 RTT 112 ms）。建议用 IP 而非 mDNS、关掉 WLAN 网卡"允许关闭以省电"、或换手机热点 |
| 2a | 待机眼睛动画（只眼睛/无嘴巴/零位图） | ✅ 已实现，**待你目视确认** | 交叉淡入(180/420ms smoothstep)、随机眨眼 2.6~7.5s/110~165ms 且 18% 连眨、左右看 4~12s、微扫视 1.8~5.2s、呼吸 12~19s 重抽；约 +230B 静态内存、零新对象、单 `lv_timer`。**修掉一个真 bug**：`tick_behaviour()` 曾被编译器报 unused，而眨眼/扫视/微扫视的**全部触发逻辑**和临时表情到期恢复都只在它里面 —— 即那张脸原本**根本不会自己眨眼**、`idle_face_express()` 的表情**永不结束**。已把它接回 `idle_face_tick()`（且必须在 `effective_state()` 之前调用）。**待上机确认**：`idle_face_standby_exit()` 的 700ms 睁眼几乎看不见 —— `ui_core` 在 `on_hide()` 后立刻给页面容器加 `LV_OBJ_FLAG_HIDDEN` |
| 2b | 单词书 cet4→cet6 | ✅ **已端到端验证** | 卡上 `F:\todohelper\words\cet6.json` 与仓库 `assets/words/cet6.json`，**833 条**/120311 字节/sha256 一致。真机日志：`loaded 833 words from 'words/cet6.json'`、`ready (book 'cet6', 833 words, 10/day)`、HTTP `words/stats`→`book=cet6 total=833`。**注意：833 条是自编精选子集，不是官方约 2000 词表**。`WORD_DEFAULT_BOOK` 已改为 `cet6`；**已存在的设备必须 `POST /api/v1/words/config {"book":"cet6"}` 才会切**（NVS 存的是书名）。顺带修了 `progress_key()` 未按书名命名空间（裸 `w<id>` 会让 CET4 进度套到 CET6 前 32 词），现为 `<book>#w<id>` |
| 3 | SD 卡资源 | ✅ | 六级词库已上卡；字体 `ui_cjk.ttf` 本就在卡上；**待机动画是程序化绘制，不需要任何图片** |
| 4 | 设备端待办逻辑 | ✅ **按用户最终确认实现** | 用户中途澄清：设备端**保留"勾选完成 + 删除"**，只是不能新增（新增/编辑要中文输入，本板无 IME）。已加回 `row_toggle_cb`（失败回滚勾选状态，不让 UI 撒谎）与行尾 52×34「删除」按钮；**新增浮层与软键盘保持移除**；详情页只读；提示语「此页可勾选完成 / 删除 \| 新增请到电脑网页端」 |
| 5 | 优化网页上位机 | ✅ 主体完成 | `components/sync_server/web/index.html` 重写为 1214 行/54KB，纯原生 JS + 内联 CSS、**零外部资源**。SSE 实时（无 token 用 `EventSource`；有 token 改用 `fetch`+`ReadableStream` 以便带 `X-Auth-Token`）、指数退避重连 + 断开降级 15s 轮询、`pagehide` 关流释放设备仅有的 5 个 socket、增删改查失败**回滚 UI 不假装成功**、筛选/搜索、设备状态面板、离线/401/501 分类错误横幅。**未做**：WiFi 配置面板（子 agent 被中断，只留下页头注释，无 DOM/无函数） |
| 6 | 开机屏幕滚动启动日志 | ✅ 已实现，**待你目视确认** | `sys_log` 新增无锁 64×80 环形缓冲（seqlock 发布、PSRAM 分配+内部 RAM 兜底、serial 路径不变）；`page_splash.c` 用 `lv_timer` 刷新并 tail 最新行。`main/app_main.c` 改为固定 3s 延迟（用户要求）后切 HOME |
| 7 | 时刻更新 harness 待办 | ✅ | 各轮均同步维护 |
| 8 | 子 agent + 文档 + git | ✅ | 用了 8 个子 agent 分工；构建 0 错误 0 警告；文档已同步；全部提交见 §6 |

---

## 4. 只有"人"能完成的验证（AI 做不了）

1. **待机动画**：待机时是否**保持睁眼并按时眨眼/扫视**（这正是修 bug 的地方）、进出待机是否平滑。
2. **开机日志**：3 秒内日志是否顺畅滚动、是否卡启动。
3. **待办页**：勾选完成/删除按钮的观感与误触率（删除按钮 52×34，放在行尾）。
4. **网页**：SSE 实时联动、离线/401 横幅、移动端布局。
5. 若要继续做 WiFi 面板：先在浏览器确认 `/api/v1/wifi/status` 能返回（设备端 7 条路由都已注册，实测 GET 返回 200）。

---

## 5. 容易踩的坑（这些是我实际踩过的，别再踩）

1. **子 agent 的自述不能当验收。** 本轮 8 个子 agent 里有 3 个的自述与事实不符：
   - 一个声称动画完成、还声称 `-fsyntax-only` 零诊断，但它的触发层函数**从未被调用**（那张脸不会眨眼）；
   - 一个报告 `/api/v1/wifi/*` 未注册（404），实测**有 7 条路由且返回 200**；
   - 一个交回的是**页头注释**而非它声称的面板。
   → **验收必须自己 grep/读代码/上机**，`编译通过 ≠ 被调用`。
2. **不要让子 agent 构建或烧录。** `build/` 目录和 COM34 都只有一个，并行 `idf.py build` 会互相
   破坏。让它们只改文件，构建/烧录由你统一做。
3. **解析 panic 栈时的 addr2line 陷阱**：不要用 `-pfiaC` 然后"两行一取" —— 遇到内联函数时
   每个地址会输出多行，配对会**静默错位**（我第一次就是这么读错的）。正确做法：**逐个地址
   单独调用** `addr2line -f -C -e build\todohelper.elf 0x...`，**再用 `nm --numeric-sort`
   核对它落在哪个函数区间**（本次两处误读都是"函数相邻"造成的：`esp_system_abort` 的
   `jal panic_abort` 让 RA 恰好等于下一个函数 `esp_vApplicationTickHook` 的入口地址）。
   并且**先把整段 panic 文本读完**：`ESP_ERROR_CHECK failed: esp_err_t 0x...` 这种行是最直接的
   答案，比栈解析可靠得多（§2 的根因就写在里面，却被忽略了整整一轮）。
4. **`-Werror` 很严**：`format-truncation`（缓冲区要留够，例如 `char key[48]` 存 `"<book>#w<id>"`）、
   `unused-function`、隐式声明都会让构建失败。**但注意 `unused-function` 只是警告**（构建仍过），
   所以它会掩盖"函数根本没被调用"这类真 bug —— 看到这类警告**要查语义，不要直接删**。
   （新增：`discarded-qualifiers` 也会让构建失败 —— `cJSON_InitHooks(&hooks)` 不能传 `const` 指针。）
5. **设备从 PC 不可达 ≠ 代码错**。先 `ping` + 看设备自报的 `link:` 日志；Windows 热点会间歇性丢包
   （ARP 缓存/网卡省电；本次实测 ping 丢包 33%，同一时刻设备 `link: connected rssi=-33`）。
   `arp -d` 需要提权。
6. **换存储介质会"丢数据"观感**：存储根在 `/littlefs` 与 `/sdcard` 之间切换时，待办数据文件也
   跟着换位置（无卡时建的待办插卡后看不见）。这是设计使然，但用户会当成 bug。
7. **字体**：LVGL v9 的 FreeType **不能**用内存字体，必须从文件路径加载（现在是
   `/sdcard/todohelper/fonts/ui_cjk.ttf`）；无卡时回退片内字库（约 2500 字，生僻字会方框）。
   字库已从 app 里移出（镜像 4.16MB→2.94MB，烧录 43.6s→31.6s）；要恢复内嵌需**同时**改
   `ui_core/CMakeLists.txt` 与 `ui_fonts.c` 两半，否则链接失败。
8. **`ESP_ERR_NO_MEM` 不一定是"没有内存"，而是"没有*内部*内存"**。`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`
   让 ≤4 KB 的分配优先吃内部 RAM，且**部分调用（如 `MALLOC_CAP_INTERNAL`）根本不允许回退 PSRAM**。
   批量数据（JSON、词库、缓存）必须显式要求 PSRAM —— 否则就会复现 §2 那类崩溃。
   判据是 `TH:heap:` 行的 `largest` 与 `int-low`，不是总空闲量。
9. 更完整的坑位清单（内部 RAM 最紧处、PSRAM 栈不能碰 flash、日志格式陷阱、
   `esp_log_writev` 原样输出、PHY 需 30KB 连续块……）全部在 **`AGENT.md §9.0 ~ §9.0.5`**。

---

## 6. 提交记录（最新在前）

| commit | 内容 |
|---|---|
| `0936a27` | 交接文档重写 + 现阶段目标与需求 + 崩溃修复尝试的诚实结论 |
| `e365448` | `AGENT.md 9.0.5`：把启动崩溃定位到 crypto 锁惰性初始化竞态（**该归因后被推翻**） |
| `2dfa7b7` | 开机日志改为固定 3 秒延迟 + 恢复原启动时序 + 崩溃实测数据 |
| `e126c31` | 设备端待办：保留勾选完成 + 删除，仅新增留给网页 |
| `561a591` | 第二轮收尾：待机动画、cet6 词库、设备端只读、网页上位机、开机滚动日志 |
| `1ae576d` | `AGENT.md 9.7` 更新 + 目标项 2 验收证据 |
| `0397874` | 删除天气页残留的不可达输入浮层代码 |
| `162ba2d` | 字库移出 app：镜像 4.16MB→2.94MB，烧录 43.6s→31.6s |

> 交接时工作区应当是**干净**的。若 `git status` 有改动，先判断是不是构建产物。

---

## 7. 建议的接手顺序

1. ~~验完 §2 的崩溃修复~~ —— **已完成**：插卡 3/3 冷启动 `abort=0`、`TH:app: ready` 3/3，
   词库 833 词 3/3 加载成功（证据见 §2）。
2. 请用户做 §4 的四项目视确认，按反馈微调（动画/日志时长/按钮尺寸都是单个 `#define` 或一行）。
3. 若要继续加功能：**网页 WiFi 配置面板**仍是最具性价比的一项 —— 设备端输 WiFi 密码体验很差
   （480px 屏、无 IME），而 `/api/v1/wifi/*` 7 条路由都已就绪。**本次已实测的报文语义**：
   `GET /api/v1/wifi/status` → connected/ssid/ip/rssi；`POST /api/v1/wifi/scan` 触发扫描并返回
   `{"scanning":true}`，**扫描结果要靠 `GET /api/v1/wifi/scan` 轮询**（它返回的是上一次结果，
   `POST` 之后立刻 `GET` 会得到空数组——本次踩过）。路由表在
   `components/wifi_manager/src/wifi_manager_http.c:259-284`。照 `index.html` 现有的请求漏斗与
   样式写，**不要**另起一套。
4. 若用户反馈"插拔卡数据丢失"不可接受：把待办存储固定在片内存储，只把词库/字体放卡上
   （需要看 `storage_manager` 的根选择逻辑）。
5. **加功能前先看内部 RAM 余量**：本次实测 boot complete 只剩 `internal 67 KB / largest 24 KB`
   （`int-low 55 KB`），明显低于 `AGENT.md §8` 记录的上一版基线（118 KB / 74 KB）。
   任何批量数据都必须显式 `MALLOC_CAP_SPIRAM`（§2 的崩溃就是没这么做造成的）。

---

## 8. 关键文件索引

| 关注点 | 文件 |
|---|---|
| 环境/命令/全部实测坑 | `AGENT.md`（**§9.0 起必读**，§9.0.5 = 崩溃，§9.7 = 剩余待办） |
| 启动流程与这次修复 | `main/app_main.c` |
| **崩溃根因与修复** | `components/sys_utils/src/sys_json.c`（`sys_json_use_psram()`）、`components/word_service/src/word_service.c`（`parse_book()`） |
| 内部 RAM 诊断（`largest` / `int-low`） | `components/sys_utils/src/sys_heap.c` |
| 待办页面（设备端） | `components/ui_pages/src/page_todo.c` |
| 待机动画 | `components/idle_face/src/idle_face.c`（`tick_behaviour()` 必须被调用！） |
| 开机日志缓冲 | `components/sys_utils/src/sys_log.c`、`include/sys_log.h` |
| 开机日志页面 | `components/ui_pages/src/page_splash.c` |
| 词库与进度 | `components/word_service/src/word_service.c`（`progress_key()`、`parse_book()`） |
| HTTP 路由总览 | `docs/API.md` |
| 网页上位机 | `components/sync_server/web/index.html` |
