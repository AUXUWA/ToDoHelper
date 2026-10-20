/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_font_mem.c
 * @brief 字形位图住进 PSRAM（只改 FreeType 这一条路）
 *
 * ## 为什么界面不流畅
 *
 * LVGL 9 的 FreeType 集成**所有字号共用一份位图缓存**：缓存节点的 key 是
 * （字体文件, 样式, 渲染模式），**没有字号**（lv_freetype.c 的 search_key）。
 * 所以 `CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT` 是**整个界面共用**的上限：
 * 64 条 / 4 个字号 ≈ 每个字号只有 16 个字的位置，而一屏就有 30~40 个汉字。
 *
 * 缓存一满，新字就必须重新光栅化：读 SD 卡里的 TTF + FreeType 渲染，
 * 一个字约 10 ms —— 滚动时就是一卡一卡。v1.8.2 为了不崩把上限压到 32，
 * v1.8.3 放回 64，用户反馈「还是没有之前流畅」（v1.8.2 之前是默认的 256）。
 *
 * ## 为什么不能直接开 256
 *
 * 256 个位图要占几百 KB 内部 RAM，设备直接崩（Guru Meditation）。
 * 之前试过把 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 压到 128（让所有小分配都落 PSRAM），
 * 结果 **TLS 握手失败**（mbedtls_ssl_handshake 返回 -0x008D = 内存不足）——
 * 全局阈值一动，mbedTLS 握手时的小分配也被赶去 PSRAM，连续块就不够了。
 *
 * ## 这一版的做法
 *
 * 换一条更窄的路：**只把 LVGL 的「字体绘制缓冲」换成 PSRAM 分配**。
 * FreeType 的字形位图正是走这条路（lv_freetype_image.c 用 font_draw_buf_handlers
 * 分配位图），而这条路**不经过 malloc()**，所以：
 *   - 全局阈值一个字没动 → TLS 握手不受影响；
 *   - 只有「字体绘制缓冲」这一类搬去了 PSRAM。
 *
 * **这条路的影响面比「只有 FreeType 的位图」宽一点**（独立复核员查出来的）：
 * LVGL 给每个 label 准备的 A8 字形暂存缓冲也走同一组回调
 * （lv_draw_label.c:646），画完就释放 —— 那是**每帧每个 label 一次
 * PSRAM 分配/释放**。它不影响正确性，实测整屏重画 24 ms 里已经算上了；
 * 要把两者分开只能改 LVGL 源码（两者用的是同一组 handler），本轮不动。
 *
 * 位图本身也从内部 RAM 换到了 PSRAM，于是**混合（blend）时掩码是从 PSRAM 读的**
 * （lv_draw_sw_letter.c 的 mask_buf）。PSRAM 250 MHz，够用。
 *
 * 缓存的**元数据**（红黑树节点 + 缓存条目 + draw_buf 结构，每个字形约 200 B）
 * 也搬去了 PSRAM —— 那是另一条路（ui_lv_mem.c，接管 LVGL 的分配器）。
 *
 * ## 前提：显式指定 MALLOC_CAP_SPIRAM 一定能拿到 PSRAM
 *
 * 这块板子 `CONFIG_SPIRAM_USE_CAPS_ALLOC` 没开，但 PSRAM 仍然被
 * `esp_psram_extram_add_to_heap_allocator()` 用 `MALLOC_CAP_SPIRAM` 注册进了
 * heap_caps 分配器（esp_psram.c: `heap_caps_add_region_with_caps()`）。
 *
 * 而 ALWAYSINTERNAL 的限制只作用在 `heap_caps_malloc_default()`（也就是普通的
 * `malloc()`）：它按 size 决定去内部还是外部。显式指定 caps 的
 * `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` 走的是
 * `heap_caps_malloc_base()`，**不受这个限制**，一定拿到 PSRAM。
 *
 * 参见 AGENT.md §9.0.1.8。
 */

#include "ui_font_mem.h"
#include "ui_lv_mem.h"

#include <string.h>

#include "lvgl.h"
#include "draw/lv_draw_buf.h"
#include "draw/lv_draw_buf_private.h"
#include "libs/freetype/lv_freetype_private.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "ui_font_mem";

static bool s_initialized;

/** 跟踪用到的字体，好让 ftstats 能报缓存情况。 */
static const lv_font_t *s_tracked[UI_FONT_MEM_MAX_FONTS];

/** 包装过的字体的原始回调。 */
typedef struct {
    const lv_font_t *font;
    bool (*orig_get_glyph_dsc)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
    const void *(*orig_get_glyph_bitmap)(lv_font_glyph_dsc_t *, lv_draw_buf_t *);
} tracked_font_t;

static tracked_font_t s_wrapped[UI_FONT_MEM_MAX_FONTS];

/**
 * 字形位图分配：优先 PSRAM
 *
 * PSRAM 满了就退回普通堆 —— 宁可慢，也不能画不出字。
 */
static void * glyph_buf_malloc(size_t size_bytes, lv_color_format_t color_format)
{
    LV_UNUSED(color_format);

    /* 与 LVGL 默认的 buf_malloc() 一致：多留出对齐的余量，
     * lv_draw_buf_align() 之后要按 LV_DRAW_BUF_ALIGN 对齐。 */
    size_bytes += LV_DRAW_BUF_ALIGN - 1;

    void *buf = heap_caps_malloc(size_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG, "PSRAM full, glyph buffer falls back to internal RAM (%u bytes)",
                 (unsigned)size_bytes);
        buf = heap_caps_malloc(size_bytes, MALLOC_CAP_8BIT);
    }
    return buf;
}

static void glyph_buf_free(void *buf)
{
    heap_caps_free(buf);
}

/* ------------------------------------------------------------------------- */
/* Font callback wrappers                                                     */
/* ------------------------------------------------------------------------- */

/**
 * The two callbacks are where LVGL asks the font for a glyph. Everything
 * allocated inside them belongs to the glyph cache (the red-black tree nodes,
 * the cache entries, the draw_buf structs) plus FreeType's own scratch
 * buffers - and the cache metadata is what does not fit into internal RAM.
 *
 * So the wrappers only open and close the "this is a font allocation" scope
 * (ui_lv_mem.h) and let the allocator send those blocks to PSRAM.
 */
static const tracked_font_t *find_tracked(const lv_font_t *font)
{
    if (font == NULL) {
        return NULL;
    }
    for (int i = 0; i < UI_FONT_MEM_MAX_FONTS; i++) {
        if (s_wrapped[i].font == font) {
            return &s_wrapped[i];
        }
    }
    return NULL;
}

static bool wrapped_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                                  uint32_t letter, uint32_t letter_next)
{
    const tracked_font_t *t = find_tracked(font);
    if (t == NULL || t->orig_get_glyph_dsc == NULL) {
        return false;
    }

    ui_lv_mem_font_scope_enter();
    const bool ok = t->orig_get_glyph_dsc(font, dsc, letter, letter_next);
    ui_lv_mem_font_scope_leave();
    return ok;
}

static const void *wrapped_get_glyph_bitmap(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *draw_buf)
{
    /* The bitmap callback has no font pointer, but the descriptor knows which
     * font the glyph came from. */
    const tracked_font_t *t = find_tracked(dsc->resolved_font);
    if (t == NULL || t->orig_get_glyph_bitmap == NULL) {
        return NULL;
    }

    ui_lv_mem_font_scope_enter();
    const void *bitmap = t->orig_get_glyph_bitmap(dsc, draw_buf);
    ui_lv_mem_font_scope_leave();
    return bitmap;
}

/**
 * Replace the font's glyph callbacks with wrappers that mark the allocations
 * as "font allocations" (so they end up in PSRAM).
 *
 * Only FreeType fonts are wrapped: the built-in fonts are flash tables and
 * allocate nothing, and wrapping them would open the PSRAM scope for
 * lookups that do not need it.
 */
static void wrap_font(const lv_font_t *font)
{
    lv_font_t *f = (lv_font_t *)font;

    if (f->dsc == NULL) {
        return;
    }
    /* Already wrapped? Then the "original" callback would be the wrapper
     * itself and the font would recurse into the stack. */
    if (f->get_glyph_dsc == wrapped_get_glyph_dsc) {
        return;
    }
    /* The caller already told us this is a FreeType font, so reading its
     * descriptor is not a cross-type read - and the magic number is a cheap
     * sanity check that the two sides agree. */
    const lv_freetype_font_dsc_t *dsc = (const lv_freetype_font_dsc_t *)f->dsc;
    if (dsc->magic_num != LV_FREETYPE_FONT_DSC_MAGIC_NUM) {
        return; /* not a FreeType font after all */
    }

    for (int i = 0; i < UI_FONT_MEM_MAX_FONTS; i++) {
        if (s_wrapped[i].font != NULL) {
            continue;
        }

        tracked_font_t *t = &s_wrapped[i];
        t->font = font;
        t->orig_get_glyph_dsc = f->get_glyph_dsc;
        t->orig_get_glyph_bitmap = f->get_glyph_bitmap;

        f->get_glyph_dsc = wrapped_get_glyph_dsc;
        f->get_glyph_bitmap = wrapped_get_glyph_bitmap;
        return;
    }
}

esp_err_t ui_font_mem_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    lv_draw_buf_handlers_t *handlers = lv_draw_buf_get_font_handlers();
    if (handlers == NULL) {
        ESP_LOGE(TAG, "LVGL has no font draw buffer handlers");
        return ESP_FAIL;
    }
    if (handlers->align_pointer_cb == NULL || handlers->buf_malloc_cb == NULL) {
        /* LVGL fills these in lv_init(). Overriding them before that would
         * leave the rest of the handler set empty (a NULL align callback
         * makes lv_draw_buf_align() return NULL, and every glyph would come
         * out blank). */
        ESP_LOGE(TAG, "LVGL is not initialised yet");
        return ESP_ERR_INVALID_STATE;
    }

    /* 只换分配/释放这两个回调，拷贝、对齐、stride 那些一律保持 LVGL 原样。 */
    handlers->buf_malloc_cb = glyph_buf_malloc;
    handlers->buf_free_cb = glyph_buf_free;

    s_initialized = true;

    ESP_LOGI(TAG, "font draw buffers now come from PSRAM (free %u bytes, internal largest %u bytes)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

void ui_font_mem_set_cache_cnt(uint32_t cnt)
{
    if (cnt == 0) {
        return;
    }

    /* The FreeType context is created by lv_init(); before that there is
     * nothing to configure. */
    lv_freetype_context_t *ctx = lv_freetype_get_context();
    if (ctx == NULL) {
        ESP_LOGW(TAG, "FreeType not initialised yet, cache limit not applied");
        return;
    }

    ctx->max_glyph_cnt = cnt;
}

uint32_t ui_font_mem_cache_cnt(void)
{
    lv_freetype_context_t *ctx = lv_freetype_get_context();
    return ctx ? ctx->max_glyph_cnt : 0;
}

void ui_font_mem_track(const lv_font_t *font, bool is_freetype)
{
    if (font == NULL || !is_freetype) {
        return; /* only FreeType fonts have a glyph cache */
    }

    for (int i = 0; i < UI_FONT_MEM_MAX_FONTS; i++) {
        if (s_tracked[i] == font) {
            return;
        }
    }

    for (int i = 0; i < UI_FONT_MEM_MAX_FONTS; i++) {
        if (s_tracked[i] == NULL) {
            s_tracked[i] = font;
            wrap_font(font);
            return;
        }
    }
}

void ui_font_mem_forget_all(void)
{
    /*
     * Put the original callbacks back before dropping the bookkeeping.
     *
     * The wrappers live inside the font structs, so forgetting the fonts
     * without restoring the originals would leave a font whose "original"
     * callback is the wrapper - and wrapping it again would make it call
     * itself. Restore first, then forget.
     *
     * The fonts must still be alive when this runs.
     */
    for (int i = 0; i < UI_FONT_MEM_MAX_FONTS; i++) {
        if (s_wrapped[i].font == NULL) {
            continue;
        }
        lv_font_t *f = (lv_font_t *)s_wrapped[i].font;
        if (f->get_glyph_dsc == wrapped_get_glyph_dsc) {
            f->get_glyph_dsc = s_wrapped[i].orig_get_glyph_dsc;
            f->get_glyph_bitmap = s_wrapped[i].orig_get_glyph_bitmap;
        }
    }

    memset(s_tracked, 0, sizeof(s_tracked));
    memset(s_wrapped, 0, sizeof(s_wrapped));
}

uint32_t ui_font_mem_stats(ui_font_cache_stat_t *out, uint32_t max)
{
    if (out == NULL || max == 0) {
        return 0;
    }

    uint32_t written = 0;

    for (int i = 0; i < UI_FONT_MEM_MAX_FONTS && written < max; i++) {
        const lv_font_t *font = s_tracked[i];
        if (font == NULL || font->dsc == NULL) {
            continue;
        }

        /* Only FreeType fonts get tracked (see ui_font_mem_track), so this
         * descriptor really is a lv_freetype_font_dsc_t - no guessing. */
        const lv_freetype_font_dsc_t *dsc = (const lv_freetype_font_dsc_t *)font->dsc;
        if (dsc->magic_num != LV_FREETYPE_FONT_DSC_MAGIC_NUM) {
            continue;
        }
        if (dsc->cache_node == NULL) {
            continue;
        }

        out[written].size = dsc->size;
        out[written].glyph_entries =
            dsc->cache_node->glyph_cache ? (uint32_t)lv_cache_get_size(dsc->cache_node->glyph_cache, NULL) : 0;
        out[written].bitmap_entries =
            dsc->cache_node->draw_data_cache ? (uint32_t)lv_cache_get_size(dsc->cache_node->draw_data_cache, NULL) : 0;
        written++;
    }

    return written;
}
