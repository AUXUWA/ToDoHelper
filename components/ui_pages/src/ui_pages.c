/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_pages.h"

#include "pages_internal.h"

static const char *TAG = "ui_pages";

/**
 * The page table.
 *
 * Order does not matter: ui_core indexes pages by ui_page_id_t, and the enum
 * order in ui_core.h is what must stay aligned with
 * docs/TODOHELPER_REQUIREMENTS.md 10.1.
 */
static const ui_page_t *const s_pages[] = {
    &page_splash,
    &page_home,
    &page_wifi,
    &page_todo,
    &page_todo_detail,
    &page_weather,
    &page_calendar,
    &page_word,
    &page_ai_chat,
    &page_settings,
    &page_idle_face,
};

esp_err_t ui_pages_init(void)
{
    static ui_page_t table[sizeof(s_pages) / sizeof(s_pages[0])];
    const size_t count = sizeof(s_pages) / sizeof(s_pages[0]);

    for (size_t i = 0; i < count; i++) {
        table[i] = *s_pages[i];
    }

    return ui_page_register(table, count);
}
