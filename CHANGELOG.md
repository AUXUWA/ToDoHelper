# 变更历史（CHANGELOG）

版本号规则见 `AGENT.md §8.1`：**MAJOR** = 一轮需求全部完成或有破坏性变更（存储格式 / HTTP API /
NVS 键语义）；**MINOR** = 新增功能或页面；**PATCH** = 修 bug、调参、文档。
固件版本来自工程根目录 `version.txt`，并由 git tag `vX.Y.Z` 标记。

---

## [1.3.0] — 2026-09-27

**单词学习接入墨墨背单词（开放 API）+ 音标字形修复。** 需求条目没有变化，改的是"这个词到底
从哪来、进度算谁的"这件事。**没有破坏性变更**：HTTP API 只增不改，存储格式向后兼容
（新增一个可选的 `words/enrich.json`），NVS 只新增 `mm_token` / `mm_mode` / `mm_push` 三个键。

两件事各有一条"根因写在现象之外"的教训：**音标缺失不是字库选字少了，而是 Noto Sans SC
根本没有 IPA 字形**；**构建失败不是代码错，是 MSYS 下 `idf.py` 根本没法被驱动**。

### Added
- **新组件 `components/maimemo_client/`（L5，墨墨背单词 OpenAPI）**，实现用户选定的三个方案：
  - **方案 A —— 学习同步**：今日词队列与账号**真实进度**改从墨墨取，不再只认本地词库。
    端点 `POST /study/get_study_progress`（finished/total/study_time）、
    `POST /study/get_today_items`（voc_id / voc_spelling / is_new / is_finished /
    first_response）、`POST /study/query_study_records`（body `{"as_count":true}` 取计划词数）。
    **实测（真实令牌）**：10/20 已完成、20 词队列。
  - **方案 B —— 助记/例句预取**：`/notes`（助记）与 `/phrases`（例句）缓存到
    `<app_dir>/words/enrich.json`，走 `storage_read_text()` / `storage_write_file()`
    （**原子写**，tmp + rename），所以卡片离线也能显示助记与例句。
    **整个文件一次性载入 PSRAM、整文件重写**，不做逐词小文件（这台设备的内部 RAM 只有
    ~310 KB，批量数据必须显式走 PSRAM，见 `AGENT.md §9.0.5`）。
  - **方案 C —— 反向推送**：评为「不认识」的词用 `POST /study/add_words` 推回墨墨学习计划。
    **默认关闭、必须显式开启**（NVS `mm_push`），因为它会**修改用户在云端的学习计划**，
    不是只读操作。
- **令牌的作用域被当成一等问题处理**：用户的令牌是**学习作用域**的，`/study/*` 与
  `/vocabulary` 可用，而 `/notes`、`/phrases` 返回 `common_permission_denied`。
  这被映射为 `ESP_ERR_NOT_SUPPORTED`（"这个令牌不许做这件事，别再问了"）并**按词缓存为
  不可用**，而不是当成错误反复重试。
- **新增 6 条 HTTP 路由**：`GET /api/v1/maimemo/status`、`POST /api/v1/maimemo/config`、
  `POST /api/v1/maimemo/sync`、`GET /api/v1/maimemo/today`、
  `GET /api/v1/maimemo/enrich?spelling=`、`POST /api/v1/maimemo/push`。
- **网页上位机新增「墨墨背单词（开放 API）」面板**（`components/sync_server/web/index.html`）：
  令牌录入（**只写不读**，已核实回显路径不存在）、推送开关、带轮询的「立即同步」按钮、
  今日词表查看器。
- **构建工具 `tools/build.sh`（bash 入口）+ `tools/build.ps1`**：
  `bash tools/build.sh build`（或 `flash`，或 `-v build`）。
  MSYS/Git-Bash **无法直接驱动 `idf.py`** —— ESP-IDF 的 python wrapper 会识别 MSYS 环境
  并拒绝运行（`MSys/Mingw is no longer supported`）。三个坑都会**静默产出空日志 + 退出码 0**，
  修法见本节 `### Fixed` 与 `AGENT.md §4.2.1`。`build_last.log` 已加入 `.gitignore`。
- **构建期字库校验 `validate_font()`**（`tools/gen_cjk_font.py`）：重新打开**已保存**的文件，
  当任一分字形度量表与 `maxp.numGlyphs` 不一致、`numberOf*Metrics` 超过字形数、
  或必需的内容字符不在 cmap 里时**直接让构建失败**。

### Fixed
- **「HTTP 鉴权已开启」弹窗整块溢出屏幕**（用户上机截图）：标题被切掉、正文从底部跑出屏幕
  以外，整页被它盖住且无法读完。
  根因是 `ui_dialog()` 建出来的 msgbox **按内容自适应尺寸**，既没有最大高度也没有滚动，
  而这个弹窗的正文是全 UI 最长的一段多行中文 —— 内容一长就必然超出 480 px 面板。
  现改为把 msgbox **夹到屏幕的 88%（高）/ 92%（宽）**，并让**正文自己可滚动**：
  这样无论正文多长，标题与「知道了」按钮始终可达 —— 这才是弹窗需要的性质，
  无上限增长不是。
- **同一个弹窗还犯了一个更直接的错：它让用户"复制下面的 token"，但正文里根本没有 token。**
  原文案只有一段解释（"见串口日志，或调用 GET /api/v1/device/status …"），
  用户拿不到那个 32 位令牌本身。
  新增 `ui_dialog_secret()`：把内容拆成**引导语 + 高亮令牌 + 灰色说明**三段，
  令牌单独放在底色方框里、**按 32 个十六进制字符换行**（不换行会在 480 px 上被截断），
  说明文字压到一行。生成令牌的按钮现在走这个弹窗。
- **设置页的"地址"一直藏在正文里，并且可能过期**：
  `sync_server_url()` 原来只在**首次调用**时计算并缓存，DHCP 换址后会一直显示旧地址。
  现在每次调用都重算（一次 `snprintf` 加一次网卡查询，代价可忽略）。
  同时在「局域网与安全」卡片给网址**单独一行、标题字号、主题色**（它是全页唯一
  需要用户"看一眼再手打"的字符串），mDNS 开启时**同时显示 `.local` 与纯 IP 两种**
  —— Windows 只有装了 Bonjour 才解析 `*.local`，部分网络还直接封多播，
  所以只给 `.local` 是不够的。新增 `sync_server_ip_url()` 提供纯 IP 形式。
- **音标（IPA）字形缺失：`/əˈbrʌpt/` 在屏上渲染成 `/ br pt/`**（元音符号全是空白）。
  **根因不是"字库选字少了"，而是 Noto Sans SC（CJK 子集来源）里根本没有 IPA 字形** ——
  U+0259 ə、U+028C ʌ、U+026A ɪ、U+02D0 ː、U+02C8 ˈ 等**不在它的 cmap 里**；
  LVGL 内置的 Montserrat 兜底也帮不上（cmap 稀疏，只有 FontAwesome 区段）。
  修法是在 `tools/gen_cjk_font.py` 新增 `merge_ipa_glyphs()`：把 IPA 字形从一份拉丁字体
  （Linux 用 DejaVu Sans，Windows 用 segoeui / arial / calibri）**合并进 CJK 子集**，
  坐标从拉丁 em 方格（2048）**缩放到** CJK 的（1000）。
  - `fontTools.merge` **不能用** —— 它断言两边 `unitsPerEm` 相等，这里必然不等，于是只能手写。
  - 字形必须**展平成普通轮廓**：组件（composite）会引用目标字体里不存在的字形，
    编译期就炸 `KeyError: 'uni02BE'`。
  - 实测从 `segoeui.ttf` 合入 **203 个字形**，缩放系数 **0.488**。
- **合并逻辑里还藏着一个潜伏 bug：只更新了 `hmtx`，没更新 `vmtx`。**
  两张表都以**字形名**为键、都必须**每个字形一条记录**；只补一半的结果是文件本身已经不合法
  —— `vmtx` 比应有长度**少 406 字节（2 字节 × 203 字形）**，fontTools 连**重新解析**都做不到：
  `not enough 'vmtx' table data: expected 10686 bytes, got 10280`。
  这份 CJK 子集**确实带中文竖排度量**（`vmtx` / `vhea` 都在），所以这不是理论问题。
  修法是给每个合入的字形**同时写 `vmtx` 记录**，并把 `hhea.numberOfHMetrics` /
  `vhea.numberOfVMetrics` **夹到字形总数以内**。
  - **这个 bug 是被"cmap 检查通过了"放过去的**：第一版校验只问"文件里有没有 ə？"，
    而坏字体**每个 cmap 条目都在**，于是检查通过、坏字体照样出厂。
    这就是新增 `validate_font()` 的动机 —— 必须**重新打开保存后的文件**逐表核对，而不是查 cmap。
  - `validate_font()` 只要求**内容字符**（词书字符 + 有拉丁源时的 IPA），不要求装饰性
    Unicode 区段，否则检查会变成人人忽略的噪音。
- **构建脚本的三个静默失败**（都会产出空日志 + 退出码 0，最难查的一类问题）：
  1. IDF 的 PowerShell profile 把 `idf.py` 定义成**函数**并用 `@args` 转发参数，而调用方是
     `powershell -Command` / `-File` 时该简写**不绑定** → `idf.py build` 静默什么都不做。
     改为显式调用解释器：`& $python $idfPy @idfArgs`。
  2. **把原生 exe 接进 cmdlet 管道会失败**：`Cannot run a document in the middle of a
     pipeline: ...python.exe`，所以 `& $python ... | Tee-Object` 用不了；改为重定向到
     `build_last.log`。
  3. **用 `env -i` 清空环境是致命一击**：powershell 能起来并打印横幅，但原生 python 子进程
     **一点输出都没有**，且 `$LASTEXITCODE` 保持 `$null`。可行做法是**保留 bash 继承的环境**，
     在 PowerShell **内部**删掉 MSYS 标记（`Remove-Item Env:MSYSTEM`）—— 在 bash 里 unset
     `MSYSTEM` 毫无用处，因为 MSYS 运行时会把它**重新注入到每一个原生子进程**。

### Changed
- **`page_word.c` 在云端模式下显示账号真实进度**：「墨墨今日 10/20 · 已用 59 秒 · 计划 20 词」，
  否则回退本地词库计数器。**小于 1 分钟时显示秒**，因为把 59 秒四舍五入成「0 分」
  读起来就是"你今天没学过"。
- **助记/例句显示在答案下方**（方案 B），拿不到时**整块隐藏**（不是显示空标签）。
- **评「不认识」时把词推回账号**（方案 C 开启时）。
- **线程规则在 `maimemo_client.h` 里写明**：所有做 TLS 的函数都是阻塞的，
  **绝不允许从 LVGL 任务调用**（LVGL 回调持有适配器锁，见 `AGENT.md §9.0.1`）。
  UI 路径把抓取交给 worker 任务，结果经 `lv_async_call` / 事件应用回界面。
- **`word_service` 新增云端队列**：`word_cloud_mode()`、`word_set_cloud_mode()`、
  `word_cloud_refresh()`、`word_cloud_count()`（NVS `mm_mode`）。云端词会用 `voc_spelling`
  匹配回本地词库，匹配不上也要能在设备上评分（`WORD_ID_CLOUD`）。
- **`main/app_main.c` 的模块表多一项** `maimemo_client`（`optional = true`，排在
  `word_service` **之前** —— 云端队列要先能读到令牌）；开机时若处于云端模式则自动发起一次同步。
- **`tools/gen_cjk_font.py` 现在会真正校验产物**，所以"字库生成成功"不再等于
  "字库能用"。数值：词书 **1252 个不同字符，0 个缺失**；`/əˈbrʌpt/` 全部有真实轮廓且
  advance 正确缩放（ə=523 vs b=618 / r=388 / p=620）。
- **令牌处理与 AI Key 同规则**（需求 §8.8）：存 NVS 用 `sys_config_set_str`，从网页控制台写入，
  **从不记日志、任何 API 都不回读**。`GET /api/v1/maimemo/status` 只返回 `configured`
  与掩码提示（形如 `g_zV****`）。
- **无令牌时静默降级**：云端模式无声关闭，本地词库照常工作（不报错、不弹窗）。

### Verified（真机）
- ⚠️ **本轮没有烧录到设备，所以以下几项都不是真机验证**，请按"已验证 / 未验证"分别理解：
  - **离线构建（已验证）**：`todohelper.bin` **0x2e4fd0 字节**，app 分区剩 **64%**；
    构建 **0 错误 0 警告**。
  - **字体产物（已验证）**：`validate_font()` 通过，词书 1252 字符 **0 缺失**，
    合入 203 个 IPA 字形，advance 缩放正确。
  - **墨墨接口（已验证，对线上 API 实测）**：用真实令牌 —— `get_study_progress` 返回 10/20、
    `get_today_items` 返回 20 词队列、`add_words` 成功（服务器自报 `added_count`）。
  - **令牌作用域（已验证）**：`/study/*` 正常、`/notes` 与 `/phrases` 返回
    `common_permission_denied`，设备侧正确映射为 `ESP_ERR_NOT_SUPPORTED` 并按词缓存为不可用。
  - **错误模型（已验证）**：确认 API **即使权限错误也返回 HTTP 200**，设备读的是信封里的
    `success:false` + `errors[].code`，不是状态码。
  - **`advance` 必填（已验证）**：漏掉它即使请求其它部分完全合法也会失败 ——
    `common_invalid_param: {base} must have required property 'advance'`。
    现在**永远发送**（含 `false`），并回报服务器自己的 `added_count` 而不是发送条数。
    请求体形状是 `{"words":[{"id":<voc_id>},...],"advance":bool}` ——
    **按 id 加词而非拼写**，设备侧先用 `GET /vocabulary?spelling=` 解析
    （与官方 CLI `@maimemo/memo-api` v1.2.0 的 `advance` + `words[].id` 一致）。
  - **助记/例句的读取端点是 `GET`，不是 `POST`（已验证）**：
    `GET /api/v1/notes?voc_id=...`、`GET /api/v1/phrases?voc_id=...`。
    同路径的 `POST /api/v1/notes` 是**创建**笔记（body `{"note":{voc_id,note_type,note}}`），
    用 POST 去读会拿到完全不同的东西（甚至可能改写用户的笔记），所以本组件为其单独实现了
    `mm_get()`。响应结构 `{"data":{"notes":[{"note":"..."}]}}` /
    `{"data":{"phrases":[{"phrase":"..."}]}}`，已对照官方 CLI 源码核实。
  - **网页面板令牌只写不读（已验证）**：已核实没有任何回显路径。
  - **未验证（需上机）**：字库在屏上的实际观感、设备端云端队列、方案 B 的助记显示、
    方案 C 的推送落地、无令牌降级的实机行为。

### Known gaps
- **方案 B 的"快乐路径"没有在设备上被观察到**：当前令牌是学习作用域的，`/notes` 与
  `/phrases` 恒返回 `common_permission_denied`。**代码路径已实现、权限拒绝的处理已验证**，
  但"卡片上真的显示出一条助记"这件事**尚未在设备上看到过**。
  要验证需要换一个能读 `/notes` 的令牌，或等墨墨放开该作用域。
- **本轮所有功能都没有上机**：字库是运行时从 `<storage>/todohelper/fonts/ui_cjk.ttf`
  加载的（**没有烘进固件镜像**），所以部署新字库是**拷文件而不是重新烧录** ——
  但也因此，"拷上去之后屏幕上真的没有方框了"这件事仍待目视确认。
- 墨墨云端模式与本地进度是**两套记账**：云端词匹配回本地词库后，本地的
  level / 复习间隔照常生效；匹配不上的词（`WORD_ID_CLOUD`）在设备上一评分就只留痕在账号侧，
  本地不记进度。这是刻意的取舍（不把云端 id 硬塞进本地进度键），但换回本地书时两边的数字
  不会合并。
- 方案 C 一旦开启就无法"撤销"已经推上去的词：接口只负责加词，没有反向删除。

---

## [1.2.0] — 2026-09-24

**系统流畅度优化（第一轮：消除锁内阻塞 I/O + 高频无效重绘）。** 需求条目没有变化，
改的是"界面跟不跟手"这件事。**没有破坏性变更**：HTTP API、NVS 键语义、存储格式全部不变。

根因一句话：**适配器在持有递归锁的情况下调用 `lv_timer_handler()`，所以所有 `lv_timer`
回调与控件事件回调都在锁内运行**，回调里任何同步 I/O 都会冻结整个 UI。详见新增的
`AGENT.md §9.0.1.1`（流畅度五条铁律）。

### Fixed
- **点"校时"会冻屏最多 10 秒**（`page_calendar` / `page_settings`）：点击回调直接调
  `calendar_sync_ntp(0)` → `esp_netif_sntp_sync_wait()`，持锁等待网络往返；而且因为
  `ui_post_event()` 当时要取同一把锁，**整个事件系统一起冻住**（SSE、待机表情、
  各服务的事件消费者全部停摆）。
- **每次单词评分都会卡一下**（`page_word` → `word_review` → `word_save_progress`）：
  持锁做 cJSON 序列化 + SD 卡整文件重写（open→write→remove→rename）。设置页的
  "每日单词数 ±5"、时区切换（NVS flash 写）同理。
- **设置页的 250 ms 滑杆定时器永不释放**：句柄没保存、页面隐藏后仍以 4 Hz 重写 4 个标签。
  现在保存句柄、`on_hide` 暂停、`on_show` 恢复，且值没变就不写。
- **上机实测的一次 `interrupt wdt timeout`**（`MCAUSE=0x18`，两核栈都在
  `vListInsert`/`xQueueSemaphoreTake`；触发条件是"WiFi 切换期 DNS 失败 + 通知突发"）：
  事件排空对每个节点调一次 `lv_async_call()`，打满其小固定池后退化为反复 malloc/free。
  改为在排空回调内直接派发（见 Changed）。

### Changed
- **事件投递改为无锁队列**（`ui_core.c`）：`ui_post_event()` 不再取 LVGL 锁，只把节点挂到
  单链表（短临界区），由 ui_core 自己的 20 ms `lv_timer` 在 LVGL 任务内排空并派发。
  队列上限 64，满了丢事件并告警（丢通知可接受，阻塞发布者不可接受）。发布者
  （事件循环 / 各 worker）与渲染任务彻底解耦，不再有双向阻塞。
- **新增异步 API**：
  - `calendar_sync_ntp_async()` + `CALENDAR_EVENT_SYNC_RESULT` 事件；校时跑在按需创建的
    `th_ntp` 任务（3072 B / prio 4）上，UI 用 toast 呈现结果。
  - `word_save_progress_async()`：600 ms 合并窗口，连续评分 / 连点只写一次卡；
    `word_review()` / `word_set_daily_count()` / `word_reset_progress()` 全部改走它。
  - 时区 NVS 提交改由一次性任务 `th_tz` 执行（flash 写会短暂关闭 cache）。
- **所有周期性 label 刷新加脏检查**（文本 / 字段不变则不写；`lv_label_set_text*` 即使文本
  相同也会 invalidate 对象并触发重排）：状态栏 4 个标签、首页 summary 与时钟、日历时钟、
  WiFi 状态块、设置页信息块。状态栏此前每秒无脑重写 4 标签 + 4 次改色。
- `calendar_sync_ntp()` 与 `word_save_progress()` 的**头注释明确标注"禁止在 LVGL 任务调用"**，
  避免后来人再把同步版本接回 UI 路径。
- **切页动画全部删除，改为同帧原子替换**（`ui_core`）。原来有 220/240 ms 的
  `translate_x`/`translate_y` + 整屏 `opa` 双向动画。删除理由：
  1. **正确性**：两个页面容器同尺寸且不透明，动画期间只要不完全重叠就会把上一页从缝隙里
     露出来；更糟的是**旧页的隐藏只依赖退场动画的完成回调**，而 `page_anim_reset()` 会
     `lv_anim_delete()` 掉那个动画（回调一并删除），于是"退场结束前切走或切回"就永久留下
     残影 —— 这正是用户报告的"旧页面残留/重影不消失"。
  2. **成本**：整屏 800×480 alpha 动画让每个像素走软件混合路径十几帧。
  新增 `pages_show_only()`：每次切页把其余容器复位并隐藏，把"任意时刻只有一个页面容器可见"
  这条不变量显式钉死（不再依赖动画回调的时序）。
  副作用（已记录在 `idle_face/README.md`）：`idle_face_standby_exit()` 的睁眼过冲因此
  **在线下不再可见**，因为待机页同帧就消失了。这是为切页正确性付出的代价，不是 bug。

### Fixed
- **切页重影**（用户实测报告）：见上方 Changed。

### Added
- **待机动画新增四层连续/反射性动作**（`idle_face`）。此前所有层（眨眼/注视/扫视/呼吸/
  自发表情/困倦/瞳孔缩放/整脸漂移）都是**离散事件**，事件之间眼睛完全静止，盯久了仍能看出
  "在摆姿势"。四层全部是加法、有界、零分配（只扰动 `apply_params()` 已算出的值）：
  1. **连续视线游移**：两轴 Lissajous 缓慢漂移，频率故意不可通约，路径永不闭合；
  2. **眼睑回弹**：眨眼睁开后眼睑过冲 5.5% 再回落（260 ms），不再是"到位即停"；
  3. **注意力提振**：每 17–46 s 一次，眼睛放大 + 瞳孔收缩约 520 ms，像"注意到什么"。
     不改表情状态，所以读起来是反射而非情绪；
  4. **眨眼不对称**：30% 的眨眼两片眼睑深浅不同，且哪只眼更深随机。
  四层都会在任何真实表情到来时复位（否则进行中的 perk 会把 ERROR 脸拉宽）。
- **待机动画再增三层，打破"两只眼一起动"**（同样零分配、有界）：
  5. **呼吸联动**：呼吸的正弦同时把整对眼上下抬降 1.6 px。复用已算好的
     `breath_phase`，所以不存在第二个时钟，两者不会走散；
  6. **双眼扫视错峰**：微扫视时一只眼带衰减偏移，约 60 ms 后跟上，两眼从不完全同步。
     哪只眼迟到每次重抽（否则同一只眼总慢半拍，规律本身成为破绽）；
  7. **困倦点头**：困倦度 ≥ 0.55 后偶尔眼睑慢慢合上再抬起（520–900 ms）。
     曲线故意不对称（前 35% 合上、后 65% 抬起），才像"抗不住睡意"而非慢眨眼。
     三道门闩（困倦度阈值 / 8–26 s 随机间隔 / 45% 概率）让它保持为事件而非节奏。
- **`CONFIG_TODOHELPER_FPS_PROBE`**（默认关）：打开后状态栏的 1 Hz 定时器每秒打一行
  `TH:ui_core: fps N`，用于用数据而非印象判断流畅度。

### Fixed
- **切页重影**（用户实测报告）：整页过渡动画被完全移除。应用户要求切页改为**同帧原子替换**，
  详见下方 Changed 与 `AGENT.md §9.0.1.1`。
- **待机时眼睛发灰**：待机亮度默认值 20 → 100。眼睛本身一直是纯白
  （`UI_COLOR_IDLE_FG=0xFFFFFF`），发灰是背光被降亮造成的。设置页「待机亮度」滑杆保留，
  想调暗仍可用。
- **一次性 NVS 迁移**（`brt_migr` 标记位）：旧设备 NVS 里的 `idle_bright=20` 会盖过新默认值。
  用标记位而非直接 `erase` —— `nvs_erase_key()` 对不存在的 key 也返回 `ESP_OK`，
  无保护地调用会**每次开机都执行**，从而抹掉用户后来设置的亮度（实测复现）。

### 实测（COM34，插卡 `root=/sdcard`）
- 冷启动 ×4：`abort=0`、9 模块全 ready、`boot complete internal 64 KB (largest 20 KB)`、
  `int-low 52 KB` —— 与优化前的 67/24/55 KB 基本持平，说明异步化与新增动画没有吃掉内部 RAM
  （新任务都是按需创建、平时不占栈；新动画无新对象、无新定时器）。
- 触发条件复现 ×3（DNS 失败 + 12 路并发写 + weather/calendar 通知）：0 panic。
- HTTP 端到端：8 路并发 POST 全部落库、GET/DELETE 正常，设备稳定。
- **关键验证**：3.48 s 的 `POST /api/v1/weather/refresh` 进行期间，5 个 `/api/v1/todos`
  请求全部约 60 ms 返回（改前会排队到超时）。
- **待机动画**：打开 FPS 探针实测稳定 **28~30 fps**（设计值 `IDLE_FACE_TICK_MS=33`），
  无掉帧；用临时 INFO 探针确认新层确实执行：`attention perk` 与 `idle mood` 各触发 5 次，
  且困倦度正常累积（15%→28%→49%→67%）；另一轮 250 s 窗口确认
  **困倦点头按设计门闩触发**（第一次 `drowsy 70%`、第二次 `86%`，时长 690/865 ms，
  即它会等困倦度积累而不是按时钟乱放）。
- 用户目视确认：切页无残留、待机全亮纯白、新动画正常。

---

## [1.1.0] — 2026-09-23

**待机动画与开机交接动画的增强。** 需求条目没有变化，改的是"看起来像活的"这件事。

### Added
- **待机时的慢动作三层**（`idle_face`，全部只改已有对象属性：零新对象、零分配）：
  1. **自发表情**：每 13–34 s 随机演一个 320–1100 ms 的表情（HAPPY / SLEEPY / LISTENING /
     THINKING / SPEAKING），优先级 `眨眼 > 通知/AI 表情 > 自发表情 > 基础状态`，
     绝不超出自己的时长，也不会在注视/扫视飞行途中启动；
  2. **困倦度** 0→1：无人交互约 2.5 分钟攒满 —— 眨眼更密（间隔 ×0.65）更慢（时长 ×1.6）、
     眼睛矮 10%、视线下垂 5 px；任何真实交互（通知/AI 表情、唤醒）立刻清零，
     **自发表情不清零**（那只是发呆；第一版每次发呆都清零，实测 `drowsy` 永远在 0–3%）；
  3. **瞳孔缩放**（5–11 s 重抽 0.91–1.14 的缓慢随机游走，LISTENING 再放大 6%）与
     **整脸 ±3/±2 px 缓慢漂移**（两条不相干频率正弦合成；表情占用时缓动归零，
     保证 ERROR 的 X 线与眼睛始终对齐）。
- 眨眼/注视的多样性：28% 的眨眼只眯到 40–62%、9% 是 240–380 ms 的慢眨、
  22% 的注视结束后立刻向另一侧再看一眼（幅度 55–80%）。
- **切页改成交叉淡入**（`ui_core`）：新页面 `translate_x 28→0` + 淡入（220 ms ease-out），
  旧页面 `translate_y 0→-16` + 淡出（240 ms ease-in），**动画结束才加 `HIDDEN`**。
  开机日志 → 主界面因此不再是一瞬间的硬切；页面也第一次有了"离场动画窗口"。
- 开机页状态行加**动画省略号**（每 420 ms 多一个点），启动过程不再像卡死。

### Fixed
- **`idle_face_standby_exit()` 是死代码**：它要求"当前处于 SLEEPY 姿态"，而
  `standby_enter()` 早已改成不闭眼，于是恒返回 `ESP_ERR_INVALID_STATE`，那段睁眼过场
  从未执行过。现在只要求没有**仍然有效**的表情占用。
- **首次进待机被静默拒绝**：动画器定时器是待机页**创建时**才启动的，所以页面创建那一刻
  可能还挂着一个几分钟前就该过期的临时表情（实测：t=7.7 s 的 2.5 s `wifi_connected`
  表情在 t=82 s 仍被当成"有效"）。现在 `standby_enter/exit` 都先调 `override_expire_due()`
  把已过期的清掉。
- 开机页显示的固件版本一直是 **`0.1.0`**：它读 `CONFIG_APP_PROJECT_VER`，而该符号只在
  `CONFIG_APP_PROJECT_VER_FROM_CONFIG` 打开时才存在。现在与设置页一致，用
  `esp_app_get_description()->version`（即 `version.txt`）。

### Verified（真机 COM34，插卡 `root=/sdcard`）
- 冷启动：0 panic、9 个模块全部 ready、`device/info` 的 `version` = `1.1.0`、
  内部 RAM `int-low 55 KB`（新动画没有增加内部 RAM 压力）。
- 200 s 静置：`idle timeout reached` → `standby enter (face stays awake)`（**不再被拒绝**）→
  自发表情实测 4 次，类型与时长各不相同（SPEAKING 353 ms、HAPPY 416 ms、LISTENING 828 ms、
  SLEEPY 633 ms），间隔 19.8–29.3 s（设计区间 13–34 s）；困倦度 15% → 29% → 42% → 61% 单调上升。
- **主观观感已由用户目视确认（2026-09-23，"可以了"）**：三种慢动作自然、退出待机时的睁眼
  可见、开机日志 → 主界面的淡入符合预期，无需再调参。

---

## [1.0.0] — 2026-09-23

**首个编号版本。** 第二轮 8 项需求全部完成，两个阻塞性缺陷定位到真实根因并修复，
真机（ESP32-S31-Korvo，COM34，插卡 `root=/sdcard`）验证通过；四项只能靠人眼确认的
体验项也已由用户确认正常。

### Added
- `version.txt`（= `1.0.0`）：固件版本从此可追踪，并出现在设备「设置」页顶部
  （`固件 1.0.0 · IDF v6.1`）、`GET /api/v1/device/info` 的 `version` 与 app descriptor 里。
- `components/sys_utils/src/sys_json.c`：`sys_json_use_psram()` 把 cJSON 的分配改成
  **先 PSRAM、失败回退默认堆**（`cJSON_InitHooks`）。这是插卡启动崩溃的修复本体。
- `sys_heap_log()` 新增 **`int-low`** 列（内部 RAM 历史最低水位），替代原来被 12 MB PSRAM
  主导、永远不动的 `min` 列 —— 这一类"内部 RAM 被瞬时抽干"的 bug 从此一眼可见。
- 单词书解析诊断：`book tree alive: internal N KB free`、`loaded 833 words from '...' (120311 B)`。
- SSE 每个订阅者一个流式任务（`sync_server.c` 的 `sse_client_task`），
  以及"挂着流时普通请求必须仍然可用"的回归测试步骤。

### Fixed
- **插 SD 卡即崩溃重启循环**（阻塞级）。真因不是此前记录的"crypto 锁惰性初始化竞态"，
  而是：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` 让 ≤4 KB 的分配优先落在内部 RAM，
  而解析 546 KB / 833 词 / 约 7000 个 cJSON 节点的词库把 ~107 KB 内部空闲抽到接近 0；
  同时刻正在连网的 WiFi 驱动调用 `esp_timer_create()`（`MALLOC_CAP_INTERNAL`，**不许回退
  PSRAM**）拿到 `ESP_ERR_NO_MEM`，外面包着 `ESP_ERROR_CHECK` → abort。拔卡时解析的是内置
  32 词小词库，故"只有插卡才崩"。
  验证：插卡冷启动 ×3，`abort=0/3`、`TH:app: ready 3/3`、词库 3/3 加载成功。
- **SSE 长连接冻住整个 HTTP 服务器**（用户实测：页面显示"推送：实时推送中"却"设备不可达/
  读取失败"）。`esp_http_server` 的 URI handler 在唯一 httpd 任务里串行执行，而
  `/api/v1/events` 是"不返回"的循环，于是挂上 1 个订阅者后 `accept()` 都不再执行
  （ping 通、TCP 全超时）。改为 `httpd_req_async_handler_begin()` + 每客户端任务。
  验证：挂 3 条流时 `GET /` 仍 200/42 ms、第 4 条流 503、`POST /api/v1/todos` 后流内收到
  `event: todo_changed`、客户端退出后 `sse_clients` 归 0。

### Changed
- SSE 并发上限 **4 → 3**（`SYNC_SSE_MAX_CLIENTS`）：每个订阅者占 1 个 httpd 会话槽（共 5）
  加 1 个 3 KB 任务，必须给页面与 REST 留槽；网页相应文案同步更新，超限仍优雅降级到 15 s 轮询。
- 删除 `main/app_main.c` 里"启动期预初始化 crypto 锁"的代码 —— 它出自被推翻的归因、实测无效，
  留着只会误导后来人。
- `word_service` 的 `items[]` / `progress[]` 改为显式
  `heap_caps_*_prefer(..., MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)`。
- 文档全量回改：根 `README.md` 由上游 factory_demo 说明改写为 ToDoHelper 说明；
  `AGENT.md §9.0.1` 从"三个自伤 bug"扩为四个（新增 HTTP 长连接 handler）；
  `AGENT.md §9.0.5` 重写为实测根因并保留两处误读作为教训；`AGENT.md §8` 基线表更新为本机实测值；
  `docs/HANDOVER.md`、`docs/API.md`、`docs/TODOHELPER_FRAMEWORK.md`、
  `docs/TODOHELPER_REQUIREMENTS.md` 与相关组件 README 同步。

### Verified（真机）
- 插卡冷启动 ×3：0 panic、9 模块 ready、`TH:app: ready (storage=/sdcard)`、词库 833 词。
- 启动结束内部 RAM：`internal 67 KB / largest 24 KB / int-low 55 KB`。
- 端到端：`GET /api/v1/words/stats` → `book=cet6 total=833`；`GET /api/v1/wifi/status` → connected；
  `POST+GET /api/v1/wifi/scan` → 22 个 AP 且设备存活；网页 `GET /` 200/65 ms。
- 人眼确认（用户）：待机小眼睛保持睁眼并按时眨眼/扫视；开机 3 秒滚动日志顺畅；
  待办页勾选/删除按钮可用且不误触；网页刷新后状态与列表正常、推送与页面共存。

### Known gaps
- `/api/v1/ai/chat/stream` 与阻塞版 `/api/v1/ai/chat` 仍是同步 handler：一次 AI 回复期间
  整台设备的 HTTP 会不可用（网页不用这两个接口，所以不影响上位机）。**用户决定本轮先不动**，
  修法记录在 `AGENT.md §9.0.1` 与 `docs/HANDOVER.md §7`。
- 网页 WiFi 配置面板未做（设备端 7 条 `/api/v1/wifi/*` 路由已就绪）。
- 语音 ASR/TTS 端点选型未定（录音回放本身可用）。
- CET-6 词库是 833 条自编精选子集，不是官方约 2000 词表。

---

## 无版本号阶段（0.1.0 … 0.9.0，按提交顺序）

编号版本之前的工作，按时间顺序（提交见 `docs/HANDOVER.md §6`）：

1. **需求书全量实现 + 文档回改**：`docs/TODOHELPER_REQUIREMENTS.md` v0.5，含 15 处实现偏差记录。
2. **硬件 bring-up 与内存优化**：大量数据改放 SD 卡；UI 参考官方工程；待机动画参考 esp-vocat；
   字体放 SD 解决"中文方框"；烧录提速。
3. **一批现场 bug**：WiFi 扫描崩溃、连接崩溃、日志可读性、键盘样式统一、日历排版、
   天气页显示 "Text"、待机/唤醒行为、待机后设备失联。
4. **天气纯 IP 定位**：去掉手输城市、修 `lat/lon=0` 解析、默认城市设为南昌。
5. **性能与体验**：烧录 147.9 s → 31.6 s（4.7×）；丰富待机动画；单词书 CET-4 → CET-6。
6. **第二轮收尾 8 项**（`561a591`）：待办同步排查、待机眼睛动画、cet6 词库、SD 卡资源、
   设备端待办保留勾选/删除、网页上位机重写、开机屏幕滚动日志、子 agent 分工与文档同步。
7. **字库移出 app**（`162ba2d`）：镜像 4.16 MB → 2.94 MB，烧录 43.6 s → 31.6 s；
   字库改为只从 `<storage>/todohelper/fonts/ui_cjk.ttf` 加载。
8. **交接与两次根因更正**（`0936a27` → `b4d7683` → `86ba312`）：
   交接文档重写；把插卡崩溃从"crypto 锁竞态"更正为"JSON 解析抽干内部 RAM"并修复；
   把 SSE 冻住服务器的问题修复（详见 1.0.0 的 Fixed）。
