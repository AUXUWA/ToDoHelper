/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_settings.c
 * @brief Settings: display and idle, AI chat, timezone and city, daily words,
 *        HTTP auth token and a device summary.
 *
 * Everything writes straight to NVS through the owning component's API, so the
 * values survive a reboot without this page knowing any key names.
 *
 * ## Layout
 *
 * Six cards in one scrollable column.  The page container is created by
 * ui_core.c's page_container_create(), which puts LV_OBJ_FLAG_SCROLLABLE on
 * every page container, so the page simply grows downwards instead of being
 * squeezed into one screen.
 *
 * The vertical budget of one screen is 412 px (480 - 40 status bar - 2x14 page
 * padding).  What the six cards add up to is roughly:
 *
 *   显示与待机 ~150   AI 对话 ~300   时间与时区 ~185
 *   单词 ~220          局域网与安全 ~200          设备信息 ~320
 *   + 抬头 ~35 + 6 个卡间距 6x12 + 页内边距 28  ≈ 1500 px
 *
 * So this page is meant to be scrolled - roughly four screens.  Nothing is
 * given a fixed height, so no text is ever clipped; the alternative (squeezing
 * the same content into 412 px) would mean unreadable rows.
 *
 * ## Two rules this file follows everywhere
 *
 *   - **No blocking work in an LVGL callback.**  NVS writes are handed to a
 *     one-shot task and the cloud word list is fetched in a worker task
 *     (AGENT.md 9.0.1).
 *   - **Write only what changed.**  Every label update compares against the
 *     value that is on screen before touching the widget
 *     (AGENT.md 9.0.1.1, rule 2).
 *
 * ## Shell and content (ROADMAP v1.8.3 §1)
 *
 * This is the heaviest page in the project: six cards, 182 widgets, about
 * 42 KB of internal RAM.  So it is split in two:
 *
 *   - the **shell** (the header with the back button) is built once and stays;
 *   - the **content** (the six cards) is destroyed when the page is hidden and
 *     rebuilt when it comes back.
 *
 * ui_core only hides the other pages (lv_obj_add_flag(HIDDEN)) - it never
 * destroys them - so without this split "visiting this page once" would cost
 * 42 KB of internal heap for good.  The rebuild reads nothing but RAM: the
 * slider values come from s_pending, everything else from the owning
 * components' in-RAM caches.  The event subscription stays in create() (it
 * runs exactly once) - subscribing again on every rebuild would handle every
 * calendar event twice.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_service.h"
#include "calendar_service.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pages_internal.h"
#include "storage_manager.h"
#include "sync_server.h"
#include "th_bsp.h"
#include "weather_service.h"
#include "wifi_manager.h"
#include "word_service.h"
#include "esp_attr.h"

static const char *TAG = "page_settings";

/* ------------------------------------------------------------------------ */
/* Widget state                                                              */
/* ------------------------------------------------------------------------ */

/* ---- AI card ----------------------------------------------------------- */

static const char *const s_ai_models[2] = { "deepseek-chat", "deepseek-reasoner" };

static lv_obj_t *s_ai_key_label;
static lv_obj_t *s_ai_hint_label;
static lv_obj_t *s_ai_model_btn[2];
static lv_obj_t *s_ai_model_label;
static lv_obj_t *s_ai_rounds_label;
static lv_obj_t *s_ai_prompt_label;

/* Dirty-check buffers for the AI card (see refresh_ai_card). */
static char s_ai_key_text[16];
static char s_ai_model_text[32];
static char s_ai_model_name_text[48];
static char s_ai_rounds_text[16];
static char s_ai_prompt_text[256];

/* ---- Word card --------------------------------------------------------- */

static lv_obj_t *s_word_book_label;
static lv_obj_t *s_word_cloud_btn;
static lv_obj_t *s_word_stats_label;

static char s_word_book_text[48];
static char s_word_cloud_text[32];
static char s_word_stats_text[96];

/* ---- Time card --------------------------------------------------------- */

/** Read only line: active timezone and whether the clock is synchronised. */
static lv_obj_t *s_tz_state_label;
static char s_tz_state_text[64];

/* ---- Device info grid -------------------------------------------------- */
/**
 * Number of key/value rows in the device card.
 *
 * The address is the one value that is too long for a two column cell, so it
 * gets a row of its own (add_kv_wide_row) - that is why the count is 8 and not
 * 9.
 */
#define INFO_ROW_COUNT 8

static lv_obj_t *s_info_value[INFO_ROW_COUNT];
static EXT_RAM_BSS_ATTR char s_info_cache[INFO_ROW_COUNT][80];

/* ---- Security card ----------------------------------------------------- */

static lv_obj_t *s_auth_label;
/** Web console address, shown large so it can be read off the screen. */
static lv_obj_t *s_url_label;

/* Dirty-check buffers for the security card's two lines (see refresh_info). */
static char s_url_text[128];
static char s_auth_text[96];

/* ------------------------------------------------------------------------ */
/* Sliders                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char *title;
    int32_t value;
    int32_t min;
    int32_t max;
    void (*on_change)(int32_t value);
} setting_slider_t;

/**
 * 滑块的新值：先在内存里生效，落盘交给一次性任务。
 *
 * 一次拖动会产生上百个 LV_EVENT_VALUE_CHANGED，每一个都直接 nvs_commit
 * 就是上百次 flash 擦写 —— 写的时候 IDF 会关掉 flash 缓存，整个界面停住，
 * 用户看到的是「滑块拖不动」（AGENT.md 9.0.1.1 铁律一）。
 *
 * 所以回调里只做两件事：把新值用到 RAM 上（亮度立刻生效），以及把新值记在
 * 这里；去抖定时器在用户停手 600 ms 之后把值交给一次性任务去写 NVS。
 * 跟同一个文件里的时区按钮（tz_apply_task）是同一个写法。
 */
typedef struct {
    int32_t brightness;
    int32_t idle_brightness;
    int32_t idle_timeout;
    bool brightness_dirty;
    bool idle_brightness_dirty;
    bool idle_timeout_dirty;
} slider_pending_t;

static slider_pending_t s_pending;

/** 去抖定时器：页面自己拥有（on_hide 暂停），见 slider_save_tick。 */
static lv_timer_t *s_save_timer;

/**
 * 上一次落盘里**哪几个键写失败了**（位掩码）。
 *
 * 由落盘任务写、由 LVGL 任务读，所以用原子量而不是普通变量
 * （两个任务真的会同时碰它）。
 *
 * 为什么要有它：NVS 提交可能失败（分区满、被别人占着），
 * 而「写失败」如果不告诉用户，就等于用户刚调的值**静默丢了** ——
 * 亮度立刻生效了（那是内存里的值），但下次开机会回到旧值。
 * 这里把失败记下来，LVGL 任务下一拍把它变回「脏」并提示用户。
 */
#define SLIDER_SAVE_FAILED_BRIGHTNESS    (1u << 0)
#define SLIDER_SAVE_FAILED_IDLE_BRIGHT   (1u << 1)
#define SLIDER_SAVE_FAILED_IDLE_TIMEOUT  (1u << 2)
static uint32_t s_save_failed_mask;

/** 落盘请求：一次性任务拿着它去写 NVS。 */
typedef struct {
    int32_t brightness;
    int32_t idle_brightness;
    int32_t idle_timeout;
    bool save_brightness;
    bool save_idle_brightness;
    bool save_idle_timeout;
} slider_save_req_t;

/**
 * 一次性任务：真正写 NVS。
 *
 * NVS 提交是 flash 写，写期间 flash 缓存被关，PSRAM 随即不可访问 ——
 * 所以这个任务的栈必须在内部 RAM（AGENT.md 9.0.1）。写完自己收尾。
 */
static void slider_save_task(void *arg)
{
    slider_save_req_t *req = (slider_save_req_t *)arg;
    uint32_t failed = 0;
    if (req != NULL) {
        if (req->save_brightness) {
            if (sys_config_set_i32(SYS_KEY_BRIGHTNESS, req->brightness) != ESP_OK) {
                failed |= SLIDER_SAVE_FAILED_BRIGHTNESS;
            }
        }
        if (req->save_idle_brightness) {
            if (sys_config_set_i32(SYS_KEY_IDLE_BRIGHT, req->idle_brightness) != ESP_OK) {
                failed |= SLIDER_SAVE_FAILED_IDLE_BRIGHT;
            }
        }
        if (req->save_idle_timeout) {
            if (sys_config_set_i32(SYS_KEY_IDLE_TIMEOUT, req->idle_timeout) != ESP_OK) {
                failed |= SLIDER_SAVE_FAILED_IDLE_TIMEOUT;
            } else {
                /* 看门狗的超时值是从 NVS 读的：新值落盘之后重新 arm 一次，
                 * 它才会用上新值。 */
                (void)th_bsp_idle_init();
            }
        }
        free(req);
    }
    /*
     * 把失败告诉 LVGL 任务（那里才有脏标记和 toast）。
     * 用原子写：这个变量两个任务都会碰。
     */
    __atomic_store_n(&s_save_failed_mask, failed, __ATOMIC_RELEASE);
    vTaskDelete(NULL);
}

/**
 * 把记下来的值交给一次性任务落盘（在 LVGL 任务里调用，本身不做 flash 写）。
 */
static void slider_save_flush(void)
{
    if (!s_pending.brightness_dirty && !s_pending.idle_brightness_dirty &&
        !s_pending.idle_timeout_dirty) {
        return;
    }

    slider_save_req_t *req = calloc(1, sizeof(*req));
    if (req == NULL) {
        SYS_LOGE(TAG, "no memory for the slider save request");
        return;   /* 脏标记还在，下一拍再试 */
    }
    req->brightness = s_pending.brightness;
    req->idle_brightness = s_pending.idle_brightness;
    req->idle_timeout = s_pending.idle_timeout;
    req->save_brightness = s_pending.brightness_dirty;
    req->save_idle_brightness = s_pending.idle_brightness_dirty;
    req->save_idle_timeout = s_pending.idle_timeout_dirty;

    /* 先清脏标记：req 里已经是这一次要写的值，之后再有新值会重新置位。 */
    s_pending.brightness_dirty = false;
    s_pending.idle_brightness_dirty = false;
    s_pending.idle_timeout_dirty = false;

    /* 3072 B 内部栈：跟 tz_apply_task 一样，写 flash 的任务栈不能放 PSRAM。 */
    if (xTaskCreate(slider_save_task, "th_slider", 3072, req, 3, NULL) != pdPASS) {
        /* 建不出任务就把脏标记放回去，下一拍再试。 */
        s_pending.brightness_dirty = req->save_brightness;
        s_pending.idle_brightness_dirty = req->save_idle_brightness;
        s_pending.idle_timeout_dirty = req->save_idle_timeout;
        free(req);
        ui_toast("设置保存失败：无法创建任务");
    }
}

/** 去抖定时器：用户停手 600 ms 之后才落盘（一次拖动只写一次 NVS）。 */
static void slider_save_tick(lv_timer_t *timer)
{
    (void)timer;

    /*
     * 上一次落盘有没有失败？失败了就把它变回「脏」，下一拍再试一次，
     * 并且告诉用户 —— 不然用户刚调的值就静默丢了（亮度立刻生效了，
     * 但那是内存里的值，下次开机还是旧的）。
     */
    const uint32_t failed = __atomic_exchange_n(&s_save_failed_mask, 0u, __ATOMIC_ACQ_REL);
    if (failed != 0u) {
        if ((failed & SLIDER_SAVE_FAILED_BRIGHTNESS) != 0u) {
            s_pending.brightness_dirty = true;
        }
        if ((failed & SLIDER_SAVE_FAILED_IDLE_BRIGHT) != 0u) {
            s_pending.idle_brightness_dirty = true;
        }
        if ((failed & SLIDER_SAVE_FAILED_IDLE_TIMEOUT) != 0u) {
            s_pending.idle_timeout_dirty = true;
        }
        SYS_LOGE(TAG, "the settings could not be written to NVS (%s)",
                 failed == SLIDER_SAVE_FAILED_BRIGHTNESS ? "brightness" : "another key");
        ui_toast("设置保存失败，将重试");
    }

    slider_save_flush();
}

/** 重新计时：每次拖动都调用，所以落盘只发生在停手之后。 */
static void slider_save_arm(void)
{
    if (s_save_timer != NULL) {
        lv_timer_reset(s_save_timer);
    }
}

static void brightness_changed(int32_t value)
{
    /* 立刻生效：屏幕马上变亮（只改叠加层，不碰 flash）。 */
    (void)th_bsp_set_brightness((uint8_t)value);
    s_pending.brightness = value;
    s_pending.brightness_dirty = true;
    slider_save_arm();
}

static void idle_brightness_changed(int32_t value)
{
    /* 只影响下一次进待机，所以现在什么都不用做。 */
    s_pending.idle_brightness = value;
    s_pending.idle_brightness_dirty = true;
    slider_save_arm();
}

static void idle_timeout_changed(int32_t value)
{
    /* 超时值是从 NVS 读的，这里只记下来；落盘之后由一次性任务重新 arm。
     * 顺便报一次活动：原来每次改超时都会重置待机倒计时，这里保持一致
     * （th_bsp_report_activity() 只动内存，不碰 flash）。 */
    th_bsp_report_activity();
    s_pending.idle_timeout = value;
    s_pending.idle_timeout_dirty = true;
    slider_save_arm();
}

static void slider_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    const setting_slider_t *def = (const setting_slider_t *)lv_event_get_user_data(e);
    if (def == NULL || def->on_change == NULL) {
        return;
    }
    def->on_change(lv_slider_get_value(slider));
}

static lv_obj_t *add_slider(lv_obj_t *parent, const setting_slider_t *def)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, def->title);
    lv_obj_set_style_text_font(title, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_set_width(title, 150);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_height(slider, 22);
    lv_slider_set_range(slider, def->min, def->max);
    lv_slider_set_value(slider, def->value, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED, (void *)def);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_width(value, 56);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(value, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(value, UI_COLOR_PRIMARY, 0);
    lv_label_set_text_fmt(value, "%ld", (long)def->value);

    /* The shared settings_tick() timer reads the slider back through this
     * pointer, which avoids one callback per slider. */
    lv_obj_set_user_data(slider, value);

    return row;
}

/**
 * LVGL has no "bind label to slider" helper, so a small shared timer refreshes
 * the readouts of the sliders this page created.
 *
 * The timer is owned by the page (s_slider_timer) and paused in on_hide(): it
 * used to be created without keeping the handle, so it kept re-formatting four
 * labels four times a second *forever* - including while the user was on a
 * completely different page (found in the smoothness audit).
 */
#define SETTINGS_MAX_SLIDERS 4
static lv_obj_t *s_slider_objs[SETTINGS_MAX_SLIDERS];
static lv_obj_t *s_slider_values[SETTINGS_MAX_SLIDERS];
static size_t s_slider_count;
static lv_timer_t *s_slider_timer;

static void settings_tick(lv_timer_t *timer)
{
    (void)timer;
    for (size_t i = 0; i < s_slider_count; i++) {
        if (s_slider_objs[i] == NULL || s_slider_values[i] == NULL) {
            continue;
        }
        /* Only touch the label when the value actually changed: writing the same
         * text still invalidates the label and costs a repaint. */
        const int value = (int)lv_slider_get_value(s_slider_objs[i]);
        if (value == (int)(intptr_t)lv_obj_get_user_data(s_slider_values[i])) {
            continue;
        }
        lv_obj_set_user_data(s_slider_values[i], (void *)(intptr_t)value);
        lv_label_set_text_fmt(s_slider_values[i], "%d", value);
    }
}

/* ------------------------------------------------------------------------ */
/* Simple action rows                                                        */
/* ------------------------------------------------------------------------ */

/**
 * Timezone change: the NVS commit is a flash write, so it is handed to a tiny
 * one-shot task instead of running in the LVGL callback (a flash write also
 * disables the cache for its duration). The label is updated optimistically
 * because calendar_set_timezone() only fails on invalid input, which cannot
 * happen here - the strings come from a fixed table.
 */
typedef struct {
    char tz[48];
} tz_request_t;

static void tz_apply_task(void *arg)
{
    tz_request_t *req = (tz_request_t *)arg;
    const esp_err_t err = calendar_set_timezone(req->tz);
    free(req);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "timezone change failed: %s", esp_err_to_name(err));
    }
    /*
     * 一次性任务必须自己收尾。
     *
     * FreeRTOS 的任务函数**不能直接返回**：返回等于「任务结束了，但栈还没还回去」，
     * 调度器会踩到一块已经失效的栈 —— 真机上的表现是点一下时区按钮设备就卡死
     * （网页连不上、串口不出字，只能复位）。这是 v1.6.0 就有的老 bug，
     * 本轮上机时被「点时区按钮 → 设备卡死」顺带查出来的。
     */
    vTaskDelete(NULL);
}

static void tz_clicked_cb(lv_event_t *e)
{
    const char *tz = (const char *)lv_event_get_user_data(e);
    if (tz == NULL) {
        return;
    }

    tz_request_t *req = calloc(1, sizeof(*req));
    if (req == NULL) {
        ui_toast("设置失败：内存不足");
        return;
    }
    strlcpy(req->tz, tz, sizeof(req->tz));

    /* 3072 B stack, priority 3: it only does one NVS write and exits. */
    if (xTaskCreate(tz_apply_task, "th_tz", 3072, req, 3, NULL) != pdPASS) {
        free(req);
        ui_toast("设置失败：无法创建任务");
        return;
    }
    ui_toast("时区已设为 %s", tz);
}

static void sync_ntp_cb(lv_event_t *e)
{
    (void)e;
    /* Async: the old synchronous call waited up to 10 s on the network *with the
     * LVGL lock held*, freezing the whole UI (and the event loop behind it). */
    const esp_err_t err = calendar_sync_ntp_async();
    if (err == ESP_ERR_TIMEOUT) {
        ui_toast("正在校时，请稍候…");
    } else if (err != ESP_OK) {
        ui_toast("校时失败：%s", esp_err_to_name(err));
    } else {
        ui_toast("正在校时…");
    }
}

/** Result of a requested sync, delivered on the LVGL task by ui_core. */
static esp_err_t settings_on_sync_result(int32_t event_id, void *data)
{
    (void)event_id;
    const esp_err_t err = (data != NULL) ? *(const esp_err_t *)data : ESP_FAIL;
    ui_toast(err == ESP_OK ? "校时成功" : "校时失败：%s", esp_err_to_name(err));
    return ESP_OK;
}

static void city_clicked_cb(lv_event_t *e)
{
    (void)e;
    ui_toast("当前城市：%s", weather_get_city()[0] ? weather_get_city() : "未设置");
    ui_toast("请在天气页设置，或用 HTTP API POST /api/v1/weather/city");
}

static void word_count_up_cb(lv_event_t *e)
{
    (void)e;
    uint32_t count = word_get_daily_count() + 5;
    (void)word_set_daily_count(count);
    ui_toast("每日单词数：%u", (unsigned)word_get_daily_count());
}

static void word_count_down_cb(lv_event_t *e)
{
    (void)e;
    uint32_t count = word_get_daily_count();
    count = (count > 5) ? (count - 5) : 1;
    (void)word_set_daily_count(count);
    ui_toast("每日单词数：%u", (unsigned)word_get_daily_count());
}

static void word_reset_cb(lv_event_t *e)
{
    (void)e;
    (void)word_reset_progress();
    ui_toast("单词进度已清空");
}

/* ------------------------------------------------------------------------ */
/* AI card                                                                   */
/* ------------------------------------------------------------------------ */

/** Page level limits for the retained round count (ai_service allows 1..12). */
#define AI_ROUNDS_MIN 1
#define AI_ROUNDS_MAX 10

/**
 * A pending AI setting write.
 *
 * Model and round count are persisted in NVS, and an LVGL callback must not do
 * a flash write (AGENT.md 9.0.1), so a click only queues this struct for a
 * one-shot task.
 */
typedef enum {
    AI_WRITE_MODEL = 0,
    AI_WRITE_ROUNDS,
} ai_write_kind_t;

typedef struct {
    ai_write_kind_t kind;
    char model[32];
    uint32_t rounds;
} ai_write_req_t;

static void refresh_ai_card(void);
static void refresh_word_card(void);

/** Re-read the cards after a background write landed (runs in the LVGL task). */
static void settings_refresh_async(void *arg)
{
    (void)arg;
    refresh_ai_card();
    refresh_word_card();
}

static void ai_write_task(void *arg)
{
    ai_write_req_t *req = (ai_write_req_t *)arg;
    if (req != NULL) {
        if (req->kind == AI_WRITE_MODEL) {
            (void)ai_service_set_model(req->model);
        } else {
            (void)ai_service_set_history_rounds(req->rounds);
        }
        free(req);
    }
    /* The write is done: pull the new state back into the widgets. */
    (void)ui_run_async(settings_refresh_async, NULL);
    vTaskDelete(NULL);
}

static void ai_model_clicked_cb(lv_event_t *e)
{
    const char *model = (const char *)lv_event_get_user_data(e);
    if (model == NULL || strcmp(model, ai_service_get_model()) == 0) {
        return;   /* already the active model */
    }

    ai_write_req_t *req = calloc(1, sizeof(*req));
    if (req == NULL) {
        ui_toast("设置失败：内存不足");
        return;
    }
    req->kind = AI_WRITE_MODEL;
    strlcpy(req->model, model, sizeof(req->model));

    /* 4096 B of internal RAM: a task that touches flash must not have a PSRAM
     * stack (AGENT.md 9.0.1). */
    if (xTaskCreate(ai_write_task, "th_ai_set", 4096, req, 3, NULL) != pdPASS) {
        free(req);
        ui_toast("设置失败：无法创建任务");
    }
}

static void ai_rounds_clicked_cb(lv_event_t *e)
{
    const int delta = (int)(intptr_t)lv_event_get_user_data(e);

    int32_t rounds = (int32_t)ai_service_get_history_rounds() + delta;
    if (rounds < AI_ROUNDS_MIN) {
        rounds = AI_ROUNDS_MIN;
    }
    if (rounds > AI_ROUNDS_MAX) {
        rounds = AI_ROUNDS_MAX;
    }
    if ((uint32_t)rounds == ai_service_get_history_rounds()) {
        ui_toast(delta > 0 ? "最多保留 %d 轮" : "至少保留 %d 轮",
                 delta > 0 ? AI_ROUNDS_MAX : AI_ROUNDS_MIN);
        return;
    }

    ai_write_req_t *req = calloc(1, sizeof(*req));
    if (req == NULL) {
        ui_toast("设置失败：内存不足");
        return;
    }
    req->kind = AI_WRITE_ROUNDS;
    req->rounds = (uint32_t)rounds;

    if (xTaskCreate(ai_write_task, "th_ai_set", 4096, req, 3, NULL) != pdPASS) {
        free(req);
        ui_toast("设置失败：无法创建任务");
    }
}

static void ai_clear_clicked_cb(lv_event_t *e)
{
    (void)e;
    /* Clearing the context only touches RAM (no NVS write), so it is safe to
     * do right here in the callback. */
    if (ai_service_clear_history() == ESP_OK) {
        ui_toast("对话已清空");
    } else {
        ui_toast("清空失败");
    }
}

/* ------------------------------------------------------------------------ */
/* Refresh                                                                   */
/* ------------------------------------------------------------------------ */

/* Defined below; the auth callbacks use it to re-render the security card. */
static void refresh_info(void);

/* ------------------------------------------------------------------------ */
/* Widget helpers                                                            */
/* ------------------------------------------------------------------------ */

/**
 * Text colour for a button background.
 *
 * UI_COLOR_ON_PRIMARY is a near-black meant for the bright accent colour.  On
 * the dark UI_COLOR_SURFACE_ALT it was unreadable (the old timezone and daily
 * word buttons used it), so the colour now follows the background: light text
 * on every dark button, dark text on the bright one.
 */
static lv_color_t button_fg(lv_color_t bg)
{
    return lv_color_eq(bg, UI_COLOR_PRIMARY) ? UI_COLOR_ON_PRIMARY : UI_COLOR_TEXT;
}

/**
 * Build a card and restyle its title.
 *
 * ui_card_create() hard-codes the title style (UI_FONT_TITLE + body colour) in
 * ui_core, and ui_core is not touched this round.  Doing it here keeps the six
 * cards of this page looking like one set: body sized title in the accent
 * colour.
 */
static lv_obj_t *add_card(lv_obj_t *parent, const char *title, lv_obj_t **out_body)
{
    lv_obj_t *card = ui_card_create(parent, title, out_body);
    if (card == NULL) {
        return NULL;
    }
    /* The title label is the first child, the content body the second. */
    lv_obj_t *title_label = lv_obj_get_child(card, 0);
    if (title_label != NULL) {
        lv_obj_set_style_text_font(title_label, ui_font(UI_FONT_BODY), 0);
        lv_obj_set_style_text_color(title_label, UI_COLOR_PRIMARY, 0);
    }
    return card;
}

static lv_obj_t *add_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                            const void *user_data, lv_color_t color)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(label, button_fg(color), 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)user_data);
    return btn;
}

static lv_obj_t *add_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/**
 * Write a label only when its text really changed.
 *
 * lv_label_set_text() invalidates the label even when the string is identical,
 * and this page is refreshed every 5 s (AGENT.md 9.0.1.1, rule 2).
 */
static void label_set_cached(lv_obj_t *label, char *cache, size_t cache_size,
                             const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }
    if (cache != NULL && strcmp(cache, text) == 0) {
        return;
    }
    if (cache != NULL) {
        strlcpy(cache, text, cache_size);
    }
    lv_label_set_text(label, text);
}

/** Change a button's background (and its text colour to match). */
static void set_button_color(lv_obj_t *btn, lv_color_t color)
{
    if (btn == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label != NULL) {
        lv_obj_set_style_text_color(label, button_fg(color), 0);
    }
}

/** Change a button's caption, but only when it differs from the one on screen. */
static void set_button_text(lv_obj_t *btn, char *cache, size_t cache_size, const char *text)
{
    if (btn == NULL || text == NULL) {
        return;
    }
    if (cache != NULL && strcmp(cache, text) == 0) {
        return;
    }
    if (cache != NULL) {
        strlcpy(cache, text, cache_size);
    }
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label != NULL) {
        lv_label_set_text(label, text);
    }
}

/**
 * One key/value row of the device card: key on the left (muted, small),
 * value on the right (body colour, small).
 */
static lv_obj_t *add_kv_row(lv_obj_t *parent, const char *key, int index)
{
    lv_obj_t *row = add_row(parent);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_font(k, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(k, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_width(k, 92);

    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_flex_grow(v, 1);
    lv_obj_set_style_text_font(v, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(v, UI_COLOR_TEXT, 0);

    if (index >= 0 && index < INFO_ROW_COUNT) {
        s_info_value[index] = v;
    }
    return row;
}

/**
 * A key/value row whose value is too long for a two column cell: the key gets
 * its own line and the value takes the full width below it.
 *
 * The web console address is such a value - squeezed into the right hand
 * column it would be cut off.
 */
static void add_kv_wide_row(lv_obj_t *parent, const char *key, int index)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 2, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *k = lv_label_create(box);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_font(k, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(k, UI_COLOR_TEXT_MUTED, 0);

    lv_obj_t *v = lv_label_create(box);
    lv_obj_set_width(v, LV_PCT(100));
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(v, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(v, UI_COLOR_TEXT, 0);

    if (index >= 0 && index < INFO_ROW_COUNT) {
        s_info_value[index] = v;
    }
}

/* ------------------------------------------------------------------------ */
/* Auth / reboot actions                                                     */
/* ------------------------------------------------------------------------ */

static void auth_enable_cb(lv_event_t *e)
{
    (void)e;

    /* Generating the token here is more usable than typing 32 characters on an
     * on-screen keyboard; it is displayed once so it can be copied to the PC. */
    char token[33];
    for (int i = 0; i < 32; i++) {
        token[i] = "0123456789abcdef"[(esp_random() >> (i % 8)) & 0x0F];
    }
    token[32] = '\0';

    esp_err_t err = sync_server_set_auth_token(token);
    if (err == ESP_OK) {
        /*
         * The token is the whole point of this dialog, so it is rendered as a
         * highlighted, wrapped value rather than described in prose. The earlier
         * version told the user to copy "the token below" while showing only
         * explanatory text - and the text was long enough to push the box off
         * the screen.
         *
         * Strings are kept short on purpose: ui_dialog_secret() lays the lead-in
         * and the token out side by side (the panel is 800x480 landscape), so
         * long prose in the left column would squeeze the token column instead
         * of merely making the box taller.
         */
        ui_dialog_secret("HTTP 鉴权已开启",
                         "把下面的 token 填进网页控制台的「访问令牌」。",
                         token,
                         "串口日志里也有；随时可在这里重新生成。",
                         "知道了");
        /* Logged once on purpose: this is the only way to read it off the
         * device, and requirement 8.8 only forbids it appearing in backups and
         * routine logs. Rotate it if that matters to you. */
        SYS_LOGW(TAG, "new HTTP auth token: %s", token);
    } else {
        ui_toast("开启失败：%s", esp_err_to_name(err));
    }
    /* Go through refresh_info() rather than writing the label here, so the
     * dirty-check buffer stays in sync with what is on screen. */
    refresh_info();
}

static void auth_disable_cb(lv_event_t *e)
{
    (void)e;
    (void)sync_server_set_auth_token(NULL);
    ui_toast("HTTP 鉴权已关闭");
    refresh_info();
}

static void reboot_cb(lv_event_t *e)
{
    (void)e;
    ui_toast("重启中…");
    /* Delayed so the toast can actually be drawn before the reset. */
    extern void esp_restart(void);
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

/* ------------------------------------------------------------------------ */
/* Cloud word book (continued)                                              */
/* ------------------------------------------------------------------------ */

/** The worker finished: refresh the card in the LVGL task. */
static void word_cloud_done_async(void *arg)
{
    const bool enable = (arg != NULL);
    refresh_word_card();
    if (enable) {
        ui_toast("已开启云端词库");
    } else {
        ui_toast("已关闭云端词库");
    }
}

static void word_cloud_worker(void *arg)
{
    const bool enable = (arg != NULL);

    (void)word_set_cloud_mode(enable);
    if (enable) {
        /* Blocking TLS plus SD card writes - never in an LVGL callback. */
        (void)word_cloud_refresh();
    }

    (void)ui_run_async(word_cloud_done_async, (void *)(uintptr_t)(enable ? 1 : 0));
    vTaskDelete(NULL);
}

/** The user confirmed the switch: hand the work to the worker thread. */
static void word_cloud_confirm_cb(lv_event_t *e)
{
    /* The confirm button hands the message box over as its event user data
     * (see ui_dialog_confirm); the payload rides on the box. */
    lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);
    if (mbox == NULL) {
        return;
    }
    const bool enable = (lv_obj_get_user_data(mbox) != NULL);

    /* Do the work first, close the box afterwards: lv_msgbox_close() deletes
     * the box synchronously and LVGL then drops the rest of this click. */
    if (xTaskCreate(word_cloud_worker, "th_word_mode", 6144,
                    (void *)(uintptr_t)(enable ? 1 : 0), 3, NULL) != pdPASS) {
        ui_toast("操作失败：无法创建任务");
    }

    lv_msgbox_close(mbox);
}

static void word_cloud_toggle_cb(lv_event_t *e)
{
    (void)e;
    const bool enable = !word_cloud_mode();

    lv_obj_t *mbox = ui_dialog_confirm(
        enable ? "开启云端词库" : "关闭云端词库",
        enable ? "将从墨墨账号拉取今天的单词（需要联网）。确定开启？"
               : "将改用本地词书。确定关闭？",
        enable ? "开启" : "关闭", "取消",
        word_cloud_confirm_cb, (void *)(uintptr_t)(enable ? 1 : 0));
    if (mbox == NULL) {
        ui_toast("无法打开确认框");
    }
}

/* ------------------------------------------------------------------------ */
/* Card refresh                                                              */
/* ------------------------------------------------------------------------ */

/**
 * One sentence summary of the system prompt.
 *
 * The prompt can be a whole paragraph (it is editable from the web console),
 * so the device only shows its opening sentence.  The cut never happens in
 * the middle of a UTF-8 character, otherwise the last glyph would be garbage.
 */
static void ai_prompt_summary(const char *prompt, char *out, size_t out_size)
{
    const char *p = (prompt != NULL) ? prompt : "";
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (p[0] == '\0') {
        snprintf(out, out_size, "%s", "");
        return;
    }

    /* At most 120 bytes (about 40 Chinese characters); stop before a UTF-8
     * continuation byte so that the summary always ends on a whole character. */
    size_t n = 0;
    while (p[n] != '\0' && n < 120) {
        if (((unsigned char)p[n] & 0xC0u) == 0x80u) {
            break;
        }
        n++;
    }

    /* Prefer the first sentence.  The Chinese full stop is a three byte
     * sequence, so it cannot be written as a character constant (that would be
     * a multi-character constant, which the build rejects). */
    size_t end = n;
    bool truncated = true;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '!' || p[i] == '?') {
            end = i + 1;
            truncated = false;
            break;
        }
        if (i + 2 < n && (unsigned char)p[i] == 0xE3u &&
            (unsigned char)p[i + 1] == 0x80u && (unsigned char)p[i + 2] == 0x82u) {
            end = i + 3;
            truncated = false;
            break;
        }
    }

    snprintf(out, out_size, "%.*s%s", (int)end, p, truncated ? "…" : "");
}

/** Re-render the AI card. Every write is dirty-checked. */
static void refresh_ai_card(void)
{
    if (s_ai_key_label == NULL) {
        return;
    }

    /* API key: only the state is shown, the key itself never reaches the
     * screen (same rule as every other secret on this device). */
    const bool has_key = ai_service_has_api_key();
    const char *key_text = has_key ? "已配置" : "未配置";
    if (strcmp(key_text, s_ai_key_text) != 0) {
        strlcpy(s_ai_key_text, key_text, sizeof(s_ai_key_text));
        lv_label_set_text(s_ai_key_label, s_ai_key_text);
        lv_obj_set_style_text_color(s_ai_key_label,
                                    has_key ? UI_COLOR_SUCCESS : UI_COLOR_WARNING, 0);
    }

    /* Without a key the user has to go to the web console - this board has no
     * keyboard, so typing one here is impossible. */
    if (s_ai_hint_label != NULL) {
        const bool hidden = has_key;
        if (lv_obj_has_flag(s_ai_hint_label, LV_OBJ_FLAG_HIDDEN) != hidden) {
            if (hidden) {
                lv_obj_add_flag(s_ai_hint_label, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(s_ai_hint_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    /* Model: the active one is highlighted.  An unknown model name highlights
     * neither and is spelled out below instead. */
    const char *model = ai_service_get_model();
    if (strcmp(model, s_ai_model_text) != 0) {
        strlcpy(s_ai_model_text, model, sizeof(s_ai_model_text));
        for (int i = 0; i < 2; i++) {
            const bool active = (strcmp(model, s_ai_models[i]) == 0);
            set_button_color(s_ai_model_btn[i],
                             active ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE_ALT);
        }
    }
    const bool known_model = (strcmp(model, s_ai_models[0]) == 0) ||
                             (strcmp(model, s_ai_models[1]) == 0);
    if (s_ai_model_label != NULL) {
        if (known_model) {
            if (!lv_obj_has_flag(s_ai_model_label, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(s_ai_model_label, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            char text[48];
            snprintf(text, sizeof(text), "当前模型：%s", model);
            label_set_cached(s_ai_model_label, s_ai_model_name_text,
                             sizeof(s_ai_model_name_text), text);
            if (lv_obj_has_flag(s_ai_model_label, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_remove_flag(s_ai_model_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    /* Retained rounds. */
    char rounds[16];
    snprintf(rounds, sizeof(rounds), "%u", (unsigned)ai_service_get_history_rounds());
    label_set_cached(s_ai_rounds_label, s_ai_rounds_text, sizeof(s_ai_rounds_text), rounds);

    /* System prompt: read only, one sentence, plus where to change it. */
    if (s_ai_prompt_label != NULL) {
        char summary[160];
        char text[224];
        ai_prompt_summary(ai_service_get_system_prompt(), summary, sizeof(summary));
        snprintf(text, sizeof(text), "%s（可在网页端修改）", summary);
        label_set_cached(s_ai_prompt_label, s_ai_prompt_text, sizeof(s_ai_prompt_text), text);
    }
}

/** Re-render the word card. Every write is dirty-checked. */
static void refresh_word_card(void)
{
    if (s_word_book_label == NULL) {
        return;
    }

    label_set_cached(s_word_book_label, s_word_book_text, sizeof(s_word_book_text),
                     word_get_book());

    /* word_cloud_mode() is false while no cloud account is configured, so this
     * shows the state that is actually in effect. */
    set_button_text(s_word_cloud_btn, s_word_cloud_text, sizeof(s_word_cloud_text),
                    word_cloud_mode() ? "云端词库：开" : "云端词库：关");

    word_stats_t stats;
    char text[96];
    if (word_stats(&stats) == ESP_OK) {
        snprintf(text, sizeof(text), "已学 %u · 模糊 %u · 掌握 %u · 未记住 %u",
                 (unsigned)stats.learned, (unsigned)stats.fuzzy_total,
                 (unsigned)stats.known_total, (unsigned)stats.unknown_total);
    } else {
        snprintf(text, sizeof(text), "学习统计暂不可用");
    }
    label_set_cached(s_word_stats_label, s_word_stats_text, sizeof(s_word_stats_text), text);
}

/**
 * The whole page refresh (5 s tick and after every action).
 *
 * Everything is dirty-checked: this runs forever while the page is open, and
 * an unconditional lv_label_set_text() would repaint the page every time.
 */
static void refresh_info(void)
{
    if (s_info_value[0] == NULL) {
        return;
    }

    char ip[16] = { 0 };
    char mac[18] = { 0 };
    (void)wifi_manager_get_ip(ip, sizeof(ip));
    (void)wifi_manager_get_mac(mac, sizeof(mac));

    /* Version comes from version.txt at build time (see AGENT.md 8.1); it is the
     * quickest way to tell which firmware is actually flashed on the board. */
    const esp_app_desc_t *app = esp_app_get_description();

    char text[80];

    snprintf(text, sizeof(text), "%s", (app != NULL) ? app->version : "?");
    label_set_cached(s_info_value[0], s_info_cache[0], sizeof(s_info_cache[0]), text);

    snprintf(text, sizeof(text), "%s", (app != NULL) ? app->idf_ver : "?");
    label_set_cached(s_info_value[1], s_info_cache[1], sizeof(s_info_cache[1]), text);

    snprintf(text, sizeof(text), "%s", ip[0] ? ip : "未连接");
    label_set_cached(s_info_value[2], s_info_cache[2], sizeof(s_info_cache[2]), text);

    snprintf(text, sizeof(text), "%s", mac[0] ? mac : "-");
    label_set_cached(s_info_value[3], s_info_cache[3], sizeof(s_info_cache[3]), text);

    snprintf(text, sizeof(text), "%s · 剩余 %llu KB",
             storage_sd_available() ? "SD 卡" : "内置",
             (unsigned long long)(storage_free_bytes() / 1024));
    label_set_cached(s_info_value[4], s_info_cache[4], sizeof(s_info_cache[4]), text);

    snprintf(text, sizeof(text), "%s", calendar_get_timezone());
    label_set_cached(s_info_value[5], s_info_cache[5], sizeof(s_info_cache[5]), text);

    /* Timezone and clock state as one read only line. */
    if (s_tz_state_label != NULL) {
        char tz_state[64];
        snprintf(tz_state, sizeof(tz_state), "%s · %s", calendar_get_timezone(),
                 calendar_time_synced() ? "已同步" : "未同步");
        label_set_cached(s_tz_state_label, s_tz_state_text, sizeof(s_tz_state_text), tz_state);
    }

    snprintf(text, sizeof(text), "%s · %s", weather_get_provider(),
             weather_get_city()[0] ? weather_get_city() : "未设置");
    label_set_cached(s_info_value[6], s_info_cache[6], sizeof(s_info_cache[6]), text);

    /*
     * The security card is refreshed here too, not just at creation. Its URL line
     * depends on the DHCP address, and this function is the page's periodic
     * (on_show + tick) refresh point, so this is where a changed address is
     * picked up. Both writes are dirty-checked: a 1 Hz unconditional
     * lv_label_set_text would repaint the page forever.
     */
    /*
     * The IP comes first on purpose.
     *
     * The mDNS name is the pretty one, but it only resolves where mDNS really
     * works - Windows does not by default (it needs Bonjour) and some routers
     * drop multicast - and on this bench `todohelper.local` simply does not
     * open. The IP always works; the pretty name is the second choice.
     */
    char url[128];
    char ip_url[40];
    sync_server_ip_url(ip_url, sizeof(ip_url));
    const char *primary = sync_server_url();
    if (strstr(primary, ".local") != NULL && strncmp(ip_url, "http", 4) == 0) {
        snprintf(url, sizeof(url), "%s\n或 %s", ip_url, primary);
    } else {
        snprintf(url, sizeof(url), "%s", primary);
    }

    if (s_url_label != NULL) {
        if (strcmp(url, s_url_text) != 0) {
            strlcpy(s_url_text, url, sizeof(s_url_text));
            lv_label_set_text(s_url_label, s_url_text);
        }
    }
    if (s_auth_label != NULL) {
        char auth[96];
        snprintf(auth, sizeof(auth), "鉴权：%s · 二维码/网址填这个地址",
                 sync_server_auth_required() ? "已开启（网页需填 token）" : "未开启");
        if (strcmp(auth, s_auth_text) != 0) {
            strlcpy(s_auth_text, auth, sizeof(s_auth_text));
            lv_label_set_text(s_auth_label, s_auth_text);
        }
    }

    /* The address is the one value that does not fit into a two column cell. */
    label_set_cached(s_info_value[7], s_info_cache[7], sizeof(s_info_cache[7]), url);

    refresh_ai_card();
    refresh_word_card();
}

/* ------------------------------------------------------------------------ */
/* Page                                                                      */
/* ------------------------------------------------------------------------ */

/**
 * 这一页是全工程最重的一页：六张卡片、182 个控件、约 42 KB 内部 RAM。
 *
 * 所以它分成两半（ROADMAP v1.8.3 §1）：
 *
 *   - **外壳**：页面容器 + 抬头（返回按钮）。只建一次，留在原地；
 *   - **内容**：六张卡片。页面被切走时销毁，回来时重建。
 *
 * 重建只用内存里的东西：滑块的有效值在 s_pending 里，AI / 单词 / 时区 /
 * 设备信息都由各自的模块在 RAM 里缓存 —— 重建既不写 NVS 也不读 SD 卡。
 *
 * 可见内容一个字节都没变：卡片的顺序、标题、每一行的键值、字体、颜色、
 * 间距都跟改动前一模一样，这一轮只改「什么时候建、什么时候拆」。
 */
#define SETTINGS_CARD_COUNT 6

/** 六张卡片，顺序跟界面上从上到下一样。 */
static lv_obj_t *s_cards[SETTINGS_CARD_COUNT];

/** 页面容器（ui_core 建的）：重建内容、恢复滚动位置都要用它。 */
static lv_obj_t *s_page_parent;

/** 内容是否已经建好：on_show 只在「被销毁过」之后才重建。 */
static bool s_content_built;

/** 离开本页时记住的滚动位置。 */
static int32_t s_scroll_y;

/**
 * 清掉内容控件的指针，以及「屏上是什么」的缓存。
 *
 * 控件被销毁之后，缓存里的字符串已经不对应屏幕上的任何一个标签了 ——
 * 如果留着，下一次重建时 label_set_cached() 会认为「文字没变」而跳过写入，
 * 新建的标签就一直是空的。所以销毁时必须一起清掉。
 */
static void settings_state_reset(void)
{
    s_ai_key_label = NULL;
    s_ai_hint_label = NULL;
    s_ai_model_btn[0] = NULL;
    s_ai_model_btn[1] = NULL;
    s_ai_model_label = NULL;
    s_ai_rounds_label = NULL;
    s_ai_prompt_label = NULL;

    s_word_book_label = NULL;
    s_word_cloud_btn = NULL;
    s_word_stats_label = NULL;

    s_tz_state_label = NULL;

    for (int i = 0; i < INFO_ROW_COUNT; i++) {
        s_info_value[i] = NULL;
    }

    s_auth_label = NULL;
    s_url_label = NULL;

    /* 滑块：控件没了，句柄也必须清掉，否则 settings_tick() 会去读野指针。 */
    s_slider_count = 0;
    for (size_t i = 0; i < SETTINGS_MAX_SLIDERS; i++) {
        s_slider_objs[i] = NULL;
        s_slider_values[i] = NULL;
    }

    /* 「屏上是什么」的缓存，见上面的说明。 */
    memset(s_ai_key_text, 0, sizeof(s_ai_key_text));
    memset(s_ai_model_text, 0, sizeof(s_ai_model_text));
    memset(s_ai_model_name_text, 0, sizeof(s_ai_model_name_text));
    memset(s_ai_rounds_text, 0, sizeof(s_ai_rounds_text));
    memset(s_ai_prompt_text, 0, sizeof(s_ai_prompt_text));
    memset(s_word_book_text, 0, sizeof(s_word_book_text));
    memset(s_word_cloud_text, 0, sizeof(s_word_cloud_text));
    memset(s_word_stats_text, 0, sizeof(s_word_stats_text));
    memset(s_tz_state_text, 0, sizeof(s_tz_state_text));
    memset(s_info_cache, 0, sizeof(s_info_cache));
    memset(s_url_text, 0, sizeof(s_url_text));
    memset(s_auth_text, 0, sizeof(s_auth_text));
}

/**
 * 销毁六张卡片（内容）。
 *
 * 只删自己建的对象，外壳（抬头、返回按钮）留着 —— 它们很少，
 * 销毁重建只会让页面闪一下。
 *
 * 这一步是这一页省内存的关键：ui_core 切页时只是把别的页面藏起来
 * （lv_obj_add_flag(HIDDEN)），对象并不销毁，所以「逛过这一页」
 * 会让内部堆永久少 42 KB（ROADMAP v1.8.3 §0/§1）。
 */
static void settings_content_destroy(void)
{
    for (int i = 0; i < SETTINGS_CARD_COUNT; i++) {
        if (s_cards[i] != NULL) {
            lv_obj_delete(s_cards[i]);
            s_cards[i] = NULL;
        }
    }
    settings_state_reset();
    s_content_built = false;
}

/** 三个滑块的定义（初值在重建时从 s_pending 里取）。 */
static setting_slider_t s_brightness = {
    .title = "屏幕亮度",
    .min = 5,
    .max = 100,
    .on_change = brightness_changed,
};
static setting_slider_t s_idle_bright = {
    /* 100 = the idle face keeps its pure white eyes; lower to dim the panel
     * while idle (also the default in ui_core.c). */
    .title = "待机亮度",
    .min = 0,
    .max = 100,
    .on_change = idle_brightness_changed,
};
static setting_slider_t s_idle_timeout = {
    .title = "待机超时(秒)",
    .min = 5,
    .max = 300,
    .on_change = idle_timeout_changed,
};

/**
 * 建六张卡片（内容）。
 *
 * 顺序、标题、每一行的键值、字体、颜色、间距都跟改动前一字不差 ——
 * 这一轮只改「什么时候建、什么时候拆」。
 */
static esp_err_t settings_content_build(lv_obj_t *parent)
{
    /* 上一次建到一半失败时留下的卡片先清掉，免得叠出两套。 */
    settings_content_destroy();

    /* ---- Display ---- */
    lv_obj_t *display = NULL;
    s_cards[0] = add_card(parent, "显示与待机", &display);
    if (s_cards[0] == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * 滑块的初值来自内存里的有效值：第一次进来时从 NVS 读一次
     * （NVS 分区在 RAM 里，读它不碰 flash），之后用户拖动就改这个值
     * （s_pending）。所以重建既不读盘也不写盘，而且屏上的数字跟
     * 离开本页时一模一样。
     */
    s_brightness.value = s_pending.brightness;
    s_idle_bright.value = s_pending.idle_brightness;
    s_idle_timeout.value = s_pending.idle_timeout;

    setting_slider_t *defs[] = { &s_brightness, &s_idle_bright, &s_idle_timeout };
    s_slider_count = 0;
    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]) && s_slider_count < SETTINGS_MAX_SLIDERS;
         i++) {
        lv_obj_t *row = add_slider(display, defs[i]);
        if (row == NULL) {
            break;   /* 内存不够：这一行没有，剩下两行照建 */
        }
        /* add_slider stored the value label in the slider's user data. */
        lv_obj_t *slider = lv_obj_get_child(row, 1);
        s_slider_objs[s_slider_count] = slider;
        s_slider_values[s_slider_count] = (slider != NULL) ? lv_obj_get_user_data(slider) : NULL;
        s_slider_count++;
    }

    /* ---- AI ---- */
    lv_obj_t *ai_body = NULL;
    s_cards[1] = add_card(parent, "AI 对话", &ai_body);
    if (s_cards[1] == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* API key state.  The key itself is never shown - requirement 8.8. */
    lv_obj_t *key_row = add_row(ai_body);
    lv_obj_t *key_name = lv_label_create(key_row);
    lv_label_set_text(key_name, "API Key");
    lv_obj_set_style_text_font(key_name, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(key_name, UI_COLOR_TEXT, 0);

    s_ai_key_label = lv_label_create(key_row);
    lv_obj_set_flex_grow(s_ai_key_label, 1);
    lv_obj_set_style_text_align(s_ai_key_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_ai_key_label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_ai_key_label, UI_COLOR_WARNING, 0);

    /* This board has no keyboard, so a missing key can only be filled in from
     * the web console. */
    s_ai_hint_label = lv_label_create(ai_body);
    lv_obj_set_width(s_ai_hint_label, LV_PCT(100));
    lv_label_set_long_mode(s_ai_hint_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_ai_hint_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_ai_hint_label, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_ai_hint_label, "本机没有键盘，请在网页端配置");

    /* Model selection. */
    lv_obj_t *model_row = add_row(ai_body);
    for (int i = 0; i < 2; i++) {
        s_ai_model_btn[i] = add_button(model_row, s_ai_models[i], ai_model_clicked_cb,
                                       s_ai_models[i], UI_COLOR_SURFACE_ALT);
    }

    /* Only shown when the configured model is neither of the two known ones. */
    s_ai_model_label = lv_label_create(ai_body);
    lv_obj_set_width(s_ai_model_label, LV_PCT(100));
    lv_obj_set_style_text_font(s_ai_model_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_ai_model_label, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_add_flag(s_ai_model_label, LV_OBJ_FLAG_HIDDEN);

    /* Retained rounds. */
    lv_obj_t *rounds_row = add_row(ai_body);
    lv_obj_t *rounds_name = lv_label_create(rounds_row);
    lv_label_set_text(rounds_name, "保留轮数");
    lv_obj_set_style_text_font(rounds_name, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(rounds_name, UI_COLOR_TEXT, 0);

    /* The two steppers are narrow: the value in between grows instead. */
    lv_obj_t *minus = add_button(rounds_row, "-", ai_rounds_clicked_cb,
                                 (void *)(intptr_t)-1, UI_COLOR_SURFACE_ALT);
    lv_obj_set_flex_grow(minus, 0);
    lv_obj_set_width(minus, 56);

    s_ai_rounds_label = lv_label_create(rounds_row);
    lv_obj_set_flex_grow(s_ai_rounds_label, 1);
    lv_obj_set_style_text_align(s_ai_rounds_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ai_rounds_label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_ai_rounds_label, UI_COLOR_PRIMARY, 0);

    lv_obj_t *plus = add_button(rounds_row, "+", ai_rounds_clicked_cb,
                                (void *)(intptr_t)1, UI_COLOR_SURFACE_ALT);
    lv_obj_set_flex_grow(plus, 0);
    lv_obj_set_width(plus, 56);

    /* System prompt: read only, one sentence. */
    s_ai_prompt_label = lv_label_create(ai_body);
    lv_obj_set_width(s_ai_prompt_label, LV_PCT(100));
    lv_label_set_long_mode(s_ai_prompt_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_ai_prompt_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_ai_prompt_label, UI_COLOR_TEXT_MUTED, 0);

    lv_obj_t *ai_row = add_row(ai_body);
    add_button(ai_row, "清空对话", ai_clear_clicked_cb, NULL, UI_COLOR_DANGER);

    /* ---- Time ---- */
    lv_obj_t *time_card = NULL;
    s_cards[2] = add_card(parent, "时间与时区", &time_card);
    if (s_cards[2] == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_t *tz_row = add_row(time_card);
    static const char *const tz_presets[] = { "CST-8", "UTC0", "JST-9", "EST5" };
    for (size_t i = 0; i < sizeof(tz_presets) / sizeof(tz_presets[0]); i++) {
        add_button(tz_row, tz_presets[i], tz_clicked_cb, tz_presets[i], UI_COLOR_SURFACE_ALT);
    }

    lv_obj_t *time_row = add_row(time_card);
    add_button(time_row, "NTP 校时", sync_ntp_cb, NULL, UI_COLOR_PRIMARY);
    add_button(time_row, "当前城市", city_clicked_cb, NULL, UI_COLOR_SURFACE_ALT);

    /* Read only status line: which timezone is active and whether the clock
     * has ever been synchronised. */
    s_tz_state_label = lv_label_create(time_card);
    lv_obj_set_width(s_tz_state_label, LV_PCT(100));
    lv_obj_set_style_text_font(s_tz_state_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_tz_state_label, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_tz_state_label, "CST-8 · 未同步");

    /* ---- Words ---- */
    lv_obj_t *word_card = NULL;
    s_cards[3] = add_card(parent, "单词", &word_card);
    if (s_cards[3] == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_t *word_info = add_row(word_card);
    lv_obj_t *book_name = lv_label_create(word_info);
    lv_label_set_text(book_name, "当前词书");
    lv_obj_set_style_text_font(book_name, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(book_name, UI_COLOR_TEXT, 0);

    s_word_book_label = lv_label_create(word_info);
    lv_obj_set_flex_grow(s_word_book_label, 1);
    lv_obj_set_style_text_align(s_word_book_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_word_book_label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_word_book_label, UI_COLOR_PRIMARY, 0);

    /* Cloud book switch: switching pulls a new queue, so it asks first. */
    lv_obj_t *cloud_row = add_row(word_card);
    s_word_cloud_btn = add_button(cloud_row, "云端词库：关", word_cloud_toggle_cb,
                                  NULL, UI_COLOR_SURFACE_ALT);

    lv_obj_t *word_row = add_row(word_card);
    add_button(word_row, "每日 -5", word_count_down_cb, NULL, UI_COLOR_SURFACE_ALT);
    add_button(word_row, "每日 +5", word_count_up_cb, NULL, UI_COLOR_SURFACE_ALT);
    add_button(word_row, "清空进度", word_reset_cb, NULL, UI_COLOR_DANGER);

    s_word_stats_label = lv_label_create(word_card);
    lv_obj_set_width(s_word_stats_label, LV_PCT(100));
    lv_obj_set_style_text_font(s_word_stats_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_word_stats_label, UI_COLOR_TEXT_MUTED, 0);

    /* ---- Security ---- */
    lv_obj_t *sec_card = NULL;
    s_cards[4] = add_card(parent, "局域网与安全", &sec_card);
    if (s_cards[4] == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * The web console address gets its own prominent row.
     *
     * It is the one string on this page the user has to read and then type into a
     * browser on another device, so it is rendered at the title font in the
     * primary colour on its own line - not buried in the muted info block.
     * Refreshed in refresh_info() so a DHCP change is reflected.
     */
    s_url_label = lv_label_create(sec_card);
    lv_obj_set_width(s_url_label, LV_PCT(100));
    lv_label_set_long_mode(s_url_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_url_label, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(s_url_label, UI_COLOR_PRIMARY, 0);

    s_auth_label = lv_label_create(sec_card);
    lv_obj_set_style_text_font(s_auth_label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_auth_label, UI_COLOR_TEXT, 0);

    lv_obj_t *sec_row = add_row(sec_card);
    add_button(sec_row, "生成并开启 token", auth_enable_cb, NULL, UI_COLOR_PRIMARY);
    add_button(sec_row, "关闭鉴权", auth_disable_cb, NULL, UI_COLOR_SURFACE_ALT);

    /* ---- Info ---- */
    lv_obj_t *info_card = NULL;
    s_cards[5] = add_card(parent, "设备信息", &info_card);
    if (s_cards[5] == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Two column key/value grid: the key is muted, the value is readable.
     * Eight rows at most, the address gets a row of its own. */
    add_kv_row(info_card, "固件", 0);
    add_kv_row(info_card, "IDF", 1);
    add_kv_row(info_card, "IP", 2);
    add_kv_row(info_card, "MAC", 3);
    add_kv_row(info_card, "存储", 4);
    add_kv_row(info_card, "时区", 5);
    add_kv_row(info_card, "天气", 6);
    add_kv_wide_row(info_card, "网页地址", 7);

    lv_obj_t *info_row = add_row(info_card);
    add_button(info_row, "重启设备", reboot_cb, NULL, UI_COLOR_DANGER);

    s_content_built = true;
    return ESP_OK;
}

/**
 * 页面外壳：抬头 + 定时器 + 事件订阅。
 *
 * 只跑一次（ui_core 建页面容器时调一次），所以事件订阅放在这里是安全的 ——
 * 重建内容时**绝不能**再订阅一次，否则同一个事件会被处理两遍
 * （ROADMAP v1.8.3 §1 硬约束一）。
 */
static esp_err_t settings_create(lv_obj_t *parent)
{
    s_page_parent = parent;

    (void)page_header_create(parent, "设置");

    /*
     * 滑块的初值只在这里读一次 NVS（NVS 分区在 RAM 里，读它不碰 flash）。
     * 之后用户拖动就改 s_pending —— 重建时用的是它，所以重建路径上
     * 一次 flash 访问都没有（ROADMAP v1.8.3 §1 硬约束二）。
     */
    s_pending.brightness = sys_config_get_i32_or(SYS_KEY_BRIGHTNESS, 100);
    s_pending.idle_brightness = sys_config_get_i32_or(SYS_KEY_IDLE_BRIGHT, 100);
    s_pending.idle_timeout = sys_config_get_i32_or(SYS_KEY_IDLE_TIMEOUT, 30);
    s_pending.brightness_dirty = false;
    s_pending.idle_brightness_dirty = false;
    s_pending.idle_timeout_dirty = false;

    const esp_err_t err = settings_content_build(parent);
    if (err != ESP_OK) {
        return err;
    }

    refresh_info();
    /* Owned by the page so on_hide() can stop it (see settings_tick). */
    s_slider_timer = lv_timer_create(settings_tick, 250, NULL);
    if (s_slider_timer != NULL) {
        lv_timer_pause(s_slider_timer);
    }

    /* 滑块落盘的去抖定时器：同样由页面拥有，on_hide() 暂停。 */
    s_save_timer = lv_timer_create(slider_save_tick, 600, NULL);
    if (s_save_timer != NULL) {
        lv_timer_pause(s_save_timer);
    }

    /* The NTP outcome is delivered on the LVGL task by ui_core, so the toast can
     * be shown without the page ever blocking on the network.
     * 订阅只在这里做一次（create() 只跑一次），重建内容时不再订阅。 */
    (void)ui_event_subscribe(CALENDAR_EVENT, CALENDAR_EVENT_SYNC_RESULT,
                             sizeof(esp_err_t), settings_on_sync_result);

    SYS_LOGD(TAG, "settings page built");
    return ESP_OK;
}

/**
 * 页面回来之后把滚动位置复原。
 *
 * 内容被销毁又重建之后，容器的内容高度变了：内容不够高时 LVGL 会把滚动
 * 位置夹回 0（readjust_scroll），所以这里按记住的值复原。
 *
 * 判据是「跟记住的值一样吗」，**不是**「是不是 0」：夹断后的值不是 0，
 * 只看「是不是 0」会直接返回 → 位置根本没复原（AGENT.md 9.0.1.6 第 12 条）。
 */
static void settings_scroll_restore(void)
{
    if (s_page_parent == NULL || s_scroll_y <= 0) {
        return;
    }
    if (lv_obj_get_scroll_y(s_page_parent) == s_scroll_y) {
        /* 位置正是我们要的：什么都不不要做。 */
        return;
    }
    /* LV_ANIM_OFF：不带动画，直接落位。 */
    lv_obj_scroll_to_y(s_page_parent, s_scroll_y, LV_ANIM_OFF);
}

static esp_err_t settings_on_show(void)
{
    /*
     * 内容在 on_hide 里被销毁了，这里按内存里的缓存重建。
     * 重建只碰 RAM：不写 NVS、不读盘（ROADMAP v1.8.3 §1）。
     */
    if (!s_content_built && s_page_parent != NULL) {
        if (settings_content_build(s_page_parent) != ESP_OK) {
            SYS_LOGE(TAG, "the settings content could not be rebuilt");
        }
    }

    refresh_info();

    /* 回到离开本页时的滚动位置（跟改动前的观感一致）。 */
    settings_scroll_restore();

    if (s_slider_timer != NULL) {
        lv_timer_resume(s_slider_timer);
    }
    if (s_save_timer != NULL) {
        lv_timer_resume(s_save_timer);
    }
    return ESP_OK;
}

static esp_err_t settings_on_hide(void)
{
    if (s_slider_timer != NULL) {
        lv_timer_pause(s_slider_timer);
    }
    if (s_save_timer != NULL) {
        /* 刚拖完就离开页面：先把值落盘，别把用户刚调的值丢掉。 */
        slider_save_flush();
        lv_timer_pause(s_save_timer);
    }

    /* 记住滚动位置：卡片马上要被销毁，容器的内容高度会塌回去。 */
    if (s_page_parent != NULL) {
        /*
         * 惯性滚动（手指甩出去之后的那段动画）要停掉。
         * 否则记住的是**动画中途**的值，而动画还在跑 —— 回来时复原到的位置
         * 会跟动画打架（两边都以为自己说了算），位置就飘了。
         */
        lv_anim_delete(s_page_parent, NULL);
        s_scroll_y = lv_obj_get_scroll_y(s_page_parent);
    }

    /*
     * 卡片是这一页最大的一批对象（182 个控件，约 42 KB 内部 RAM）。
     * ui_core 切页时只是把别的页面藏起来，对象并不销毁，所以「逛过这一页」
     * 会让内部堆永久少 42 KB（ROADMAP v1.8.3 §0/§1）。这里把它们销毁掉，
     * 页面外壳（抬头、返回按钮）留着。
     *
     * on_hide 由 ui_core 的 page_switch_locked() 调用，跑在 LVGL 任务里，
     * 所以这里动 LVGL 对象是安全的（不在 httpd 回调里）。
     */
    const int released = s_content_built ? SETTINGS_CARD_COUNT : 0;
    settings_content_destroy();

    SYS_LOGD(TAG, "settings page hidden: %d cards released", released);
    return ESP_OK;
}

static esp_err_t settings_on_tick(void)
{
    refresh_info();
    return ESP_OK;
}

const ui_page_t page_settings = {
    .id = UI_PAGE_SETTINGS,
    .name = "settings",
    .create = settings_create,
    .on_show = settings_on_show,
    .on_hide = settings_on_hide,
    .on_tick = settings_on_tick,
    .tick_period_ms = 5000,
};
