# ToDoHelper 交接说明（HANDOVER）

> 面向**接手的下一个 Agent / 开发者**。本文档描述「当前真实状态、阻塞问题、下一步做什么、哪里最容易踩坑」。
>
> 配套阅读顺序：**本文档 → `AGENT.md`（环境/命令/全部实测坑，尤其 §9.0~§9.0.5、§9.7）→
> `docs/TODOHELPER_REQUIREMENTS.md`（需求）→ `docs/TODOHELPER_FRAMEWORK.md`（框架）→ `docs/API.md`（组件与 HTTP 接口）**。
>
> 最后更新：**`v1.5.0`** —— 待办主线补强（设备端看得到截止与优先级、可筛选、删除带确认；
> 网页端批量/排序/导出导入；落盘改去抖后台、坏文件不再让设备开机就崩）
> + 全工程一次六方向并行体检（69 条问题，13 条高危经独立复核全部成立）后的 24 项整改。
> **本轮的代码只构建、未烧录**，所以文中凡"真机"字样都指 v1.4.0 及更早。
> 本轮的施工规格与**下一轮候选**见 `docs/ROADMAP_v1.5.0.md`。
> 提交与标签见 §6；版本变更细节见 `CHANGELOG.md`。

---

## 0. 一句话现状

**版本 `1.5.0`。构建 0 错误 0 警告（`todohelper.bin` 0x2f85b0，app 分区剩 63%）。
本轮做了一次「体检 → 敲定范围 → 集群施工 → 构建闸门 → 独立复核」的完整闭环：
六方向并行审查出 69 条问题（13 条高危**全部经独立复核确认成立**），
按 `docs/ROADMAP_v1.5.0.md` 敲定的范围修完 24 项、补齐一批待办功能。
当前没有已知阻塞问题，但**本轮一行代码都没上过真机**——
所有 UI 效果、时序、内存数值都还是纸面结论。**

本轮（墨墨同步 + 音标）的成果与判据：

| 项 | 修前 / 之前 | 修后 |
|---|---|---|
| 音标 `/əˈbrʌpt/` | 渲染成 `/ br pt/`（元音全空白）—— 根源是 **Noto Sans SC 里没有 IPA 字形**，Montserrat 兜底也无效 | `merge_ipa_glyphs()` 从拉丁字体合入 **203 个字形**（×0.488），词书 1252 字符 **0 缺失** |
| 字体产物校验 | 只查 cmap（**所以坏字体通过了**） | `validate_font()` **重新打开产物**逐表核对度量，不过就构建失败 |
| 单词来源 | 只认本地 CET-6 词库（833 词） | 云端模式今日队列与**账号真实进度**来自墨墨（实测 10/20、20 词队列） |
| 助记/例句 | 没有 | 缓存到 `words/enrich.json`，卡片离线也能显示（**快乐路径未在设备上观察到**，见 §9） |
| 不会的词 | 只记在本地 | 可选推回墨墨学习计划（**默认关闭**，因为会改账号数据） |
| 从 Git-Bash 构建 | `idf.py` 直接拒绝运行 | `bash tools/build.sh build`（三个坑见 §5 第 11 条） |

> **最重要的认知之一**（已写入 `AGENT.md §9.0.4.2`）：**检查写错了比没有检查更糟。**
> 第一版字体校验只问"文件里有没有 ə？"，而 `vmtx` 短了 406 字节的坏字体
> **每个 cmap 条目都在**，于是检查通过、坏字体照样出厂。事后校验必须
> **重新打开产物**并**按表逐项核对**，而不是在原对象上查一个字段。

> **最重要的认知**（已写入 `AGENT.md §9.0.1.1`，改 UI 前必读）：适配器在**持有递归锁**
> 的情况下调用 `lv_timer_handler()`，所以 `lv_timer` 回调与**所有控件事件回调都在锁内**。
> 回调里出现 SD / flash / 网络调用就是 bug。


两个已修缺陷（都在 1.0.0，都被更早的交接文档误判过）：

| # | 现象 | 真因 |
|---|---|---|
| §2 | 插 SD 卡即崩溃重启循环（3/3） | **不是**"crypto 锁惰性初始化竞态"（那次归因是 panic 栈误读），而是**解析 546 KB 词库时 cJSON 把内部 RAM 抽干**，同时刻连网的 WiFi 驱动 `esp_timer_create()` 拿不到内部内存 → `ESP_ERROR_CHECK` abort |
| §3 需求 5 | 网页"推送：实时推送中"却"设备不可达/读取失败"（ping 通、TCP 全超时） | **不是**丢包，而是 `/api/v1/events` 是**永不返回的同步 handler**，而 `esp_http_server` 的 handler 在唯一 httpd 任务里串行执行 → 一个 SSE 订阅者冻住整台服务器 |

> 两处的详细根因、修复与实测数据见 §2 与 §3 需求 5；`AGENT.md §9.0.5`（内部 RAM）与
> `AGENT.md §9.0.1` 第 4 条（HTTP 长连接）是必须读的教训。`CHANGELOG.md` 有版本视角的汇总。

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

### 0.5.5 v1.3.0 这一轮做了什么（墨墨同步 + 音标修复）

**用户要的两件事**：① 把单词学习接到**墨墨背单词（开放 API）**，让设备显示账号的真实进度；
② 修掉音标显示成 `/ br pt/` 的问题。用户从三个方案里确认了 **A / B / C 全做**。

#### ① 音标缺失：根因是"源字体里根本没有 IPA 字形"

- **现象**：`/əˈbrʌpt/` 渲染成 `/ br pt/`，元音符号整片空白。
- **根因**：**Noto Sans SC（CJK 子集的来源）里一个 IPA 字形都没有** —— U+0259 ə、U+028C ʌ、
  U+026A ɪ、U+02D0 ː、U+02C8 ˈ 等**不在它的 cmap 里**，字符集写多大都取不出来。
  **LVGL 内置的 Montserrat 兜底也无效**（cmap 稀疏，只有 FontAwesome 区段）。
  也就是说 §9.0.4 那条 fallback 链对 IPA 帮不上忙。
- **修法**（`tools/gen_cjk_font.py` 的 `merge_ipa_glyphs()`）：把 IPA 字形从**一份拉丁字体**
  合入 CJK 子集（Linux DejaVu Sans；Windows `segoeui`/`arial`/`calibri`）。
  - `fontTools.merge` **不能用**：它断言两边 `unitsPerEm` 相等，而拉丁是 **2048**、CJK 是 **1000**
    → 只能手写合并 + **坐标/advance 缩放 0.488**。
  - 字形必须**展平成普通轮廓**：组件会引用目标字体里不存在的字形 → 编译期 `KeyError: 'uni02BE'`。
  - 实测从 `segoeui.ttf` 合入 **203 个字形**。
- **验收数值**：词书 **1252 个不同字符、0 缺失**；`/əˈbrʌpt/` 全部有真实轮廓，
  advance 已正确缩放（ə=523 vs b=618 / r=388 / p=620）。

#### ② 顺带挖出的潜伏 bug：`vmtx` 没跟着 `hmtx` 一起更新

- 第一版合并更新了 `hmtx` 却漏了 `vmtx`。两张表都以**字形名**为键、都必须**每个字形一条记录**，
  所以产物本身已经不合法：**`vmtx` 少 406 字节（2 × 203）**，
  fontTools 连**重新解析**都做不到 —— `not enough 'vmtx' table data: expected 10686, got 10280`。
  **这份 CJK 子集确实带中文竖排度量**（`vmtx`/`vhea` 都在），所以不是理论问题。
- **为什么它会出厂**：当时的校验只问 **"文件里有没有 ə？"**，而坏字体**每个 cmap 条目都在** →
  **检查通过**。`cmap` 只描述码位到字形名的映射，**完全不涉及度量表**。
- **修法两半**：给每个合入的字形**同时写 `vmtx` 记录**，并把
  `hhea.numberOfHMetrics` / `vhea.numberOfVMetrics` **夹到字形总数以内**；
  新增 **`validate_font()`**：重新打开**保存后**的文件，核对
  `len(hmtx.metrics) == len(vmtx.metrics) == maxp.numGlyphs`、计数不越界、必需字符在 cmap 里，
  **不过就 `sys.exit()` 让构建失败**。校验只要求**内容字符**（词书字符 + 有拉丁源时的 IPA），
  不要求装饰性 Unicode 区段 —— 否则会变成人人无视的噪音。

#### ③ 新组件 `components/maimemo_client/`（墨墨开放 API，三个方案）

| 方案 | 内容 | 默认 |
|---|---|---|
| **A 学习同步** | 今日队列与**账号真实进度**来自墨墨，而不是本地书。`/study/get_study_progress`、`/study/get_today_items`、`/study/query_study_records`（`{"as_count":true}` 取计划词数）。**实测线上**：10/20 完成、20 词队列 | 需令牌 + NVS `mm_mode=cloud` |
| **B 助记/例句预取** | `/notes`、`/phrases` 缓存到 `<app_dir>/words/enrich.json`（`storage_read_text()` / `storage_write_file()` **原子写**），整文件读进 PSRAM、整文件重写 | 需令牌**且有读取作用域** |
| **C 反向推送** | 评为「不认识」的词用 `/study/add_words` 推回墨墨学习计划 | **默认关闭**（会改账号数据），NVS `mm_push` |

- **令牌与作用域是一等问题**：存 NVS `mm_token`（`sys_config_set_str`），网页控制台录入，
  **不记日志、任何 API 都不回读**（与 AI Key 同规则，需求 §8.8）；
  `GET /api/v1/maimemo/status` 只回 `configured` + 掩码提示（`g_zV****`）。
  **用户的令牌是学习作用域的**：`/study/*` 可用，`/notes`、`/phrases` 返回
  `common_permission_denied`。这被映射为 **`ESP_ERR_NOT_SUPPORTED`**（"别再问了"）
  并**按词缓存为不可用**，而不是当错误反复重试 —— 能同步队列但不显示助记的设备**仍是有用的**。
- **错误模型**：墨墨**权限错误也返回 HTTP 200**，错误写在信封的 `success:false` +
  `errors[].code` 里 → **只看状态码会漏掉全部权限/参数错误**，必须解析信封
  （`AGENT.md §9.0.6 ①`）。
- **`advance` 是 `/study/add_words` 的必需属性**：漏掉它即使请求其它部分合法也报
  `common_invalid_param: {base} must have required property 'advance'`。现在**永远发送**
  （`false` 也发），并回报**服务器自己的 `added_count`**而不是发送条数（`AGENT.md §9.0.6 ②`）。
- **无令牌静默降级**：云端模式无声关闭，本地 CET-6 词库照常工作，不报错不弹窗
  （`maimemo_client` 在 `app_main.c` 里是 `optional = true`）。
- **线程规则**：组件内所有网络函数都是**阻塞 TLS**，**绝不允许从 LVGL 任务调用**
  （LVGL 回调持有适配器锁，见 `AGENT.md §9.0.1.1`）。UI 只调 `maimemo_sync_start()`
  （worker 任务 + 事件回调）或只读内存的 `maimemo_get_cached_progress()`；
  `page_word.c` 的助记预取用 `lv_async_call` 起一次性任务。

#### ④ UI 与网页

- **`page_word.c`**：云端模式下显示账号真实进度
  （「墨墨今日 10/20 · 已用 59 秒 · 计划 20 词」，否则回退本地计数器）；
  **小于 1 分钟时显示秒**，因为把 59 秒四舍五入成「0 分」读起来是"你今天没学过"。
  助记/例句显示在答案下方（方案 B），拿不到时**整块隐藏**。
- **网页上位机新增「墨墨背单词（开放 API）」面板**
  （`components/sync_server/web/index.html`）：令牌录入（**只写不读**，已核实无回显路径）、
  推送开关、带轮询的「立即同步」、今日词表查看器。
- **`word_service` 新增云端队列**：`word_cloud_mode()` / `word_set_cloud_mode()` /
  `word_cloud_refresh()` / `word_cloud_count()`。云端词按 `voc_spelling` **匹配回本地词库**，
  匹配得上就继续用本地 level / 间隔；匹配不上的标 `WORD_ID_CLOUD`。

#### ⑤ 构建工具 `tools/build.sh`（MSYS 下不能直接驱动 `idf.py`）

MSYS/Git-Bash 里 `idf.py` **拒绝运行**（ESP-IDF 的 wrapper 检测 MSYS 环境后打印
`MSys/Mingw is no longer supported`）。新增 `tools/build.sh`（bash 入口）+ `tools/build.ps1`：

```bash
bash tools/build.sh build      # 或 flash，或 -v build
```

三个坑**都会静默产出空日志 + 退出码 0**，详见 §5 第 11 条与 `AGENT.md §4.2.1`。

#### ⑥ 本轮新增的 HTTP 路由（6 条）

`GET /api/v1/maimemo/status`、`POST /api/v1/maimemo/config`、`POST /api/v1/maimemo/sync`、
`GET /api/v1/maimemo/today`、`GET /api/v1/maimemo/enrich?spelling=`、
`POST /api/v1/maimemo/push`。组件细节见 `components/maimemo_client/README.md`。

---

## 1. 环境与命令（原文档漏了标题）

- 目标芯片 `esp32s31`（ESP32-S31-Korvo，800×480 RGB 屏 + GT1151 触摸 + ES8389 + SDMMC 4-bit）
- ESP-IDF **v6.1**，路径 `D:\ESP-IDF\v6.1\esp-idf`；工程 `D:\project\ESP-IDFv6.1\ToDoHelper`
- 串口 **COM34**（115200）；设备走 DHCP，地址以 mDNS `todohelper.local` 或
  `GET /api/v1/device/info` 的 `ip` 为准（**不是固定 IP**，换网络就变）

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

**在 Git-Bash / 自动化里构建**（v1.3.0 新增；MSYS 下 `idf.py` 会直接拒绝运行）：

```bash
bash tools/build.sh build          # 等价 idf.py build（默认动作就是 build）
bash tools/build.sh flash          # 等价 idf.py flash
bash tools/build.sh -v build       # 详细构建；日志同时落在 build_last.log（已 gitignore）
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
> **v1.3.0 起模块数从 9 变成 10**（新增 `maimemo_client`，`optional = true`，
> 排在 `word_service` 之前）—— 上面这行"9 模块全 ready"是 v1.0.0 的记录，
> 下次上机应当看到 **10 模块 ready**（无令牌时 `TH:maimemo: ready (token=unset, push=off)`
> 也算 ready，**不会**因为没有令牌而失败）。
> **注意**：PC 侧偶发 `http 000` / ping 丢包是 Windows 热点的老问题（实测丢包 33%），
> 不是设备问题 —— 判断设备死活请看串口的 `TH:wifi: link:` 行。

---

## 3. 八项需求的状态

| # | 需求 | 状态 | 说明 |
|---|---|---|---|
| 1 | 修复待办无法同步 | ✅ **结论：不是同步的问题** | 端到端实测通的：`POST /api/v1/todos`→201→设备串口 `TH:todo: added #2`；`todo_manager.c:183-190` 同时发 `TODO_EVENT_LIST_CHANGED`（设备页刷新）与 `sys_notify("todo_changed")`（SSE 推网页）。用户遇到的"无法同步"是**设备从 PC 不可达** —— Windows 移动热点的间歇性丢包（实测：设备自称 `connected rssi=-33` 而 PC 同时 100% 丢包，本地 RTT 112 ms）。建议用 IP 而非 mDNS、关掉 WLAN 网卡"允许关闭以省电"、或换手机热点 |
| 2a | 待机眼睛动画（只眼睛/无嘴巴/零位图） | ✅ **基础层与 1.1 三层均经用户目视确认正常（2026-09-23）** | 交叉淡入(180/420ms smoothstep)、随机眨眼 2.6~7.5s/110~165ms 且 18% 连眨、左右看 4~12s、微扫视 1.8~5.2s、呼吸 12~19s 重抽；约 +230B 静态内存、零新对象、单 `lv_timer`。**修掉一个真 bug**：`tick_behaviour()` 曾被编译器报 unused，而眨眼/扫视/微扫视的**全部触发逻辑**和临时表情到期恢复都只在它里面。**1.1 新增三层慢动作**：① 自发表情 13~34s 一次（320~1100ms，5 种候选）；② 困倦度 2.5 分钟攒满（眨眼更密更慢、眼睛矮 10%、视线下垂 5px，真实交互立刻清零、**自发表情不清零**）；③ 瞳孔缩放 0.91~1.14 + 整脸 ±3/±2px 漂移；另加 28% 半眯眨、9% 慢眨、22% 二连看。**顺带修掉 3 个真 bug**（`standby_exit` 恒返回 INVALID_STATE 属死代码、首次进待机被已过期的临时表情静默拒绝、退出过场因 `ui_core` 当帧隐藏而从未可见）。细节与实测数据见 `CHANGELOG.md` 1.1.0 与 `AGENT.md §9.7 ①` |
| 2b | 单词书 cet4→cet6 | ✅ **已端到端验证** | 卡上 `F:\todohelper\words\cet6.json` 与仓库 `assets/words/cet6.json`，**833 条**/120311 字节/sha256 一致。真机日志：`loaded 833 words from 'words/cet6.json'`、`ready (book 'cet6', 833 words, 10/day)`、HTTP `words/stats`→`book=cet6 total=833`。**注意：833 条是自编精选子集，不是官方约 2000 词表**。`WORD_DEFAULT_BOOK` 已改为 `cet6`；**已存在的设备必须 `POST /api/v1/words/config {"book":"cet6"}` 才会切**（NVS 存的是书名）。顺带修了 `progress_key()` 未按书名命名空间（裸 `w<id>` 会让 CET4 进度套到 CET6 前 32 词），现为 `<book>#w<id>` |
| 3 | SD 卡资源 | ✅ | 六级词库已上卡；字体 `ui_cjk.ttf` 本就在卡上；**待机动画是程序化绘制，不需要任何图片** |
| 4 | 设备端待办逻辑 | ✅ **按用户最终确认实现，目视确认正常（2026-09-23）** | 用户中途澄清：设备端**保留"勾选完成 + 删除"**，只是不能新增（新增/编辑要中文输入，本板无 IME）。已加回 `row_toggle_cb`（失败回滚勾选状态，不让 UI 撒谎）与行尾 52×34「删除」按钮；**新增浮层与软键盘保持移除**；详情页只读；提示语「此页可勾选完成 / 删除 \| 新增请到电脑网页端」。用户已确认按钮观感与误触率可接受 |
| 5 | 优化网页上位机 | ✅ 主体完成 + **本轮修掉一个致命 bug（用户实测发现）**，刷新后确认正常（2026-09-23） | `components/sync_server/web/index.html` 1386 行/57KB（1.0.0），纯原生 JS + 内联 CSS、**零外部资源**。SSE 实时（无 token 用 `EventSource`；有 token 改用 `fetch`+`ReadableStream` 以便带 `X-Auth-Token`）、指数退避重连 + 断开降级 15s 轮询、`pagehide` 关流、增删改查失败**回滚 UI 不假装成功**、筛选/搜索、设备状态面板、离线/401/501 分类错误横幅。**但设备端 `/api/v1/events` 原本是同步 handler：`esp_http_server` 的 handler 在唯一 httpd 任务里串行执行，所以只要有一个浏览器挂着 SSE，整台设备的 HTTP 全部超时**（用户截图里"推送：实时推送中"+"设备不可达/读取失败"就是这个）。已改为 `httpd_req_async_handler_begin()` + 每客户端一个流式任务（并发从 4 降到 3，给页面/REST 留槽），实测挂 3 条流时 `GET /` 仍 200/42ms、第 4 条流 503、推送仍实时。详见 AGENT.md §9.0.1 最后一条。**未做**：WiFi 配置面板（子 agent 被中断，只留下页头注释，无 DOM/无函数） |
| 6 | 开机屏幕滚动启动日志 | ✅ 已实现，**用户目视确认正常（2026-09-23）** | `sys_log` 新增无锁 64×80 环形缓冲（seqlock 发布、PSRAM 分配+内部 RAM 兜底、serial 路径不变）；`page_splash.c` 用 `lv_timer` 刷新并 tail 最新行。`main/app_main.c` 改为固定 3s 延迟（用户要求）后切 HOME；用户确认 3 秒内滚动顺畅、不卡启动 |
| 7 | 时刻更新 harness 待办 | ✅ | 各轮均同步维护 |
| 8 | 子 agent + 文档 + git | ✅ | 用了 8 个子 agent 分工；构建 0 错误 0 警告；文档已同步；全部提交见 §6 |
| — | **v1.3.0 追加需求**：单词接墨墨（方案 A/B/C）+ 修音标 | ⚠️ **代码完成、线上 API 实测、未上机** | 详见 §0.5.5 与本轮 `CHANGELOG.md` 1.3.0 的 `### Verified（真机）` / `### Known gaps`。**用户下达的是"做"而非"烧录验证"**，本轮按此执行：没有烧录设备 |

---

## 4. 只有"人"能完成的验证（AI 做不了）

> **状态：§4 前六项均已由用户确认（2026-09-23，含 1.1 的待机三层慢动作与开机交接淡入）。**
> 下面是保留下来作为**回归清单** —— 以后改动 UI/HTTP/内存相关代码后，请再走一遍。
>
> **v1.3.0 追加了第 7 组（墨墨 + 音标），但它还没有被任何人验证过** ——
> 本轮只构建、未烧录，所以下面第 7 组是**待办清单**而不是"已确认"。

1. **待机动画**（1.1 重点）：
   - 基础层不变：待机时**保持睁眼**、按时眨眼、偶尔左右看；
   - **等 1~2 分钟**：应能看到"自发小表情"（眯眼笑 / 半闭 / 睁大一下 / 往上看 / 上弧抖一下），
     每次 0.3~1.1 s，间隔十几到三十几秒；
   - **再等到 2.5 分钟以上**：脸应明显"困"下来（眨眼更密、眼睛略矮、视线下垂）；
   - **碰一下屏幕唤醒**：应看到一次**睁眼过冲**（眼睛先睁大再回到正常），然后回到待机前页面。
2. **开机日志**：3 秒内日志是否顺畅滚动、状态行省略号是否在动、是否卡启动。
3. **待办页**：勾选完成/删除按钮的观感与误触率（删除按钮 52×34，放在行尾）。
4. **开机 → 主界面的交接**（1.1 新增）：日志页应**淡出并微微上移**、主界面同时淡入滑入
   （交叉淡入，约 0.24 s），不应有瞬间硬切；另外网页与设置页顶部应分别显示 `固件 1.3.0`。
5. **网页**：页面打开后应能读到待办与设备状态；"推送：实时推送中"与能用的页面**同时**存在
   （1.0.0 之前这两者是互斥的，见 §3 需求 5）；离线/401 横幅、移动端布局。
6. 若要继续做 WiFi 面板：`/api/v1/wifi/*` 7 条路由都已注册，本轮实测 `GET status` → connected、
   `POST scan` → `{"scanning":true}`、随后 `GET scan` → 22 个 AP（报文语义见 §7 第 4 条）。
7. **v1.3.0 待验证（本轮全部未做）**：
   - **音标**（最便宜、判据最硬）：把新生成的 `ui_cjk.ttf` 拷到
     `<storage>/todohelper/fonts/` 覆盖同名文件后**重启**（**不需要重新烧录**）。
     判据：单词卡背面的 `/əˈbrʌpt/` 里 ə、ʌ、ˈ、ː **不再是空白**；顺带确认没有新的方框。
   - **墨墨面板**（网页）：「保存并校验」是否返回 `verify: "ok"`；令牌输入框
     **刷新后应保持空白**（只写不读）；「立即同步」后设备单词页顶部是否变成
     「墨墨今日 N/M · …」。
   - **方案 B**：**当前令牌做不到**（学习作用域读不了 `/notes`）——
     能验证的只是"没有助记时卡片不出现空白块、也不报错"。
     要看到真助记得换一个能读 `/notes` 的令牌。
   - **方案 C**：默认关闭。开启后评「不认识」，去墨墨 App 确认词**真的进了学习计划**
     （设备侧只会打 `pushed to the study plan: N accepted of M resolved`）。
   - **不配令牌的降级**：清掉令牌后重启，单词页应完整回退本地词库，**不报错、不弹窗**。

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
9. **HTTP handler 不能"长期不返回"**。`esp_http_server` 的 handler 在唯一 httpd 任务里**串行**执行，
   一个 SSE/流式 handler 会把整台设备的 HTTP 堵死（ping 得通、TCP 建不上）。任何长连接都要
   `httpd_req_async_handler_begin()` + 自己的任务。**这是本轮用户实测踩到的第二个坑**，
   详见 `AGENT.md §9.0.1` 最后一条（`/api/v1/ai/chat/stream` 还有同类隐患没修）。
10. 更完整的坑位清单（内部 RAM 最紧处、PSRAM 栈不能碰 flash、日志格式陷阱、
   `esp_log_writev` 原样输出、PHY 需 30KB 连续块……）全部在 **`AGENT.md §9.0 ~ §9.0.6`**。
11. **MSYS 下 `idf.py` 根本不能被驱动，而且四个坑都静默失败（空日志 + 退出码 0）**（v1.3.0）：
    - ESP-IDF 的 wrapper 检测到 MSYS 环境**直接拒绝运行**（`MSys/Mingw is no longer supported`）；
    - IDF 的 PowerShell profile 把 `idf.py` 定义成**函数**并用 `@args` 转发，**该简写不绑定**
      当调用方是 `powershell -Command`/`-File` → `idf.py build` 静默什么都不做；
    - **原生 exe 接不进 cmdlet 管道**（`Cannot run a document in the middle of a pipeline:
      ...python.exe`），所以 `| Tee-Object` 用不了；
    - **`env -i` 清空环境是致命一击**：powershell 正常启动并打印横幅，但原生 python 子进程
      **一点输出都没有**，`$LASTEXITCODE` 保持 `$null` → 空日志 + 假成功。
      → 保留 bash 继承的环境，在 PowerShell **内部** `Remove-Item Env:MSYSTEM`
      （在 bash 里 unset 没用：MSYS 会把它重新注入每个原生子进程）。
    用 `bash tools/build.sh` 或照 `AGENT.md §4.2.1` 自己写；**日志是空的几乎必然是这四条之一**。
12. **字体这类"多表互相索引"的二进制格式，改一处往往要改好几处**（v1.3.0）：
    合并字形时只更新 `hmtx` 没更新 `vmtx`，产物**少 406 字节**、fontTools 连重新解析都做不到。
    **而只查 cmap 的校验会愉快放行**（坏字体每个 cmap 条目都在）。
    → 事后校验必须**重新打开产物、按表逐项核对**，且**校验的覆盖面要对着 bug 的类型设计**。
    详见 `AGENT.md §9.0.4.2`。**检查写错了比没有检查更糟。**
13. **对接第三方云 API 的两条通用规律**（v1.3.0，墨墨）：
    ① **HTTP 200 也可能是失败** —— 错误写在 `success:false` + `errors[].code` 信封里，
    只看状态码会漏掉全部权限/参数错误；
    ② **哪些字段是必填只能靠实际打一次来确认** —— `/study/add_words` 的 `advance` 是**必填**
    （漏了报 `common_invalid_param: ... must have required property 'advance'`，文档没写清）。
    并且**返回值以对端自报的为准**（回报服务器的 `added_count`，不是我们发过去的条数）。
    详见 `AGENT.md §9.0.6`。

---

## 6. 提交记录（最新在前）

| commit / tag | 内容 |
|---|---|
| **`v1.3.0`**（tag，本轮） | **墨墨背单词开放 API（方案 A/B/C）**：新组件 `maimemo_client`（今日队列 + 账号真实进度 + 助记例句缓存 + 反向推词）、`page_word.c` 云端进度与助记显示、网页「墨墨背单词」面板、6 条 `/api/v1/maimemo/*` 路由；**音标修复**（`merge_ipa_glyphs()` 合入 203 个 IPA 字形）与潜伏的 `vmtx` 缺条目 bug（新增 `validate_font()`）；**`tools/build.sh` / `build.ps1`** 让 Git-Bash 能构建。**只构建、未烧录** |
| **`v1.2.0`**（tag） | **流畅度第一轮**：消除 LVGL 锁内阻塞 I/O（异步校时 / 合并落盘 / 时区任务）、事件投递改无锁队列、周期性 label 全加脏检查、**整页动画删除改为同帧原子替换**（修切页重影）；待机动画第二批三层（呼吸联动 / 双眼扫视错峰 / 困倦点头）与 FPS 探针 |
| **`v1.1.0`**（tag） | **待机动画慢动作三层**（自发表情 / 困倦度 / 瞳孔缩放 + 整脸漂移）、**切页交叉淡入 + 离场动画窗口**（开机日志 → 主界面）、开机页动画省略号与真实版本号；顺带修掉 `standby_exit` 死代码、首次进待机被过期表情拒绝、开机页版本恒为 `0.1.0` |
| **`v1.0.0`**（tag） | **首个编号版本**：新增 `version.txt` 并在设备设置页显示、根 `README.md` 改写为 ToDoHelper 说明、新增 `CHANGELOG.md`、全量文档回改 |
| `86ba312` | **修复 SSE 冻住整个 HTTP 服务器**：`/api/v1/events` 改异步 handler + 每客户端流式任务，并发 4→3；用户实测"网页设备不可达"的真因 |
| `cfdf817` | 记录 `b4d7683` 提交哈希 |
| `b4d7683` | **修复插卡启动崩溃**：真因是内部 RAM 被 cJSON 解析抽干 → 新增 `sys_json_use_psram()`；删掉 crypto 锁那段错误归因代码；`sys_heap_log()` 增 `int-low` 列；AGENT.md 9.0.5 改写 |
| `0936a27` | 交接文档重写 + 现阶段目标与需求 + 崩溃修复尝试的诚实结论 |
| `e365448` | `AGENT.md 9.0.5`：把启动崩溃定位到 crypto 锁惰性初始化竞态（**该归因后被推翻**） |
| `2dfa7b7` | 开机日志改为固定 3 秒延迟 + 恢复原启动时序 + 崩溃实测数据 |
| `e126c31` | 设备端待办：保留勾选完成 + 删除，仅新增留给网页 |
| `561a591` | 第二轮收尾：待机动画、cet6 词库、设备端只读、网页上位机、开机滚动日志 |
| `1ae576d` | `AGENT.md 9.7` 更新 + 目标项 2 验收证据 |
| `0397874` | 删除天气页残留的不可达输入浮层代码 |
| `162ba2d` | 字库移出 app：镜像 4.16MB→2.94MB，烧录 43.6s→31.6s |

> 交接时工作区应当是**干净**的。若 `git status` 有改动，先判断是不是构建产物。
> **v1.3.0 交接时工作区是脏的**：`components/maimemo_client/`、`tools/build.sh`、
> `tools/build.ps1` 是新增未提交，另有若干组件与文档改动 —— 本轮**尚未提交**。

---

## 7. 建议的接手顺序

> **本节已按 v1.5.0 重写。** 下面先给当前的顺序，紧跟着的「v1.3.0 时期的清单」是历史留档
> ——那一轮列的项现在基本都做完了（AI 流式异步、天气定位异步、待办落盘去抖、
> 网页端批量与排序都已落地），别再照着它重复劳动。

### 7.0 当前顺序（v1.5.0 之后）

0. **第一件事：把 v1.5.0 上机验证掉。** 本轮**只构建、未烧录**，下面几件都还是纸面结论：
   1. **待办主线**（本轮的主戏）：设备端待办行应显示到期徽标与优先级色条；
      筛选栏四档可切；删除会弹确认框；详情页到期时间显示成 `2026-10-21 09:00`；
      首页有「今日待办」卡片且点击能跳过去。
      判据：`docs/ROADMAP_v1.5.0.md` §2 的每条「验收」。
   2. **落盘不再卡**：设备端点一下复选框**应当立刻响应**（不再同步写卡）；
      串口能看到去抖写盘 worker 的日志；连续勾 5 项最终只写 1~2 次。
   3. **掉电恢复**：把 `cache/todos.json` 手工改坏（或删掉），重启后设备应能
      恢复出一份可用的清单，而不是崩在开机路径上。
   4. **鉴权**：设备上设好 token 后，浏览器不带 token 访问 `GET /api/v1/todos`
      应得到 401（此前是公开可读的）。
   5. **AI / 天气不再冻住服务器**：AI 对话进行中，浏览器刷新待办列表应**照常刷新**。
   6. **内部 RAM**：开机后 `int-low` 应**不低于** v1.4.0 的 54 KB
      （本轮把 AI 历史的 24.4 KB 从内部 DRAM 还给了 PSRAM，应当有净收益）。
1. **上机确认后再做 v1.6.0**：候选清单在 `docs/ROADMAP_v1.5.0.md` §7（15 条，已逐条核实过）。
   性价比最高的三条是：
   - **设备端「快速添加」**（本板没有 IME，设备端现在完全无法新增待办）；
   - **待办数据模型升版**（标签 / 子任务 / 重复规则 / 自定义排序，越晚做迁移代价越大）；
   - **按日期滚动的备份**（`CONFIG_TODOHELPER_TODO_BACKUP_KEEP` 目前仍是死配置，
     Kconfig 里已注明「尚未实现」，别当成已有功能）。
2. **仍然没做的两件老需求**：
   - `POST /api/v1/weather/refresh`（等 20 s）与 `POST /api/v1/calendar/sync`（等 10 s）
     **仍是同步 handler**，会串行化唯一 httpd 任务 → 照 `sync_server.c` 的
     `events_get_handler()` + `sse_client_task()` 改成异步。
   - **AI 指令联动 todo_manager**（FR7 明确要求「帮我添加待办 XXX」，从未实现）。

### 7.1 v1.3.0 时期的清单（历史留档，基本都已完成）

0. **最新一件（v1.3.0 遗留）：把本轮上机验证掉。** 本轮**只构建、未烧录**，所以下面三件事
   都还是纸面结论。按性价比排序：
   1. **音标修复**（最便宜、判据最硬）：字体是运行期从 `<storage>/todohelper/fonts/ui_cjk.ttf`
      加载的（**没烘进镜像**），所以**拷新字库文件到卡上覆盖同名文件 + 重启**即可，
      **不必重新烧录**。判据：单词卡背面的 `/əˈbrʌpt/` 里 ə、ʌ、ˈ、ː **不再是空白**。
      顺手确认 1252 个词书字符里没有新的方框。
   2. **墨墨云端同步（方案 A）**：网页「墨墨背单词」面板录令牌 → 点「立即同步」→
      看设备单词页是否显示「墨墨今日 N/M」。**若云端队列没起来，先看串口有没有
      `cloud queue: N words`**（`word_service` 打的），以及 NVS `mm_mode` 是不是 `cloud`。
   3. **方案 B 的快乐路径**（**当前令牌做不到**）：现有令牌是学习作用域，
      `/notes`、`/phrases` 恒返回 `common_permission_denied`。要看到助记，需要一个能读
      `/notes` 的令牌；在那之前只能验证"权限拒绝被正确吞掉、卡片不留空白块"。
   4. **方案 C**：默认关闭。要测就先确认"推上去的词会出现在墨墨 App 的学习计划里"，
      再决定要不要长期开着（**接口没有反向删除**）。
1. ~~验完 §2 的崩溃修复~~ —— **已完成**：插卡 3/3 冷启动 `abort=0`、`TH:app: ready` 3/3，
   词库 833 词 3/3 加载成功（证据见 §2）。
2. **请用户做 §4 的目视确认**，重点是 1.1 的两项：待机"慢动作三层"（等 1~2 分钟看自发表情、
   等 2.5 分钟看困倦、碰屏幕看睁眼过冲）与开机日志 → 主界面的交叉淡入。
   可调旋钮全是 `idle_face.c` / `ui_core.c` 顶部的单个 `#define`：
   自发表情间隔与时长、困倦上升时间与幅度、漂移幅度、淡出/淡入时长与位移。
3. **`/api/v1/ai/chat/stream` 的同类隐患 —— 用户明确决定「先不管」**，本轮不动。
   它仍是同步 handler，会在一次 AI 回复期间（十几秒）堵住整台设备的 HTTP（网页端不用这两个
   接口，所以不影响上位机）。真要修时照 `sync_server.c` 的 `events_get_handler()` +
   `sse_client_task()` 模式改即可（`AGENT.md §9.0.1` 最后一条）。
4. 若要继续加功能：**网页 WiFi 配置面板**仍是最具性价比的一项 —— 设备端输 WiFi 密码体验很差
   （480px 屏、无 IME），而 `/api/v1/wifi/*` 7 条路由都已就绪。**本次已实测的报文语义**：
   `GET /api/v1/wifi/status` → connected/ssid/ip/rssi；`POST /api/v1/wifi/scan` 触发扫描并返回
   `{"scanning":true}`，**扫描结果要靠 `GET /api/v1/wifi/scan` 轮询**（它返回的是上一次结果，
   `POST` 之后立刻 `GET` 会得到空数组——本次踩过）。路由表在
   `components/wifi_manager/src/wifi_manager_http.c:259-284`。照 `index.html` 现有的请求漏斗与
   样式写，**不要**另起一套。
5. 若用户反馈"插拔卡数据丢失"不可接受：把待办存储固定在片内存储，只把词库/字体放卡上
   （需要看 `storage_manager` 的根选择逻辑）。
6. **加功能前先看内部 RAM 余量**：v1.2.0 实测 boot complete 为
   `internal 66 KB / largest 22 KB`（`int-low 54 KB`）；**v1.3.0 未在设备上复核**
   （新增的 `th_mm_sync` 任务是按需创建、6 KB 栈，平时不占）。
   任何批量数据都必须显式 `MALLOC_CAP_SPIRAM`（§2 的崩溃就是没这么做造成的）；SSE 这类
   每客户端一个任务的设计也要算进内部 RAM（每客户端 3 KB）。
   **判据是 largest 与 int-low，不是总空闲量。**
7. **流畅度还剩两批没做**（第一轮已完成，见 §0）：
   - **第二批（结构性，收益中等）**：`page_splash` 每 100 ms 重建 >5 KB 日志文本并强制
     `lv_obj_update_layout()`；`page_todo` 用 `lv_obj_clean()` + 全量重建（每项 5~7 个
     对象 × 最多 256 项，勾选一项等于整页重建）；`page_ai_chat` 每个流式 chunk 都
     `lv_obj_scroll_to_view(..., LV_ANIM_ON)` 导致动画堆积。
   - **第三批（体验微调）**：切页交叉淡入对两个 800×480 容器动画 `opa`，是 SW 渲染最贵的
     路径（位移便宜、整屏透明度贵）；`ui_fonts` 从 SD 卡 TTF 初始化 4 个独立 FreeType face
     （16/20/28/48 px），48 px 只用于天气温度一个 label。
   - **HTTP 侧**：`/api/v1/ai/chat/stream`（以及阻塞版）、`/api/v1/weather/refresh`（等 20 s）、
     `/api/v1/calendar/sync`（等 10 s）仍是同步 handler，会串行化唯一 httpd 任务。
     修法照 `sync_server.c` 的 `events_get_handler()` + `sse_client_task()`。

---

## 8. 关键文件索引

| 关注点 | 文件 |
|---|---|
| 环境/命令/全部实测坑 | `AGENT.md`（**§9.0 起必读**，§9.0.5 = 崩溃，§9.7 = 剩余待办） |
| 启动流程与这次修复 | `main/app_main.c` |
| **崩溃根因与修复** | `components/sys_utils/src/sys_json.c`（`sys_json_use_psram()`）、`components/word_service/src/word_service.c`（`parse_book()`） |
| 内部 RAM 诊断（`largest` / `int-low`） | `components/sys_utils/src/sys_heap.c` |
| SSE 实时推送（异步 handler 模式） | `components/sync_server/src/sync_server.c`（`events_get_handler()` + `sse_client_task()`）、`web/index.html` |
| 待办页面（设备端） | `components/ui_pages/src/page_todo.c` |
| 待机动画 | `components/idle_face/src/idle_face.c`（`tick_behaviour()` 必须被调用！） |
| 开机日志缓冲 | `components/sys_utils/src/sys_log.c`、`include/sys_log.h` |
| 开机日志页面 | `components/ui_pages/src/page_splash.c` |
| 词库与进度 | `components/word_service/src/word_service.c`（`progress_key()`、`parse_book()`） |
| **流畅度：事件投递与页面框架** | `components/ui_core/src/ui_core.c`（`ui_post_event()` / `ui_event_drain_cb()` 无锁队列） |
| **流畅度：状态栏脏检查** | `components/ui_core/src/ui_statusbar.c` |
| **流畅度：异步校时** | `components/calendar_service/src/calendar_service.c`（`calendar_sync_ntp_async()`、`sync_worker`） |
| **流畅度：异步落盘** | `components/word_service/src/word_service.c`（`word_save_progress_async()`、`word_save_worker`） |
| **墨墨开放 API（方案 A/B/C）** | `components/maimemo_client/src/maimemo_client.c`（`mm_post()` / `mm_classify()` / `enrich_cache_*`）、`src/maimemo_http.c`（6 条路由）、`components/maimemo_client/README.md` |
| **云端队列（本地书 ↔ 墨墨）** | `components/word_service/src/word_service.c`（`word_cloud_mode()` / `word_cloud_refresh()` / `cloud_index_for()`） |
| **单词页：云端进度与助记** | `components/ui_pages/src/page_word.c`（`render_progress()` / `enrich_start()` / `grade_clicked_cb()`） |
| 网页「墨墨背单词」面板 | `components/sync_server/web/index.html`（`mm*` 函数，约 1221 行起） |
| **字库生成（IPA 合并 + 校验）** | `tools/gen_cjk_font.py`（`merge_ipa_glyphs()` / `flatten_components()` / `validate_font()`） |
| **Git-Bash 构建包装器** | `tools/build.sh` + `tools/build.ps1`（日志 `build_last.log`） |
| HTTP 路由总览 | `docs/API.md`（含 §2.5 墨墨接口详解；**51 条路由待上机校准**） |
| 网页上位机 | `components/sync_server/web/index.html` |

---

## 9. 已知的文档不一致（尚未修，留待下次）

- `AGENT.md §9.0` 第 4 条写"本工程用 32 KB 且 `stack_in_psram=true`"，但
  `components/bsp/src/th_bsp.c` 实际设的是 **`stack_in_psram = false`**，且该文件有
  详细注释说明为什么必须 false（UI 任务会写 NVS，flash 写期间 cache 关闭使 PSRAM 栈
  不可访问 → `esp_task_stack_is_sane_cache_disabled()` 断言）。**以代码注释为准**，
  `AGENT.md` 那句需要回改。
- `AGENT.md §8` 的内部 RAM 预算表已被同文件新增的"v1.2.0 现行基线"一行取代为判据，
  两处数值一致（66/22/54 vs 67/24/55），不需要回改，但表头年份可更新。
- ✅ **已关闭（v1.3.0）**：~~`docs/API.md` 没有收录 6 条 `/api/v1/maimemo/*` 路由、
  `maimemo_client` 组件章节与 `words/enrich.json` 存储条目~~ —— **已补齐**：
  `docs/API.md` 新增 `maimemo_client` 组件一节（函数表、事件、**线程规则**、令牌/作用域模型、
  错误模型、最小测试步骤）、§2.1 的 6 条路由、**§2.5 墨墨接口详解**（逐条请求/响应 +
  状态码语义 + `X-Auth-Token` 规则）、§3 的 `mm_token`/`mm_mode`/`mm_push`、
  §4.2 的 `words/enrich.json`；并更正了该文件若干 1.0.0 时代的过期记录
  （§1 依赖表、§2.4 路由数、§5.3 的"WiFi API 未注册 / `voice_service` 不存在 /
  `ai_service` 未接入 / FreeType 未启用"、§5.4 第 1、2 条）。
- ⚠️ **`docs/API.md` 的新遗留项（下次上机请核对）**：
  - ✅ **已复核（1.3.0 上机）**：`GET /api/v1/device/status` 返回 `"routes":51`，
    与注册表推算一致；串口无 `route table full`。
  - ✅ **已修**：`HTTP_ROUTER_MAX_ROUTES` 原为 48，而注册项正好 48 条（含 1.3.0 新增 6 条），
    表**刚好满、零余量**——再加任何路由都会静默失败。已提高到 **64**（多占 <2 KB .bss），
    并在头文件写明"表满是静默失败、没有编译期告警"。
- `docs/API.md §1` 记录了一处**架构债**：`word_service`(L3) 与 `ui_pages`(L4) 都
  `REQUIRES maimemo_client`(L5)，**属于向上依赖**，与"依赖只能向下"的规则相悖
  （1.3.0 的实现取舍，已在文中写明理由与"干净路线"的改法）。不是 bug，但值得留意。

