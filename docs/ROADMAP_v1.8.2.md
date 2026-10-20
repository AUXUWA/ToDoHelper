# ROADMAP v1.8.2 —— 页面巡览体检：字形缓存与 SD 挂载

> 上一轮：[ROADMAP_v1.8.1.md](ROADMAP_v1.8.1.md)（把内部堆的最大连续块拉回来）
> 本轮性质：**PATCH**（存储格式、HTTP API、NVS 键语义都没变，也没有新功能或新页面）。

这一轮是被**用户的抱怨**逼出来的：「现在部分汉字变方块了」。
查下去发现的不是字形问题，而是两个更严重的东西。

---

## 1. 方块：SD 卡挂不上 → FreeType 用不了

真机日志：

```
E (2693) vfs_fat_sdmmc: esp_vfs_fat_sdmmc_sdcard_init failed (0x107).
E (2699) S31-Korvo: SD card mount failed (ESP_ERR_TIMEOUT)
I (2710) TH:storage: SD card not available (ESP_ERR_TIMEOUT)
W (2905) TH:ui_fonts: no CJK font at /littlefs/todohelper/fonts/ui_cjk.ttf ...
W (2925) TH:ui_fonts: FreeType unavailable, falling back to the built-in CJK font
        (glyph coverage is limited, expect boxes for rarer hanzi)
```

中文字库（`fonts/ui_cjk.ttf`，1.25 MB）**放在 SD 卡上**。卡挂不上 →
FreeType 用不了 → 退回内嵌的 GB2312 一级子集（~2500 字）→ **生僻字变方块**。

312 次开机里有 **2 次**这样 —— 概率不高，但**用户看得见**，而且没有任何提示。

**改法**（已做）：挂载失败就**重试 3 次**（间隔 500 ms）。
IDF 在挂载失败时会把 SDMMC host 反初始化（`vfs_fat_sdmmc.c` 的 `call_host_deinit`），
所以重试是安全的。连抓 4 次开机：SD 每次都一次挂上。

**更彻底的一步（没做）**：把字库复制一份到**内部 flash**（LittleFS 分区 2 MB，
现在只用 120 KB，放得下 1.25 MB），让文字渲染彻底不依赖 SD 卡。

---

## 2. 逛一遍页面会把内存吃光，设备崩掉

为了找「内存到底被谁吃了」，临时在 `app_main.c` 里加了一个**自动巡览页面**的任务
（切到每一个页 → 打印一次堆 → 下一轮）。**这个任务是临时的，没有提交。**

结果（同一个固件，只改 `CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT` 这一个数）：

| 缓存个数 | 巡览三遍之后 |
|---|---|
| **256（默认）** | 内部堆 **0 KB**，最大连续块 0 KB → **Guru Meditation，设备崩了** |
| 64 | 54~64 KB，最大连续块 5~17 KB |
| **32** | **76~88 KB，最大连续块 22~35 KB**（过了 PHY 需要的 ~30 KB） |

**根因**：LVGL 的 FreeType 字形缓存里装的是**每一个用过的汉字的位图**
（28 px 约 784 B，48 px 约 2.3 KB），而 LVGL 用的是 C 库的 malloc
（`CONFIG_LV_USE_CLIB_MALLOC=y`），所以这些位图全落在**内部堆**里
（≤4096 B 的分配优先内部，见 `AGENT.md §9.0.3`）。

**改法**（已做）：`CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT` 256 → **32**，
写进 `sdkconfig.defaults` 并附上实测数据。

**代价**：被挤掉的字形要重新渲染（FreeType 每个汉字约 100~200 µs，
一屏 30~40 个汉字 → 每帧多几毫秒）。滚动略慢，但不掉帧。

**顺带的收获**：开机时的堆也养好了 ——
`after boot complete` 的 largest 从 108 KB → **128 KB**。

---

## 3. 下一轮的清单

按「值不值得做 / 风险多大」排：

1. **字库搬进内部 flash**（让文字渲染不依赖 SD 卡）——
   内部 LittleFS 分区 2 MB、现在只用 120 KB。要处理「卡上有才复制、复制后以内部为准」。
2. **各页面常驻的对象**：设置页 182 个、单词页、日历页 ——
   按待办列表那套办法（页面隐藏时销毁、回来重建）处理。
   巡览三遍后的稳态最大连续块 22~35 KB，还在 PHY 需要的 ~30 KB 上下浮动。
3. **keep-alive / 连接复用**：墨墨客户端每次请求都新建 TLS 连接，
   一次开机 3~4 次握手，按实测每次约 6 秒电台时间。
4. **httpd 槽位**：`max_open_sockets = 5`，而 3 个 SSE 常驻 + 异步 handler 各占一槽 ——
   网页有连不上的风险。
5. **SD 写放大**：`enrich.json` 每补全一个单词就整文件重写一遍（原子写三槽协议 = 4 次 SD 操作）。

---

## 4. 验收结果（真机实测）

| 验收项 | 结果 |
|---|---|
| 构建 | 0 error 0 warning |
| 连抓 4 次开机 | SD 每次一次挂上、FreeType 就绪、无方块、无 abort / 栈溢出 |
| `after boot complete` | internal **172~173 KB** / largest **128 KB**（v1.8.1 是 151 KB / 108 KB） |
| 页面巡览三遍 | 不再崩（v1.8.1 的字形缓存下会崩） |
