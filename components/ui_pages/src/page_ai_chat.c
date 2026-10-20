/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_ai_chat.c
 * @brief FR7: bubble-style chat page with an on-screen keyboard.
 *
 * The device has no way to type Chinese on its own keyboard, so this page is the
 * *display* half of the conversation (bubbles + state), while the PC browser and
 * the HTTP API are the practical input paths. Speaking the question is the P2
 * voice path handled by voice_service.
 *
 * The page never calls the model directly: it queues a turn and renders whatever
 * ai_service reports through events, so a slow model cannot freeze the UI.
 */

#include <stdio.h>
#include <string.h>

#include "ai_service.h"
#include "pages_internal.h"
#include "esp_attr.h"

static const char *TAG = "page_ai";

#define AI_CHAT_MAX_BUBBLES 40

/** 开场白：第一次进来（还没有对话）时的那一条气泡。 */
#define AI_CHAT_GREETING \
    "你好，我是这个设备的 AI 助手。\n" \
    "在电脑浏览器打开设备地址，或调用 POST /api/v1/ai/chat 就能对话。"

/** 「新对话」之后的那一条气泡。 */
#define AI_CHAT_NEW_CONVERSATION \
    "新对话已开始。可以在电脑浏览器里用 /api/v1/ai/chat 提问，" \
    "回答会显示在这里。"

static lv_obj_t *s_scroll;
static lv_obj_t *s_state_label;
static lv_obj_t *s_input_overlay;
static lv_obj_t *s_input_area;
static bool s_waiting_reply;

/**
 * 页面是否在屏幕上（on_show 置真、on_hide 置假）。
 *
 * 隐藏时收到的事件只往内存里攒数据，不建控件 —— 铁律四：隐藏的页面不许
 * 继续干活。ui_core 会把带数据的事件照样发给隐藏的页面（那是内容，不能丢），
 * 所以这里必须自己判断。
 */
static bool s_visible;

/**
 * 开场气泡是哪一条。
 *
 * 这两条气泡是页面自己加的，不在 ai_service 的对话历史里，所以重建时要单独
 * 贴回去，否则回来就少了一条。
 */
typedef enum {
    AI_INTRO_NONE = 0,             /**< 没有开场气泡 */
    AI_INTRO_GREETING,             /**< 第一次进来时的问候 */
    AI_INTRO_NEW_CONVERSATION,     /**< 「新对话」之后的那一条 */
} ai_intro_t;

static ai_intro_t s_intro = AI_INTRO_NONE;

/**
 * 设备上刚输入、还没进对话历史的那条提问。
 *
 * worker 线程把这条提问写进历史要等一小会儿（离线时根本不会写），这中间如果
 * 离开本页，重建时得靠它把气泡贴回去。放在 PSRAM 里（EXT_RAM_BSS_ATTR）：
 * 内部 RAM 太紧张（AGENT.md 9.0）。
 */
static EXT_RAM_BSS_ATTR char s_pending_user[AI_MESSAGE_MAX];

/**
 * 记住的滚动位置。
 *
 * 气泡被销毁之后列表的内容高度变了，回来时要按这个位置复原（跟列表页的
 * s_scroll_y 一个道理）。
 */
static int32_t s_scroll_y;

/* ------------------------------------------------------------------------ */
/* Bubbles                                                                   */
/* ------------------------------------------------------------------------ */

/**
 * Bubbles are reused for streamed replies.
 *
 * A streaming answer arrives as many AI_EVENT_REPLY_CHUNK events. Creating one
 * bubble per chunk (the old behaviour) built dozens of widget trees for a single
 * answer, and each one started its own `LV_ANIM_ON` scroll - overlapping
 * animations on the same container, plus a new object every few words
 * (smoothness audit P2).
 *
 * Now chunks are appended into one bubble, and the follow-the-text scroll is
 * throttled so a fast stream cannot queue up animations.
 */
#define AI_CHAT_SCROLL_MS 120
/** Accumulation buffer for the bubble currently being streamed into. */
#define AI_CHAT_STREAM_MAX 2048

static lv_obj_t *s_stream_bubble;      /**< bubble currently receiving chunks */
static lv_obj_t *s_stream_label;
static EXT_RAM_BSS_ATTR char s_stream_text[AI_CHAT_STREAM_MAX];
static size_t s_stream_len;
static uint32_t s_last_scroll_ms;

/**
 * 一段流式回答正在进行中。
 *
 * 与 s_stream_bubble 分开：气泡是控件，页面隐藏时会被销毁；这段文本不会
 * （它就存在 s_stream_text 里），回来时照着它重建气泡。
 */
static bool s_stream_active;

/** Create one empty bubble; *out_label receives its label. */
static lv_obj_t *bubble_create(bool from_user, lv_obj_t **out_label)
{
    lv_obj_t *bubble = lv_obj_create(s_scroll);
    if (bubble == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(bubble);
    lv_obj_set_width(bubble, LV_PCT(78));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, from_user ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE_ALT, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, UI_CARD_RADIUS, 0);
    lv_obj_set_style_pad_all(bubble, 12, 0);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    /* Align left/right to make the speaker obvious without extra chrome. */
    lv_obj_set_style_align(bubble, from_user ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, 0);

    lv_obj_t *label = lv_label_create(bubble);
    if (label == NULL) {
        /* 半成品不能留在列表里：删掉它。 */
        lv_obj_delete(bubble);
        return NULL;
    }
    lv_label_set_text(label, "");
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(label, from_user ? UI_COLOR_ON_PRIMARY : UI_COLOR_TEXT, 0);

    if (out_label != NULL) {
        *out_label = label;
    }
    return bubble;
}

/** Throttled follow-the-text scroll: at most one animation per AI_CHAT_SCROLL_MS. */
static void bubble_scroll_to(lv_obj_t *bubble, bool animate)
{
    if (bubble == NULL) {
        return;
    }
    const uint32_t now = (uint32_t)sys_time_ms();
    if (animate && (now - s_last_scroll_ms) < AI_CHAT_SCROLL_MS) {
        return;
    }
    s_last_scroll_ms = now;
    lv_obj_scroll_to_view(bubble, animate ? LV_ANIM_ON : LV_ANIM_OFF);
}

/** Keep the transcript bounded: an idle device running for days must not grow. */
static void bubble_trim(void)
{
    while (lv_obj_get_child_count(s_scroll) > AI_CHAT_MAX_BUBBLES) {
        lv_obj_t *oldest = lv_obj_get_child(s_scroll, 0);
        if (oldest == NULL || oldest == s_stream_bubble) {
            break;   /* never delete the bubble still being streamed into */
        }
        lv_obj_delete(oldest);
    }
}

static void add_bubble(const char *text, bool from_user)
{
    if (s_scroll == NULL || text == NULL) {
        return;
    }

    lv_obj_t *label = NULL;
    lv_obj_t *bubble = bubble_create(from_user, &label);
    if (bubble == NULL || label == NULL) {
        /* 内存不够：这一个气泡建不出来，放弃它（不往里写半个气泡）。 */
        return;
    }
    lv_label_set_text(label, text);
    bubble_trim();
    bubble_scroll_to(bubble, true);
}

/**
 * 删掉所有气泡（内容），滚动容器本身留着。
 *
 * 气泡都是这个页面自己建的（s_scroll 的子对象就是气泡），所以逐个删干净；
 * 滚动容器是外壳，不动它。
 */
static void bubbles_clear(void)
{
    if (s_scroll == NULL) {
        return;
    }
    uint32_t count = lv_obj_get_child_count(s_scroll);
    while (count-- > 0u) {
        lv_obj_t *child = lv_obj_get_child(s_scroll, 0);
        if (child != NULL) {
            lv_obj_delete(child);
        }
    }
    /* 流式气泡也是刚被删掉的：控件没了，文本留着（s_stream_text）。 */
    s_stream_bubble = NULL;
    s_stream_label = NULL;
}

/**
 * 屏幕上最后一条气泡是不是就是这段文字。
 *
 * 用来判断「这条回答是不是已经贴上去了」：
 *   - 页面隐藏时回来的那一次重建，已经按对话历史把回答贴回去了，
 *     再贴一次就重了（ui_core 会把隐藏期间收到的事件在页面回来时重放一遍）；
 *   - 流式输出结束后，历史里也已经有这条回答了，重建时不能再补一个气泡。
 */
static bool tail_shows_text(const char *text)
{
    if (s_scroll == NULL || text == NULL) {
        return false;
    }
    const uint32_t count = lv_obj_get_child_count(s_scroll);
    if (count == 0u) {
        return false;
    }
    lv_obj_t *last = lv_obj_get_child(s_scroll, count - 1u);
    if (last == NULL) {
        return false;
    }
    lv_obj_t *label = lv_obj_get_child(last, 0);
    if (label == NULL) {
        return false;
    }
    return strcmp(lv_label_get_text(label), text) == 0;
}

/**
 * 重建对话气泡（内容）。
 *
 * 数据全部来自内存：
 *   - ai_service 的对话历史（RAM 里的快照；system 那条不显示）；
 *   - 设备上刚输入、还没进历史的那条提问（s_pending_user）；
 *   - 正在流式输出的那段文本（s_stream_text）。
 *
 * 不读盘、不写 NVS、不发网络请求（ROADMAP v1.8.3 §1 约束 2）。
 */
static void bubbles_build(void)
{
    if (s_scroll == NULL) {
        return;
    }

    bubbles_clear();

    /* 开场白 / 新对话提示：页面自己的气泡，不在对话历史里。 */
    switch (s_intro) {
    case AI_INTRO_GREETING:
        add_bubble(AI_CHAT_GREETING, false);
        break;
    case AI_INTRO_NEW_CONVERSATION:
        add_bubble(AI_CHAT_NEW_CONVERSATION, false);
        break;
    case AI_INTRO_NONE:
    default:
        break;
    }

    /* 对话历史：system 那条不显示。 */
    bool pending_found = false;
    const size_t count = ai_service_history_count();
    for (size_t i = 0; i < count; i++) {
        ai_message_t msg;
        if (ai_service_history_get(i, &msg) != ESP_OK) {
            continue;
        }
        if (strcmp(msg.role, "system") == 0) {
            continue;
        }
        const bool from_user = (strcmp(msg.role, "user") == 0);
        if (from_user && s_pending_user[0] != '\0' &&
            strcmp(msg.content, s_pending_user) == 0) {
            pending_found = true;
        }
        add_bubble(msg.content, from_user);
    }

    /* 设备上刚输入的那条提问还没进历史（离线，或者请求刚发出去）：
     * 补在最后，跟它当时出现在屏幕上的位置一样。 */
    if (pending_found) {
        s_pending_user[0] = '\0';
    } else if (s_pending_user[0] != '\0') {
        add_bubble(s_pending_user, true);
    }

    /* 正在流式输出的那段：控件被销毁了，文本还在，补一个气泡 ——
     * 除非屏幕上最后一条已经是它了（回答已经结束、进了对话历史）。 */
    if (s_stream_active && s_stream_len > 0u && !tail_shows_text(s_stream_text)) {
        s_stream_bubble = bubble_create(false, &s_stream_label);
        if (s_stream_bubble != NULL) {
            lv_label_set_text_static(s_stream_label, s_stream_text);
        }
    }
}

/**
 * Append a streamed chunk to the reply bubble, creating it on the first chunk.
 *
 * The label is re-pointed at s_stream_text, which grows one chunk at a time, so a
 * long answer costs one widget tree instead of one per chunk.
 *
 * 页面不在屏幕上时只攒文本，不建控件（铁律四）：控件留给回来的时候建。
 */
static void stream_chunk(const char *chunk)
{
    if (s_scroll == NULL || chunk == NULL) {
        return;
    }

    if (!s_stream_active) {
        /* 第一块：一段新的回答。 */
        s_stream_active = true;
        s_stream_len = 0;
        s_stream_text[0] = '\0';
        s_stream_bubble = NULL;
    }

    size_t n = strlen(chunk);
    if (n > sizeof(s_stream_text) - 1u - s_stream_len) {
        n = sizeof(s_stream_text) - 1u - s_stream_len;
        if (n == 0u) {
            /* Buffer full: start a fresh bubble so the tail is not lost.
             * 页面不在屏幕上时没有气泡可开，这段文字就截断在这里。 */
            if (s_visible) {
                s_stream_bubble = bubble_create(false, &s_stream_label);
                s_stream_len = 0;
                s_stream_text[0] = '\0';
                n = strlen(chunk);
                if (n > sizeof(s_stream_text) - 1u) {
                    n = sizeof(s_stream_text) - 1u;
                }
            }
        }
    }
    memcpy(&s_stream_text[s_stream_len], chunk, n);
    s_stream_len += n;
    s_stream_text[s_stream_len] = '\0';

    if (!s_visible) {
        /* 页面不在屏幕上：文本攒着，回来的时候再建气泡。 */
        return;
    }

    if (s_stream_bubble == NULL) {
        s_stream_bubble = bubble_create(false, &s_stream_label);
        if (s_stream_bubble == NULL) {
            /* 建不出气泡：这段文本先攒着，等下一个气泡（或者页面回来时）。 */
            return;
        }
    }

    /* set_text_static: the label borrows our buffer, so a growing answer does not
     * re-allocate on every chunk. */
    lv_label_set_text_static(s_stream_label, s_stream_text);
    bubble_trim();
    bubble_scroll_to(s_stream_bubble, true);
}

/** End the current streamed bubble (reply finished, failed, or a new question). */
static void stream_finish(void)
{
    s_stream_bubble = NULL;
    s_stream_label = NULL;
    s_stream_len = 0;
    s_stream_text[0] = '\0';
    s_stream_active = false;
}

static void render_state(void)
{
    if (s_state_label == NULL) {
        return;
    }

    const ai_state_t state = ai_service_get_state();
    static const char *const names[] = { "空闲", "聆听中", "识别中", "思考中", "回答中", "错误" };
    const char *name = (state < 6) ? names[state] : "?";

    if (!ai_service_has_api_key()) {
        lv_label_set_text(s_state_label,
                          "未配置 DeepSeek API Key\n"
                          "POST /api/v1/ai/config {\"api_key\":\"sk-...\"}");
        lv_obj_set_style_text_color(s_state_label, UI_COLOR_WARNING, 0);
        return;
    }

    lv_label_set_text_fmt(s_state_label, "状态：%s · 模型 %s · 上下文 %u 条",
                          name, ai_service_get_model(),
                          (unsigned)ai_service_history_count());
    lv_obj_set_style_text_color(s_state_label,
                                state == AI_STATE_ERROR ? UI_COLOR_DANGER : UI_COLOR_TEXT_MUTED, 0);
}

/* ------------------------------------------------------------------------ */
/* Input                                                                     */
/* ------------------------------------------------------------------------ */

static void input_close(void)
{
    if (s_input_overlay != NULL) {
        lv_obj_delete(s_input_overlay);
        s_input_overlay = NULL;
        s_input_area = NULL;
    }
}

static void input_cancel_cb(lv_event_t *e)
{
    (void)e;
    input_close();
}

static void input_submit_cb(lv_event_t *e)
{
    (void)e;
    if (s_input_area == NULL) {
        return;
    }

    const char *text = lv_textarea_get_text(s_input_area);
    if (text == NULL || text[0] == '\0') {
        ui_toast("请输入内容");
        return;
    }

    add_bubble(text, true);
    /* 记下这条提问：worker 要过一小会儿才把它写进对话历史（离线时根本不会
     * 写），这中间如果离开本页，重建时得靠它把气泡贴回去。
     * 注意要赶在 input_close() 之前拷贝 —— 浮层一删，textarea 的文本就没了。 */
    strlcpy(s_pending_user, text, sizeof(s_pending_user));
    /* A new turn starts a new reply bubble, so the previous stream cannot be
     * appended into. */
    stream_finish();

    esp_err_t err = ai_service_chat_async(text);
    input_close();

    if (err == ESP_OK) {
        s_waiting_reply = true;
        ui_toast("已发送，等待回答…");
    } else if (err == ESP_ERR_INVALID_STATE) {
        ui_toast("上一轮还没结束，请稍候");
    } else {
        ui_toast("发送失败：%s", esp_err_to_name(err));
    }
}

static void input_open(void)
{
    if (s_input_overlay != NULL) {
        return;
    }

    s_input_overlay = lv_obj_create(lv_layer_top());
    if (s_input_overlay == NULL) {
        return;
    }
    lv_obj_remove_style_all(s_input_overlay);
    lv_obj_set_size(s_input_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_input_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_input_overlay, LV_OPA_70, 0);
    lv_obj_set_flex_flow(s_input_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_input_overlay, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_input_overlay, 8, 0);
    lv_obj_set_style_pad_row(s_input_overlay, 8, 0);
    lv_obj_remove_flag(s_input_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row = lv_obj_create(s_input_overlay);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    s_input_area = lv_textarea_create(row);
    lv_textarea_set_one_line(s_input_area, true);
    lv_textarea_set_max_length(s_input_area, AI_MESSAGE_MAX - 1);
    /* Chinese needs an IME, which this device does not have: Latin input works
     * here, and Chinese questions are best typed in the PC browser. */
    lv_textarea_set_placeholder_text(s_input_area, "输入问题（英文/数字；中文请用电脑网页）");
    lv_obj_set_flex_grow(s_input_area, 1);
    lv_obj_set_style_text_font(s_input_area, ui_font(UI_FONT_BODY), 0);

    lv_obj_t *ok = lv_button_create(row);
    lv_obj_set_height(ok, UI_TOUCH_MIN);
    lv_obj_t *ok_label = lv_label_create(ok);
    lv_label_set_text(ok_label, "发送");
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok, input_submit_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel = lv_button_create(row);
    lv_obj_set_height(cancel, UI_TOUCH_MIN);
    lv_obj_set_style_bg_color(cancel, UI_COLOR_SURFACE_ALT, 0);
    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, input_cancel_cb, LV_EVENT_CLICKED, NULL);

    (void)ui_keyboard_create(s_input_overlay, s_input_area);
}

/* ------------------------------------------------------------------------ */
/* Toolbar                                                                   */
/* ------------------------------------------------------------------------ */

static void send_clicked_cb(lv_event_t *e)
{
    (void)e;
    input_open();
}

static void new_clicked_cb(lv_event_t *e)
{
    (void)e;
    (void)ai_service_clear_history();
    /* 气泡是「内容」：逐个删掉（滚动容器留着）。bubbles_clear() 也会把流式
     * 气泡的指针清掉，免得它变成野指针。 */
    bubbles_clear();
    stream_finish();
    s_waiting_reply = false;
    /* 这条气泡不在对话历史里，记下来，重建的时候要贴回去。 */
    s_intro = AI_INTRO_NEW_CONVERSATION;
    s_pending_user[0] = '\0';
    add_bubble(AI_CHAT_NEW_CONVERSATION, false);
    render_state();
}

static void abort_clicked_cb(lv_event_t *e)
{
    (void)e;
    (void)ai_service_abort();
    ui_toast("已请求中断");
}

/* ------------------------------------------------------------------------ */
/* Page                                                                      */
/* ------------------------------------------------------------------------ */

static esp_err_t ai_chat_create(lv_obj_t *parent)
{
    (void)page_header_create(parent, "AI 对话");

    s_state_label = lv_label_create(parent);
    lv_obj_set_width(s_state_label, LV_PCT(100));
    lv_label_set_long_mode(s_state_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_state_label, ui_font(UI_FONT_SMALL), 0);
    lv_label_set_text(s_state_label, "");

    s_scroll = lv_obj_create(parent);
    if (s_scroll == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_scroll);
    lv_obj_set_width(s_scroll, LV_PCT(100));
    lv_obj_set_flex_grow(s_scroll, 1);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_scroll, 8, 0);
    lv_obj_set_style_pad_right(s_scroll, 6, 0);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);

    /* 气泡是「内容」：页面隐藏时销毁，回来时按对话历史重建
     * （bubbles_build()，在 on_show 里跑）。这里只决定有没有开场白 ——
     * 改动前也是这样：还没有对话时才问候一句。 */
    s_intro = (ai_service_history_count() <= 1) ? AI_INTRO_GREETING : AI_INTRO_NONE;

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    const struct {
        const char *text;
        lv_event_cb_t cb;
        lv_color_t color;
    } buttons[] = {
        { "输入问题", send_clicked_cb, UI_COLOR_PRIMARY },
        { "新对话", new_clicked_cb, UI_COLOR_SURFACE_ALT },
        { "中断", abort_clicked_cb, UI_COLOR_SURFACE_ALT },
    };

    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        lv_obj_t *btn = lv_button_create(bar);
        lv_obj_set_height(btn, UI_TOUCH_MIN);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_bg_color(btn, buttons[i].color, 0);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, buttons[i].text);
        lv_obj_set_style_text_font(label, ui_font(UI_FONT_BODY), 0);
        lv_obj_center(label);
        lv_obj_add_event_cb(btn, buttons[i].cb, LV_EVENT_CLICKED, NULL);
    }

    render_state();

    (void)ui_event_bind(AI_EVENT, AI_EVENT_STATE_CHANGED, UI_PAGE_AI_CHAT, sizeof(ai_state_t));
    (void)ui_event_bind(AI_EVENT, AI_EVENT_REPLY_CHUNK, UI_PAGE_AI_CHAT, AI_MESSAGE_MAX);
    (void)ui_event_bind(AI_EVENT, AI_EVENT_REPLY_DONE, UI_PAGE_AI_CHAT, 0);
    (void)ui_event_bind(AI_EVENT, AI_EVENT_ERROR, UI_PAGE_AI_CHAT, sizeof(esp_err_t));
    SYS_LOGD(TAG, "ai chat page built");
    return ESP_OK;
}

static esp_err_t ai_chat_on_show(void)
{
    s_visible = true;

    /* 气泡在 on_hide 里被销毁了：按内存里的对话历史重建（不读盘、不发网络
     * 请求），不然用户回来看到的是一段空对话。 */
    bubbles_build();

    /* 回到离开本页时的滚动位置（跟改动前的观感一致）。 */
    if (s_scroll != NULL && s_scroll_y > 0) {
        /* 先停掉重建时那个「滚到最新一条」的动画，否则它会跟复原抢位置。 */
        lv_anim_delete(s_scroll, NULL);
        lv_obj_scroll_to_y(s_scroll, s_scroll_y, LV_ANIM_OFF);
    }

    render_state();
    return ESP_OK;
}

/**
 * 页面被切走：把「内容」（气泡）销毁掉，把内部堆还回去（ROADMAP v1.8.3 §1）。
 *
 * 外壳（抬头、状态行、滚动容器、工具条）留着 —— 它们很少，重建只会让页面
 * 闪一下。对话本身在 ai_service 的内存里，回来时照着它重建。
 */
static esp_err_t ai_chat_on_hide(void)
{
    if (s_scroll != NULL) {
        /* 惯性滚动（手指甩出去之后的那段动画）要停掉，否则记住的是动画中途
         * 的值，而动画还在跑 —— 回来时复原到的位置会跟动画打架。 */
        lv_anim_delete(s_scroll, NULL);
        s_scroll_y = lv_obj_get_scroll_y(s_scroll);
    }

    bubbles_clear();

    /* 输入浮层挂在 lv_layer_top() 上，不属于本页：页面切走时必须收掉，
     * 否则它会盖在下一个页面上（而且永远不释放）。 */
    input_close();

    s_visible = false;
    return ESP_OK;
}

static esp_err_t ai_chat_on_event(int32_t event_id, void *data)
{
    switch (event_id) {
    case AI_EVENT_REPLY_CHUNK:
        if (data != NULL) {
            /* Chunks accumulate into one bubble instead of one bubble each. */
            stream_chunk((const char *)data);
        }
        break;
    case AI_EVENT_REPLY_DONE: {
        const char *reply = ai_service_get_last_reply();
        /* The chunk event already printed the text for the non-streaming path;
         * for the streaming path chunks were emitted incrementally, so only
         * announce completion. */
        if (s_stream_bubble == NULL && reply != NULL && s_visible &&
            !tail_shows_text(reply)) {
            /* Non-streaming answer: it arrives whole on REPLY_DONE.
             * 页面不在屏幕上时不建气泡（铁律四）：这条回答已经在对话历史里，
             * 回来时重建会把它贴回去；已经贴过了就不再贴一次。 */
            add_bubble(reply, false);
        }
        stream_finish();
        if (s_waiting_reply) {
            s_waiting_reply = false;
            if (reply != NULL) {
                ui_toast("回答完成");
            }
        }
        break;
    }
    case AI_EVENT_ERROR:
        ui_toast("AI 请求失败，请检查网络与 API Key");
        break;
    default:
        break;
    }
    render_state();
    return ESP_OK;
}

const ui_page_t page_ai_chat = {
    .id = UI_PAGE_AI_CHAT,
    .name = "ai_chat",
    .create = ai_chat_create,
    .on_show = ai_chat_on_show,
    .on_hide = ai_chat_on_hide,
    .on_event = ai_chat_on_event,
};
