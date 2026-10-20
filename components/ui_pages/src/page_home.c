/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_home.c
 * @brief Main menu: status summary, today's todo card, navigation tiles.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pages_internal.h"

#include "todo_manager.h"

static const char *TAG = "page_home";

static lv_obj_t *s_summary_label;
static lv_obj_t *s_clock_label;

/* ------------------------------------------------------------------------- */
/* 今日待办卡片                                                               */
/* ------------------------------------------------------------------------- */

/** 卡片最多列出几条标题，剩下的用「还有 N 条」概括。 */
#define HOME_TODAY_MAX_ITEMS 3

static lv_obj_t *s_today_card;
static lv_obj_t *s_today_title_label;
static lv_obj_t *s_today_count_label;
static lv_obj_t *s_today_bar;
static lv_obj_t *s_today_item_label[HOME_TODAY_MAX_ITEMS];
static lv_obj_t *s_today_more_label;
static lv_obj_t *s_today_hint_label;

/**
 * 上一次渲染卡片时的输入。
 *
 * 首页是 1 Hz tick（tick_period_ms = 1000），每 tick 都重画整张卡片等于
 * 每秒让整页重排一次（AGENT.md 9.0.1.1 铁律二）。所以只有下面四件事之一
 * 变了才允许重新取数、重新写控件：
 *   - todo_version()：清单内容变了（增删改、状态变化）；
 *   - 日期：跨天后「今天」指的是另一批条目；
 *   - 校时状态：从「未同步」变成「已同步」（或反过来）；
 *   - 存储状态：介质换了，清单可能随之换掉。
 * 其余情况一个字都不改（每个标签写入前都先比较，见 label_set_text_cached()）。
 */
static uint32_t s_today_version;
static int s_today_day = -1;
static bool s_today_synced;
static int s_today_storage = -1;
static bool s_today_rendered;

/** 进度条上一次写入的量程与值，避免重复写同样的东西。 */
static int32_t s_today_bar_max = -1;
static int32_t s_today_bar_value = -1;

/**
 * @brief 只在文本真的变了的时候才写标签。
 *
 * lv_label_set_text() 即使文本完全相同也会 invalidate 控件并触发重排。
 */
static void label_set_text_cached(lv_obj_t *label, const char *text)
{
    if (label == NULL) {
        return;
    }
    if (text == NULL) {
        text = "";
    }
    if (strcmp(lv_label_get_text(label), text) == 0) {
        return;
    }
    lv_label_set_text(label, text);
}

/** 显隐也走同样的原则：状态没变就不要动对象标志。 */
static void obj_set_visible_cached(lv_obj_t *obj, bool visible)
{
    if (obj == NULL) {
        return;
    }
    const bool hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (hidden == !visible) {
        /* 已经是目标状态。 */
        return;
    }
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/** 进度条：量程和值都没变就不写。 */
static void today_set_bar(bool visible, int32_t max, int32_t value)
{
    obj_set_visible_cached(s_today_bar, visible);
    if (!visible) {
        s_today_bar_max = -1;
        s_today_bar_value = -1;
        return;
    }
    if (max != s_today_bar_max) {
        s_today_bar_max = max;
        lv_bar_set_range(s_today_bar, 0, max);
    }
    if (value != s_today_bar_value) {
        s_today_bar_value = value;
        lv_bar_set_value(s_today_bar, value, LV_ANIM_OFF);
    }
}

/** 第 @p index 条标题；@p text 为 NULL 表示这一行不显示。 */
static void today_set_item(int index, const char *text)
{
    if (index < 0 || index >= HOME_TODAY_MAX_ITEMS) {
        return;
    }
    lv_obj_t *label = s_today_item_label[index];
    if (text == NULL) {
        obj_set_visible_cached(label, false);
        return;
    }
    label_set_text_cached(label, text);
    obj_set_visible_cached(label, true);
}

/**
 * @brief 统计今天到期且**已完成**的条目数。
 *
 * todo_count_due_today() 只给未完成数；已完成的那部分只能从「已完成」快照里
 * 按 todo_due_bucket() 过滤出来（§3.5 规定分档与筛选必须用同一套「今天」边界）。
 * 快照是整份清单的拷贝，分配失败时返回 false —— 卡片退化成只显示条数，
 * 不影响其它内容。
 */
static bool today_count_done(size_t *out_done)
{
    *out_done = 0;

    todo_item_t *items = NULL;
    size_t count = 0;
    const esp_err_t err = todo_list_filtered(TODO_FILTER_DONE, &items, &count);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "done list unavailable, today card shows count only");
        return false;
    }
    if (items == NULL) {
        /* 空清单是合法结果：今天确实没有已完成的事项。 */
        return count == 0;
    }

    for (size_t i = 0; i < count; i++) {
        if (todo_due_bucket(items[i].due_ts) == TODO_DUE_TODAY) {
            (*out_done)++;
        }
    }
    todo_free_list(items);
    return true;
}

/**
 * @brief 重画今日待办卡片。
 *
 * 调用方保证已经做过脏检查（见 s_today_* 的注释）。
 */
static void today_render(const sys_status_t *st)
{
    if (s_today_card == NULL) {
        return;
    }

    bool synced = sys_time_synced() && st->now_ts > 0;
    int day = -1;
    if (synced) {
        const time_t now = (time_t)st->now_ts;
        struct tm tm_local;
        if (localtime_r(&now, &tm_local) != NULL) {
            /* 跨天即换日：tm_yday 一年内唯一，配上年份不会撞。 */
            day = (tm_local.tm_year + 1900) * 400 + tm_local.tm_yday;
        } else {
            synced = false;
        }
    }

    const int storage = st->sd_available ? 2 : (st->storage_available ? 1 : 0);
    const uint32_t version = todo_version();

    if (s_today_rendered && version == s_today_version && day == s_today_day &&
        synced == s_today_synced && storage == s_today_storage) {
        return;
    }
    s_today_rendered = true;
    s_today_version = version;
    s_today_day = day;
    s_today_synced = synced;
    s_today_storage = storage;

    label_set_text_cached(s_today_title_label, "今日待办");

    if (!synced) {
        /* FR4：未校时不能编日期，卡片给中性文案。 */
        label_set_text_cached(s_today_count_label, "时间未同步");
        today_set_bar(false, 0, 0);
        for (int i = 0; i < HOME_TODAY_MAX_ITEMS; i++) {
            today_set_item(i, NULL);
        }
        label_set_text_cached(s_today_more_label, "");
        obj_set_visible_cached(s_today_more_label, false);
        label_set_text_cached(s_today_hint_label, "校时之后才能显示今天的待办");
        obj_set_visible_cached(s_today_hint_label, true);
        return;
    }

    const size_t count = todo_count_due_today();

    /* 列表与计数同口径：今天到期且未完成。 */
    todo_item_t *items = NULL;
    size_t item_count = 0;
    if (todo_list_filtered(TODO_FILTER_DUE_TODAY, &items, &item_count) != ESP_OK) {
        items = NULL;
        item_count = 0;
        SYS_LOGW(TAG, "today list unavailable, today card shows count only");
    }

    size_t done_today = 0;
    const bool have_progress = today_count_done(&done_today);
    const size_t total_today = count + done_today;

    char buf[96];
    if (count == 0) {
        if (have_progress && done_today > 0) {
            /* 今天的条目都在清单里做完了：进度条走满，比「没有待办」更让人安心。 */
            label_set_text_cached(s_today_count_label, "今天的待办都做完了");
            today_set_bar(true, (int32_t)done_today, (int32_t)done_today);
            label_set_text_cached(s_today_hint_label, "辛苦啦，剩下的明天再说");
        } else {
            label_set_text_cached(s_today_count_label, "今天没有到期的待办");
            today_set_bar(false, 0, 0);
            label_set_text_cached(s_today_hint_label, "待办都排在别的日子，安心过今天");
        }
        obj_set_visible_cached(s_today_hint_label, true);
    } else {
        if (have_progress && total_today > 0) {
            snprintf(buf, sizeof(buf), "今天到期 %u 项 · 已完成 %u/%u",
                     (unsigned)count, (unsigned)done_today, (unsigned)total_today);
        } else {
            snprintf(buf, sizeof(buf), "今天到期 %u 项", (unsigned)count);
        }
        label_set_text_cached(s_today_count_label, buf);
        /* 进度条表示今天的完成度：已完成 / 今日到期总数。 */
        today_set_bar(have_progress && total_today > 0, (int32_t)total_today,
                       (int32_t)done_today);
        label_set_text_cached(s_today_hint_label, "");
        obj_set_visible_cached(s_today_hint_label, false);
    }

    for (int i = 0; i < HOME_TODAY_MAX_ITEMS; i++) {
        const size_t index = (size_t)i;
        if (items != NULL && index < item_count) {
            today_set_item(i, items[index].title);
        } else {
            today_set_item(i, NULL);
        }
    }

    if (count > (size_t)HOME_TODAY_MAX_ITEMS) {
        snprintf(buf, sizeof(buf), "还有 %u 条", (unsigned)(count - HOME_TODAY_MAX_ITEMS));
        label_set_text_cached(s_today_more_label, buf);
        obj_set_visible_cached(s_today_more_label, true);
    } else {
        label_set_text_cached(s_today_more_label, "");
        obj_set_visible_cached(s_today_more_label, false);
    }

    if (items != NULL) {
        todo_free_list(items);
    }
}

/* ------------------------------------------------------------------------- */
/* 磁贴                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *title;
    const char *subtitle;
    ui_page_id_t target;
} home_tile_def_t;

static const home_tile_def_t s_tiles[] = {
    { "WiFi",   "扫描 · 连接 · 自动重连", UI_PAGE_WIFI },
    { "待办",   "与电脑端同步清单",       UI_PAGE_TODO },
    { "天气",   "城市 · 温度 · 湿度",     UI_PAGE_WEATHER },
    { "日历",   "月历 · NTP 校时",        UI_PAGE_CALENDAR },
    { "单词",   "卡片背诵 · 复习进度",    UI_PAGE_WORD },
    { "设置",   "亮度 · 时区 · 密钥",     UI_PAGE_SETTINGS },
};

/* ------------------------------------------------------------------------- */
/* 状态栏摘要                                                                 */
/* ------------------------------------------------------------------------- */

/**
 * Last values painted into the two labels.
 *
 * home_refresh() runs at 1 Hz (tick_period_ms = 1000) but the summary changes
 * only when a todo is synced or the storage medium appears/disappears, and the
 * clock only needs a repaint when its *displayed* fields change (it shows
 * HH:MM, so the seconds field is irrelevant). Writing identical text still
 * invalidates the label and re-lays-out the flex head above the tile grid, which
 * is what made the home page re-render every second for nothing
 * (smoothness audit P1-7).
 */
static long s_summary_total = -1;
static long s_summary_pending = -1;
static int s_summary_storage = -1;   /**< 0 = unavailable, 1 = internal, 2 = SD */
static bool s_clock_shown;           /**< label currently shows a real time */
static int s_last_min = -1;
static int s_last_hour = -1;
static int s_last_mday = -1;
static int s_last_mon = -1;
static int s_last_year = -1;

static void home_refresh(void)
{
    sys_status_t st;
    if (sys_status_get(&st) != ESP_OK) {
        return;
    }

    if (s_summary_label != NULL) {
        const int storage = st.sd_available ? 2 : (st.storage_available ? 1 : 0);
        if (st.todo_total != s_summary_total || st.todo_pending != s_summary_pending ||
            storage != s_summary_storage) {
            s_summary_total = st.todo_total;
            s_summary_pending = st.todo_pending;
            s_summary_storage = storage;
            lv_label_set_text_fmt(s_summary_label, "待办 %ld 项 · 待同步 %ld 项 · 存储 %s",
                                  (long)st.todo_total, (long)st.todo_pending,
                                  (storage == 2) ? "SD 卡"
                                                 : ((storage == 1) ? "内置" : "不可用"));
        }
    }

    /* 今日待办卡片自己有脏检查（版本号 + 跨天 + 存储状态），与摘要/时钟互不影响。 */
    today_render(&st);

    if (s_clock_label == NULL) {
        return;
    }

    if (!sys_time_synced() || st.now_ts <= 0) {
        /* Requirement FR4: never render an epoch date before NTP succeeds. */
        if (s_clock_shown) {
            s_clock_shown = false;
            lv_label_set_text(s_clock_label, UI_STATUS_TIME_UNSYNCED);
        }
        return;
    }

    const time_t now = (time_t)st.now_ts;
    struct tm tm_local;
    if (localtime_r(&now, &tm_local) == NULL) {
        if (s_clock_shown) {
            s_clock_shown = false;
            lv_label_set_text(s_clock_label, UI_STATUS_TIME_UNSYNCED);
        }
        return;
    }

    /* The label shows date + 星期 + HH:MM: repaint only when one of those moved. */
    if (s_clock_shown && tm_local.tm_min == s_last_min && tm_local.tm_hour == s_last_hour &&
        tm_local.tm_mday == s_last_mday && tm_local.tm_mon == s_last_mon &&
        tm_local.tm_year == s_last_year) {
        return;
    }

    static const char *const weekdays[] = { "日", "一", "二", "三", "四", "五", "六" };
    const char *wd = (tm_local.tm_wday >= 0 && tm_local.tm_wday < 7) ? weekdays[tm_local.tm_wday] : "?";
    lv_label_set_text_fmt(s_clock_label, "%04d-%02d-%02d 星期%s  %02d:%02d",
                          tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday, wd,
                          tm_local.tm_hour, tm_local.tm_min);
    s_clock_shown = true;
    s_last_min = tm_local.tm_min;
    s_last_hour = tm_local.tm_hour;
    s_last_mday = tm_local.tm_mday;
    s_last_mon = tm_local.tm_mon;
    s_last_year = tm_local.tm_year;
}

/* ------------------------------------------------------------------------- */
/* 创建                                                                       */
/* ------------------------------------------------------------------------- */

/** 点卡片进待办页。 */
static void today_card_clicked_cb(lv_event_t *e)
{
    (void)e;
    (void)ui_nav_push(UI_PAGE_TODO);
}

/** 建出今日待办卡片的全部控件；控件建好后由 today_render() 填内容。 */
static esp_err_t today_card_create(lv_obj_t *parent)
{
    lv_obj_t *body = NULL;
    s_today_card = ui_card_create(parent, NULL, &body);
    if (s_today_card == NULL || body == NULL) {
        s_today_card = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 整张卡片就是一个触摸目标（远大于 UI_TOUCH_MIN），点它进待办页。 */
    lv_obj_add_flag(s_today_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_today_card, UI_COLOR_SURFACE_ALT, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_today_card, today_card_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* 标题区：左边是卡片标题，右边是「今天到期 N 项 · 已完成 x/y」。 */
    lv_obj_t *row = lv_obj_create(body);
    if (row == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    s_today_title_label = lv_label_create(row);
    lv_label_set_text(s_today_title_label, "今日待办");
    lv_obj_set_style_text_font(s_today_title_label, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(s_today_title_label, UI_COLOR_TEXT, 0);
    lv_obj_set_flex_grow(s_today_title_label, 1);

    s_today_count_label = lv_label_create(row);
    lv_label_set_text(s_today_count_label, "");
    lv_obj_set_style_text_font(s_today_count_label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_today_count_label, UI_COLOR_PRIMARY, 0);

    /* 进度条：今天的完成度。 */
    s_today_bar = lv_bar_create(body);
    if (s_today_bar == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(s_today_bar, LV_PCT(100));
    lv_obj_set_height(s_today_bar, 6);
    lv_obj_set_style_pad_all(s_today_bar, 0, 0);
    lv_obj_set_style_border_width(s_today_bar, 0, 0);
    lv_obj_set_style_shadow_width(s_today_bar, 0, 0);
    lv_obj_set_style_bg_color(s_today_bar, UI_COLOR_SURFACE_ALT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_today_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_today_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_today_bar, UI_COLOR_SUCCESS, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_today_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_today_bar, 3, LV_PART_INDICATOR);
    obj_set_visible_cached(s_today_bar, false);

    /* 条目区：最多三条标题，过长的用省略号截断（LV_LABEL_LONG_MODE_DOTS）。 */
    for (int i = 0; i < HOME_TODAY_MAX_ITEMS; i++) {
        lv_obj_t *label = lv_label_create(body);
        if (label == NULL) {
            return ESP_ERR_NO_MEM;
        }
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(label, ui_font(UI_FONT_SMALL), 0);
        lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
        obj_set_visible_cached(label, false);
        s_today_item_label[i] = label;
    }

    /* 「还有 N 条」与说明行（空状态/未校时提示）。 */
    s_today_more_label = lv_label_create(body);
    if (s_today_more_label == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(s_today_more_label, LV_PCT(100));
    lv_label_set_long_mode(s_today_more_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_today_more_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_today_more_label, UI_COLOR_TEXT_MUTED, 0);
    obj_set_visible_cached(s_today_more_label, false);

    s_today_hint_label = lv_label_create(body);
    if (s_today_hint_label == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(s_today_hint_label, LV_PCT(100));
    lv_label_set_long_mode(s_today_hint_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_today_hint_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_today_hint_label, UI_COLOR_TEXT_MUTED, 0);
    obj_set_visible_cached(s_today_hint_label, false);

    return ESP_OK;
}

static esp_err_t home_create(lv_obj_t *parent)
{
    lv_obj_t *head = lv_obj_create(parent);
    if (head == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(head, 4, 0);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *greeting = lv_label_create(head);
    lv_label_set_text(greeting, "你好，今天要做什么？");
    lv_obj_set_style_text_font(greeting, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(greeting, UI_COLOR_TEXT, 0);

    s_clock_label = lv_label_create(head);
    lv_label_set_text(s_clock_label, UI_STATUS_TIME_UNSYNCED);
    lv_obj_set_style_text_font(s_clock_label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_clock_label, UI_COLOR_PRIMARY, 0);

    s_summary_label = lv_label_create(head);
    lv_label_set_text(s_summary_label, "");
    lv_obj_set_style_text_font(s_summary_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_summary_label, UI_COLOR_TEXT_MUTED, 0);

    /* 今日待办卡片：夹在抬头与磁贴网格之间。 */
    if (today_card_create(parent) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    /* Tile grid: a wrapping flex row keeps this free of absolute coordinates
     * (requirement 10.3). */
    lv_obj_t *grid = lv_obj_create(parent);
    if (grid == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    /*
     * The page is taller than the panel now (the card sits above the grid),
     * so the grid keeps its natural height and the page container scrolls
     * instead of the tiles being squeezed into a clipped box. LVGL 9's flex
     * has no shrink behaviour, so this is the only way to keep the tiles whole.
     */
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(grid, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, UI_PAGE_GAP, 0);
    lv_obj_set_style_pad_column(grid, UI_PAGE_GAP, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < sizeof(s_tiles) / sizeof(s_tiles[0]); i++) {
        lv_obj_t *tile = page_tile_create(grid, s_tiles[i].title, s_tiles[i].subtitle,
                                         s_tiles[i].target);
        if (tile == NULL) {
            return ESP_ERR_NO_MEM;
        }
        /* Three columns per row on an 800 px wide panel. */
        lv_obj_set_width(tile, LV_PCT(32));
    }

    home_refresh();
    SYS_LOGD(TAG, "home built with %u tiles", (unsigned)(sizeof(s_tiles) / sizeof(s_tiles[0])));
    return ESP_OK;
}

static esp_err_t home_on_show(void)
{
    home_refresh();
    return ESP_OK;
}

static esp_err_t home_on_event(int32_t event_id, void *data)
{
    (void)event_id;
    (void)data;
    home_refresh();
    return ESP_OK;
}

static esp_err_t home_on_tick(void)
{
    home_refresh();
    return ESP_OK;
}

const ui_page_t page_home = {
    .id = UI_PAGE_HOME,
    .name = "home",
    .create = home_create,
    .on_show = home_on_show,
    .on_event = home_on_event,
    .on_tick = home_on_tick,
    .tick_period_ms = 1000,
};
