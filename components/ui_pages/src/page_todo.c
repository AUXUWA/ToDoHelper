/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_todo.c
 * @brief FR2: the device side of the todo list — a viewer with two write actions.
 *
 * All todo creation/editing happens on the PC web page. The touch panel has no
 * keyboard worth typing Chinese on, and letting two writers race on the same
 * record made the sync state hard to reason about, so this page deliberately
 * exposes **no** control that can create or edit a todo. It stays useful as a
 * viewer: the list shows title, done state, priority, due date and sync state,
 * the detail page shows the remaining fields, and both point the user at the
 * web page.
 *
 * Two write actions remain, because they are single taps and a tap is the one
 * input this panel is good at:
 *   - the row checkbox toggles "done" (row_toggle_cb);
 *   - the row delete button asks for confirmation first (row_delete_cb ->
 *     ui_dialog_confirm), because a single mis-tap used to destroy a todo for
 *     good.
 *
 * Binds TODO_EVENT_LIST_CHANGED so a change made from the PC browser (which
 * arrives through sync_server -> todo_manager) redraws this page immediately.
 * The rows are rebuilt from a snapshot each time the *shape* of the list
 * changes; otherwise the widgets are updated in place.
 *
 * The row widgets are **destroyed when the page is hidden** and rebuilt from the
 * cached snapshot when it comes back (ROADMAP v1.8.1 §2): ui_core only *hides*
 * the page container, so without this the rows (the biggest batch of objects on
 * this page) would stay in the heap forever and shred the internal heap into
 * unusable fragments. The page's chrome (header, filter bar, stats line, the
 * two height placeholders) stays alive - it is tiny, and rebuilding it would
 * make the page flicker.
 *
 * Every LVGL call below runs in the LVGL task: create()/on_show()/on_event()
 * are invoked by ui_core, and ui_event_bind() marshals into that task — see
 * AGENT.md 9.0.1.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"

#include "pages_internal.h"

#include "todo_manager.h"

static const char *TAG = "page_todo";

/** Shown in place of the removed editing controls. */
#define TODO_READONLY_HINT "此页可勾选完成 / 删除 | 新增请到电脑网页端"

/**
 * How many rows are built at most.
 *
 * Every row is ~8 LVGL objects, and small objects land in the internal heap
 * first (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096). 256 rows would be ~2000
 * widget objects while the internal RAM headroom after boot is only 66 KB
 * (AGENT.md 9.0) — that is a reboot waiting to happen. 30 rows is also all
 * that fits on the 480 px panel (56 px per row); the rest stays reachable
 * from the PC web page.
 */
#define TODO_RENDER_MAX_ROWS 30

/**
 * 行距：行高（UI_LIST_ROW_HEIGHT = 56）+ 列表的行间距（s_list 的 pad_row 8 px）。
 *
 * 视口里放得下几行、滚动时窗口往哪挪，全靠这个数。行高是固定的：勾选框
 * 44 px 加上下各 6 px 内边距正好 56 px，所以这个数是准的（第一次布局之后
 * 还会用实测行高再校准一次，见 s_row_h）。
 */
#define TODO_ROW_GAP    8
#define TODO_ROW_PITCH  (UI_LIST_ROW_HEIGHT + TODO_ROW_GAP)

/** 视口上下各多渲染一行：滚动时这一行已经在屏外，不会看到空白。 */
#define TODO_WINDOW_BUFFER 1

/** 同时存活的行数上限（可视行数 + 上下缓冲）。 */
#define TODO_WINDOW_MAX_ROWS 10

/** 视口高度还量不出来时按这个值算（480 - 40 状态栏 - 页内边距）。 */
#define TODO_FALLBACK_VIEW_H 435

/** 筛选档位数量，与 s_filter_tabs[] 一致。 */
#define TODO_FILTER_COUNT 4

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

static lv_obj_t *s_list;
static lv_obj_t *s_stats;
static lv_obj_t *s_more;      /**< 「还有 N 条未显示」提示 */
static lv_obj_t *s_empty;     /**< 空状态标签 */
static lv_obj_t *s_filter_btn[TODO_FILTER_COUNT];
static int s_filter_index;    /**< 当前选中的筛选档 */
static bool s_stats_warn = true;  /**< 统计行是否已写成「待同步」色 */

static const struct {
    todo_filter_t filter;
    const char *label;
} s_filter_tabs[TODO_FILTER_COUNT] = {
    { TODO_FILTER_ALL, "全部" },
    { TODO_FILTER_OPEN, "未完成" },
    { TODO_FILTER_DUE_TODAY, "今天到期" },
    { TODO_FILTER_DONE, "已完成" },
};

/*
 * Incremental list state.
 *
 * The list used to be torn down and rebuilt on every change: lv_obj_clean() plus
 * 5-7 fresh objects per item, up to 256 items - so tapping one checkbox destroyed
 * and re-created well over a thousand widgets, which read as a full page reload
 * (smoothness audit P2).
 *
 * Now each row remembers the todo id it was built for. A refresh walks the
 * existing rows and updates the widgets in place; the list is only rebuilt when
 * the *set or order* of ids changed (add, delete, reorder), which is exactly the
 * case where new widget objects are genuinely needed.
 *
 * The bookkeeping array is allocated from PSRAM on first use rather than being
 * static: 256 entries live in internal RAM otherwise, and internal RAM is this
 * board's scarcest resource (measured: static version cost 9 KB of the
 * `boot complete` headroom, AGENT.md 9.0).
 */
typedef struct {
    uint32_t id;
    lv_obj_t *row;
    lv_obj_t *prio;      /**< 优先级色条 */
    lv_obj_t *checkbox;
    lv_obj_t *title;
    lv_obj_t *due;       /**< 到期徽标 */
    lv_obj_t *badge;     /**< 同步徽标 */
    lv_obj_t *del_row;   /**< 删除按钮 */
    uint8_t done_state;  /**< 上次写入的完成态 */
    uint8_t prio_state;  /**< 上次写入的优先级档（0 普通 / 1 重要 / 2 紧急） */
    uint8_t due_state;   /**< 上次写入的到期档（0 = 无到期日） */
    uint8_t sync_state;  /**< 上次写入的同步徽标状态（0 = 无徽标） */
    char due_text[16];   /**< 上次写入的到期文案 */
} todo_row_t;

static todo_row_t *s_rows;
static size_t s_rows_cap;
static size_t s_row_count;

/*
 * 只渲染视口内的行。
 *
 * 以前一次把 30 行全建出来（240 个 LVGL 控件），其中 5/6 永远看不见；现在
 * 只建视口里那几行加上上下各一行缓冲，滚动时窗口跟着挪。窗口之外的行用两个
 * 空的占位对象把高度补出来，所以列表的总高度、滚动条长度、每一行的位置都跟
 * 「全部行都在」时一模一样 —— 用户看到的内容一个字节都没变。
 */

/** 当前快照：滚动时要按它重画，所以不能像以前那样画完就释放。 */
static todo_item_t *s_items;
static size_t s_item_count;

/** 当前渲染的窗口：[s_win_start, s_win_start + s_row_count)。 */
static size_t s_win_start;

/** 窗口上方 / 下方的占位对象（空对象，只占高度）。 */
static lv_obj_t *s_gap_top;
static lv_obj_t *s_gap_bottom;

/** 占位对象上一次写入的高度（-1 = 没写过 / 已隐藏）。 */
static int32_t s_gap_top_h = -1;
static int32_t s_gap_bottom_h = -1;

/** 视口（列表可见区）高度，量一次用一阵。 */
static int32_t s_view_h;

/** 实测行高：第一次布局之后才知道，用来校准行距。 */
static int32_t s_row_h = UI_LIST_ROW_HEIGHT;

/** 现在的行距（实测行高 + 行间距）。 */
static int32_t row_pitch(void)
{
    return (s_row_h > 0) ? (s_row_h + TODO_ROW_GAP) : TODO_ROW_PITCH;
}

/**
 * 记住的滚动位置。
 *
 * 页面被切走时行控件会被销毁（on_hide），列表的内容因此会塌回顶部；回来时
 * 要按这个位置复原。改动之前行控件一直留在页面上，滚动位置自然不会丢，
 * 所以这里复刻的是改动前的观感 —— 不是「回到顶部」。
 */
static int32_t s_scroll_y;

/**
 * Make sure the row table can hold @p need entries. Returns false on OOM.
 *
 * The table is only grown from render_window(), which knows the final row count
 * up front. Reserving per row (the old code did) copied the whole table again
 * for every row — O(n^2) memcpy for a 256 item list.
 */
static bool rows_reserve(size_t need)
{
    if (s_rows_cap >= need) {
        return true;
    }
    if (need > TODO_MAX_ITEMS) {
        need = TODO_MAX_ITEMS;
    }

    /* PSRAM first: this is bulk bookkeeping, not something DMA or an ISR touches
     * (heap_caps_malloc_prefer falls back to the default heap if PSRAM is gone). */
    todo_row_t *grown = heap_caps_malloc_prefer(need * sizeof(todo_row_t), 2,
                                                MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (grown == NULL) {
        SYS_LOGE(TAG, "cannot allocate %u row slots", (unsigned)need);
        return false;
    }
    if (s_rows != NULL) {
        const size_t keep = (s_row_count < need) ? s_row_count : need;
        memcpy(grown, s_rows, keep * sizeof(todo_row_t));
        if (need > keep) {
            memset(grown + keep, 0, (need - keep) * sizeof(todo_row_t));
        }
        heap_caps_free(s_rows);
    }
    s_rows = grown;
    s_rows_cap = need;
    return true;
}

/**
 * 到期徽标的文案与颜色。
 *
 * 分档一律走 todo_due_bucket()：设备没校过时它对任何到期时间都返回
 * TODO_DUE_NONE（不编造日期），所以这里不必自己判断时钟状态。
 *
 * @param[out] out    文案缓冲（至少 16 字节：「12月31日」是 10 字节）。
 * @param[out] color  文案颜色。
 * @return 到期分档。
 */
static todo_due_bucket_t due_badge_text(int64_t due_ts, char *out, size_t out_len,
                                        lv_color_t *color)
{
    switch (todo_due_bucket(due_ts)) {
    case TODO_DUE_OVERDUE:
        strlcpy(out, "逾期", out_len);
        *color = UI_COLOR_DANGER;
        return TODO_DUE_OVERDUE;
    case TODO_DUE_TODAY:
        strlcpy(out, "今天", out_len);
        *color = UI_COLOR_WARNING;
        return TODO_DUE_TODAY;
    case TODO_DUE_TOMORROW:
        strlcpy(out, "明天", out_len);
        *color = UI_COLOR_PRIMARY;
        return TODO_DUE_TOMORROW;
    case TODO_DUE_LATER: {
        const time_t due = (time_t)due_ts;
        struct tm tm_due;
        if (localtime_r(&due, &tm_due) == NULL) {
            strlcpy(out, "稍后", out_len);
        } else {
            /* 夹到日历可能的范围，再格式化到定长临时缓冲后才拷贝。
             * out_len 是运行期参数，直接格式化会让编译器无法证明
             * 「12月31日」放得下，于是触发 -Werror=format-truncation。 */
            int mon = tm_due.tm_mon + 1;
            int day = tm_due.tm_mday;
            if (mon < 1) {
                mon = 1;
            } else if (mon > 12) {
                mon = 12;
            }
            if (day < 1) {
                day = 1;
            } else if (day > 31) {
                day = 31;
            }
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%d月%d日", mon, day);
            strlcpy(out, tmp, out_len);
        }
        *color = UI_COLOR_TEXT_MUTED;
        return TODO_DUE_LATER;
    }
    case TODO_DUE_NONE:
    default:
        out[0] = '\0';
        return TODO_DUE_NONE;
    }
}

/**
 * 事件里的 user data 是行在窗口里的序号，不是待办 id。
 *
 * 滚动时同一批行控件会被换成另一批待办（只渲染视口内的行），所以 id 不能
 * 写死在事件里 —— 每次都从行表里读当前这一行对应的 id 才是对的。
 */
static uint32_t row_event_id(lv_event_t *e)
{
    const size_t slot = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (s_rows == NULL || slot >= s_row_count) {
        return 0;
    }
    return s_rows[slot].id;
}

/** Tapping a row is navigation only: it opens the read-only detail page. */
static void row_open_cb(lv_event_t *e)
{
    const uint32_t id = row_event_id(e);
    if (id == 0) {
        return;
    }
    page_todo_detail_set_id(id);
    (void)ui_nav_push(UI_PAGE_TODO_DETAIL);
}

/* Defined below with the other row callbacks, but bound while a row is built. */
static void row_toggle_cb(lv_event_t *e);
static void row_delete_cb(lv_event_t *e);

/**
 * Apply one snapshot item to an existing row, without touching its structure.
 *
 * Only writes what actually differs: an unchanged label would still be
 * invalidated by lv_label_set_text(), and the row would re-layout.
 */
static void row_apply(todo_row_t *r, const todo_item_t *item)
{
    const bool done = item->done;

    /* 完成态：勾选框、标题颜色与「✓ 」前缀一起变，值没变就一个字节都不写。 */
    if (r->done_state != (done ? 1u : 0u)) {
        if (done) {
            lv_obj_add_state(r->checkbox, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(r->checkbox, LV_STATE_CHECKED);
        }
        lv_obj_set_style_text_color(r->title, done ? UI_COLOR_TEXT_MUTED : UI_COLOR_TEXT, 0);
        r->done_state = done ? 1u : 0u;
    }

    /* Title text: "✓ " prefix when done, so a value change is visible. */
    char want[192];
    if (done) {
        snprintf(want, sizeof(want), "✓ %s", item->title);
    } else {
        snprintf(want, sizeof(want), "%s", item->title);
    }
    if (strcmp(want, lv_label_get_text(r->title)) != 0) {
        lv_label_set_text(r->title, want);
    }

    /*
     * 优先级色条：紧急=危险色、重要=警告色、普通=不着色。
     * 「普通」保留一个等宽的透明占位，标题才不会在两档之间左右跳。
     */
    const uint8_t prio = (item->priority >= 2) ? 2u : ((item->priority == 1) ? 1u : 0u);
    if (r->prio_state != prio) {
        switch (prio) {
        case 2:
            lv_obj_set_style_bg_color(r->prio, UI_COLOR_DANGER, 0);
            lv_obj_set_style_bg_opa(r->prio, LV_OPA_COVER, 0);
            break;
        case 1:
            lv_obj_set_style_bg_color(r->prio, UI_COLOR_WARNING, 0);
            lv_obj_set_style_bg_opa(r->prio, LV_OPA_COVER, 0);
            break;
        default:
            lv_obj_set_style_bg_opa(r->prio, LV_OPA_TRANSP, 0);
            break;
        }
        r->prio_state = prio;
    }

    /* 到期徽标：没有到期日就藏起来（对象一直存在，行的子控件顺序才稳定）。 */
    char due_text[16];
    lv_color_t due_color;
    const todo_due_bucket_t bucket =
        due_badge_text(item->due_ts, due_text, sizeof(due_text), &due_color);
    if (bucket == TODO_DUE_NONE) {
        if (r->due_state != 0) {
            lv_obj_add_flag(r->due, LV_OBJ_FLAG_HIDDEN);
            r->due_state = 0;
            r->due_text[0] = '\0';
        }
    } else {
        if (strcmp(due_text, r->due_text) != 0) {
            lv_label_set_text(r->due, due_text);
            strlcpy(r->due_text, due_text, sizeof(r->due_text));
        }
        if (r->due_state != (uint8_t)bucket) {
            lv_obj_set_style_text_color(r->due, due_color, 0);
            if (r->due_state == 0) {
                lv_obj_remove_flag(r->due, LV_OBJ_FLAG_HIDDEN);
            }
            r->due_state = (uint8_t)bucket;
        }
    }

    /* 同步徽标：颜色与文案分开写，只有真的变了才动。 */
    if (item->sync_state != TODO_SYNC_OK) {
        const char *text = (item->sync_state == TODO_SYNC_FAILED) ? "失败" : "待同步";
        const uint8_t state = (item->sync_state == TODO_SYNC_FAILED) ? 2u : 1u;
        if (strcmp(text, lv_label_get_text(r->badge)) != 0) {
            lv_label_set_text(r->badge, text);
        }
        if (r->sync_state != state) {
            lv_obj_set_style_text_color(r->badge,
                                        (state == 2u) ? UI_COLOR_DANGER : UI_COLOR_WARNING, 0);
            if (r->sync_state == 0) {
                lv_obj_remove_flag(r->badge, LV_OBJ_FLAG_HIDDEN);
            }
            r->sync_state = state;
        }
    } else if (r->sync_state != 0) {
        lv_obj_add_flag(r->badge, LV_OBJ_FLAG_HIDDEN);
        r->sync_state = 0;
    }
}

/**
 * 建一行（从零开始），并记住它的控件。
 *
 * 每个 lv_obj_* 的返回值都要检查：分配失败就把半行删掉、返回 false，让调用
 * 方停下来并给用户一句提示 —— 而不是拿着空指针往下走（Load access fault）。
 *
 * @param item 这一行要显示的待办。
 * @param slot 这一行在窗口里的序号（事件里带的 user data 就是它）。
 * @return false 表示内存不够，这一行没建成。
 */
static bool row_create(const todo_item_t *item, size_t slot)
{
    lv_obj_t *row = NULL;
    lv_obj_t *prio = NULL;
    lv_obj_t *checkbox = NULL;
    lv_obj_t *title = NULL;
    lv_obj_t *due = NULL;
    lv_obj_t *badge = NULL;
    lv_obj_t *del = NULL;
    lv_obj_t *del_label = NULL;

    if (s_rows == NULL || slot >= s_rows_cap) {
        /* render_window() 预留过行表；走到这里说明那次预留失败了。 */
        return false;
    }
    todo_row_t *r = &s_rows[slot];
    memset(r, 0, sizeof(*r));
    r->id = item->id;

    row = lv_obj_create(s_list);
    if (row == NULL) {
        SYS_LOGE(TAG, "row %u: no memory for the row", (unsigned)slot);
        return false;
    }
    r->row = row;
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, UI_LIST_ROW_HEIGHT, 0);
    lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_CARD_RADIUS, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* 优先级色条：纯色小方块，零位图。普通优先级是不着色的透明占位。 */
    prio = lv_obj_create(row);
    if (prio == NULL) {
        goto fail;
    }
    r->prio = prio;
    lv_obj_remove_style_all(prio);
    lv_obj_set_size(prio, 8, 24);
    lv_obj_set_style_radius(prio, 2, 0);
    lv_obj_set_style_bg_opa(prio, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(prio, LV_OBJ_FLAG_SCROLLABLE);

    /* The checkbox *is* a control: marking a todo done is one tap.
     * 44x44 (UI_TOUCH_MIN)：34x34 时手指落在下缘会滑到标题上，变成
     * 「打开详情页」而不是勾选。 */
    checkbox = lv_checkbox_create(row);
    if (checkbox == NULL) {
        goto fail;
    }
    r->checkbox = checkbox;
    lv_checkbox_set_text(checkbox, "");
    lv_obj_set_size(checkbox, UI_TOUCH_MIN, UI_TOUCH_MIN);
    lv_obj_add_flag(checkbox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(checkbox, row_toggle_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)slot);

    title = lv_label_create(row);
    if (title == NULL) {
        goto fail;
    }
    r->title = title;
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(title, ui_font(UI_FONT_BODY), 0);

    /* 到期徽标：无到期日时隐藏。 */
    due = lv_label_create(row);
    if (due == NULL) {
        goto fail;
    }
    r->due = due;
    lv_obj_set_style_text_font(due, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(due, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_add_flag(due, LV_OBJ_FLAG_HIDDEN);

    /* 同步徽标：已同步时不显示。 */
    badge = lv_label_create(row);
    if (badge == NULL) {
        goto fail;
    }
    r->badge = badge;
    lv_obj_set_style_text_font(badge, ui_font(UI_FONT_SMALL), 0);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);

    /* 删除按钮：同样要 >= 44px（原来是 52x34）。 */
    del = lv_button_create(row);
    if (del == NULL) {
        goto fail;
    }
    r->del_row = del;
    lv_obj_set_size(del, 52, UI_TOUCH_MIN);
    lv_obj_set_style_bg_color(del, UI_COLOR_DANGER, 0);
    lv_obj_set_style_radius(del, UI_CARD_RADIUS - 4, 0);
    lv_obj_set_style_shadow_width(del, 0, 0);
    del_label = lv_label_create(del);
    if (del_label == NULL) {
        goto fail;
    }
    lv_label_set_text(del_label, "删除");
    lv_obj_set_style_text_font(del_label, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(del_label, UI_COLOR_ON_PRIMARY, 0);
    lv_obj_center(del_label);
    lv_obj_add_event_cb(del, row_delete_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)slot);

    /* Tapping the row (or its title) opens the read-only detail page. */
    lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(title, row_open_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)slot);
    lv_obj_add_event_cb(row, row_open_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)slot);

    row_apply(r, item);
    return true;

fail:
    SYS_LOGE(TAG, "row %u: out of memory, row dropped", (unsigned)slot);
    if (r->row != NULL) {
        /* 半行不能留在列表里：删掉它，剩下的控件跟着一起没了。 */
        lv_obj_delete(r->row);
        r->row = NULL;
    }
    return false;
}

/**
 * Drop every row (structural change, an empty list, or the page being hidden).
 *
 * 「行还在不在」由 s_row_count / s_win_start 这两个标志说了算，所以这里必须
 * 把它们一起清掉 —— 否则第二次进页面时会以为行还在，结果屏幕上空空如也。
 */
static void rows_clear(void)
{
    if (s_rows != NULL) {
        for (size_t i = 0; i < s_row_count; i++) {
            if (s_rows[i].row != NULL) {
                lv_obj_delete(s_rows[i].row);
            }
        }
        memset(s_rows, 0, s_row_count * sizeof(todo_row_t));
    }
    s_row_count = 0;
    s_win_start = 0;
    if (s_empty != NULL) {
        /* 空状态标签住在列表里，它跟着一起没了。 */
        lv_obj_delete(s_empty);
        s_empty = NULL;
    }
}

/** Create the "nothing here yet" label (once). */
static void list_empty_show(void)
{
    if (s_list == NULL || s_empty != NULL) {
        return;
    }
    s_empty = lv_label_create(s_list);
    if (s_empty == NULL) {
        return;
    }
    lv_label_set_text(s_empty, "还没有待办。\n请在电脑网页端新增（浏览器打开设备地址）。");
    lv_obj_set_style_text_font(s_empty, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_empty, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_pad_all(s_empty, 24, 0);
}

/**
 * 建一个只占高度的空对象（窗口之外的行用它占位）。
 *
 * 没有背景、没有边框，也不接收触摸：它唯一的作用是让列表的总高度跟
 * 「全部行都在」时一模一样，滚动条长度因此跟以前一样。
 */
static lv_obj_t *gap_create(void)
{
    lv_obj_t *gap = lv_obj_create(s_list);
    if (gap == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(gap);
    lv_obj_set_width(gap, LV_PCT(100));
    lv_obj_set_height(gap, 0);
    lv_obj_remove_flag(gap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(gap, LV_OBJ_FLAG_HIDDEN);
    return gap;
}

/**
 * 让列表的总高度跟「全部行都在」时一模一样。
 *
 * 窗口上方缺 start 行、下方缺 (total - start - slots) 行，这两段高度由两个
 * 占位对象补出来。算式是照着 flex 的排版来的：行与行之间有一个 TODO_ROW_GAP
 * 的间距，所以占位对象的高度要减掉这个间距（而且最后一个占位对象如果高度
 * 归零就整个藏起来 —— 藏起来之后 flex 不再给它留间距，总高度才对得上）。
 *
 * 高度没变就不写（滚动事件每个像素都来一次，无脑重设样式会让整页重排）。
 */
static void window_gaps(size_t start, size_t slots, size_t total)
{
    if (s_gap_top == NULL || s_gap_bottom == NULL) {
        return;
    }

    if (slots == 0 || total == 0) {
        if (!lv_obj_has_flag(s_gap_top, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_gap_top, LV_OBJ_FLAG_HIDDEN);
        }
        if (!lv_obj_has_flag(s_gap_bottom, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_gap_bottom, LV_OBJ_FLAG_HIDDEN);
        }
        s_gap_top_h = -1;
        s_gap_bottom_h = -1;
        return;
    }

    if (start > 0) {
        const int32_t h = (int32_t)(start * (size_t)row_pitch()) - TODO_ROW_GAP;
        if (lv_obj_has_flag(s_gap_top, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(s_gap_top, LV_OBJ_FLAG_HIDDEN);
        }
        if (h != s_gap_top_h) {
            s_gap_top_h = h;
            lv_obj_set_height(s_gap_top, h);
        }
    } else {
        if (!lv_obj_has_flag(s_gap_top, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_gap_top, LV_OBJ_FLAG_HIDDEN);
        }
        s_gap_top_h = -1;
    }

    const size_t rest = total - start - slots;
    if (rest > 0) {
        const int32_t h = (int32_t)(rest * (size_t)row_pitch()) - TODO_ROW_GAP;
        if (lv_obj_has_flag(s_gap_bottom, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(s_gap_bottom, LV_OBJ_FLAG_HIDDEN);
        }
        if (h != s_gap_bottom_h) {
            s_gap_bottom_h = h;
            lv_obj_set_height(s_gap_bottom, h);
        }
    } else {
        if (!lv_obj_has_flag(s_gap_bottom, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_gap_bottom, LV_OBJ_FLAG_HIDDEN);
        }
        s_gap_bottom_h = -1;
    }
}

/**
 * 把窗口里的每一行画成对应的那条待办。
 *
 * 行控件是复用的：窗口挪动时同一批控件换成另一批待办，所以这里只改内容，
 * 不重建控件（铁律二：先比较再写）。
 */
static void window_paint(size_t start, size_t slots, size_t total)
{
    for (size_t i = 0; i < slots; i++) {
        const size_t idx = start + i;
        if (idx >= total || s_items == NULL) {
            break;
        }
        todo_row_t *r = &s_rows[i];
        r->id = s_items[idx].id;
        row_apply(r, &s_items[idx]);
    }
}

/**
 * 画列表：只画视口里的那几行。
 *
 * @param measure true = 重新量视口高度（数据变了的时候），false = 沿用上次
 *                量到的高度（滚动的时候用，免得每滚一个像素就重排一次）。
 */
static void render_window(bool measure)
{
    if (s_list == NULL || s_items == NULL) {
        return;
    }

    const size_t total = (s_item_count > TODO_RENDER_MAX_ROWS) ? TODO_RENDER_MAX_ROWS : s_item_count;

    /* 底部提示：被截断的条数（是清单本身的截断，与渲染窗口无关）。 */
    if (s_more != NULL) {
        if (s_item_count > total) {
            /* 中文按 UTF-8 每字 3 字节，整句加上数字最多约 70 字节；
             * 缓冲区给足，否则编译器会报 format-truncation。 */
            char buf[96];
            snprintf(buf, sizeof(buf), "还有 %u 条未显示（在电脑网页端查看全部）",
                     (unsigned)(s_item_count - total));
            if (strcmp(buf, lv_label_get_text(s_more)) != 0) {
                lv_label_set_text(s_more, buf);
            }
            if (lv_obj_has_flag(s_more, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_remove_flag(s_more, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            if (lv_label_get_text(s_more)[0] != '\0') {
                lv_label_set_text(s_more, "");
            }
            if (!lv_obj_has_flag(s_more, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(s_more, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (total == 0) {
        if (s_row_count != 0) {
            rows_clear();
        }
        window_gaps(0, 0, 0);
        list_empty_show();
        return;
    }

    if (measure) {
        /* 视口多高？量一次就够，滚动时不再量。 */
        lv_obj_update_layout(s_list);
        const int32_t h = lv_obj_get_height(s_list);
        if (h > 0) {
            s_view_h = h;
        }
        if (s_row_count > 0 && s_rows[0].row != NULL) {
            const int32_t rh = lv_obj_get_height(s_rows[0].row);
            if (rh > 0) {
                s_row_h = rh;
            }
        }
    }
    if (s_view_h <= 0) {
        s_view_h = TODO_FALLBACK_VIEW_H;
    }

    /* 视口里放得下几行 —— 再加上下各一行缓冲。 */
    size_t visible = (size_t)((s_view_h + row_pitch() - 1) / row_pitch());
    size_t win = visible + 2 * TODO_WINDOW_BUFFER;
    if (win > TODO_WINDOW_MAX_ROWS) {
        win = TODO_WINDOW_MAX_ROWS;
    }
    size_t slots = (win < total) ? win : total;

    /* 窗口起点：视口里第一条行往上留一行缓冲。 */
    size_t first = 0;
    const int32_t scroll_y = lv_obj_get_scroll_y(s_list);
    if (scroll_y > 0) {
        first = (size_t)(scroll_y / row_pitch());
    }
    size_t start = (first > TODO_WINDOW_BUFFER) ? first - TODO_WINDOW_BUFFER : 0;
    if (start + slots > total) {
        start = total - slots;
    }

    if (measure) {
        /* 行的个数变了才重建控件；只是换了内容就原地改。 */
        if (slots != s_row_count) {
            rows_clear();
            if (!rows_reserve(slots)) {
                return;
            }
            for (size_t i = 0; i < slots; i++) {
                if (!row_create(&s_items[start + i], i)) {
                    /* 内存不够：不再往下建，给用户一句提示。 */
                    ui_toast("列表显示不完整：内存不足");
                    break;
                }
                s_row_count++;
            }
            /* 占位对象要排在所有行后面（flex 按子对象顺序排版）。 */
            if (s_gap_bottom != NULL) {
                lv_obj_move_to_index(s_gap_bottom, -1);
            }
        }
    } else {
        /* 滚动路径：控件已经在，只挪窗口，不重建。 */
        if (s_row_count == 0) {
            return;
        }
        if (start + s_row_count > total) {
            start = total - s_row_count;
        }
        if (start == s_win_start) {
            /* 窗口没动：内容也不用重画（滚动事件每个像素都来一次）。 */
            return;
        }
    }

    s_win_start = start;
    window_gaps(start, s_row_count, total);
    window_paint(start, s_row_count, total);
}

/* ------------------------------------------------------------------------ */
/* Render                                                                    */
/* ------------------------------------------------------------------------ */

/**
 * 统计行：跟着当前筛选走。
 *
 * 「全部」保留原来的三项统计；其它档位显示「<档位> N / 共 M」，让用户看得见
 * 筛选之后还剩多少（需求 F2）。
 */
static void render_stats(size_t shown)
{
    if (s_stats == NULL) {
        return;
    }

    todo_stats_t stats;
    if (todo_stats(&stats) != ESP_OK) {
        return;
    }

    char buf[96];
    if (s_filter_index == 0) {
        snprintf(buf, sizeof(buf), "共 %u 项 · 已完成 %u · 待同步 %u",
                 (unsigned)stats.total, (unsigned)stats.done, (unsigned)stats.pending);
    } else {
        snprintf(buf, sizeof(buf), "%s %u / 共 %u", s_filter_tabs[s_filter_index].label,
                 (unsigned)shown, (unsigned)stats.total);
    }

    if (strcmp(buf, lv_label_get_text(s_stats)) != 0) {
        lv_label_set_text(s_stats, buf);
    }

    const bool warn = stats.pending > 0;
    if (warn != s_stats_warn) {
        lv_obj_set_style_text_color(s_stats,
                                    warn ? UI_COLOR_WARNING : UI_COLOR_TEXT_MUTED, 0);
        s_stats_warn = warn;
    }
}

/* ------------------------------------------------------------------------ */
/* Filter bar                                                                */
/* ------------------------------------------------------------------------ */

/** refresh() 在下面定义，但筛选按钮要调用它。 */
static void refresh(void);

/** 选中态用主色，未选中用次要面色 —— 一眼能看出当前在看哪一档。 */
static void filter_chip_style(lv_obj_t *btn, bool selected)
{
    if (btn == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(btn, selected ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE_ALT, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);

    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label != NULL) {
        lv_obj_set_style_text_color(label,
                                    selected ? UI_COLOR_ON_PRIMARY : UI_COLOR_TEXT_MUTED, 0);
    }
}

static void filter_tab_cb(lv_event_t *e)
{
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= TODO_FILTER_COUNT) {
        return;
    }
    if (index == s_filter_index) {
        return;
    }

    /* 只改两个 chip 的样式，不整页重建。 */
    filter_chip_style(s_filter_btn[s_filter_index], false);
    s_filter_index = index;
    filter_chip_style(s_filter_btn[s_filter_index], true);

    /* 切换筛选只影响显示：不改数据，也不碰持久化。 */
    refresh();
}

static void filter_bar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    if (bar == NULL) {
        return;
    }
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, UI_TOUCH_MIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < TODO_FILTER_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(bar);
        if (btn == NULL) {
            continue;
        }
        s_filter_btn[i] = btn;
        lv_obj_set_height(btn, UI_TOUCH_MIN);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_radius(btn, UI_CARD_RADIUS, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_hor(btn, 6, 0);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, s_filter_tabs[i].label);
        lv_obj_set_style_text_font(label, ui_font(UI_FONT_SMALL), 0);
        lv_obj_center(label);

        lv_obj_add_event_cb(btn, filter_tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    /* 初始态：第 0 档选中。 */
    for (int i = 0; i < TODO_FILTER_COUNT; i++) {
        filter_chip_style(s_filter_btn[i], i == s_filter_index);
    }
}

/* ------------------------------------------------------------------------ */
/* Row callbacks                                                             */
/* ------------------------------------------------------------------------ */

/*
 * Marking done and deleting stay on the device: both are single taps, and a tap is
 * the one input this panel is good at. *Creating and editing* stay on the web page
 * - they need Chinese text entry and this board has no IME, which is the whole
 * reason the add overlay was removed (see README).
 *
 * Both callbacks are safe to run even though the resulting
 * TODO_EVENT_LIST_CHANGED refreshes this very list: the event is posted, not
 * delivered inline, so the rows are only rebuilt after the callback returns and
 * LVGL is never asked to delete the object that is currently handling an event.
 */
static void row_toggle_cb(lv_event_t *e)
{
    lv_obj_t *checkbox = lv_event_get_target(e);
    const uint32_t id = row_event_id(e);
    if (id == 0) {
        return;
    }
    const bool done = lv_obj_has_state(checkbox, LV_STATE_CHECKED);

    esp_err_t err = todo_set_done(id, done);
    if (err != ESP_OK) {
        /* Put the box back rather than leaving the UI showing a state the store
         * does not have. */
        if (done) {
            lv_obj_remove_state(checkbox, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(checkbox, LV_STATE_CHECKED);
        }
        ui_toast("操作失败：%s", esp_err_to_name(err));
    }
}

/**
 * 删除确认框里点「删除」之后才真正删。
 *
 * 回调拿到的是弹窗对象本身（ui_dialog_confirm 的约定）：先把业务做完，
 * 再关弹窗——同一次点击里「先关再做事」会把后面的动作一起吞掉。
 */
static void delete_confirm_cb(lv_event_t *e)
{
    lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);
    if (mbox == NULL) {
        return;
    }
    const uint32_t id = (uint32_t)(uintptr_t)lv_obj_get_user_data(mbox);

    esp_err_t err = todo_delete(id);
    if (err == ESP_OK) {
        ui_toast("已删除");
    } else {
        ui_toast("删除失败：%s", esp_err_to_name(err));
    }

    lv_msgbox_close(mbox);
}

/** 删除按钮：一按先问一句，误触不再等于永久丢失（需求 B11）。 */
static void row_delete_cb(lv_event_t *e)
{
    const uint32_t id = row_event_id(e);
    if (id == 0) {
        return;
    }

    /* 把标题带进确认文案，用户才看得出要删的是哪一条。 */
    char text[256];
    todo_item_t item;
    if (todo_get(id, &item) == ESP_OK) {
        const int n = snprintf(text, sizeof(text), "确认删除「%s」？删除后无法恢复。", item.title);
        if (n < 0 || (size_t)n >= sizeof(text)) {
            /* 标题太长装不下：宁可少写标题，也不要从多字节字符中间截断。 */
            strlcpy(text, "确认删除这条待办？删除后无法恢复。", sizeof(text));
        }
    } else {
        strlcpy(text, "确认删除这条待办？删除后无法恢复。", sizeof(text));
    }

    lv_obj_t *mbox = ui_dialog_confirm("删除待办", text, "删除", "取消",
                                       delete_confirm_cb, (void *)(uintptr_t)id);
    if (mbox == NULL) {
        ui_toast("无法打开确认框");
    }
}

static void readonly_hint_create(lv_obj_t *parent)
{
    lv_obj_t *hint = lv_label_create(parent);
    if (hint == NULL) {
        return;
    }
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(hint, TODO_READONLY_HINT);
    lv_obj_set_style_text_font(hint, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(hint, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

/* ------------------------------------------------------------------------ */
/* Refresh                                                                   */
/* ------------------------------------------------------------------------ */

/**
 * 取一次快照，统计行与列表共用。
 *
 * 取数统一走 todo_list_filtered()：排序口径与 todo_list() 一致
 * （未完成 → 优先级 → 截止日 → id），所以「只渲染前 30 行」截掉的正是
 * 最不紧急的那几条。
 *
 * 快照这次**不立刻释放**：滚动时要按它重画窗口里的行（只渲染视口内的行），
 * 所以它得留到下一次刷新或者离开本页（on_hide）为止。
 */
static void refresh(void)
{
    todo_item_t *items = NULL;
    size_t count = 0;
    if (todo_list_filtered(s_filter_tabs[s_filter_index].filter, &items, &count) != ESP_OK) {
        return;
    }

    render_stats(count);

    if (s_items != NULL) {
        todo_free_list(s_items);
    }
    s_items = items;
    s_item_count = count;

    render_window(true);
}

/**
 * 列表滚动了：窗口跟着挪。
 *
 * 只换内容、不重建控件（窗口外面那几行用占位对象顶着，滚动条长度不变）。
 */
static void list_scroll_cb(lv_event_t *e)
{
    (void)e;
    render_window(false);
}

/* ------------------------------------------------------------------------ */
/* Page                                                                      */
/* ------------------------------------------------------------------------ */

static esp_err_t todo_create(lv_obj_t *parent)
{
    (void)page_header_create(parent, "待办清单");

    filter_bar_create(parent);

    s_stats = lv_label_create(parent);
    if (s_stats == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_text_font(s_stats, ui_font(UI_FONT_SMALL), 0);
    lv_label_set_text(s_stats, "");

    s_list = lv_obj_create(parent);
    if (s_list == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_style_pad_right(s_list, 6, 0);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    /* 窗口上下的占位对象：把窗口之外的行的高度补出来，滚动条长度不变。
     * 上面的那个必须是列表的第一个子对象，下面的那个排在所有行后面
     * （行是后来才建的，建完要把占位对象挪到末尾）。 */
    s_gap_top = gap_create();
    s_gap_bottom = gap_create();
    if (s_gap_top == NULL || s_gap_bottom == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 滚动：窗口跟着挪，只换内容。 */
    lv_obj_add_event_cb(s_list, list_scroll_cb, LV_EVENT_SCROLL, NULL);

    /* 渲染上限的提示：被截断的条数在这里说明白。 */
    s_more = lv_label_create(parent);
    if (s_more == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(s_more, LV_PCT(100));
    lv_label_set_long_mode(s_more, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(s_more, "");
    lv_obj_set_style_text_font(s_more, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_more, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(s_more, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_more, LV_OBJ_FLAG_HIDDEN);

    /* The "新增 / 标记已同步 / 清除已完成" toolbar used to live here; editing is
     * done on the PC web page, so its slot now carries that hint instead. */
    readonly_hint_create(parent);

    /* 这里**不**取数：ui_core 建完页面紧接着就会调 on_show()，那边取一次就够
     * （以前这里取一次、on_show 又取一次，白白多跑一遍全量快照 + 30 行重画）。 */

    /* Data can change from the HTTP task, so only refresh through events. */
    (void)ui_event_bind(TODO_EVENT, TODO_EVENT_LIST_CHANGED, UI_PAGE_TODO, 0);
    SYS_LOGD(TAG, "todo page built (read-only)");
    return ESP_OK;
}

/**
 * 页面回来之后把列表滚回离开时的位置。
 *
 * 行控件被销毁又重建之后，列表的内容高度变了：内容不够高时 LVGL 会把滚动
 * 位置夹回 0（lv_obj_readjust_scroll），这时按 s_scroll_y 复原。目标位置超出
 * 内容高度时 lv_obj_scroll_to_y() 自己会夹到最大值，所以这里不用再算一遍上限。
 *
 * 如果位置还在（LVGL 一直保留着它，或者惯性滚动又往后走了），就一点都不动 ——
 * 改动之前行控件一直留在页面上，列表停在它自己滚到的位置，观感不能变。
 */
static void scroll_restore(void)
{
    if (s_list == NULL || s_scroll_y <= 0) {
        return;
    }

    /*
     * 判据是「跟记住的值一样吗」，**不是**「是不是 0」。
     *
     * 原来的写法（`if (lv_obj_get_scroll_y(s_list) != 0) return;`）在复核里被判成
     * 一个用户可见的回归：行控件被销毁后列表内容塌成两个占位对象，LVGL 会在
     * 下一次布局时把滚动位置**夹**到「剩余内容允许的最大值」
     * （lv_obj.c 的 readjust_scroll_after_layout → lv_obj_pos.c →
     * lv_obj_scroll.c 的 readjust_scroll）。于是列表里留下的是**夹断后的值**
     * （例如 30 条 / 槽位 10 / 视口 435 时，原始 1477 被夹成 837，差约 10 行），
     * 而上面那条判据只看「是不是 0」，夹断后的值不是 0 → 直接返回 →
     * **位置根本没复原**。这条路径最常被触发的是「点开一条待办 → 从详情页返回」。
     */
    if (lv_obj_get_scroll_y(s_list) == s_scroll_y) {
        /* 位置正是我们要的：什么都不要做。 */
        return;
    }

    /* LV_ANIM_OFF：不带动画，直接落位。
     * lv_obj_scroll_to_y 内部会先 lv_obj_update_layout()，这时行控件已经重建，
     * 所以夹断是按**最终内容**算的 —— 位置能精确复原。 */
    lv_obj_scroll_to_y(s_list, s_scroll_y, LV_ANIM_OFF);

    /* 滚动事件会把渲染窗口挪到对应的位置；万一事件没来（例如目标位置没变），
     * 这里再补一次重画，保证窗口和滚动位置对得上。 */
    render_window(false);
}

static esp_err_t todo_on_show(void)
{
    /* 行控件在 on_hide 里被销毁了，这里按缓存里的数据重建。
     * 数据取自 todo_manager 的内存缓存：todo_list_filtered() 只在 RAM 里拷
     * 一份快照，不碰 SD 卡 / flash（落盘是 todo_manager 自己异步做的）。 */
    refresh();

    /* 回到离开本页时的滚动位置（跟改动前的观感一致）。 */
    scroll_restore();
    return ESP_OK;
}

static esp_err_t todo_on_hide(void)
{
    /* 先记住滚动位置：行控件马上要被销毁，列表的内容会塌回顶部。 */
    if (s_list != NULL) {
        /*
         * 惯性滚动（手指甩出去之后的那段动画）要停掉。
         * 否则记住的是**动画中途**的值，而动画还在跑 —— 回来时复原到的位置
         * 会跟动画打架（两边都以为自己说了算），位置就飘了。
         */
        lv_anim_delete(s_list, NULL);
        s_scroll_y = lv_obj_get_scroll_y(s_list);
    }

    /* 行控件是这一页最大的一批对象（每行 8 个控件），页面被切走时它们留在堆里
     * 正是内部堆碎片化的来源之一（ROADMAP v1.8.1 §2）：ui_core 只把别的页面
     * 藏起来（lv_obj_add_flag(HIDDEN)），对象并不销毁。这里把它们销毁掉，
     * 页面外壳（标题、筛选条、统计行、两个占位对象）留着 —— 它们很少，
     * 销毁重建只会让页面闪一下。
     *
     * on_hide 由 ui_core 的 page_switch_locked() 调用，跑在 LVGL 任务里，
     * 所以这里动 LVGL 对象是安全的（不在 httpd 回调里）。 */
    const size_t released = s_row_count;
    rows_clear();

    /* 离开本页就把快照放了：滚动时要用它重画窗口里的行，别的页面用不上。 */
    if (s_items != NULL) {
        todo_free_list(s_items);
        s_items = NULL;
        s_item_count = 0;
    }

    SYS_LOGD(TAG, "todo page hidden: %u row widgets released", (unsigned)released);
    return ESP_OK;
}

static esp_err_t todo_on_event(int32_t event_id, void *data)
{
    (void)event_id;
    (void)data;
    refresh();
    return ESP_OK;
}

const ui_page_t page_todo = {
    .id = UI_PAGE_TODO,
    .name = "todo",
    .create = todo_create,
    .on_show = todo_on_show,
    .on_hide = todo_on_hide,
    .on_event = todo_on_event,
};

/* ------------------------------------------------------------------------ */
/* Detail page (FR2 / UI_PAGE_TODO_DETAIL) — display only                    */
/* ------------------------------------------------------------------------ */

static uint32_t s_detail_id;
static lv_obj_t *s_detail_title;
static lv_obj_t *s_detail_meta;
static lv_obj_t *s_detail_desc;

void page_todo_detail_set_id(uint32_t id)
{
    s_detail_id = id;
}

/**
 * 写标签前先比较。
 *
 * lv_label_set_text() 即使内容一样也会 invalidate 整个对象，详情页每次事件都
 * 无脑重写三个标签等于每次都让整页重排（铁律二）。
 */
static void detail_set_text(lv_obj_t *label, const char *text)
{
    if (label == NULL) {
        return;
    }
    if (strcmp(lv_label_get_text(label), text) == 0) {
        return;
    }
    lv_label_set_text(label, text);
}

/** 优先级：0 普通 / 1 重要 / 2 紧急。 */
static const char *priority_name(uint8_t priority)
{
    if (priority >= 2) {
        return "紧急";
    }
    if (priority == 1) {
        return "重要";
    }
    return "普通";
}

/**
 * 到期时间：本地时区的「2026-10-21 09:00」。
 *
 * 没校过时（sys_time_synced() 为假）只显示「未校时」，不编一个日期出来
 * （FR4 / 需求 F3）。
 */
static void due_text_format(int64_t due_ts, char *out, size_t out_len)
{
    if (due_ts <= 0) {
        strlcpy(out, "无到期时间", out_len);
        return;
    }
    if (!sys_time_synced()) {
        strlcpy(out, "未校时", out_len);
        return;
    }

    const time_t due = (time_t)due_ts;
    struct tm tm_due;
    if (localtime_r(&due, &tm_due) == NULL) {
        strlcpy(out, "未校时", out_len);
        return;
    }
    /* 夹到日历可能的范围，再格式化到定长临时缓冲后拷贝：
     * out_len 是运行期参数，直接格式化编译器无法证明放得下，
     * 会触发 -Werror=format-truncation。 */
    int year = tm_due.tm_year + 1900;
    int mon = tm_due.tm_mon + 1;
    int day = tm_due.tm_mday;
    int hour = tm_due.tm_hour;
    int min = tm_due.tm_min;
    if (year < 1970) {
        year = 1970;
    } else if (year > 9999) {
        year = 9999;
    }
    if (mon < 1) {
        mon = 1;
    } else if (mon > 12) {
        mon = 12;
    }
    if (day < 1) {
        day = 1;
    } else if (day > 31) {
        day = 31;
    }
    if (hour < 0) {
        hour = 0;
    } else if (hour > 23) {
        hour = 23;
    }
    if (min < 0) {
        min = 0;
    } else if (min > 59) {
        min = 59;
    }
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%04d-%02d-%02d %02d:%02d", year, mon, day, hour, min);
    strlcpy(out, tmp, out_len);
}

static void detail_refresh(void)
{
    if (s_detail_title == NULL) {
        return;
    }

    todo_item_t item;
    if (todo_get(s_detail_id, &item) != ESP_OK) {
        detail_set_text(s_detail_title, "该待办已不存在");
        detail_set_text(s_detail_meta, "");
        detail_set_text(s_detail_desc, "");
        return;
    }

    detail_set_text(s_detail_title, item.title);
    detail_set_text(s_detail_desc, item.desc[0] != '\0' ? item.desc : "（无备注）");

    static const char *const sync_names[] = { "未同步", "待同步", "已同步", "同步失败" };
    const char *sync = (item.sync_state < 4) ? sync_names[item.sync_state] : "?";

    char due[32];
    due_text_format(item.due_ts, due, sizeof(due));

    char meta[160];
    snprintf(meta, sizeof(meta), "#%lu · %s · 优先级%s · %s · 到期 %s",
             (unsigned long)item.id, item.done ? "已完成" : "未完成",
             priority_name(item.priority), sync, due);
    detail_set_text(s_detail_meta, meta);
}

static esp_err_t todo_detail_create(lv_obj_t *parent)
{
    (void)page_header_create(parent, "待办详情");

    lv_obj_t *body = NULL;
    if (ui_card_create(parent, NULL, &body) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_detail_title = lv_label_create(body);
    lv_obj_set_width(s_detail_title, LV_PCT(100));
    lv_label_set_long_mode(s_detail_title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_detail_title, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(s_detail_title, UI_COLOR_TEXT, 0);

    s_detail_meta = lv_label_create(body);
    lv_obj_set_width(s_detail_meta, LV_PCT(100));
    lv_label_set_long_mode(s_detail_meta, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_detail_meta, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_detail_meta, UI_COLOR_TEXT_MUTED, 0);

    s_detail_desc = lv_label_create(body);
    lv_obj_set_width(s_detail_desc, LV_PCT(100));
    lv_label_set_long_mode(s_detail_desc, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_detail_desc, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_detail_desc, UI_COLOR_TEXT, 0);

    /* The "完成/取消 / 优先级 / 删除" bar used to live here; the detail page is
     * a viewer now, so its slot carries the read-only hint instead. */
    readonly_hint_create(parent);

    detail_refresh();
    (void)ui_event_bind(TODO_EVENT, TODO_EVENT_LIST_CHANGED, UI_PAGE_TODO_DETAIL, 0);
    return ESP_OK;
}

static esp_err_t todo_detail_on_show(void)
{
    detail_refresh();
    return ESP_OK;
}

static esp_err_t todo_detail_on_event(int32_t event_id, void *data)
{
    (void)event_id;
    (void)data;
    detail_refresh();
    return ESP_OK;
}

const ui_page_t page_todo_detail = {
    .id = UI_PAGE_TODO_DETAIL,
    .name = "todo_detail",
    .create = todo_detail_create,
    .on_show = todo_detail_on_show,
    .on_event = todo_detail_on_event,
};
