# debug_shell — 板子上的调试 shell（`/api/v1/debug/exec`）

**为什么有这个东西**：v1.3.1 修的"鉴权弹窗关不掉"这类 bug，**只能靠看屏幕确认**，
而当时的验证手段是"请你在板子上点一下、告诉我看到什么"。板子不在手边就完全没法验证。
另外以前没有任何办法从脚本切页或按控件，所以**凡是在"点一下"后面的功能都是不可测的**。

这个组件补上这一环：发一条命令，拿回文本输出，像 shell 一样。

## 开关

```ini
# sdkconfig.defaults（默认打开，便于开发；**发布前关掉**）
CONFIG_TODOHELPER_DEBUG_SHELL=y
```

关掉时整个组件**不参与编译**，`debug_shell_init()` 是个返回 `ESP_OK` 的空函数
（所以 `main/app_main.c` 里的模块行可以无条件留着）。

## 用法

```bash
# GET 形式最适合脚本 / curl
curl "http://todohelper.local/api/v1/debug/exec?cmd=heap"

# POST 形式：body 直接就是命令行（不用编 JSON）
curl -X POST -d 'widgets token' http://todohelper.local/api/v1/debug/exec
curl -X POST -d 'click 知道了'  http://todohelper.local/api/v1/debug/exec

# 开了 HTTP 鉴权时带上令牌（设备设过令牌之后，所有路由包括这条都要令牌）
curl -X POST -H 'X-Auth-Token: <token>' -d 'heap' http://<ip>/api/v1/debug/exec
```

> **鉴权**（v1.5.0 起）：设备**没设过令牌**时，局域网默认不校验，这条路由直接可用；
> 设备**设过令牌**之后，所有路由（包括这条）都要带 `X-Auth-Token`。
> 所以刚开机的板子可以直接 `curl -d 'heap'`，开了鉴权的板子要带令牌——
> 这也是 `tools/dsh.sh` 要「自动找令牌」的原因。

## 命令

| 命令 | 作用 |
|---|---|
| `help` | 命令列表 |
| `version` | 固件/IDF 版本、编译时间、芯片信息 |
| `status` | `sys_status` 快照（WiFi/IP/时间/待办/存储）|
| `heap` | 内部 RAM / PSRAM 空闲、**最大连续块**、`int-low` |
| `tasks` | 任务数与栈高水位（需 `FREERTOS_USE_TRACE_FACILITY`，当前为关）|
| `routes` / `pages` | 已注册路由数 / 页面表 + 当前页 |
| `time` / `storage` | 时钟与时区 / 存储后端与剩余空间 |
| `cfg get\|set <k> [v]` / `nvs <k>` | 读写/擦除 NVS 键 |
| `http-auth [set <hex>]` | **读/改 HTTP 鉴权令牌**（仅调试构建，见下）|
| `log <tag> [0-5]` | 运行时调某个 tag 的日志级别 |
| `reboot` | 重启 |
| **`ui`** | 当前页 + 顶层概览（浅层树）|
| **`widgets [过滤] [深度]`** | 打印对象树：类名、坐标、尺寸、`[click]`/`[hidden]`、标签文本 |
| **`click <标签或类名> [序号]`** | 按控件（按可见标签匹配，如 `click 知道了`）|
| **`text <匹配> <新文本>`** | 直接改标签文本 |
| **`page <0-10\|名字>`** | 切页（`page settings` / `page word` / …）|
| **`tap <x> <y> [click]`** | 报出该坐标命中的控件（加 `click` 就真的按下去）|
| **`idle enter\|exit`** / **`bright <0-100>`** | 强制进/出待机、调背光 |
| **`idle-timeout [秒]`** | 读写待机超时并**立即重新生效**（0 = 永不待机）|

配套脚本：

| 脚本 | 作用 |
|---|---|
| `tools/dsh.sh <命令...>` | 发一条命令、打印文本输出；**自动找当前令牌**并在 401 时换一个重试 |
| `tools/dsh_debug_probe.sh --token` | 单独取当前 HTTP 令牌（先查日志，再读 NVS）|

```bash
bash tools/dsh.sh heap
bash tools/dsh.sh page settings
bash tools/dsh.sh click 生成并开启      # 会顺手报出弹窗对象树
bash tools/dsh.sh tap 745 162 click     # 按弹窗的「×」
```

> **`tools/dsh.sh` 为什么要"自动找令牌"**：那个生成 token 的按钮**本身就会轮换令牌**，
> 而本组件的路由永远要求鉴权。所以脚本每次调用都重新发现令牌，并在 401 时换下一个候选重试；
> 成功的令牌会记进 `build/.dsh_tokens`（日志只在**生成**令牌时才有那一行，之后无从得知）。
> `DSH_TOKEN=<token>` 可跳过发现流程。
>
> **读 NVS 会重启板子**：esptool 无法在应用运行时附加（`--no-reset` 会报
> `Failed to connect ... No serial data received`），必须把芯片拉进 ROM 下载模式。
> 所以 `dsh.sh` **优先查日志**，NVS 只是兜底 —— 一次复位会关掉正在测的弹窗、丢掉当前页，
> 那和"UI 有 bug"根本分不清。
| **`idle-timeout [秒]`** | 读写待机超时并**立即重新生效**（0 = 永不待机）|

## 三个"踩过才知道"的实现要点

1. **`click <标签>` 必须自己往上找可点击祖先。**
   第一版直接把事件发给匹配到的对象 —— 而按钮的文字在**子 label** 上，于是
   `click 生成并开启` 匹配到 label、对不可点击对象发事件、**静默什么都不发生**
   （弹窗根本没开）。LVGL 真实触摸是路由到最近的可点击祖先的，shell 必须照做，
   否则无法复现"手指点下去会怎样"。
2. **弹窗要在"点击的同一个调用里"同步检查。**
   隔一次 HTTP 请求再看是**分不清**"没开"和"已经关掉"的 —— 而那个按钮本身会
   轮换鉴权令牌，客户端再取一次令牌就已经来不及了。所以 `click` 会当场报出
   `top layer now: N task(s), M msgbox(es)`，并在有弹窗时**顺手打印它的对象树**。
   这正是让"弹窗关不掉"这类 bug 可以无人值守验证的关键。
3. **`tap` 用 `lv_indev_search_obj()`，不是 `lv_obj_hit_test()`。**
   后者只回答"这个点在不在这个对象里"，**不搜索**。而且要**先搜顶层**（顶层画在
   上面，这正是模态弹窗会吞掉页面点击的原因）。`tap 745 162 click` 也是唯一能按到
   **没有文字的控件**的办法 —— 弹窗的「×」是个 image，`click <标签>` 找不到它。

## 三条实现上的要点（改代码前先读）

1. **UI 命令必须回到 LVGL 任务执行。** `ui`/`widgets`/`click`/`text`/`page`/`tap`
   都通过 `ui_run_async()` + 信号量"派回 LVGL 任务并等结果"。直接在 httpd 任务里
   调 LVGL 会破坏对象树（或触发 `lv_obj_has_class` 断言）；纯异步又会**在结果存在前
   就回复**。等待是安全的，因为 LVGL 任务从不反向阻塞 httpd 任务。
2. **不用 LVGL 私有 API。** `LV_USE_PRIVATE_API` 是关的，所以拿不到
   `lv_obj_class_private.h` 里的类名字符串。改为把本工程真正用到的类**逐个与公开的
   `lv_obj_check_type()` 对比**，映射成名字（`class_name_of()`）。未知的一律显示 `obj`。
3. **`lv_obj_hit_test()` 不能用来找坐标上的控件** —— 它只回答"这点在不在这个对象里"，
   不搜索。`tap` 用的是 LVGL 自己的 `lv_indev_search_obj()`，且**先搜顶层**
   （顶层画在上面，这正是模态弹窗会吞掉页面点击的原因）。

## 安全

- 它能切页、按控件、改 NVS、重启设备 —— 所以：
  - 默认**关**（`default n`，本仓库的开发用 `sdkconfig.defaults` 里显式打开）；
  - 它的路由用 `http_router_require_auth(path, method, true)` **强制要求鉴权**，
    **即使全局鉴权是关的**。也就是说"编了调试固件"不等于"把设备控制权暴露在开放网络上"；
  - 发布前把 `CONFIG_TODOHELPER_DEBUG_SHELL` 关掉。
- **`http-auth` 会回读令牌**，这是对整个"令牌只写不读"约定的一处**有意的例外**：
  读它出来本来得复位板子（见上），而"测试中途把板子重启了"对一个调试工具来说
  比"调试构建里能读回自己的令牌"糟得多。产品侧不受影响 —— 关掉这个 Kconfig，
  该命令连同整个组件一起不参与编译，任何常规路由都不回读令牌。

## 实测（2026-09-28，真机 COM34）

这个组件写出来就是为了验证 v1.3.1 那个"只靠看屏幕才能确认"的鉴权弹窗。
用它跑通的证据（**全程无人值守**）：

```
$ bash tools/dsh.sh page settings
switch -> ESP_OK (9), now on 9 (settings)

$ bash tools/dsh.sh click 生成并开启
clicked button (matched label '生成并开启')
top layer now: 2 child(ren), 1 msgbox(es)  <- dialog is OPEN

$ bash tools/dsh.sh widgets          # 排版稳定后
  msgbox     24,136   752x208  [click]                  ← 752 宽 / 208 高 = 横的
    obj        40,152   720x20
      label      45,145   685x34   HTTP 鉴权已开启
      obj       735,157    20x10
        image     739,150    12x24                      ← 「×」关闭键在
    obj        40,226   720x72
      label      40,238   353x48   把下面的 token 填进…   ← 左列 353 px
      obj       407,226   353x72
        label     419,238   329x48   d84ad67d…ca        ← 右列 353 px，令牌可见
    obj        40,308   720x20
      obj       352,296    96x44
        label     370,306    60x24   知道了

$ bash tools/dsh.sh click 知道了
clicked obj (matched obj '知道了')
top layer now: 1 child(ren), 0 msgbox(es)                ← 关掉了
```

即：**打开 → 令牌真的渲染出来 → 两列横排（各 353 px）→ 「知道了」能关**，
四件事全部由脚本断言，不需要人在板子旁边。

副产品结论（同一轮用 `heap` 测出来的）：**首次进入单词页会让内部 RAM 从
74 KB / 最大连续块 18 KB 掉到 49 KB / 5 KB**，且离开页面不回收。那个 5 KB
正是 `page_word.c` 里每个卡片一个、5 KB 栈的 `th_mm_enrich` worker —— 和
v1.3.1 给评分路径做 NO_MEM 降级时的判据完全一致（见 `AGENT.md §9.0.7 ③`）。
不是持续泄漏（反复采样稳定），但它是本板最紧的一处，改单词页时请注意。
