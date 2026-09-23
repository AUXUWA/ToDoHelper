/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_placeholders.c
 * @brief Pages that are still to be built.
 *
 * Every page id in ui_core.h is registered from day one so navigation and the
 * status bar can be exercised end to end. As each feature lands, its block moves
 * out of here into page_<name>.c and ui_pages.c is updated
 * (docs/TODOHELPER_FRAMEWORK.md 7.2). Only the P2 AI page is left.
 */

#include "pages_internal.h"

/* ---- FR7 AI chat (P2) -------------------------------------------------- */

static esp_err_t ai_chat_create(lv_obj_t *parent)
{
    return page_placeholder_create(parent, "AI 对话",
                                   "接入 DeepSeek 多轮对话，支持语音输入与播报，"
                                   "并与待机小眼睛状态联动（LISTENING / THINKING / "
                                   "SPEAKING / ERROR）。",
                                   "P2");
}
const ui_page_t page_ai_chat = {
    .id = UI_PAGE_AI_CHAT, .name = "ai_chat", .create = ai_chat_create,
};
