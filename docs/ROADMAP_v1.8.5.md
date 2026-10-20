# ROADMAP v1.8.5 —— 把界面流畅度还回去

> 起因：v1.8.4 之后用户仍然反馈「还是没有之前流畅」。
> 目标：让界面回到 v1.8.2 之前（字形缓存 256 那个时代）的流畅度，
> 而且不能再出现「逛一遍页面就把内存吃光然后崩掉」。

## 1. 先量，别猜

这一版先花力气把「卡」变成数字，工具都留在仓库里：

| 工具 | 干什么 | 位置 |
|---|---|---|
| `bench <target> <text> [index] [repeats]` | 设标签文本 → `lv_refr_now()` 同步重绘 → 打印耗时（µs） | `components/debug_shell/src/debug_shell.c` |
| `ftstats` | 打印各字号的字形缓存条目数（字形描述 / 位图） | 同上 |
| `page <id\|name>` | 现在会打印**切页耗时**和首次重绘耗时 | 同上 |
| `tools/page_tour.py` | 走一遍所有页面，每页打印内部堆 | `tools/` |
| `tools/bench_glyphs.py` | 连续用不同的新汉字测重绘耗时 | `tools/` |

`bench` 是关键：光设文本不会触发画字（画字发生在下一次刷新里），
所以必须 `lv_refr_now()` 同步重绘，测出来的时间才包含光栅化。

## 2. 卡在哪：三个查出来的真相

### 真相一：字形缓存是**整个界面共用**的，不是每个字号一份

LVGL 9 的 FreeType 按 **（字体文件, 样式, 渲染模式）** 给缓存做 key
（`lv_freetype.c` 的 `search_key`），**没有把字号算进去**。
四个字号（16/20/28/48 px）来自同一个 .ttf，所以**共用同一份位图缓存**。

于是 `CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT=64` 的真实含义是
「四个字号加起来 64 个字」，每个字号平均只有 16 个位置 ——
而一屏就有 30~40 个汉字。缓存永远不够用，每个新字都要重新光栅化。

（`ftstats` 四个字号报出来的数字完全一样，就是这个原因。）

### 真相二：光栅化一个字要 **9 ms**

`bench` 实测：30 个没缓存过的汉字，一次重绘 **1.35 s**（缓存上限 64）。
拆开来是约 9 ms 一个字 —— 主要是从 SD 卡读 TTF（字库在 SD 卡上），
FreeType 渲染本身只有 100~200 µs。

### 真相三：卡的不只是滚动，**切页也一样卡**

切页实测 16~174 ms，第一反应是「v1.8.3 把页面控件隐藏时销毁了，
每次切页都要重建」—— 结果不是。缓存热了以后切页只要 **4~6 µs**。
慢的同样是画字。

## 3. 怎么修

### 3.1 字形位图 → PSRAM（`components/ui_core/src/ui_font_mem.c`）

FreeType 的位图是走 LVGL 的 `font_draw_buf_handlers` 分配的
（`lv_freetype_image.c`）。把这个 handler 的 malloc/free 换成
显式带 `MALLOC_CAP_SPIRAM` 的 `heap_caps_malloc`：

```c
void * buf = heap_caps_malloc(size_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
```

**不经过 `malloc()`，所以 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 一个字没动** ——
这正是 v1.8.2 试过并失败的那条路（压到 128 会让 TLS 握手失败，`-0x008D`）。

前提（查过 IDF 源码）：这块板子虽然没开 `CONFIG_SPIRAM_USE_CAPS_ALLOC`，
但 PSRAM 仍然被 `esp_psram_extram_add_to_heap_allocator()` 用
`MALLOC_CAP_SPIRAM` 注册进了 heap_caps 分配器；而 ALWAYSINTERNAL 的限制
只作用在 `heap_caps_malloc_default()`（即普通 `malloc()`），
显式带 caps 的 `heap_caps_malloc()` 走的是 `heap_caps_malloc_base()`，不受它管。

### 3.2 字形缓存的**元数据** → PSRAM（`components/ui_core/src/ui_lv_mem.c`）

光搬位图不够。位图之外，每个缓存条目还有元数据
（红黑树节点 + 缓存条目 + draw_buf 结构），**每个字形约 200 B**，
517 个组合就是 **100 KB** —— 内部堆一共才 290 KB，搬走位图也腾不出这个空间。

这层元数据是 LVGL 的 `lv_cache` 通过 `lv_malloc()` 分配的，
要单独搬它就得接管 LVGL 的分配器（`CONFIG_LV_USE_CUSTOM_MALLOC=y`）。

**接管了也不能一股脑全放 PSRAM** —— LVGL 的控件对象每帧都要读，
放 PSRAM 会让整个界面变慢。所以只在一个很窄的区间里改道：
**FreeType 取字形的两个回调**（`get_glyph_dsc` / `get_glyph_bitmap`）。
这两个回调里 LVGL 分配的正好就是字形缓存的元数据（外加 FreeType 自己的
临时缓冲）；回调之外，分配行为跟原来一模一样。

区间由 `ui_font_mem.c` 包装字体回调时打开/关闭。

### 3.3 上限 64 → 512

实测整个界面用到 **517 个（字形, 字号）组合、416 个位图**，
所以 512 刚好装得下。可以用 NVS 键 `ft_glyph_cache` 在运行时调
（范围 16~4096），不用重新编译就能对比。

## 4. 实测结果

同一个固件、同一段 30 个新汉字，只改缓存上限（`bench` 命令）：

| 缓存上限 | 重绘耗时 | 说明 |
|---|---|---|
| 64（v1.8.4） | **1 350 000 µs** | 界面卡就是这个 |
| 512（v1.8.5） | **65 000 ~ 99 000 µs** | 快了 14~20 倍 |
| 512，缓存已热 | **24 000 ~ 27 000 µs** | 稳态 |

切页：16~174 ms → **4~6 µs**。

页面巡游三遍之后的内部堆（`tools/page_tour.py`）：

| | v1.8.4（上限 64，CLIB 分配器） | v1.8.5（上限 512，自定义分配器） |
|---|---|---|
| 内部堆空闲 | 73 KB | **150 KB** |
| 内部堆最大连续块 | 12 KB | **80 KB** |
| 最低水位 | 38 KB | **109 KB** |
| PSRAM 空闲 | 9 986 KB | 9 828 KB |

开机后：内部堆 166 KB / 最大连续块 122 KB → **186 KB / 140 KB**。

TLS：天气请求正常返回（20.0 C），日志里 **没有 -0x008D**，
没有 mbedtls 握手失败 —— 全局分配阈值没动，代价只有字形缓存那一块。

## 5. 验收标准（都过了）

- [x] 页面巡游 3 轮不崩，内部堆最大连续块 ≥ 30 KB（实测 80 KB）
- [x] 内部堆最低水位不为 0（实测 109 KB）
- [x] 日志里 `-0x008D` 次数为 0，TLS 握手正常
- [x] 字形缓存装得下整个界面（417/416 条，上限 512）
- [x] 重绘耗时比 v1.8.4 快一个数量级
- [ ] **人手验证：实际滑动和切页的手感**（子代理碰不到设备）

## 6. 下一版可以做的

1. **字库搬进内部 flash。** 现在每个新字要 9 ms，其中约 8.8 ms 是从 SD 卡
   读 TTF。把字库放进内部 flash（或做个 mmap 的 LVGL fs 驱动），
   光栅化就能从 9 ms 掉到 0.2 ms 上下 —— 缓存偶尔没命中也不卡。
   （顺带解决「SD 卡挂不上就满屏方块」的老问题。）
2. **页面控件别销毁重建。** v1.8.3 为了内存放弃了常驻。
   现在字形缓存不占内部堆了，页面内容常驻的账重新算一遍：
   实测各页内容大约 todo 8 KB / calendar 23 KB / settings 28 KB / 其余 2~6 KB，
   全部常驻约 80~90 KB。内部堆现在有 150 KB 空闲 —— 够，但要给
   WiFi PHY 留出连续块（它要几十 KB），这账要单独验。
3. **字形缓存的元数据还能再省。** 现在元数据在 PSRAM，但每个字形仍占
   约 200 B 的 PSRAM；416 个字形约 85 KB PSRAM，够用但不是白拿的。
