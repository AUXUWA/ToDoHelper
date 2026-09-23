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

static const char *TAG = "page_ai";

#define AI_CHAT_MAX_BUBBLES 40

static lv_obj_t *s_scroll;
static lv_obj_t *s_state_label;
static lv_obj_t *s_input_overlay;
static lv_obj_t *s_input_area;
static bool s_waiting_reply;

/* ------------------------------------------------------------------------ */
/* Bubbles                                                                   */
/* ------------------------------------------------------------------------ */

static void add_bubble(const char *text, bool from_user)
{
    if (s_scroll == NULL || text == NULL) {
        return;
    }

    /* Keep the transcript bounded: an idle device running for days must not
     * grow this list without limit. */
    if (lv_obj_get_child_count(s_scroll) >= AI_CHAT_MAX_BUBBLES) {
        lv_obj_t *oldest = lv_obj_get_child(s_scroll, 0);
        if (oldest != NULL) {
            lv_obj_delete(oldest);
        }
    }

    lv_obj_t *bubble = lv_obj_create(s_scroll);
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
    lv_label_set_text(label, text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(label, from_user ? UI_COLOR_ON_PRIMARY : UI_COLOR_TEXT, 0);

    lv_obj_scroll_to_view(bubble, LV_ANIM_ON);
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

    lv_obj_t *keyboard = lv_keyboard_create(s_input_overlay);
    lv_obj_set_width(keyboard, LV_PCT(100));
    lv_obj_set_height(keyboard, 220);
    lv_keyboard_set_textarea(keyboard, s_input_area);
    lv_obj_set_style_text_font(keyboard, ui_font(UI_FONT_SMALL), 0);
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
    if (s_scroll != NULL) {
        lv_obj_clean(s_scroll);
    }
    s_waiting_reply = false;
    add_bubble("新对话已开始。可以在电脑浏览器里用 /api/v1/ai/chat 提问，"
               "回答会显示在这里。", false);
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

    /* Restore whatever is already in context, so switching away and back does
     * not look like the conversation was lost. */
    const size_t count = ai_service_history_count();
    for (size_t i = 0; i < count; i++) {
        ai_message_t msg;
        if (ai_service_history_get(i, &msg) != ESP_OK) {
            continue;
        }
        if (strcmp(msg.role, "system") == 0) {
            continue;
        }
        add_bubble(msg.content, strcmp(msg.role, "user") == 0);
    }
    if (count <= 1) {
        add_bubble("你好，我是这个设备的 AI 助手。\n"
                   "在电脑浏览器打开设备地址，或调用 POST /api/v1/ai/chat 就能对话。",
                   false);
    }

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
    render_state();
    return ESP_OK;
}

static esp_err_t ai_chat_on_event(int32_t event_id, void *data)
{
    switch (event_id) {
    case AI_EVENT_REPLY_CHUNK:
        if (data != NULL) {
            add_bubble((const char *)data, false);
        }
        break;
    case AI_EVENT_REPLY_DONE: {
        const char *reply = ai_service_get_last_reply();
        /* The chunk event already printed the text for the non-streaming path;
         * for the streaming path chunks were emitted incrementally, so only
         * announce completion. */
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
    .on_event = ai_chat_on_event,
};
