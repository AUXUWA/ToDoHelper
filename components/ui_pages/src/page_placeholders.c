/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_placeholders.c
 * @brief Not-yet-implemented pages.
 *
 * Every page id in ui_core.h is registered from day one so that navigation and
 * the status bar can be exercised end to end while the features land. As each
 * feature is implemented, delete its block here and add a real descriptor to
 * ui_pages.c (docs/TODOHELPER_FRAMEWORK.md 7.2).
 */

#include "pages_internal.h"

/* ---- FR1 WiFi ---------------------------------------------------------- */

static esp_err_t wifi_create(lv_obj_t *parent)
{
    return page_placeholder_create(parent, "WiFi",
                                   "扫描周边热点、输入密码、保存并开机自动重连，"
                                   "显示 IP / RSSI / MAC 并发布 mDNS 名称。",
                                   "P0");
}
const ui_page_t page_wifi = {
    .id = UI_PAGE_WIFI, .name = "wifi", .create = wifi_create,
};

/* ---- FR2 Todo ---------------------------------------------------------- */

static esp_err_t todo_create(lv_obj_t *parent)
{
    return page_placeholder_create(parent, "待办清单",
                                   "显示列表、勾选完成、断网可离线操作，"
                                   "通过局域网 HTTP 与电脑端双向同步。",
                                   "P0");
}
const ui_page_t page_todo = {
    .id = UI_PAGE_TODO, .name = "todo", .create = todo_create,
};

static esp_err_t todo_detail_create(lv_obj_t *parent)
{
    return page_placeholder_create(parent, "待办详情",
                                   "单条待办的完整信息、到期时间与同步状态。",
                                   "P1");
}
const ui_page_t page_todo_detail = {
    .id = UI_PAGE_TODO_DETAIL, .name = "todo_detail", .create = todo_detail_create,
};

/* ---- FR3 Weather ------------------------------------------------------- */

static esp_err_t weather_create(lv_obj_t *parent)
{
    return page_placeholder_create(parent, "天气",
                                   "手动城市 + 网络 API 获取温度、体感、湿度与图标，"
                                   "默认 15 分钟刷新，断网显示上次缓存。",
                                   "P0");
}
const ui_page_t page_weather = {
    .id = UI_PAGE_WEATHER, .name = "weather", .create = weather_create,
};

/* ---- FR4 Calendar ------------------------------------------------------ */

static esp_err_t calendar_create(lv_obj_t *parent)
{
    return page_placeholder_create(parent, "日历",
                                   "月历浏览、上一月/下一月、NTP 校时与时区设置。"
                                   "注意：本板无 RTC，断电后需重新同步。",
                                   "P0");
}
const ui_page_t page_calendar = {
    .id = UI_PAGE_CALENDAR, .name = "calendar", .create = calendar_create,
};

/* ---- FR5 Words --------------------------------------------------------- */

static esp_err_t word_create(lv_obj_t *parent)
{
    return page_placeholder_create(parent, "单词背诵",
                                   "卡片翻面、认识/模糊/不认识标记、"
                                   "学习进度与熟悉度记录、每日数量可配置。",
                                   "P0");
}
const ui_page_t page_word = {
    .id = UI_PAGE_WORD, .name = "word", .create = word_create,
};

/* ---- FR7 AI chat ------------------------------------------------------- */

static esp_err_t ai_chat_create(lv_obj_t *parent)
{
    return page_placeholder_create(parent, "AI 对话",
                                   "接入 DeepSeek 多轮对话，支持语音输入与播报，"
                                   "并与待机小眼睛状态联动。",
                                   "P2");
}
const ui_page_t page_ai_chat = {
    .id = UI_PAGE_AI_CHAT, .name = "ai_chat", .create = ai_chat_create,
};

/* ---- Settings ---------------------------------------------------------- */

static esp_err_t settings_create(lv_obj_t *parent)
{
    return page_placeholder_create(parent, "设置",
                                   "屏幕亮度、待机超时、时区、城市、"
                                   "天气与 AI 密钥、HTTP 鉴权 token、语音开关。",
                                   "P1");
}
const ui_page_t page_settings = {
    .id = UI_PAGE_SETTINGS, .name = "settings", .create = settings_create,
};
