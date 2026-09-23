/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_fonts.c
 * @brief Font resolution with a guaranteed-usable fallback chain.
 *
 * Order (first usable wins), per docs/TODOHELPER_FRAMEWORK.md 5.5:
 *   1. FreeType TrueType font from storage  -> <app_dir>/fonts/ui.ttf
 *   2. FreeType TrueType font embedded in the firmware (CJK subset, scalable)
 *   3. LVGL built-in Source Han Sans SC CJK  (small, but always has Chinese)
 *   4. LVGL built-in Montserrat              (Latin only, last resort)
 *
 * Adding a better font is a pure data change: drop a .ttf at
 * <storage>/todohelper/fonts/ui.ttf - no code edit required.
 */

#include "ui_theme.h"

#include <string.h>

#include "esp_lv_adapter.h"
#include "storage_manager.h"

#include "ui_fonts.h"

#include "storage_manager.h"
#include "sys_utils.h"

static const char *TAG = "ui_fonts";

static const lv_font_t *s_fonts[UI_FONT_ROLE_MAX];
static bool s_initialized;
/** True when the active fonts came from FreeType, for logs and reload(). */
static bool s_using_freetype;

#if CONFIG_TODOHELPER_FONT_FREETYPE
/** Pixel size for each role, used by the FreeType path. */
static const uint16_t s_role_size[UI_FONT_ROLE_MAX] = {
    [UI_FONT_SMALL] = 16,
    [UI_FONT_BODY] = 20,
    [UI_FONT_TITLE] = 28,
    [UI_FONT_HUGE] = 48,
};

/**
 * Font file inside <app_dir>/ that the embedded subset is written to.
 *
 * LVGL v9's FreeType integration **cannot** render from a memory buffer - the
 * adapter says so at runtime ("LVGL v9 FreeType does not support memory-based
 * fonts, will use file path") and then fails the init, which is exactly what
 * happened on the first hardware bring-up. So the embedded subset is written to
 * storage once and loaded from there. That also means replacing the font no
 * longer needs a reflash: drop a .ttf at <root>/todohelper/fonts/ui.ttf.
 */
#define UI_FONT_FILE_REL "fonts/ui_cjk.ttf"

/* The CJK subset is NOT compiled into the application any more.
 *
 * It is a 1.2 MB TTF - 30% of the whole app image - and it always lived on the SD
 * card anyway (ui_fonts loads the card copy first). Embedding it meant esptool
 * rewrote 1.2 MB of identical data on every flash. It is now loaded from storage
 * only; see AGENT.md 9.7 and ui_core/CMakeLists.txt for how to re-embed it if a
 * self-contained image is ever needed. */

static esp_lv_adapter_ft_font_handle_t s_ft_handles[UI_FONT_ROLE_MAX];
#endif

/* ------------------------------------------------------------------------- */
/* Built-in fallbacks                                                        */
/* ------------------------------------------------------------------------- */

static const lv_font_t *builtin_font(ui_font_role_t role)
{
    switch (role) {
#if CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
    case UI_FONT_SMALL:
        return &lv_font_source_han_sans_sc_14_cjk;
#endif
#if CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
    case UI_FONT_BODY:
    case UI_FONT_TITLE:
        return &lv_font_source_han_sans_sc_16_cjk;
#endif
    case UI_FONT_HUGE:
#if CONFIG_LV_FONT_MONTSERRAT_46
        return &lv_font_montserrat_46;
#else
        break;
#endif
    default:
        break;
    }

    /* Any role without a dedicated built-in shares the CJK 16 font so Chinese
     * never degrades into empty boxes. */
#if CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
    return &lv_font_source_han_sans_sc_16_cjk;
#elif CONFIG_LV_FONT_MONTSERRAT_14
    return &lv_font_montserrat_14;
#else
    return lv_font_get_default();
#endif
}

/* ------------------------------------------------------------------------- */
/* FreeType path                                                             */
/* ------------------------------------------------------------------------- */

#if CONFIG_TODOHELPER_FONT_FREETYPE

/**
 * Make sure a usable TTF exists on storage, writing the embedded subset out on
 * first use. Returns the absolute VFS path, or NULL.
 */
static const char *ft_ensure_file(void)
{
    static char path[192];

    if (!storage_available()) {
        SYS_LOGW(TAG, "no storage: cannot materialise the font file");
        return NULL;
    }
    if (storage_path(path, sizeof(path), UI_FONT_FILE_REL) != ESP_OK) {
        return NULL;
    }

    size_t existing = 0;
    if (storage_exists(UI_FONT_FILE_REL, &existing) == ESP_OK && existing > 1024) {
        /* A font is already there; it wins over the embedded one so the look can
         * be changed without reflashing. */
        SYS_LOGI(TAG, "using font from storage: %s (%u bytes)", path, (unsigned)existing);
        return path;
    }

    /* Nothing embedded to fall back on: point at the expected location. */
    SYS_LOGW(TAG, "no CJK font at %s - copy a .ttf there (see AGENT.md 9.5); "
                  "falling back to the built-in CJK font, which only covers the "
                  "~2500 most common glyphs so rarer hanzi will show as boxes", path);
    return NULL;
}

static bool ft_load_from_file(void)
{
    const char *path = ft_ensure_file();
    if (path == NULL) {
        return false;
    }

    for (int i = 0; i < UI_FONT_ROLE_MAX; i++) {
        const esp_lv_adapter_ft_font_config_t cfg =
            ESP_LV_ADAPTER_FT_FONT_FILE_CONFIG(path, s_role_size[i],
                                               ESP_LV_ADAPTER_FT_FONT_STYLE_NORMAL);
        esp_err_t err = esp_lv_adapter_ft_font_init(&cfg, &s_ft_handles[i]);
        if (err != ESP_OK) {
            SYS_LOGW(TAG, "freetype init (%s at %u px) failed: %s",
                     path, s_role_size[i], esp_err_to_name(err));
            return false;
        }
        s_fonts[i] = esp_lv_adapter_ft_font_get(s_ft_handles[i]);
        if (s_fonts[i] == NULL) {
            SYS_LOGW(TAG, "freetype returned no lv_font_t for %u px", s_role_size[i]);
            return false;
        }
    }

    SYS_LOGI(TAG, "FreeType ready: %s at %u/%u/%u/%u px", path,
             s_role_size[UI_FONT_SMALL], s_role_size[UI_FONT_BODY],
             s_role_size[UI_FONT_TITLE], s_role_size[UI_FONT_HUGE]);
    return true;
}

/**
 * Per-role fallback for codepoints the embedded subset does not contain.
 *
 * The subset covers GB2312 level 1 plus ASCII, so it has no LVGL symbol glyphs
 * (LV_SYMBOL_* live in U+F000..) and no rare hanzi. Without a fallback those
 * render as boxes - which is exactly what the on-screen keyboard did, because its
 * shift/backspace/ok keys are drawn with LV_SYMBOL_*.
 *
 * LVGL resolves `lv_font_t.fallback` recursively, so each role falls back to the
 * Montserrat size closest to it (symbols and punctuation), and that in turn falls
 * back to the built-in CJK font for hanzi outside the subset.
 */
static const lv_font_t *role_symbol_fallback(ui_font_role_t role)
{
    switch (role) {
#if CONFIG_LV_FONT_MONTSERRAT_18
    case UI_FONT_SMALL:
    case UI_FONT_BODY:
        return &lv_font_montserrat_18;
#endif
#if CONFIG_LV_FONT_MONTSERRAT_24
    case UI_FONT_TITLE:
        return &lv_font_montserrat_24;
#endif
#if CONFIG_LV_FONT_MONTSERRAT_46
    case UI_FONT_HUGE:
        return &lv_font_montserrat_46;
#endif
    default:
        break;
    }
#if CONFIG_LV_FONT_MONTSERRAT_18
    return &lv_font_montserrat_18;
#else
    return NULL;
#endif
}

static void install_fallbacks(void)
{
    for (int i = 0; i < UI_FONT_ROLE_MAX; i++) {
        if (s_fonts[i] == NULL) {
            continue;
        }
        /* The FreeType fonts are heap objects owned by the adapter; LVGL has no
         * lv_font_set_fallback() in 9.5, only this public field. */
        lv_font_t *font = (lv_font_t *)s_fonts[i];
        font->fallback = role_symbol_fallback((ui_font_role_t)i);
    }
}

#endif /* CONFIG_TODOHELPER_FONT_FREETYPE */

/* ------------------------------------------------------------------------- */
/* Public                                                                    */
/* ------------------------------------------------------------------------- */

void ui_fonts_init(void)
{
    if (s_initialized) {
        return;
    }
    s_initialized = true;

    memset(s_fonts, 0, sizeof(s_fonts));

#if CONFIG_TODOHELPER_FONT_FREETYPE
    /* Single FreeType path: the embedded subset is materialised on storage and
     * loaded from there, because LVGL v9's FreeType integration has no
     * memory-buffer mode. If anything about that fails, the built-in CJK font
     * below still guarantees Chinese glyphs. */
    if (ft_load_from_file()) {
        s_using_freetype = true;
    } else {
        memset(s_fonts, 0, sizeof(s_fonts));
        SYS_LOGW(TAG, "FreeType unavailable, falling back to the built-in CJK font "
                      "(glyph coverage is limited, expect boxes for rarer hanzi)");
    }
#else
    SYS_LOGI(TAG, "FreeType disabled, using built-in fonts");
#endif

#if CONFIG_TODOHELPER_FONT_FREETYPE
    if (s_using_freetype) {
        install_fallbacks();
    }
#endif

    for (int i = 0; i < UI_FONT_ROLE_MAX; i++) {
        if (s_fonts[i] == NULL) {
            s_fonts[i] = builtin_font((ui_font_role_t)i);
        }
    }
}

const lv_font_t *ui_fonts_get(ui_font_role_t role)
{
    if (role >= UI_FONT_ROLE_MAX) {
        role = UI_FONT_BODY;
    }
    return s_fonts[role];
}

void ui_fonts_reload(void)
{
    if (!s_initialized) {
        return;
    }

#if CONFIG_TODOHELPER_FONT_FREETYPE
    /* Release the FreeType handles so a replaced file can be re-opened. The
     * adapter owns the underlying objects and drops them from its list here. */
    for (int i = 0; i < UI_FONT_ROLE_MAX; i++) {
        if (s_ft_handles[i] != NULL) {
            (void)esp_lv_adapter_ft_font_deinit(s_ft_handles[i]);
            s_ft_handles[i] = NULL;
        }
    }
#endif

    memset(s_fonts, 0, sizeof(s_fonts));
    s_initialized = false;
    ui_fonts_init();

    SYS_LOGI(TAG, "fonts reloaded (%s)", s_using_freetype ? "FreeType" : "built-in");
}
