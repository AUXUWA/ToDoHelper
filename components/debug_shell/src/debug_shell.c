/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file debug_shell.c
 * @brief Implementation of the on-device debug shell. See debug_shell.h for why.
 *
 * ## Design
 *
 * The shell is a plain text-in / text-out command interpreter:
 *
 *   POST /api/v1/debug/exec   {"cmd": "widgets /api/v1/"}   -> {"code":0,"data":{"out":"..."}}
 *   GET  /api/v1/debug/exec?cmd=heap
 *
 * A GET form exists because it makes `curl "http://dev/api/v1/debug/exec?cmd=heap"`
 * a one-liner, which is how this is actually used from a script. Output is
 * accumulated into a growable buffer and returned as **plain text lines** rather
 * than JSON, so the caller does not have to unquote a blob.
 *
 * ## Threading
 *
 * Two kinds of command:
 *
 *  - pure data (`heap`, `tasks`, `routes`, `cfg`, ...) run on the **httpd task**
 *    and only read;
 *  - UI commands (`ui`, `widgets`, `click`, `page`, `text`, `idle`, `bright`)
 *    touch LVGL and therefore **must** run on the LVGL task with the adapter
 *    lock held (AGENT.md 9.0.1). They are marshalled through
 *    ui_run_async() + a semaphore, so the handler waits for the LVGL task to do
 *    the work and then replies with the captured text.
 *
 * The marshal-and-wait pattern is the important part: a debug tool that calls
 * LVGL from the httpd task would corrupt the object tree (or trip the assertion
 * in lv_obj_has_class), and one that runs asynchronously would reply before the
 * result exists. Waiting on the LVGL task is safe here because that task never
 * blocks on the httpd task.
 */

#include "debug_shell.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ai_service.h"
#include "http_router.h"
#include "sys_keys.h"
#include "sys_status.h"
#include "sys_utils.h"
#include "ui_core.h"
#include "ui_font_mem.h"
#include "voice_service.h"

/* lv_refr_now(): refresh the screen synchronously, so a redraw can be timed. */
#include "core/lv_refr.h"

#include "storage_manager.h"
#include "th_bsp.h"

#if CONFIG_TODOHELPER_DEBUG_SHELL

static const char *TAG = "dbg_shell";

/* ------------------------------------------------------------------------ */
/* Output buffer                                                             */
/* ------------------------------------------------------------------------ */

/**
 * Growable text sink.
 *
 * Commands can print a lot (a 200-line widget dump), so this starts at 1 KB and
 * doubles. Allocation prefers PSRAM: this is bulk device data, and the internal
 * pool is the one resource this board never has (AGENT.md 9.0.5).
 */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool oom;
} dbg_out_t;

#define DBG_OUT_MAX (64 * 1024)

static void out_reserve(dbg_out_t *out, size_t extra)
{
    if (out->oom || out->len + extra + 1 <= out->cap) {
        return;
    }
    size_t want = (out->cap == 0) ? 1024 : out->cap;
    while (want < out->len + extra + 1 && want < DBG_OUT_MAX) {
        want *= 2;
    }
    if (want > DBG_OUT_MAX) {
        want = DBG_OUT_MAX;
    }
    char *grown = heap_caps_realloc_prefer(out->buf, want, 2,
                                           MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (grown == NULL) {
        /* Keep whatever we already have rather than losing the whole reply. */
        out->oom = true;
        return;
    }
    out->buf = grown;
    out->cap = want;
}

/**
 * Append one formatted chunk to @p out.
 *
 * Uses a small stack buffer for the common case and falls back to a heap buffer
 * when the formatted text does not fit.
 *
 * The fallback is not hypothetical: `cmd_help` prints its whole table in a single
 * call, and with a fixed 512-byte `vsnprintf` buffer the reply was silently cut
 * off mid-line ("  http-auth " and then nothing). Silent truncation in a debug
 * tool is the worst failure mode - it looks like the command itself is broken -
 * so `vsnprintf`'s return value (the length it *would* have written) is used to
 * size a second attempt.
 */
static void out_printf(dbg_out_t *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char stack_buf[512];
    const int needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);
    if (needed <= 0) {
        return;
    }

    if ((size_t)needed < sizeof(stack_buf)) {
        out_reserve(out, (size_t)needed + 1);
        if (out->oom) {
            return;
        }
        memcpy(out->buf + out->len, stack_buf, (size_t)needed);
        out->len += (size_t)needed;
        out->buf[out->len] = '\0';
        return;
    }

    /* Long output: format again into a right-sized buffer. Capped so a runaway
     * format string cannot try to allocate megabytes. */
    const size_t want = (size_t)needed + 1;
    if (want > DBG_OUT_MAX) {
        out_printf(out, "(a single message wanted %u bytes; refusing)\n",
                   (unsigned)want);
        return;
    }
    char *big = heap_caps_malloc_prefer(want, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (big == NULL) {
        out->oom = true;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(big, want, fmt, ap);
    va_end(ap);

    out_reserve(out, want);
    if (!out->oom) {
        memcpy(out->buf + out->len, big, (size_t)needed);
        out->len += (size_t)needed;
        out->buf[out->len] = '\0';
    }
    heap_caps_free(big);
}

static void out_free(dbg_out_t *out)
{
    heap_caps_free(out->buf);
    out->buf = NULL;
    out->len = 0;
    out->cap = 0;
}

/* ------------------------------------------------------------------------ */
/* Marshalling UI work onto the LVGL task                                    */
/* ------------------------------------------------------------------------ */

/** Work item handed to the LVGL task. */
typedef struct {
    void (*fn)(void *arg);
    void *arg;
    SemaphoreHandle_t done;
} ui_job_t;

static void ui_job_trampoline(void *arg)
{
    ui_job_t *job = (ui_job_t *)arg;
    /* Runs in the LVGL task with the adapter lock already held by ui_core. */
    if (job->fn != NULL) {
        job->fn(job->arg);
    }
    xSemaphoreGive(job->done);
}

/**
 * Run @p fn on the LVGL task and wait for it.
 *
 * Why a semaphore rather than `ui_lock()` directly: taking the adapter lock from
 * the httpd task would *work*, but it is the pattern AGENT.md 9.0.1.1 warns about
 * - the lock is held across the whole render, so the caller can be parked for a
 * frame. Marshalling to the owning task is both correct by construction and
 * faster, because it cannot collide with the render loop.
 */
static bool run_on_lvgl(void (*fn)(void *arg), void *arg, uint32_t timeout_ms)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == NULL) {
        return false;
    }
    ui_job_t job = { .fn = fn, .arg = arg, .done = done };

    if (ui_run_async(ui_job_trampoline, &job) != ESP_OK) {
        vSemaphoreDelete(done);
        return false;
    }

    const bool ok = (xSemaphoreTake(done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
    vSemaphoreDelete(done);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* UI helpers                                                                */
/* ------------------------------------------------------------------------ */

/**
 * Class-name lookup.
 *
 * LVGL keeps the human-readable class name in `lv_obj_class_private.h`, which is
 * only compiled in with LV_USE_PRIVATE_API (off here). So instead of reading the
 * name, the handful of classes this project actually builds are mapped to
 * strings by comparing against their public class symbols. Anything unknown
 * prints as "obj", which is honest and still enough to read the tree shape.
 */
typedef struct {
    const lv_obj_class_t *class_p;
    const char *name;
} class_name_t;

static const char *class_name_of(const lv_obj_t *obj)
{
    static const class_name_t known[] = {
        { &lv_obj_class, "obj" },
        { &lv_label_class, "label" },
        { &lv_button_class, "button" },
        { &lv_slider_class, "slider" },
        { &lv_msgbox_class, "msgbox" },
        { &lv_image_class, "image" },
        { &lv_bar_class, "bar" },
        { &lv_switch_class, "switch" },
        { &lv_checkbox_class, "checkbox" },
        { &lv_keyboard_class, "keyboard" },
        { &lv_textarea_class, "textarea" },
        { &lv_dropdown_class, "dropdown" },
        { &lv_roller_class, "roller" },
        { &lv_table_class, "table" },
        { &lv_chart_class, "chart" },
        { &lv_spinner_class, "spinner" },
        { &lv_arc_class, "arc" },
        { &lv_line_class, "line" },
    };

    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (lv_obj_check_type(obj, known[i].class_p)) {
            return known[i].name;
        }
    }
    return "obj";
}

/** First line of a label's text, clipped. Labels are the useful payload. */
static void label_excerpt(const lv_obj_t *obj, char *out, size_t len)
{
    out[0] = '\0';
    if (!lv_obj_check_type(obj, &lv_label_class)) {
        return;
    }
    const char *text = lv_label_get_text(obj);
    if (text == NULL || text[0] == '\0') {
        return;
    }
    size_t i = 0;
    for (; text[i] != '\0' && text[i] != '\n' && i + 1 < len; i++) {
        out[i] = text[i];
    }
    out[i] = '\0';
}

/* ------------------------------------------------------------------------ */
/* Command context                                                           */
/* ------------------------------------------------------------------------ */

typedef struct {
    dbg_out_t *out;
    /* Parsed argv, so the UI helpers can run on the LVGL task with their args. */
    int argc;
    char **argv;
} dbg_ctx_t;

/* ------------------------------------------------------------------------ */
/* Commands: inspection                                                      */
/* ------------------------------------------------------------------------ */

static void cmd_help(dbg_out_t *o)
{
    out_printf(o,
        "commands:\n"
        "  help                     this text\n"
        "  version                  firmware/app descriptor\n"
        "  status                   sys_status snapshot\n"
        "  heap                     internal/PSRAM + lowest-water marks\n"
        "  tasks                    FreeRTOS tasks (name/state/prio/stack hwm)\n"
        "  routes                   registered HTTP routes\n"
        "  pages                    page registry + current page\n"
        "  cfg [get|set] ...        NVS config (get <key> / set <key> <value>)\n"
        "  nvs <erase-key>          erase one NVS key\n"
        "  http-auth [set <hex>]    read/replace the HTTP auth token (debug only)\n"
        "  time                     wall clock / uptime\n"
        "  log <tag> [level]        set a tag's log level (0-5)\n"
        "  storage                  storage backend + free space\n"
        "  ftstats                  glyph cache (one cache shared by all sizes)\n"
        "  ui                       current page + top-layer overview\n"
        "  widgets [filter] [depth] dump the object tree under the screen\n"
        "  click <target>           press a widget (by label text or index)\n"
        "  text <target> <value>    set a label's text\n"
        "  slider <target> <value>  move a slider (fires VALUE_CHANGED, like a tap)\n"
        "  scroll <target> <dy>     scroll a scrollable object by dy pixels\n"
        "  bench <target> <text>    time a redraw (index, repeats optional)\n"
        "  bench-screen [repeats]  time full-screen redraws (one scroll frame)\n"
        "  speak <text>           speak a line through the TTS endpoint\n"
        "  page <id|name>           switch page (0..10 or splash/home/...)\n"
        "  tap <x> <y>              report which widget is at a screen point\n"
        "  idle <enter|exit>        force idle-face enter/leave\n"
        "  idle-timeout [secs]      get/set idle timeout, 0 = never idle\n"
        "  bright <0-100>           set backlight\n"
        "  reboot                   restart the device\n");
}

static void cmd_version(dbg_out_t *o)
{
    const esp_app_desc_t *app = esp_app_get_description();
    if (app == NULL) {
        out_printf(o, "no app descriptor\n");
        return;
    }
    /* The version string is the fastest way to tell which firmware is actually
     * flashed (AGENT.md 8.1), which is why it is the first thing printed. */
    out_printf(o, "project   %s\n", app->project_name);
    out_printf(o, "version   %s\n", app->version);
    out_printf(o, "idf       %s\n", app->idf_ver);
    out_printf(o, "built     %s %s\n", app->date, app->time);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    out_printf(o, "chip      %d core(s), rev %d.%d\n", chip.cores,
               chip.revision / 100, chip.revision % 100);
}

static void cmd_status(dbg_out_t *o)
{
    sys_status_t st;
    if (sys_status_get(&st) != ESP_OK) {
        out_printf(o, "sys_status_get failed\n");
        return;
    }
    out_printf(o, "wifi      %s%s (%s, %d dBm)\n",
               st.wifi_connected ? "connected" : "offline",
               st.wifi_connecting ? " [connecting]" : "",
               st.wifi_ssid[0] ? st.wifi_ssid : "-", (int)st.wifi_rssi);
    out_printf(o, "ip        %s (online=%d)\n", st.wifi_ip[0] ? st.wifi_ip : "-",
               (int)st.online);
    out_printf(o, "time      synced=%d ts=%lld\n", (int)st.time_synced,
               (long long)st.now_ts);
    out_printf(o, "todos     total=%d pending=%d syncing=%d\n",
               (int)st.todo_total, (int)st.todo_pending, (int)st.syncing);
    out_printf(o, "storage   available=%d sd=%d\n",
               (int)st.storage_available, (int)st.sd_available);
}

static void cmd_heap(dbg_out_t *o)
{
    const size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t int_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    out_printf(o, "internal free    %8u B  (%u KB)\n",
               (unsigned)int_free, (unsigned)(int_free / 1024));
    out_printf(o, "internal largest %8u B  (%u KB)  <- the number that matters\n",
               (unsigned)sys_heap_largest_internal(),
               (unsigned)(sys_heap_largest_internal() / 1024));
    out_printf(o, "internal int-low %8u B  (%u KB)  <- lowest since boot\n",
               (unsigned)int_min, (unsigned)(int_min / 1024));
    out_printf(o, "psram free       %8u B  (%u KB)\n",
               (unsigned)psram_free, (unsigned)(psram_free / 1024));
    out_printf(o, "default free     %8u B\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
}

/*
 * Glyph cache status.
 *
 * The cache is **shared by all font sizes** - LVGL keys the cache node on
 * (file, style, render mode), not on the size - so every row below shows the
 * same numbers. (The four rows are the four font roles; they all point at the
 * one shared cache.)
 *
 * The bitmap cache is what makes scrolling smooth: a glyph that is not cached
 * has to be rasterised again (read the TTF from the SD card + render), which
 * is about 10 ms. The bitmaps and the cache metadata both live in PSRAM now,
 * so the limit decides how much PSRAM they use, not internal RAM.
 */
static void cmd_ftstats(dbg_out_t *o)
{
    ui_font_cache_stat_t stats[UI_FONT_MEM_MAX_FONTS];
    const uint32_t count = ui_font_mem_stats(stats, UI_FONT_MEM_MAX_FONTS);

    out_printf(o, "glyph cache limit now %u (shared by all sizes)\n", (unsigned)ui_font_mem_cache_cnt());
    if (count == 0) {
        out_printf(o, "no FreeType font in use\n");
        return;
    }

    out_printf(o, "  size  glyphs  bitmaps\n");
    for (uint32_t i = 0; i < count; i++) {
        out_printf(o, "  %4u  %6u  %7u\n",
                   (unsigned)stats[i].size,
                   (unsigned)stats[i].glyph_entries,
                   (unsigned)stats[i].bitmap_entries);
    }
}

/**
 * Audio hardware probe.
 *
 * The microphone and the speaker were never verified on this board (the
 * codec is brought up lazily, so the boot log says nothing about it). This
 * command answers the two questions that matter:
 *   1. does the codec come up at all?
 *   2. does the microphone deliver samples, or is it mute?
 *
 * It runs the capture in voice_service's own task, so the UI is not blocked.
 */
static void cmd_audio(dbg_out_t *o, const char *arg)
{
    const esp_err_t err = voice_service_ensure_audio();
    out_printf(o, "audio init: %s (mic=%d speaker=%d)\n",
               esp_err_to_name(err),
               voice_service_microphone_available() ? 1 : 0,
               voice_service_speaker_available() ? 1 : 0);
    out_printf(o, "internal free %u B, largest %u B\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)sys_heap_largest_internal());

    if (arg == NULL) {
        return;
    }

    const int ms = atoi(arg);
    if (ms < 200 || ms > 5000) {
        out_printf(o, "usage: audio [record_ms]  (200..5000)\n");
        return;
    }

    const esp_err_t rec = voice_service_start_record();
    out_printf(o, "record %d ms -> %s\n", ms, esp_err_to_name(rec));
    if (rec != ESP_OK) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS((uint32_t)ms));
    (void)voice_service_stop_record();

    /* Wait for the capture task to finish and publish the result.
     * The wait is bounded: the capture is over by then (it is capped at
     * VOICE_MAX_RECORD_MS), and a device that never finishes must not keep
     * the HTTP server busy. */
    for (int i = 0; i < 20 && voice_service_is_recording(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    const int32_t peak = voice_service_last_peak();
    const uint32_t captured_ms = voice_service_last_duration_ms();
    out_printf(o, "captured %u ms, peak %d / 32767%s\n", (unsigned)captured_ms, (int)peak,
               (captured_ms == 0) ? "  <- the codec delivered no samples at all"
                                   : ((peak == 0) ? "  <- silence: the microphone delivers nothing"
                                                   : ""));
    out_printf(o, "internal free %u B, largest %u B\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)sys_heap_largest_internal());
}

/**
 * Speak a piece of text through the configured TTS endpoint.
 *
 * The AI page's "read it out" button needs an answer to read, which needs an
 * API key. This command tests the other half of the path (TTS -> codec ->
 * speaker) on its own.
 */
static void cmd_speak(dbg_out_t *o, const char *text)
{
    if (text == NULL || text[0] == 0) {
        out_printf(o, "usage: speak <text>\n");
        return;
    }
    if (!voice_service_tts_configured()) {
        out_printf(o, "no TTS endpoint configured (POST /api/v1/voice/config)\n");
        return;
    }
    const esp_err_t err = voice_service_speak_async(text);
    out_printf(o, "speak -> %s (the result arrives as an event)\n",
               esp_err_to_name(err));
}

static void cmd_tasks(dbg_out_t *o)
{
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    const UBaseType_t count = uxTaskGetNumberOfTasks();
    TaskStatus_t *list = heap_caps_malloc_prefer(count * sizeof(TaskStatus_t), 2,
                                                 MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (list == NULL) {
        out_printf(o, "no memory for %u task records\n", (unsigned)count);
        return;
    }

    const UBaseType_t got = uxTaskGetSystemState(list, count, NULL);
    out_printf(o, "%-16s %-4s %-10s %s\n", "name", "pri", "stack-hwm", "state");
    for (UBaseType_t i = 0; i < got; i++) {
        /*
         * Report the stack high-water mark (free bytes left at the worst point)
         * rather than the nominal stack size: the headroom is what predicts a
         * stack overflow, and this project has been bitten by exactly that
         * (AGENT.md 9.0.1 - the 2.8 KB sys_evt stack).
         *
         * Note: xCoreID lives inside a union in TaskStatus_t and is not available
         * in every FreeRTOS configuration, so only portable fields are printed.
         */
        static const char *const states[] = {
            "running", "ready", "blocked", "suspended", "deleted", "invalid",
        };
        const unsigned st = (unsigned)list[i].eCurrentState;
        out_printf(o, "%-16s %-4u %-10u %s\n",
                   list[i].pcTaskName,
                   (unsigned)list[i].uxCurrentPriority,
                   (unsigned)list[i].usStackHighWaterMark,
                   (st < sizeof(states) / sizeof(states[0])) ? states[st] : "?");
    }
    heap_caps_free(list);
    out_printf(o, "%u task(s); stack-hwm = LOWEST free stack bytes ever seen\n",
               (unsigned)got);
#else
    /*
     * Detailed task enumeration lives behind CONFIG_FREERTOS_USE_TRACE_FACILITY,
     * which is off in this build (it costs RAM per task, and this board's
     * internal pool is the scarcest resource). Rather than link a symbol that is
     * not compiled in, report what is always available and say how to get more.
     *
     * The stack-overflow diagnosis this command exists for is still possible:
     * `heap`'s int-low plus the per-module heap log pinpoints the same bugs.
     */
    out_printf(o, "%u task(s) running\n", (unsigned)uxTaskGetNumberOfTasks());
    out_printf(o, "(detailed per-task stacks need CONFIG_FREERTOS_USE_TRACE_FACILITY=y;\n"
                 " it is off to save RAM - see components/debug_shell/README.md)\n");
#endif
}

static void cmd_routes(dbg_out_t *o)
{
    out_printf(o, "%u route(s) registered\n", (unsigned)http_router_count());
}

static void cmd_time(dbg_out_t *o)
{
    char tz[48];
    (void)sys_config_get_str_or(SYS_KEY_TZ, tz, sizeof(tz), "(unset)");

    out_printf(o, "uptime    %lld s\n", (long long)(esp_timer_get_time() / 1000000));
    out_printf(o, "wall ts   %lld\n", (long long)sys_time_ts());
    out_printf(o, "timezone  %s\n", tz);
}

static void cmd_storage(dbg_out_t *o)
{
    out_printf(o, "root      %s\n", storage_root() ? storage_root() : "(none)");
    out_printf(o, "available %d\n", (int)storage_available());
    out_printf(o, "sd        %d\n", (int)storage_sd_available());
    out_printf(o, "app dir   %s\n", storage_app_dir() ? storage_app_dir() : "(none)");
    out_printf(o, "free      %llu KB\n",
               (unsigned long long)(storage_free_bytes() / 1024));
}

/* ------------------------------------------------------------------------ */
/* Commands: UI (these run on the LVGL task)                                 */
/* ------------------------------------------------------------------------ */

typedef struct {
    dbg_out_t *out;
    int max_depth;
    const char *filter;
} dump_args_t;

static void dump_obj(dbg_out_t *o, lv_obj_t *obj, int depth, int max_depth,
                     const char *filter, int *shown)
{
    if (obj == NULL || (max_depth >= 0 && depth > max_depth) || *shown > 400) {
        return;
    }

    char text[80];
    label_excerpt(obj, text, sizeof(text));

    lv_area_t area = { 0 };
    lv_obj_get_coords(obj, &area);
    const int32_t w = lv_area_get_width(&area);
    const int32_t h = lv_area_get_height(&area);

    const char *cls = class_name_of(obj);
    const bool hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    const bool clickable = lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    /* Filter matches the class name or the label text, so
     * `widgets token` finds the token label and `widgets button` finds buttons. */
    bool keep = true;
    if (filter != NULL && filter[0] != '\0') {
        keep = (strstr(cls, filter) != NULL) ||
               (text[0] != '\0' && strstr(text, filter) != NULL);
    }

    if (keep) {
        char indent[40];
        int n = (depth < 12) ? depth : 12;
        for (int i = 0; i < n; i++) {
            indent[i * 2] = ' ';
            indent[i * 2 + 1] = ' ';
        }
        indent[n * 2] = '\0';

        out_printf(o, "%s%-8s %4ld,%-4ld %4ldx%-4ld%s%s%s%s\n",
                   indent, cls, (long)area.x1, (long)area.y1, (long)w, (long)h,
                   clickable ? " [click]" : "",
                   hidden ? " [hidden]" : "",
                   text[0] ? " " : "",
                   text);
        (*shown)++;
    }

    const uint32_t children = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < children; i++) {
        dump_obj(o, lv_obj_get_child(obj, i), depth + 1, max_depth, filter, shown);
    }
}

static void ui_dump_cb(void *arg)
{
    dump_args_t *a = (dump_args_t *)arg;
    dbg_out_t *o = a->out;

    const ui_page_id_t current = ui_page_current();
    const ui_page_t *page = ui_page_get(current);
    out_printf(o, "current page %d (%s)\n", (int)current,
               (page != NULL && page->name != NULL) ? page->name : "?");

    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL) {
        out_printf(o, "no active screen\n");
        return;
    }
    out_printf(o, "-- screen tree --\n");
    int shown = 0;
    dump_obj(o, screen, 0, a->max_depth, a->filter, &shown);

    /* The top layer is where dialogs and toasts live, and a stuck dialog is
     * exactly the class of bug this shell was written to diagnose. */
    lv_obj_t *top = lv_layer_top();
    if (top != NULL && lv_obj_get_child_count(top) > 0) {
        out_printf(o, "-- top layer (dialogs/toasts) --\n");
        dump_obj(o, top, 0, a->max_depth, a->filter, &shown);
    }
    out_printf(o, "%d object(s) shown\n", shown);
}

/* -- target resolution ---------------------------------------------------- */

/**
 * Find the nth (0-based) object whose class name or label text matches
 * @p target, walking the screen then the top layer.
 *
 * Matching on the visible label is what makes this usable from a script: the
 * caller writes `click 知道了` or `click token` instead of hardcoding a pointer,
 * and it reads like the thing on the screen.
 */
typedef struct {
    const char *target;
    int want_index;
    int seen;
    lv_obj_t *found;
    int max_depth;
} find_args_t;

static void find_walk(find_args_t *f, lv_obj_t *obj, int depth)
{
    if (obj == NULL || f->found != NULL || (f->max_depth >= 0 && depth > f->max_depth)) {
        return;
    }

    char text[80];
    label_excerpt(obj, text, sizeof(text));
    const char *cls = class_name_of(obj);

    const bool match = (strcmp(cls, f->target) == 0) ||
                       (text[0] != '\0' && strstr(text, f->target) != NULL);
    if (match) {
        if (f->seen == f->want_index) {
            f->found = obj;
            return;
        }
        f->seen++;
    }

    const uint32_t children = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < children && f->found == NULL; i++) {
        find_walk(f, lv_obj_get_child(obj, i), depth + 1);
    }
}

static lv_obj_t *find_target(const char *target, int index, int *out_count)
{
    find_args_t f = { .target = target, .want_index = index, .max_depth = 24 };
    lv_obj_t *screen = lv_screen_active();
    if (screen != NULL) {
        find_walk(&f, screen, 0);
    }
    if (f.found == NULL) {
        /* Dialogs and toasts are siblings of the screen, under the top layer -
         * search it second so `click 知道了` finds the dialog's button. */
        lv_obj_t *top = lv_layer_top();
        if (top != NULL) {
            find_walk(&f, top, 0);
        }
    }
    if (out_count != NULL) {
        *out_count = f.seen + (f.found != NULL ? 1 : 0);
    }
    return f.found;
}

typedef struct {
    dbg_out_t *out;
    const char *target;
    int index;
    bool do_text;
    bool do_slider;
    bool do_scroll;
    bool do_bench;
    bool do_bench_screen;
    int repeats;
    const char *value;
} click_args_t;

static void ui_click_cb(void *arg)
{
    click_args_t *c = (click_args_t *)arg;
    int count = 0;
    lv_obj_t *obj = (c->do_bench_screen) ? NULL : find_target(c->target, c->index, &count);
    if (c->do_bench_screen) {
        /*
         * Time a full-screen redraw.
         *
         * This is what one frame of a scrolling list costs: everything visible
         * is drawn again, and every character that is not in the FreeType
         * cache has to be rasterised again (read the TTF from the SD card and
         * render - about 9 ms each). If the cache is too small to hold what a
         * screen shows, this number stays high forever; if it is big enough,
         * the first redraw is slow and the rest are fast.
         */
        lv_display_t *disp = lv_display_get_default();
        const int repeats = c->repeats > 0 ? c->repeats : 1;
        for (int i = 0; i < repeats; i++) {
            const int64_t t0 = esp_timer_get_time();
            lv_obj_invalidate(lv_screen_active());
            if (disp != NULL) {
                lv_refr_now(disp);
            }
            const int64_t dt = esp_timer_get_time() - t0;
            out_printf(c->out, "bench %d: %lld us\n", i + 1, (long long)dt);
        }
        return;
    }
    if (obj == NULL) {
        out_printf(c->out, "no widget matching '%s' (index %d); %d match(es) total\n",
                   c->target, c->index, count);
        return;
    }
    if (c->do_bench) {
        /*
         * Time a real redraw.
         *
         * Setting a label's text only invalidates it; the glyphs are rasterised
         * later, during the next refresh - so the HTTP round trip of the plain
         * `text` command says nothing about how long the drawing took. Here the
         * screen is refreshed synchronously (lv_refr_now), so the measured time
         * includes the glyph rendering - which is exactly what a scrolling list
         * pays for every character that is not in the FreeType cache.
         */
        if (!lv_obj_check_type(obj, &lv_label_class)) {
            out_printf(c->out, "'%s' is a %s, not a label\n", c->target,
                       class_name_of(obj));
            return;
        }
        lv_display_t *disp = lv_display_get_default();
        const int repeats = c->repeats > 0 ? c->repeats : 1;
        for (int i = 0; i < repeats; i++) {
            const int64_t t0 = esp_timer_get_time();
            lv_label_set_text(obj, c->value);
            if (disp != NULL) {
                lv_refr_now(disp);
            }
            const int64_t dt = esp_timer_get_time() - t0;
            out_printf(c->out, "bench %d: %lld us\n", i + 1, (long long)dt);
        }
        return;
    }
    if (c->do_text) {
        if (!lv_obj_check_type(obj, &lv_label_class)) {
            out_printf(c->out, "'%s' is a %s, not a label\n", c->target,
                       class_name_of(obj));
            return;
        }
        lv_label_set_text(obj, c->value);
        out_printf(c->out, "set %s text to '%s'\n", class_name_of(obj), c->value);
        return;
    }

    if (c->do_scroll) {
        /*
         * Scroll a scrollable widget by a number of pixels.
         *
         * The windowed lists (the WiFi scan list, the todo list) only build the
         * rows inside the viewport; the rows outside are represented by two gap
         * objects. Whether the window actually follows the scroll is the one
         * thing a dump of the object tree cannot show - and it is exactly what a
         * regression would look like ("the list shows the same 8 rows no matter
         * how far you scroll").
         *
         * So: scroll it, and then look at the tree again.
         */
        if (!lv_obj_check_type(obj, &lv_obj_class)) {
            out_printf(c->out, "'%s' is a %s, not an object\n", c->target,
                       class_name_of(obj));
            return;
        }
        const int32_t dy = (int32_t)atoi(c->value);
        /* _bounded, not the raw one: a real finger gesture is clamped to the
         * content (lv_indev_scroll_handler -> scroll_limit_diff), and an
         * unbounded scroll would happily move past the end and then look like
         * "the list can scroll into nowhere". */
        lv_obj_scroll_by_bounded(obj, 0, dy, LV_ANIM_OFF);
        out_printf(c->out, "scrolled %s by %d -> y=%d (scrollable=%d)\n", c->target,
                   (int)dy, (int)lv_obj_get_scroll_y(obj),
                   (int)lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE));
        return;
    }

    if (c->do_slider) {
        /*
         * Move a slider without a finger.
         *
         * `click` cannot do it: an LVGL slider changes its value in its press
         * handler, which reads the pointer position from the input device -
         * a synthesised event carries no coordinates, so the value stays put.
         * That makes the settings page (brightness / idle brightness / idle
         * timeout) the one widget family the shell cannot exercise, and those
         * sliders are the ones with a **debounced NVS write** behind them
         * (roadmap v1.8.0 E2) - exactly the path that has to be verified
         * without a human dragging a finger.
         *
         * lv_slider_set_value() fires LV_EVENT_VALUE_CHANGED, which is what the
         * page's callback is registered for, so the whole chain (callback ->
         * debounce timer -> one-shot task -> NVS) runs as it would on a touch.
         */
        if (!lv_obj_check_type(obj, &lv_slider_class)) {
            out_printf(c->out, "'%s' is a %s, not a slider\n", c->target,
                       class_name_of(obj));
            return;
        }
        const int32_t value = (int32_t)atoi(c->value);
        lv_slider_set_value(obj, value, LV_ANIM_OFF);
        /*
         * LVGL 9.5: lv_slider_set_value() does **not** send LV_EVENT_VALUE_CHANGED -
         * only the press path does (lv_slider.c, the helper that computes the
         * value from the pointer). So the event has to be sent by hand,
         * otherwise the page's callback never runs and the shell would look
         * like it moved a slider that did nothing.
         */
        lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
        out_printf(c->out, "set slider to %d (now %d)\n", (int)value,
                   (int)lv_slider_get_value(obj));
        return;
    }

    /*
     * Walk up to the nearest clickable ancestor, exactly as a real tap does.
     *
     * The first version sent the events to whatever matched - and since a
     * button's text lives in a child *label*, `click 生成并开启` matched the
     * label, fired events on a non-clickable object, and silently did nothing
     * (the dialog never opened). LVGL routes a real touch to the nearest
     * CLICKABLE ancestor, so the shell has to do the same or it cannot reproduce
     * what a finger does.
     */
    lv_obj_t *clickable = obj;
    while (clickable != NULL && !lv_obj_has_flag(clickable, LV_OBJ_FLAG_CLICKABLE)) {
        clickable = lv_obj_get_parent(clickable);
    }
    if (clickable == NULL) {
        out_printf(c->out, "matched %s '%s' but no clickable ancestor; "
                           "use 'text' to set text instead\n",
                   class_name_of(obj), c->target);
        return;
    }

    /* Send the same events a finger produces. CLICKED alone is what the page
     * callbacks are registered for, but sending PRESSED/RELEASED as well means
     * widgets that only act on those (sliders, buttons with pressed styling)
     * behave the same as under a real tap. */
    lv_obj_send_event(clickable, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(clickable, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(clickable, LV_EVENT_CLICKED, NULL);

    /*
     * Report the top layer *synchronously*, in the same LVGL-task invocation.
     *
     * This is deliberate: a dialog created by the handler must still be there a
     * microsecond later, so if the count is unchanged the handler did not create
     * one (or the shell clicked the wrong object). Checking from a *later* HTTP
     * request cannot tell those apart from "the dialog was already dismissed" -
     * which is exactly the ambiguity that made the v1.3.1 dialog unverifiable
     * without a human looking at the screen.
     */
    lv_obj_t *top = lv_layer_top();
    const uint32_t top_children = (top != NULL) ? lv_obj_get_child_count(top) : 0;
    uint32_t dialogs = 0;
    for (uint32_t i = 0; i < top_children; i++) {
        if (lv_obj_check_type(lv_obj_get_child(top, i), &lv_msgbox_class)) {
            dialogs++;
        }
    }

    out_printf(c->out, "clicked %s (matched %s '%s')\n",
               class_name_of(clickable),
               (clickable != obj) ? class_name_of(obj) : "itself", c->target);
    out_printf(c->out, "top layer now: %u child(ren), %u msgbox(es)%s\n",
               (unsigned)top_children, (unsigned)dialogs,
               (dialogs > 0) ? "  <- dialog is OPEN" : "");

    /*
     * Dump the dialog we just opened, in this same call.
     *
     * This is what makes a UI change verifiable without a human at the board: a
     * second HTTP request cannot re-observe the dialog reliably (the handler may
     * have rotated the auth token, and the client would have to race to re-read
     * it before anything else changed the screen). Reporting geometry here means
     * one request proves "the click opened a dialog AND the dialog is laid out
     * like this".
     */
    for (uint32_t i = 0; i < top_children; i++) {
        lv_obj_t *child = lv_obj_get_child(top, i);
        if (lv_obj_check_type(child, &lv_msgbox_class)) {
            /*
             * Force a layout pass before dumping.
             *
             * A widget created during this very call has not been laid out yet, so
             * every coordinate would read 0x0 - which says nothing about the
             * dialog's actual size and defeats the point of reporting geometry
             * here. lv_obj_update_layout() resolves sizes/positions synchronously,
             * so one request can prove "the click opened a dialog AND the dialog
             * is laid out like this".
             */
            lv_obj_update_layout(child);

            out_printf(c->out, "--- dialog tree (from the click) ---\n");
            int shown = 0;
            dump_obj(c->out, child, 0, -1, NULL, &shown);
            break;
        }
    }
}

typedef struct {
    dbg_out_t *out;
    ui_page_id_t id;
} page_args_t;

static void ui_page_cb(void *arg)
{
    page_args_t *p = (page_args_t *)arg;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = ui_page_switch(p->id);
    const int64_t t_switch = esp_timer_get_time() - t0;
    const ui_page_id_t now = ui_page_current();
    const ui_page_t *page = ui_page_get(now);
    out_printf(p->out, "switch -> %s (%d), now on %d (%s) in %lld us\n",
               esp_err_to_name(err), (int)p->id, (int)now,
               (page != NULL && page->name != NULL) ? page->name : "?",
               (long long)t_switch);

    /*
     * Re-read the page a little later, still inside the LVGL task.
     *
     * Reporting only the immediate value cannot distinguish "the switch did not
     * take" from "something switched away again right after" - and that
     * distinction matters: the first would be a bug in the switch, the second in
     * whatever is overriding it. Sleeping here is safe because this runs on the
     * LVGL task and the delay is bounded (the caller waits on a semaphore with a
     * 5 s timeout).
     *
     * NOTE: lv_timer_handler() must NOT be used to force a redraw here - this
     * callback already runs inside one (the shell's UI work is dispatched from
     * a lv_timer callback), so the call returns immediately on LVGL's
     * already_running guard and measures nothing. lv_refr_now() is the way to
     * redraw synchronously.
     */
    lv_display_t *disp = lv_display_get_default();
    const int64_t t_redraw0 = esp_timer_get_time();
    if (disp != NULL) {
        lv_refr_now(disp);
    }
    const int64_t t_redraw = esp_timer_get_time() - t_redraw0;
    out_printf(p->out, "first redraw took %lld us\n", (long long)t_redraw);
    vTaskDelay(pdMS_TO_TICKS(300));
    /* After the delay the timers are pumped by the adapter's own task, so
     * there is nothing to do here. */

    const ui_page_id_t later = ui_page_current();
    if (later != p->id) {
        const ui_page_t *lp = ui_page_get(later);
        out_printf(p->out,
                   "WARNING: 300 ms later the current page is %d (%s), not %d - "
                   "something switched it back\n",
                   (int)later, (lp != NULL && lp->name != NULL) ? lp->name : "?",
                   (int)p->id);
    } else {
        out_printf(p->out, "still on %d (%s) after 300 ms\n", (int)later,
                   (page != NULL && page->name != NULL) ? page->name : "?");
    }
}

typedef struct {
    dbg_out_t *out;
    int16_t x;
    int16_t y;
    /** true when the caller asked for the tap to actually be delivered
     *  (`tap <x> <y> click`), false to only report what is under the point. */
    bool click_it;
} tap_args_t;

static void ui_tap_cb(void *arg)
{
    tap_args_t *t = (tap_args_t *)arg;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *hit = NULL;

    /*
     * lv_indev_search_obj() is LVGL's own hit test (lv_obj_hit_test() only
     * answers "is this point inside this object", it does not search). Searching
     * the top layer first mirrors what a real touch does: the top layer is drawn
     * above the screen, which is exactly why a modal dialog swallows taps aimed
     * at the page underneath.
     */
    lv_point_t point = { .x = t->x, .y = t->y };
    lv_obj_t *top = lv_layer_top();
    if (top != NULL) {
        hit = lv_indev_search_obj(top, &point);
    }
    if (hit == NULL && screen != NULL) {
        hit = lv_indev_search_obj(screen, &point);
    }
    if (hit == NULL) {
        out_printf(t->out, "nothing at %d,%d\n", (int)t->x, (int)t->y);
        return;
    }

    char text[80];
    label_excerpt(hit, text, sizeof(text));
    lv_area_t area = { 0 };
    lv_obj_get_coords(hit, &area);

    /* Walk up to the nearest clickable ancestor: a real tap on a label inside a
     * button activates the button (LVGL routes the click to the CLICKABLE
     * object), which is how a user experiences the UI. */
    lv_obj_t *clickable = hit;
    while (clickable != NULL && !lv_obj_has_flag(clickable, LV_OBJ_FLAG_CLICKABLE)) {
        clickable = lv_obj_get_parent(clickable);
    }

    out_printf(t->out, "at %d,%d -> %s '%s' [%ld,%ld %ldx%ld]\n",
               (int)t->x, (int)t->y, class_name_of(hit), text,
               (long)area.x1, (long)area.y1,
               (long)lv_area_get_width(&area), (long)lv_area_get_height(&area));
    if (clickable != NULL && clickable != hit) {
        out_printf(t->out, "  nearest clickable ancestor: %s\n",
                   class_name_of(clickable));
    }

    /*
     * `tap x y click` also presses it, so a coordinate can be exercised without
     * first looking up what lives there. This is the only way to reach widgets
     * that have no text to match on - the dialog's "×" is an image, so
     * `click <label>` cannot find it and only a coordinate can.
     */
    if (t->click_it) {
        if (clickable == NULL) {
            out_printf(t->out, "  not clickable; nothing sent\n");
            return;
        }
        lv_obj_send_event(clickable, LV_EVENT_PRESSED, NULL);
        lv_obj_send_event(clickable, LV_EVENT_RELEASED, NULL);
        lv_obj_send_event(clickable, LV_EVENT_CLICKED, NULL);

        lv_obj_t *layer = lv_layer_top();
        const uint32_t n = (layer != NULL) ? lv_obj_get_child_count(layer) : 0;
        uint32_t boxes = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (lv_obj_check_type(lv_obj_get_child(layer, i), &lv_msgbox_class)) {
                boxes++;
            }
        }
        out_printf(t->out, "  clicked %s; top layer now %u child(ren), %u msgbox(es)\n",
                   class_name_of(clickable), (unsigned)n, (unsigned)boxes);
    }
}

typedef struct {
    dbg_out_t *out;
    bool enter;
} idle_args_t;

static void ui_idle_cb(void *arg)
{
    idle_args_t *a = (idle_args_t *)arg;
    /* idle_face is a peer module, not a dependency of this component, so this
     * only nudges through the public event bus - the same path a button takes. */
    const esp_err_t err = sys_events_post(a->enter ? SYS_EVENT_IDLE_TIMEOUT
                                                   : SYS_EVENT_WAKEUP, NULL, 0);
    out_printf(a->out, "idle %s -> %s\n", a->enter ? "enter" : "exit",
               esp_err_to_name(err));
}

/**
 * Get or set the idle timeout, and re-arm the watchdog so it takes effect now.
 *
 * Separate from `cfg set idle_timeout` on purpose: writing the NVS key alone
 * does nothing, because `th_bsp_idle_init()` is what reads it into the running
 * timeout and it only runs at boot. Without this, "raise the timeout so the page
 * stops reverting to home while I test it" is impossible - which is precisely
 * what made the dialog hard to inspect: each shell call takes seconds (it reads
 * NVS for the auth token), so a 30 s timeout kept sending the board back to home
 * between the click and the dump.
 */
static void cmd_idle_timeout(dbg_out_t *o, const char *value)
{
    if (value == NULL) {
        /* The BSP's own default is private to that component; 30 s is its value
         * (components/bsp/src/th_bsp_input.c) and is only used to label an unset
         * key, so it cannot drift into a behavioural difference. */
        const int32_t current = sys_config_get_i32_or(SYS_KEY_IDLE_TIMEOUT, 30);
        out_printf(o, "idle_timeout = %ld s (unset = BSP default 30 s)\n",
                   (long)current);
        return;
    }

    const long seconds = strtol(value, NULL, 10);
    if (seconds < 0 || seconds > 3600) {
        out_printf(o, "idle-timeout takes 0..3600 seconds (0 = never idle)\n");
        return;
    }
    const esp_err_t werr = sys_config_set_i32(SYS_KEY_IDLE_TIMEOUT, (int32_t)seconds);
    if (werr != ESP_OK) {
        out_printf(o, "store failed: %s\n", esp_err_to_name(werr));
        return;
    }
    const esp_err_t ierr = th_bsp_idle_init();
    out_printf(o, "idle_timeout = %ld s, re-armed: %s\n", seconds,
               esp_err_to_name(ierr));
}

/* ------------------------------------------------------------------------ */
/* Command dispatch                                                          */
/* ------------------------------------------------------------------------ */

/** Map a page name (or decimal id) to an id; false when unknown. */
static bool page_from_name(const char *name, ui_page_id_t *out)
{
    static const struct {
        const char *name;
        ui_page_id_t id;
    } names[] = {
        { "splash", UI_PAGE_SPLASH },
        { "home", UI_PAGE_HOME },
        { "wifi", UI_PAGE_WIFI },
        { "todo", UI_PAGE_TODO },
        { "todo_detail", UI_PAGE_TODO_DETAIL },
        { "weather", UI_PAGE_WEATHER },
        { "calendar", UI_PAGE_CALENDAR },
        { "word", UI_PAGE_WORD },
        { "ai_chat", UI_PAGE_AI_CHAT },
        { "settings", UI_PAGE_SETTINGS },
        { "idle_face", UI_PAGE_IDLE_FACE },
    };

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(names[i].name, name) == 0) {
            *out = names[i].id;
            return true;
        }
    }

    char *end = NULL;
    const long id = strtol(name, &end, 10);
    if (end != NULL && *end == '\0' && id >= 0 && id < UI_PAGE_IDLE_FACE) {
        *out = (ui_page_id_t)id;
        return true;
    }
    return false;
}

/**
 * Split @p line into argv in place. Returns argc; argv is NUL-terminated.
 *
 * Deliberately minimal (spaces only, no quoting). Command arguments here are a
 * page name, a widget label or a number - text that needs quoting can be set
 * with `text <target> <value>` where the remainder of the line is the value.
 */
static int tokenize(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    while (*p != '\0' && argc < max - 1) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }
    argv[argc] = NULL;
    return argc;
}

/**
 * Execute one command line.
 *
 * @param line NUL-terminated, modified in place by tokenize().
 */
static void dbg_exec(char *line, dbg_out_t *out)
{
    char *argv[16];
    const int argc = tokenize(line, argv, 16);
    if (argc == 0) {
        out_printf(out, "(empty command; try 'help')\n");
        return;
    }
    const char *cmd = argv[0];
    const char *a1 = (argc > 1) ? argv[1] : NULL;
    const char *a2 = (argc > 2) ? argv[2] : NULL;

    if (strcmp(cmd, "help") == 0) {
        cmd_help(out);
    } else if (strcmp(cmd, "version") == 0) {
        cmd_version(out);
    } else if (strcmp(cmd, "status") == 0) {
        cmd_status(out);
    } else if (strcmp(cmd, "heap") == 0) {
        cmd_heap(out);
    } else if (strcmp(cmd, "audio") == 0) {
        cmd_audio(out, a1);
    } else if (strcmp(cmd, "speak") == 0) {
        cmd_speak(out, a1);
    } else if (strcmp(cmd, "ftstats") == 0) {
        cmd_ftstats(out);
    } else if (strcmp(cmd, "tasks") == 0) {
        cmd_tasks(out);
    } else if (strcmp(cmd, "routes") == 0) {
        cmd_routes(out);
    } else if (strcmp(cmd, "time") == 0) {
        cmd_time(out);
    } else if (strcmp(cmd, "storage") == 0) {
        cmd_storage(out);
    } else if (strcmp(cmd, "log") == 0) {
        if (a1 == NULL) {
            out_printf(out, "usage: log <tag> [level 0-5]   (5 = verbose)\n");
            out_printf(out, "  app level now: %d\n", (int)sys_log_get_level());
        } else if (a2 == NULL) {
            out_printf(out, "level for '%s' is %d\n", a1,
                       (int)esp_log_level_get(a1));
        } else {
            const int level = atoi(a2);
            /*
             * Two independent filters, and both must be raised.
             *
             * `esp_log_level_set()` governs IDF's own ESP_LOGx backend, but the
             * application's SYS_LOGx macros pass through sys_log_write(), which
             * applies **its own** s_level gate first (sys_log.c) before ever
             * reaching ESP_LOG. Setting only the first therefore changed nothing
             * for SYS_LOGD - the line was dropped before the backend saw it - and
             * "I enabled DEBUG and still see no ui_core lines" looked like the
             * code was not running.
             *
             * Raising the app level to VERBOSE is the price of seeing one tag's
             * debug lines, because sys_log_write filters globally. That is
             * acceptable for a debug build (and matches AGENT.md 9.0.1.2's advice
             * to avoid global DEBUG on a normal run).
             */
            esp_log_level_set(a1, (esp_log_level_t)level);
            if (level >= (int)SYS_LOG_DEBUG) {
                sys_log_set_level(SYS_LOG_DEBUG);
                out_printf(out, "app log level raised to DEBUG so SYS_LOGD "
                                "for '%s' is actually emitted\n", a1);
            }
            out_printf(out, "set '%s' log level to %d\n", a1, level);
        }
    } else if (strcmp(cmd, "reboot") == 0) {
        out_printf(out, "rebooting...\n");
        /* Give the HTTP reply a chance to flush before the reset. */
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else if (strcmp(cmd, "pages") == 0) {
        const ui_page_id_t cur = ui_page_current();
        for (int i = 0; i < UI_PAGE_IDLE_FACE; i++) {
            const ui_page_t *p = ui_page_get((ui_page_id_t)i);
            out_printf(out, "%2d %-14s %s%s\n", i,
                       (p != NULL && p->name != NULL) ? p->name : "-",
                       (p != NULL) ? (p->create != NULL ? "create" : "-") : "unregistered",
                       (i == (int)cur) ? "  <- current" : "");
        }
    } else if (strcmp(cmd, "widgets") == 0 || strcmp(cmd, "ui") == 0) {
        dump_args_t args = {
            .out = out,
            .max_depth = (strcmp(cmd, "ui") == 0) ? 1 : -1,
            .filter = (strcmp(cmd, "ui") == 0) ? NULL : a1,
        };
        if (a2 != NULL) {
            args.max_depth = atoi(a2);
        }
        if (!run_on_lvgl(ui_dump_cb, &args, 5000)) {
            out_printf(out, "LVGL task did not respond\n");
        }
    } else if (strcmp(cmd, "click") == 0) {
        if (a1 == NULL) {
            out_printf(out, "usage: click <label-or-class> [index]\n");
            return;
        }
        click_args_t args = {
            .out = out,
            .target = a1,
            .index = (a2 != NULL) ? atoi(a2) : 0,
        };
        if (!run_on_lvgl(ui_click_cb, &args, 5000)) {
            out_printf(out, "LVGL task did not respond\n");
        }
    } else if (strcmp(cmd, "text") == 0) {
        /* The value may contain spaces: re-join argv[2..] with single spaces. */
        if (a1 == NULL || argc < 3) {
            out_printf(out, "usage: text <label-match> <new value>\n");
            return;
        }
        static char value[256];
        value[0] = '\0';
        for (int i = 2; i < argc; i++) {
            strlcat(value, argv[i], sizeof(value));
            if (i + 1 < argc) {
                strlcat(value, " ", sizeof(value));
            }
        }
        click_args_t args = {
            .out = out,
            .target = a1,
            .index = 0,
            .do_text = true,
            .value = value,
        };
        if (!run_on_lvgl(ui_click_cb, &args, 5000)) {
            out_printf(out, "LVGL task did not respond\n");
        }
    } else if (strcmp(cmd, "bench") == 0) {
        if (a1 == NULL || a2 == NULL) {
            out_printf(out, "usage: bench <target> <text> [index] [repeats]\n");
            return;
        }
        click_args_t args = {
            .out = out,
            .target = a1,
            .index = (argc >= 4) ? atoi(argv[3]) : 0,
            .do_bench = true,
            .repeats = (argc >= 5) ? atoi(argv[4]) : 1,
            .value = argv[2],
        };
        if (!run_on_lvgl(ui_click_cb, &args, 15000)) {
            out_printf(out, "LVGL task did not respond\n");
        }
    } else if (strcmp(cmd, "bench-screen") == 0) {
        click_args_t args = {
            .out = out,
            .do_bench_screen = true,
            .repeats = (a1 != NULL) ? atoi(a1) : 5,
        };
        if (!run_on_lvgl(ui_click_cb, &args, 30000)) {
            out_printf(out, "LVGL task did not respond\n");
        }
    } else if (strcmp(cmd, "scroll") == 0) {
        if (a1 == NULL || argc < 3) {
            out_printf(out, "usage: scroll <target> <dy> [index]\n");
            return;
        }
        click_args_t args = {
            .out = out,
            .target = a1,
            .index = (argc >= 4) ? atoi(argv[3]) : 0,
            .do_scroll = true,
            .value = argv[2],
        };
        if (!run_on_lvgl(ui_click_cb, &args, 5000)) {
            out_printf(out, "LVGL task did not respond\n");
        }
    } else if (strcmp(cmd, "slider") == 0) {
        if (a1 == NULL || argc < 3) {
            out_printf(out, "usage: slider <label-or-class> <value> [index]\n");
            return;
        }
        click_args_t args = {
            .out = out,
            .target = a1,
            .index = (argc >= 4) ? atoi(argv[3]) : 0,
            .do_slider = true,
            .value = argv[2],
        };
        if (!run_on_lvgl(ui_click_cb, &args, 5000)) {
            out_printf(out, "LVGL task did not respond\n");
        }
    } else if (strcmp(cmd, "page") == 0) {
        ui_page_id_t id;
        if (a1 == NULL || !page_from_name(a1, &id)) {
            out_printf(out, "usage: page <0-%d|name>  (see 'pages')\n",
                       UI_PAGE_IDLE_FACE - 1);
            return;
        }
        page_args_t args = { .out = out, .id = id };
        if (!run_on_lvgl(ui_page_cb, &args, 5000)) {
            out_printf(out, "LVGL task did not respond\n");
        }
    } else if (strcmp(cmd, "tap") == 0) {
        if (a1 == NULL || a2 == NULL) {
            out_printf(out, "usage: tap <x> <y> [click]\n");
            return;
        }
        tap_args_t args = {
            .out = out,
            .x = (int16_t)atoi(a1),
            .y = (int16_t)atoi(a2),
            .click_it = (argc > 3 && strcmp(argv[3], "click") == 0),
        };
        if (!run_on_lvgl(ui_tap_cb, &args, 5000)) {
            out_printf(out, "LVGL task did not respond\n");
        }
    } else if (strcmp(cmd, "idle") == 0) {
        idle_args_t args = { .out = out, .enter = (a1 != NULL && strcmp(a1, "enter") == 0) };
        if (!run_on_lvgl(ui_idle_cb, &args, 5000)) {
            out_printf(out, "LVGL task did not respond\n");
        }
    } else if (strcmp(cmd, "idle-timeout") == 0) {
        cmd_idle_timeout(out, a1);
    } else if (strcmp(cmd, "bright") == 0) {
        if (a1 == NULL) {
            out_printf(out, "usage: bright <0-100>\n");
            return;
        }
        const esp_err_t err = th_bsp_set_brightness((uint8_t)atoi(a1));
        out_printf(out, "brightness %d -> %s\n", atoi(a1), esp_err_to_name(err));
    } else if (strcmp(cmd, "cfg") == 0) {
        if (a1 == NULL) {
            out_printf(out, "usage: cfg get <key> | cfg set <key> <value>\n");
        } else if (strcmp(a1, "get") == 0 && a2 != NULL) {
            /*
             * NVS 里同一个键可以是字符串也可以是整数，而工程两种都用：
             * 设置页的三个滑块写的是 i32（SYS_KEY_BRIGHTNESS …），
             * 别的配置写的是字符串。只读字符串的话，滑块的值读回来是
             * 「未设置」—— 一度让人以为去抖落盘没生效。
             * → 字符串读不到就按整数再读一次。
             */
            char value[256];
            esp_err_t err = sys_config_get_str(a2, value, sizeof(value));
            if (strcmp(a2, SYS_KEY_AI_API_KEY) == 0) {
                /*
                 * The API key is never read back (requirement 8.8) - not even
                 * here. Only "is it set" is answered.
                 */
                out_printf(out, "%s = %s\n", a2,
                           (err == ESP_OK && value[0] != '\0') ? "(set)" : "(unset)");
            } else if (err == ESP_OK) {
                out_printf(out, "%s = %s (%s)\n", a2, value, esp_err_to_name(err));
            } else {
                int32_t number = 0;
                const esp_err_t err_i = sys_config_get_i32(a2, &number);
                if (err_i == ESP_OK) {
                    out_printf(out, "%s = %d (i32)\n", a2, (int)number);
                } else {
                    out_printf(out, "%s = (unset) (%s)\n", a2, esp_err_to_name(err));
                }
            }
        } else if (strcmp(a1, "set") == 0 && a2 != NULL && argc > 3) {
            if (strcmp(a2, SYS_KEY_AI_API_KEY) == 0) {
                /*
                 * Go through the service, not straight to NVS: ai_service keeps
                 * a cached "is a key configured" flag (the home page reads it
                 * every 5 s from the LVGL task), and writing the key behind its
                 * back would leave that flag stale until the next reboot.
                 *
                 * The value is not echoed back either - it is a secret.
                 */
                const esp_err_t err = ai_service_set_api_key(argv[3]);
                out_printf(out, "set %s -> %s\n", a2, esp_err_to_name(err));
            } else {
                const esp_err_t err = sys_config_set_str(a2, argv[3]);
                out_printf(out, "set %s = %s -> %s\n", a2, argv[3], esp_err_to_name(err));
            }
        } else {
            out_printf(out, "usage: cfg get <key> | cfg set <key> <value>\n");
        }
    } else if (strcmp(cmd, "nvs") == 0) {
        if (a1 == NULL) {
            out_printf(out, "usage: nvs <key>   (erases the key)\n");
            return;
        }
        if (strcmp(a1, SYS_KEY_AI_API_KEY) == 0) {
            /* Through the service again, so the cached flag follows. */
            const esp_err_t err = ai_service_set_api_key("");
            out_printf(out, "erase %s -> %s\n", a1, esp_err_to_name(err));
        } else {
            const esp_err_t err = sys_config_erase(a1);
            out_printf(out, "erase %s -> %s\n", a1, esp_err_to_name(err));
        }
    } else if (strcmp(cmd, "http-auth") == 0) {
        /*
         * Read or replace the HTTP auth token.
         *
         * Deliberately the ONE place that reveals the token, and it is a debug
         * build only. It exists because reading it out of NVS over serial
         * **requires resetting the board** (esptool cannot connect to a chip that
         * is running the application with --no-reset), which silently reboots the
         * device in the middle of a test sequence - a far worse property for a
         * debugging tool than a debug-only readback.
         *
         * Requirement 8.8 still holds for the product: this compiles out with
         * CONFIG_TODOHELPER_DEBUG_SHELL=n, and no normal route returns it.
         */
        if (a1 != NULL && strcmp(a1, "set") == 0 && a2 != NULL) {
            const esp_err_t err = sys_config_set_str(SYS_KEY_AUTH_TOKEN, a2);
            out_printf(out, "auth token set to '%s' -> %s\n", a2, esp_err_to_name(err));
        } else {
            char token[80];
            const esp_err_t err = sys_config_get_str(SYS_KEY_AUTH_TOKEN, token,
                                                     sizeof(token));
            if (err == ESP_OK) {
                out_printf(out, "auth token: %s\n", token);
            } else {
                out_printf(out, "auth token: (unset, %s) - auth is off\n",
                           esp_err_to_name(err));
            }
            out_printf(out, "usage: http-auth [set <32-hex-token>]\n");
        }
    } else {
        out_printf(out, "unknown command '%s'; try 'help'\n", cmd);
    }
}

/* ------------------------------------------------------------------------ */
/* HTTP                                                                      */
/* ------------------------------------------------------------------------ */

/**
 * Shared entry point for both the GET and POST forms.
 *
 * ## Why the output is JSON-encoded rather than sent raw
 *
 * The first version handed the shell text straight to `http_reply_ok()`, which
 * takes *pre-serialised JSON*. Multi-line output therefore produced
 * `{"code":0,...,"data":project ...\nversion 1.3.1\n}` - newlines and quotes
 * unescaped, i.e. **not valid JSON**, and every client that parsed it failed
 * (verified with curl: `json.decoder.JSONDecodeError`). cJSON does the escaping.
 *
 * The reply is `{"code":0,"msg":"ok","data":{"out":"line1\nline2\n"}}` - i.e. the
 * shell text is the `out` field **inside** the project envelope, which
 * http_reply_json() builds itself. (Passing an already-wrapped object produced
 * `data.data.out`: valid JSON, but not this project's contract.)
 */
static esp_err_t exec_common(httpd_req_t *req, const char *cmd)
{
    dbg_out_t out = { 0 };
    if (cmd == NULL || cmd[0] == '\0') {
        out_printf(&out, "no command; try 'help'\n");
    } else {
        /* dbg_exec tokenizes in place, so work on a copy of the caller's text. */
        char *work = strdup(cmd);
        if (work == NULL) {
            return http_reply_esp_error(req, ESP_ERR_NO_MEM, "debug exec");
        }
        dbg_exec(work, &out);
        free(work);
    }

    cJSON *data = cJSON_CreateObject();
    if (data == NULL || !cJSON_AddStringToObject(data, "out",
                                                 (out.buf != NULL) ? out.buf : "")) {
        cJSON_Delete(data);
        out_free(&out);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "debug exec");
    }
    /* Truncation is silent otherwise, and a debug tool that quietly drops output
     * is worse than one that says so. */
    if (out.oom) {
        cJSON_AddBoolToObject(data, "truncated", true);
    }

    char *json = cJSON_PrintUnformatted(data);
    cJSON_Delete(data);
    out_free(&out);
    if (json == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "debug exec");
    }

    const esp_err_t err = http_reply_json(req, 200, "ok", json);
    cJSON_free(json);
    return err;
}

/** URL-decode in place: %XX and '+'. */
static void url_decode(char *s)
{
    char *out = s;
    for (char *in = s; *in != '\0'; in++) {
        if (*in == '+') {
            *out++ = ' ';
        }
        else if (*in == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
            const char hex[3] = { in[1], in[2], '\0' };
            *out++ = (char)strtol(hex, NULL, 16);
            in += 2;
        }
        else {
            *out++ = *in;
        }
    }
    *out = '\0';
}

static esp_err_t exec_get_handler(httpd_req_t *req)
{
    char cmd[512];
    if (!http_query_get(req, "cmd", cmd, sizeof(cmd))) {
        cmd[0] = '\0';
    }
    /* A Chinese target in the query string arrives percent-encoded. */
    url_decode(cmd);
    return exec_common(req, cmd);
}

static esp_err_t exec_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing body");
    }

    /*
     * Accept either {"cmd":"..."} or a raw command line as the whole body.
     *
     * The raw form is what makes this pleasant from a shell:
     *   curl -X POST -d 'widgets token' http://dev/api/v1/debug/exec
     * and it keeps working without a JSON encoder on the caller's side.
     *
     * The JSON form is parsed with cJSON, not by hand: a hand-rolled parser
     * that copies the text between the quotes leaves the escape sequences
     * alone, so a client that sends "\u4f60\u597d" (which is what every
     * standard JSON encoder does for non-ASCII) would run the literal
     * characters - and a measurement of "how long does drawing 30 hanzi
     * take" would silently measure nothing at all.
     */
    char cmd[512];
    cmd[0] = '\0';
    if (body[0] == '{') {
        cJSON *root = cJSON_Parse(body);
        if (root != NULL) {
            const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
            if (cJSON_IsString(item) && item->valuestring != NULL) {
                strlcpy(cmd, item->valuestring, sizeof(cmd));
            }
            cJSON_Delete(root);
        }
        if (cmd[0] == '\0') {
            /* Not a cJSON object with a "cmd" string: fall back to the raw
             * text so a hand-written body still works. */
            strlcpy(cmd, body, sizeof(cmd));
        }
    } else {
        strlcpy(cmd, body, sizeof(cmd));
    }
    free(body);

    return exec_common(req, cmd);
}

/**
 * GET /api/v1/debug/exec?cmd=... with no command lists the shell's help, which
 * doubles as a capability probe for a script.
 */
esp_err_t debug_shell_init(void)
{
    static const struct {
        const char *path;
        httpd_method_t method;
        http_route_handler_t handler;
    } routes[] = {
        { "/api/v1/debug/exec", HTTP_GET, exec_get_handler },
        { "/api/v1/debug/exec", HTTP_POST, exec_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = http_router_register(routes[i].path, routes[i].method,
                                             routes[i].handler);
        if (err != ESP_OK) {
            SYS_LOGE(TAG, "register %s failed: %s", routes[i].path, esp_err_to_name(err));
            return err;
        }
        /* Always require auth, even when the rest of the device is open: this
         * endpoint can reboot the board and move its UI, so it must never be
         * reachable just because someone built a debug firmware. */
        (void)http_router_require_auth(routes[i].path, routes[i].method, true);
    }

    SYS_LOGW(TAG, "debug shell ENABLED at /api/v1/debug/exec (auth required)");
    return ESP_OK;
}

#else /* !CONFIG_TODOHELPER_DEBUG_SHELL */

esp_err_t debug_shell_init(void)
{
    /* Compiled out: the module row in app_main stays unconditional. */
    return ESP_OK;
}

#endif /* CONFIG_TODOHELPER_DEBUG_SHELL */
