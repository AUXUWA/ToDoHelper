/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_home.c
 * @brief Main menu: status summary, today's todo card, the three status cards
 *        (AI chat / words / calendar) and the navigation tiles.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pages_internal.h"

#include "ai_service.h"
#include "todo_manager.h"
#include "word_service.h"

static const char *TAG = "page_home";

static lv_obj_t *s_summary_label;
static lv_obj_t *s_clock_label;

/* ------------------------------------------------------------------------- */
/* 今日待办卡片                                                               */
/* ------------------------------------------------------------------------- */

/**
 * 卡片最多列出几条标题，剩下的用「还有 N 条」概括。
 *
 * 两条而不是三条：真机实测（debug shell 的 widgets 树）每多一条标题就是 19 px，
 * 而首页的可视高度只有 435 px —— 三条时首页内容 466 px，磁贴整行落到折叠线以下，
 * 主导航就看不见了。
 */
#define HOME_TODAY_MAX_ITEMS 2

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
/* 状态卡：AI 对话 / 单词 / 日历                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief 一张状态卡的结构。
 *
 * 三张卡用同一套结构（标题 + 主行 + 一行小字）、同一套字号，所以它们的**高度
 * 天然一致** —— 这就是「同一级别」的视觉含义。这里刻意**不写死高度**：
 * 字体行高一旦和预算对不上，写死的高度就会把字裁掉（v1.6.0 日历页的时钟标签
 * 就是这么被裁掉第二行的）。行数固定 + 字体固定，高度自然相同。
 *
 * 行数是**三行**而不是四行：真机实测（debug shell 的 widgets 树）四行时
 * 每张卡 138 px，首页内容 466 px > 可视 435 px，磁贴被推到折叠线以下；
 * 去掉一行小字后每张卡 111 px，首页能一屏放下。
 */
typedef struct {
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *main;
    lv_obj_t *sub1;
} home_status_card_t;

static home_status_card_t s_ai_card;
static home_status_card_t s_word_card;
static home_status_card_t s_cal_card;

/**
 * @brief 取数节流。
 *
 * 首页是 1 Hz tick，三张卡各有各的「贵」：
 *   - AI 卡要读 NVS（API Key 存在 NVS 里），没必要每秒读一次；
 *   - 单词卡的 word_stats() 会遍历整份词库进度（最多 2000 条）并加锁；
 *   - 日历卡要取一次清单快照（PSRAM 拷贝）。
 * 所以每张卡都有自己的「便宜指纹」：指纹没变就一个字节都不写（AGENT.md §9.0.1.1
 * 铁律二）。
 */
#define HOME_AI_POLL_TICKS      5    /**< AI 卡每 5 个 tick 取一次数（约 5 s） */
#define HOME_WORD_TIME_BUCKET   300  /**< 单词卡的时间桶：300 个 tick ≈ 5 分钟 */

/** home_refresh() 的调用次数，兼作上面两个节流的时间基准。 */
static uint32_t s_refresh_count;

/** 三张状态卡都要重新取数（回到首页、词库事件到达时置位）。 */
static bool s_cards_dirty = true;

/** 文字颜色也要先比较：颜色没变就不重设样式，否则每秒重排一次卡片。 */
static void label_set_color_cached(lv_obj_t *label, lv_color_t color)
{
    if (label == NULL) {
        return;
    }
    if (lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), color)) {
        return;
    }
    lv_obj_set_style_text_color(label, color, 0);
}

/** 点状态卡进对应页面（AI 对话 / 单词 / 日历）。 */
static void status_card_clicked_cb(lv_event_t *e)
{
    const ui_page_id_t target = (ui_page_id_t)(intptr_t)lv_event_get_user_data(e);
    if (target >= UI_PAGE_MAX) {
        return;
    }
    (void)ui_nav_push(target);
}

/**
 * @brief 建出一张状态卡：标题 + 主行 + 两行小字。
 *
 * 整张卡片是一个触摸目标（远大于 UI_TOUCH_MIN），点它进对应页面；卡片里的
 * 标签不是可点击对象，点击会冒泡到卡片上。
 */
static esp_err_t status_card_create(lv_obj_t *parent, const char *title, ui_page_id_t target,
                                    home_status_card_t *out)
{
    memset(out, 0, sizeof(*out));

    lv_obj_t *card = lv_obj_create(parent);
    if (card == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(32));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UI_CARD_RADIUS, 0);
    lv_obj_set_style_pad_all(card, UI_CARD_PAD, 0);
    /* 行距跟 ui_card_create() 里的卡片一致（8 px），三张卡看上去是一套东西。 */
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 按下时给一点反馈，不然用户不知道卡片能点。 */
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(card, UI_COLOR_SURFACE_ALT, LV_STATE_PRESSED);
    lv_obj_add_event_cb(card, status_card_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)target);

    out->card = card;

    out->title = lv_label_create(card);
    if (out->title == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_text(out->title, title);
    lv_obj_set_style_text_font(out->title, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(out->title, UI_COLOR_TEXT, 0);

    /* 主行：这一张卡的状态本身（颜色随状态变）。 */
    out->main = lv_label_create(card);
    if (out->main == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(out->main, LV_PCT(100));
    lv_label_set_long_mode(out->main, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(out->main, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(out->main, UI_COLOR_PRIMARY, 0);

    /* 两行小字：补充说明。 */
    out->sub1 = lv_label_create(card);
    if (out->sub1 == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(out->sub1, LV_PCT(100));
    lv_label_set_long_mode(out->sub1, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(out->sub1, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(out->sub1, UI_COLOR_TEXT_MUTED, 0);

    return ESP_OK;
}

/**
 * @brief 建出三张状态卡所在的那一行。
 *
 * 各占 LV_PCT(32)，间距 UI_PAGE_GAP：3×32% + 2×12 px = 765 px < 772 px
 * （800 - 2×14 页内边距），一行放得下。
 *
 * ## 垂直预算（800×480，状态栏 40，页内边距 14×2 → 可用 412 px）
 *
 * FreeType 字号（SMALL 16 / BODY 20 / TITLE 28 px）的行高约 1.4 倍：
 *   抬头（问候 40 + 时钟 29 + 摘要 23 + 2×4 行距）              ≈ 100
 *   今日待办大卡（标题行 40 + 进度条 6 + 最多 3 条标题 + 说明行） ≈ 160…230
 *   三张状态卡（29 + 29 + 23 + 23 + 3×8 行距 + 2×14 内边距）     ≈ 156
 *   磁贴一行（page_tile_create 定的 118）                        = 118
 *   三处 UI_PAGE_GAP 间距                                         = 36
 *   合计                                                          ≈ 570…640
 *
 * 也就是说首页仍然放不进一屏 —— 和 v1.5.0 一样靠页面容器滚动（ui_core 给每个
 * 页面容器加了 LV_OBJ_FLAG_SCROLLABLE，内容不会被裁掉）。这一轮并不比原来
 * 更长：去掉「单词 / 日历」两个磁贴省下一整行磁贴（118 + 12 = 130 px），
 * 三张状态卡花掉 156 px，净增约 26 px。抬头与今日待办大卡按规格 §2.4 不动。
 */
static esp_err_t status_row_create(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    if (row == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, UI_PAGE_GAP, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if (status_card_create(row, "AI 对话", UI_PAGE_AI_CHAT, &s_ai_card) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    if (status_card_create(row, "单词", UI_PAGE_WORD, &s_word_card) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    if (status_card_create(row, "日历", UI_PAGE_CALENDAR, &s_cal_card) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* ---- AI 对话卡 ------------------------------------------------------------ */

/** 上一次取数的时间点（tick 计数），用来做 HOME_AI_POLL_TICKS 的节流。 */
static uint32_t s_ai_data_tick;

/**
 * @brief AI 对话卡。
 *
 * API Key 只报「有没有」，**永远不显示 Key 本身**（需求 8.8，与设备上的其它
 * 密钥同规则）。
 */
static void ai_card_render(bool force)
{
    if (!force && (s_refresh_count - s_ai_data_tick) < HOME_AI_POLL_TICKS) {
        /* 还没到取数的时间：上一次的显示内容继续有效。 */
        return;
    }
    s_ai_data_tick = s_refresh_count;

    const bool has_key = ai_service_has_api_key();
    const char *model = ai_service_get_model();
    const size_t history = ai_service_history_count();

    char main_text[32];
    char sub1_text[64];

    if (has_key) {
        snprintf(main_text, sizeof(main_text), "已就绪");
        snprintf(sub1_text, sizeof(sub1_text), "%s",
                 (model != NULL && model[0] != '\0') ? model : AI_DEFAULT_MODEL);
    } else {
        snprintf(main_text, sizeof(main_text), "未配置 API Key");
        snprintf(sub1_text, sizeof(sub1_text), "请在网页端设置页配置");
    }
    /*
     * 原来这里还有第三行「上下文 N 条」。ai_service_history_count() 数的是
     * 上下文里的**消息条数**（含系统提示那条），写成「轮」会让人以为轮数多了一倍，
     * 写成「条」又占掉一整行、首页就放不下一屏了 —— 所以这行去掉，
     * 对话轮数在设置页的「AI 对话」卡片里看。
     */
    (void)history;

    label_set_text_cached(s_ai_card.main, main_text);
    label_set_color_cached(s_ai_card.main, has_key ? UI_COLOR_SUCCESS : UI_COLOR_WARNING);
    label_set_text_cached(s_ai_card.sub1, sub1_text);
}

/* ---- 单词卡 -------------------------------------------------------------- */

/**
 * @brief 单词卡的「便宜指纹」。
 *
 * word_stats() 会遍历整份词库进度并加锁，所以先比这个指纹，指纹没变就完全不
 * 调它。构成：
 *   - 词书名（切书会变）；
 *   - 本地/云端模式；
 *   - 已加载词数、云端队列长度（换词库、云端同步会变）；
 *   - 每日目标；
 *   - 时间桶（到期的词会随时间自己变）。
 */
typedef struct {
    char book[32];
    bool cloud;
    size_t total;
    size_t cloud_count;
    uint32_t daily;
    uint32_t bucket;
    bool valid;
} word_fingerprint_t;

static word_fingerprint_t s_word_fp;

/** 单词卡的内容。 */
static void word_card_render(bool force)
{
    const char *book = word_get_book();
    /*
     * 词书名先截断再到处用：显示和指纹用的是同一份截断值，否则超长名字会让
     * 指纹永远「变了」，卡片就每秒重取一次 word_stats()。
     */
    char book_name[sizeof(s_word_fp.book)];
    snprintf(book_name, sizeof(book_name), "%s",
             (book != NULL && book[0] != '\0') ? book : "未选词书");
    const bool cloud = word_cloud_mode();
    const size_t total = word_count();
    const size_t cloud_count = word_cloud_count();
    const uint32_t daily = word_get_daily_count();
    const uint32_t bucket = s_refresh_count / HOME_WORD_TIME_BUCKET;

    if (!force && s_word_fp.valid &&
        s_word_fp.cloud == cloud && s_word_fp.total == total &&
        s_word_fp.cloud_count == cloud_count && s_word_fp.daily == daily &&
        s_word_fp.bucket == bucket &&
        strcmp(s_word_fp.book, book_name) == 0) {
        /* 指纹没变：内容没变，一个字都不写。 */
        return;
    }

    snprintf(s_word_fp.book, sizeof(s_word_fp.book), "%s", book_name);
    s_word_fp.cloud = cloud;
    s_word_fp.total = total;
    s_word_fp.cloud_count = cloud_count;
    s_word_fp.daily = daily;
    s_word_fp.bucket = bucket;
    s_word_fp.valid = true;

    word_stats_t stats;
    const bool have_stats = (word_stats(&stats) == ESP_OK);

    char main_text[48];
    char sub1_text[40];

    snprintf(main_text, sizeof(main_text), "%s · %s", book_name, cloud ? "云端" : "本地");

    if (have_stats) {
        /*
         * 原来写的是「今日 14 / 10」：due 是**该复习**的条数（可能含以前积下的），
         * daily 是**每日新词**目标，两个不是一个口径，并排放会让人以为坏了
         * （实测就出现了 14 / 10 这种看着像错数字的显示）。改成只报到期条数。
         * daily 仍然参与上面的指纹（每日目标改了，卡片要重画）。
         */
        snprintf(sub1_text, sizeof(sub1_text), "待复习 %u 项", (unsigned)stats.due);
    } else {
        /* 词库还没初始化好：不编数字。 */
        snprintf(sub1_text, sizeof(sub1_text), "进度暂不可用");
    }

    label_set_text_cached(s_word_card.main, main_text);
    label_set_color_cached(s_word_card.main, UI_COLOR_PRIMARY);
    label_set_text_cached(s_word_card.sub1, sub1_text);
}

/**
 * @brief 词库进度变了（复习、切书、云端同步落盘）。
 *
 * 事件回调里不干活，只置一个脏标志：真正的取数留给下一次 home_refresh()
 * （事件回调都在 LVGL 锁内）。
 */
static esp_err_t word_event_cb(int32_t event_id, void *data)
{
    (void)event_id;
    (void)data;
    s_cards_dirty = true;
    return ESP_OK;
}

/* ---- 日历卡 -------------------------------------------------------------- */

/** 上一次重画日历卡时的输入（清单版本 / 日期 / 校时状态）。 */
static uint32_t s_cal_version;
static int s_cal_day = -1;
static bool s_cal_synced;
static bool s_cal_valid;

/** 日历卡的内容：今天是哪天 + 今天有几项到期。 */
static void cal_card_render(const sys_status_t *st, bool force)
{
    bool synced = sys_time_synced() && st->now_ts > 0;
    int day = -1;
    char main_text[32] = "";

    if (synced) {
        const time_t now = (time_t)st->now_ts;
        struct tm tm_local;
        if (localtime_r(&now, &tm_local) != NULL) {
            /* 跨天即换日：tm_yday 一年内唯一，配上年份不会撞。 */
            day = (tm_local.tm_year + 1900) * 400 + tm_local.tm_yday;
            static const char *const weekdays[] = { "日", "一", "二", "三", "四", "五", "六" };
            const char *wd = (tm_local.tm_wday >= 0 && tm_local.tm_wday < 7) ? weekdays[tm_local.tm_wday] : "?";
            snprintf(main_text, sizeof(main_text), "%d 月 %d 日 星期%s",
                     tm_local.tm_mon + 1, tm_local.tm_mday, wd);
        } else {
            /* localtime_r 失败就当没校时，绝不编日期。 */
            synced = false;
        }
    }

    const uint32_t version = todo_version();
    if (!force && s_cal_valid && version == s_cal_version && day == s_cal_day &&
        synced == s_cal_synced) {
        /* 清单没动、日期没变：内容没变，一个字都不写。 */
        return;
    }
    s_cal_valid = true;
    s_cal_version = version;
    s_cal_day = day;
    s_cal_synced = synced;

    if (!synced) {
        /* FR4：未校时不能编日期。 */
        label_set_text_cached(s_cal_card.main, "时间未同步");
        label_set_color_cached(s_cal_card.main, UI_COLOR_WARNING);
        label_set_text_cached(s_cal_card.sub1, "校时后显示到期项");
        return;
    }

    label_set_text_cached(s_cal_card.main, main_text);
    label_set_color_cached(s_cal_card.main, UI_COLOR_PRIMARY);

    /*
     * 到期条数：todo_list_filtered() 会给一份清单快照（PSRAM），所以只在
     * 「版本号变了或跨天了」的时候才取，取完立刻释放。
     */
    todo_item_t *items = NULL;
    size_t count = 0;
    const esp_err_t err = todo_list_filtered(TODO_FILTER_DUE_TODAY, &items, &count);

    char sub1_text[32];
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "due-today list unavailable, calendar card shows a hint");
        snprintf(sub1_text, sizeof(sub1_text), "到期数暂不可用");
    } else if (count == 0) {
        snprintf(sub1_text, sizeof(sub1_text), "今天没有到期");
    } else {
        snprintf(sub1_text, sizeof(sub1_text), "今天 %u 项到期", (unsigned)count);
    }
    if (items != NULL) {
        todo_free_list(items);
    }

    label_set_text_cached(s_cal_card.sub1, sub1_text);
}

/* ------------------------------------------------------------------------- */
/* 磁贴                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *title;
    const char *subtitle;
    ui_page_id_t target;
} home_tile_def_t;

/**
 * @brief 首页磁贴。
 *
 * v1.6.0 起「单词」和「日历」不再是磁贴 —— 它们已经做成了上面那三张状态卡里
 * 的两张，同一件事不留两个入口。剩下四个磁贴各占 LV_PCT(23)，一行排开。
 */
static const home_tile_def_t s_tiles[] = {
    { "WiFi",   "扫描 · 自动重连",   UI_PAGE_WIFI },
    { "待办",   "清单 · 同步",       UI_PAGE_TODO },
    { "天气",   "温度 · 湿度",       UI_PAGE_WEATHER },
    { "设置",   "亮度 · 密钥",       UI_PAGE_SETTINGS },
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

    /*
     * 三张状态卡（AI 对话 / 单词 / 日历）也各自有脏检查：
     *   - AI 卡：每 5 个 tick（或回到首页时）才去读一次 NVS；
     *   - 单词卡：先比便宜指纹，指纹没变就不跑 word_stats()；
     *   - 日历卡：清单版本号 + 日期没变就不取清单快照。
     * 内容没变时一个字节都不写，整页也就不会每秒重排一次。
     */
    s_refresh_count++;
    const bool force_cards = s_cards_dirty;
    s_cards_dirty = false;
    ai_card_render(force_cards);
    word_card_render(force_cards);
    cal_card_render(&st, force_cards);

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

    /*
     * 时钟与摘要**并成一行**：真机实测（debug shell 的 widgets 树）抬头原来是
     * 85 px（问候 34 + 时钟 24 + 摘要 19 + 行距），首页内容 466 px > 可视 435 px，
     * 磁贴被推到折叠线以下。合成一行后抬头 62 px，首页一屏放得下。
     */
    lv_obj_t *info_row = lv_obj_create(head);
    if (info_row == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(info_row);
    lv_obj_set_width(info_row, LV_PCT(100));
    lv_obj_set_height(info_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(info_row, 12, 0);
    lv_obj_remove_flag(info_row, LV_OBJ_FLAG_SCROLLABLE);

    s_clock_label = lv_label_create(info_row);
    lv_label_set_text(s_clock_label, UI_STATUS_TIME_UNSYNCED);
    lv_obj_set_style_text_font(s_clock_label, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_clock_label, UI_COLOR_PRIMARY, 0);

    s_summary_label = lv_label_create(info_row);
    lv_label_set_text(s_summary_label, "");
    lv_obj_set_style_text_font(s_summary_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_summary_label, UI_COLOR_TEXT_MUTED, 0);

    /* 今日待办卡片：夹在抬头与磁贴网格之间。 */
    if (today_card_create(parent) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    /* 三张状态卡（AI 对话 / 单词 / 日历）：同一行、同一高度、同一套视觉。 */
    if (status_row_create(parent) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * 词库进度变了就把单词卡标成脏。事件只带一个标志，真正的取数在
     * home_refresh() 里做（回调里不做活）。
     */
    (void)ui_event_subscribe(WORD_EVENT, ESP_EVENT_ANY_ID, 0, word_event_cb);

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
        /* Four columns per row on an 800 px wide panel: 4×23% + 3×12 px = 746 px
         * < 772 px, so they stay on one line. */
        lv_obj_set_width(tile, LV_PCT(23));
    }

    /* 这里**不**取数：ui_core 建完页面紧接着就会调 on_show()，那边取一次就够
     * （以前这里取一次、on_show 又取一次，白白多跑一遍 word_stats() + 一次
     * 清单快照）。 */
    SYS_LOGD(TAG, "home built with %u tiles and 3 status cards",
             (unsigned)(sizeof(s_tiles) / sizeof(s_tiles[0])));
    return ESP_OK;
}

static esp_err_t home_on_show(void)
{
    /* 回到首页：AI 的对话轮数、单词的复习进度都可能已经变了，强制重新取数。 */
    s_cards_dirty = true;
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
