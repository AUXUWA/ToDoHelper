/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_wifi.c
 * @brief FR1: scan, password entry and connection status in one page.
 *
 * Requirement 10.1 keeps all three WiFi screens inside UI_PAGE_WIFI, switched
 * as sub-views, to avoid growing ui_page_id_t for every dialog.
 *
 * Scanning is always started with wifi_manager_start_scan(): the blocking
 * variant would stall the LVGL task for up to 15 s and freeze the UI.
 *
 * The AP list is windowed: only the rows inside the viewport exist as
 * widgets, the rows outside are height padding (two spacer objects). Building
 * all 24 rows meant 72 widgets on this page, which fragmented the internal
 * heap so badly that its largest free block fell to 1.9 KB - too small for
 * the WiFi PHY to re-enable (AGENT.md 9.0.3). The windowing keeps the list
 * height, the scrollbar and the row positions exactly as they were.
 */

#include "pages_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "wifi_manager.h"
#include "esp_attr.h"

static const char *TAG = "page_wifi";

typedef enum {
    WIFI_VIEW_SCAN = 0,
    WIFI_VIEW_PASSWORD,
    WIFI_VIEW_STATUS,
} wifi_view_t;

static lv_obj_t *s_view[3];
static lv_obj_t *s_scan_status;
static lv_obj_t *s_scan_list;
static lv_obj_t *s_pass_ssid;
/** The "back to DHCP" button (only visible with a fixed address). */
static lv_obj_t *s_dhcp_button;
static lv_obj_t *s_dhcp_label;
static lv_obj_t *s_pass_area;
static lv_obj_t *s_status_detail;
static char s_pending_ssid[WIFI_MANAGER_SSID_MAX + 1];
static wifi_view_t s_view_current = WIFI_VIEW_SCAN;

/* ------------------------------------------------------------------------ */
/* Scan list windowing                                                        */
/* ------------------------------------------------------------------------ */

/**
 * 行距：行高（UI_LIST_ROW_HEIGHT = 56）+ 列表的行间距（s_scan_list 的 pad_row 6 px）。
 *
 * 视口里放得下几行、滚动时窗口往哪挪、占位对象要撑多高，全靠这个数。行高是
 * 固定的（lv_obj_set_height(row, 56)），所以这个数是准的（第一次布局之后
 * 还会用实测行高再校准一次，见 s_row_h）。
 */
#define WIFI_ROW_GAP    6
#define WIFI_ROW_PITCH  (UI_LIST_ROW_HEIGHT + WIFI_ROW_GAP)

/** 视口上下各多渲染一行：滚动时这一行已经在屏外，不会看到空白。 */
#define WIFI_WINDOW_BUFFER 1

/** 同时存活的行数上限（可视行数 + 上下缓冲）。 */
#define WIFI_WINDOW_MAX_ROWS 10

/** 视口高度还量不出来时按这个值算（页面刚建好、还没排版时）。 */
#define WIFI_FALLBACK_VIEW_H 300

/**
 * 扫描结果的静态副本（PSRAM）。
 *
 * wifi_manager_get_scan_result() 每次都把结果拷出来，点击那一刻的栈上副本
 * 早没了，所以必须留一份在手上：行槽号 → AP 下标 → 这份副本里的 SSID。
 */
static EXT_RAM_BSS_ATTR wifi_ap_info_t s_aps[WIFI_MANAGER_MAX_APS];

/** 当前扫描结果的条数（≤ WIFI_MANAGER_MAX_APS）。 */
static size_t s_ap_count;

/** 行槽号 → 当前 AP 下标（窗口挪动时更新）。 */
static uint8_t s_slot_ap[WIFI_WINDOW_MAX_ROWS];

/** 一行的三个控件（复用：窗口挪动时只换内容，不重建）。 */
typedef struct {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *meta;
} wifi_row_t;

static wifi_row_t s_rows[WIFI_WINDOW_MAX_ROWS];

/** 已经建好控件的行数。 */
static size_t s_row_count;

/** 当前渲染的窗口起点（AP 下标）。 */
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

/** 「还没有扫描结果」的提示标签（列表里一个 AP 都没有时才在）。 */
static lv_obj_t *s_scan_empty;

/** 现在的行距（实测行高 + 行间距）。 */
static int32_t row_pitch(void)
{
    return (s_row_h > 0) ? (s_row_h + WIFI_ROW_GAP) : WIFI_ROW_PITCH;
}

/* ------------------------------------------------------------------------ */
/* View switching                                                            */
/* ------------------------------------------------------------------------ */

static void show_view(wifi_view_t view)
{
    for (int i = 0; i < 3; i++) {
        if (s_view[i] == NULL) {
            continue;
        }
        if (i == (int)view) {
            lv_obj_remove_flag(s_view[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_view[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_view_current = view;
}

/* ------------------------------------------------------------------------ */
/* Scan view                                                                 */
/* ------------------------------------------------------------------------ */

/**
 * 点了一行：事件的用户数据是行槽号，不是 AP 下标。
 *
 * 窗口挪动后同一批行控件会换成另一批 AP，所以下标不能写死在事件里 ——
 * 先用槽号查「槽号 → AP 下标」表，再从静态副本里取 SSID。
 */
static void ap_clicked_cb(lv_event_t *e)
{
    const size_t slot = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (slot >= s_row_count || slot >= WIFI_WINDOW_MAX_ROWS) {
        return;
    }
    const size_t ap_idx = s_slot_ap[slot];
    if (ap_idx >= s_ap_count) {
        return;
    }

    const char *ssid = s_aps[ap_idx].ssid[0] ? s_aps[ap_idx].ssid : "(隐藏网络)";

    strlcpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
    if (s_pass_ssid != NULL) {
        lv_label_set_text_fmt(s_pass_ssid, "连接到 %s", s_pending_ssid);
    }
    if (s_pass_area != NULL) {
        lv_textarea_set_text(s_pass_area, "");
    }
    show_view(WIFI_VIEW_PASSWORD);
}

static void scan_clicked_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = wifi_manager_start_scan();
    if (err == ESP_OK) {
        if (s_scan_status != NULL) {
            lv_label_set_text(s_scan_status, "正在扫描…");
        }
    } else {
        ui_toast("扫描失败：%s", esp_err_to_name(err));
    }
}

/**
 * 建一个只占高度的空对象（窗口之外的行用它占位）。
 *
 * 没有背景、没有边框，也不接收触摸：它唯一的作用是让列表的总高度跟
 * 「全部行都在」时一模一样，滚动条长度因此跟以前一样。
 */
static lv_obj_t *gap_create(void)
{
    lv_obj_t *gap = lv_obj_create(s_scan_list);
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
 * 占位对象补出来。算式是照着 flex 的排版来的：行与行之间有一个 WIFI_ROW_GAP
 * 的间距，所以占位对象的高度要减掉这个间距（而且占位对象如果高度归零就整个
 * 藏起来 —— 藏起来之后 flex 不再给它留间距，总高度才对得上）。
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
        const int32_t h = (int32_t)(start * (size_t)row_pitch()) - WIFI_ROW_GAP;
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

    /* rest 是窗口下面还差几行。调用方保证 start + slots <= total，这里再兜
     * 一次底：size_t 相减一旦下溢就是天文数字，占位对象会撑出一个巨大的高度。 */
    const size_t rest = (start + slots < total) ? (total - start - slots) : 0;
    if (rest > 0) {
        const int32_t h = (int32_t)(rest * (size_t)row_pitch()) - WIFI_ROW_GAP;
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
 * 把窗口里的每一行画成对应的那一条 AP。
 *
 * 行控件是复用的：窗口挪动时同一批控件换成另一批 AP，所以这里只改内容，
 * 不重建控件（铁律二：先比较再写）。
 */
static void window_paint(size_t start, size_t slots)
{
    for (size_t i = 0; i < slots; i++) {
        const size_t idx = start + i;
        if (idx >= s_ap_count || i >= WIFI_WINDOW_MAX_ROWS) {
            break;
        }
        wifi_row_t *r = &s_rows[i];
        if (r->name == NULL || r->meta == NULL) {
            break;
        }

        /* 槽号 → AP 下标：点击回调靠它找到要连的 SSID。 */
        s_slot_ap[i] = (uint8_t)idx;

        const char *ssid = s_aps[idx].ssid[0] ? s_aps[idx].ssid : "(隐藏网络)";
        if (strcmp(lv_label_get_text(r->name), ssid) != 0) {
            lv_label_set_text(r->name, ssid);
        }

        char meta[64];
        snprintf(meta, sizeof(meta), "%d dBm · ch%u · %s", (int)s_aps[idx].rssi,
                 s_aps[idx].channel,
                 s_aps[idx].authmode == WIFI_AUTH_OPEN ? "开放" : "加密");
        if (strcmp(lv_label_get_text(r->meta), meta) != 0) {
            lv_label_set_text(r->meta, meta);
        }
    }
}

/**
 * 建一行的控件（只建一次，之后只改内容）。
 *
 * 事件的用户数据是行槽号：窗口挪动后 AP 下标会变，槽号不会。
 */
static bool row_create(size_t slot)
{
    wifi_row_t *r = &s_rows[slot];

    lv_obj_t *row = lv_button_create(s_scan_list);
    if (row == NULL) {
        return false;
    }
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, UI_LIST_ROW_HEIGHT);
    lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_radius(row, UI_CARD_RADIUS, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name = lv_label_create(row);
    if (name == NULL) {
        lv_obj_delete(row);
        return false;
    }
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(name, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(name, UI_COLOR_TEXT, 0);

    lv_obj_t *meta = lv_label_create(row);
    if (meta == NULL) {
        lv_obj_delete(row);
        return false;
    }
    lv_obj_set_style_text_font(meta, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(meta, UI_COLOR_TEXT_MUTED, 0);

    lv_obj_add_event_cb(row, ap_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)slot);

    r->row = row;
    r->name = name;
    r->meta = meta;
    return true;
}

/** 把建好的行控件全删掉（下次按新的行数重建）。 */
static void rows_clear(void)
{
    for (size_t i = 0; i < s_row_count; i++) {
        if (s_rows[i].row != NULL) {
            lv_obj_delete(s_rows[i].row);
            s_rows[i].row = NULL;
        }
        s_rows[i].name = NULL;
        s_rows[i].meta = NULL;
    }
    s_row_count = 0;
    s_win_start = 0;
}

/** 列表里一个 AP 都没有时的提示（跟改动前一字不差）。 */
static void scan_empty_show(void)
{
    if (s_scan_list == NULL || s_scan_empty != NULL) {
        return;
    }
    s_scan_empty = lv_label_create(s_scan_list);
    if (s_scan_empty == NULL) {
        return;
    }
    lv_label_set_text(s_scan_empty, "还没有扫描结果，点“重新扫描”。");
    lv_obj_set_style_text_font(s_scan_empty, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_scan_empty, UI_COLOR_TEXT_MUTED, 0);
}

static void scan_empty_hide(void)
{
    if (s_scan_empty != NULL) {
        lv_obj_delete(s_scan_empty);
        s_scan_empty = NULL;
    }
}

/**
 * 画列表：只画视口里的那几行。
 *
 * @param measure true = 重新量视口高度（数据变了 / 页面刚显示时），false =
 *                沿用上次量到的高度（滚动的时候用，免得每滚一个像素就重排一次）。
 */
static void render_scan_window(bool measure)
{
    if (s_scan_list == NULL) {
        return;
    }

    const size_t total = s_ap_count;

    if (total == 0) {
        if (s_row_count != 0) {
            rows_clear();
        }
        window_gaps(0, 0, 0);
        scan_empty_show();
        return;
    }

    /* 有 AP 就没有「还没有扫描结果」这句：它会占一行，把下面的行挤下去。 */
    scan_empty_hide();

    if (measure) {
        /* 视口多高？量一次就够，滚动时不再量。 */
        lv_obj_update_layout(s_scan_list);
        const int32_t h = lv_obj_get_height(s_scan_list);
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
        s_view_h = WIFI_FALLBACK_VIEW_H;
    }

    /* 视口里放得下几行 —— 再加上下各一行缓冲。 */
    const size_t visible = (size_t)((s_view_h + row_pitch() - 1) / row_pitch());
    size_t win = visible + 2 * WIFI_WINDOW_BUFFER;
    if (win > WIFI_WINDOW_MAX_ROWS) {
        win = WIFI_WINDOW_MAX_ROWS;
    }
    if (win < visible) {
        /* 视口比上限还高时宁可多建几行，也不能让视口底部留空白。 */
        win = visible;
    }
    const size_t slots = (win < total) ? win : total;

    /* 窗口起点：视口里第一条行往上留一行缓冲。 */
    size_t first = 0;
    const int32_t scroll_y = lv_obj_get_scroll_y(s_scan_list);
    if (scroll_y > 0) {
        first = (size_t)(scroll_y / row_pitch());
    }
    size_t start = (first > WIFI_WINDOW_BUFFER) ? first - WIFI_WINDOW_BUFFER : 0;
    if (start + slots > total) {
        start = total - slots;
    }

    if (measure) {
        /* 行的个数变了才重建控件；只是换了内容就原地改。 */
        if (slots != s_row_count) {
            /* 先把占位对象撑到最终的样子：建行期间列表总高不变，滚动位置
             * 就不会被 LVGL 夹断（夹断了用户会被弹回列表顶上）。 */
            window_gaps(start, slots, total);
            rows_clear();
            for (size_t i = 0; i < slots; i++) {
                if (!row_create(i)) {
                    SYS_LOGE(TAG, "scan list out of memory");
                    break;
                }
                s_row_count++;
            }
            /* 占位对象要排在所有行后面（flex 按子对象顺序排版）。 */
            if (s_gap_bottom != NULL) {
                lv_obj_move_to_index(s_gap_bottom, -1);
            }
            SYS_LOGD(TAG, "scan list: %u aps, %u rows in window", (unsigned)total,
                     (unsigned)s_row_count);
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
    window_paint(start, s_row_count);
}

/**
 * 列表滚动了：窗口跟着挪。
 *
 * 只换内容、不重建控件（窗口外面那几行用占位对象顶着，滚动条长度不变）。
 */
static void scan_list_scroll_cb(lv_event_t *e)
{
    (void)e;
    render_scan_window(false);
}

/**
 * 重新读一次扫描结果并画列表。
 *
 * 结果直接拷进 PSRAM 里的静态副本 —— 点击那一刻已经没有栈上副本可用了。
 */
static void render_scan_list(void)
{
    if (s_scan_list == NULL) {
        return;
    }

    size_t count = 0;
    if (wifi_manager_get_scan_result(s_aps, WIFI_MANAGER_MAX_APS, &count) != ESP_OK ||
        count == 0) {
        s_ap_count = 0;
        render_scan_window(true);
        return;
    }

    s_ap_count = (count > WIFI_MANAGER_MAX_APS) ? WIFI_MANAGER_MAX_APS : count;
    render_scan_window(true);
}

static void open_status_cb(lv_event_t *e)
{
    (void)e;
    show_view(WIFI_VIEW_STATUS);
}

static esp_err_t wifi_create_scan_view(lv_obj_t *parent)
{
    s_view[WIFI_VIEW_SCAN] = lv_obj_create(parent);
    if (s_view[WIFI_VIEW_SCAN] == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_view[WIFI_VIEW_SCAN]);
    lv_obj_set_size(s_view[WIFI_VIEW_SCAN], LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_view[WIFI_VIEW_SCAN], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_view[WIFI_VIEW_SCAN], 8, 0);
    lv_obj_remove_flag(s_view[WIFI_VIEW_SCAN], LV_OBJ_FLAG_SCROLLABLE);

    s_scan_status = lv_label_create(s_view[WIFI_VIEW_SCAN]);
    lv_obj_set_style_text_font(s_scan_status, ui_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(s_scan_status, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_scan_status, "选择要连接的网络");

    s_scan_list = lv_obj_create(s_view[WIFI_VIEW_SCAN]);
    lv_obj_remove_style_all(s_scan_list);
    lv_obj_set_width(s_scan_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_scan_list, 1);
    lv_obj_set_flex_flow(s_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_scan_list, 6, 0);
    lv_obj_set_style_pad_right(s_scan_list, 6, 0);
    lv_obj_add_flag(s_scan_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scan_list, LV_DIR_VER);

    /* 窗口上下的占位对象：把窗口之外的行的高度补出来，滚动条长度不变。
     * 上面的那个必须是列表的第一个子对象，下面的那个排在所有行后面
     * （行是后来才建的，建完要把占位对象挪到末尾）。 */
    s_gap_top = gap_create();
    s_gap_bottom = gap_create();
    if (s_gap_top == NULL || s_gap_bottom == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 滚动：窗口跟着挪，只换内容。 */
    lv_obj_add_event_cb(s_scan_list, scan_list_scroll_cb, LV_EVENT_SCROLL, NULL);

    lv_obj_t *bar = lv_obj_create(s_view[WIFI_VIEW_SCAN]);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *scan = lv_button_create(bar);
    lv_obj_set_height(scan, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(scan, 1);
    lv_obj_t *scan_label = lv_label_create(scan);
    lv_label_set_text(scan_label, "重新扫描");
    lv_obj_center(scan_label);
    lv_obj_add_event_cb(scan, scan_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *status = lv_button_create(bar);
    lv_obj_set_height(status, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(status, 1);
    lv_obj_set_style_bg_color(status, UI_COLOR_SURFACE_ALT, 0);
    lv_obj_t *status_label = lv_label_create(status);
    lv_label_set_text(status_label, "连接状态");
    lv_obj_center(status_label);
    lv_obj_add_event_cb(status, open_status_cb, LV_EVENT_CLICKED, NULL);

    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Password view                                                             */
/* ------------------------------------------------------------------------ */

static void connect_clicked_cb(lv_event_t *e)
{
    (void)e;
    const char *password = (s_pass_area != NULL) ? lv_textarea_get_text(s_pass_area) : "";

    esp_err_t err = wifi_manager_connect(s_pending_ssid, password);
    if (err == ESP_OK) {
        ui_toast("正在连接 %s", s_pending_ssid);
        show_view(WIFI_VIEW_STATUS);
    } else {
        ui_toast("连接启动失败：%s", esp_err_to_name(err));
    }
}

static void pass_back_cb(lv_event_t *e)
{
    (void)e;
    show_view(WIFI_VIEW_SCAN);
}

static esp_err_t wifi_create_password_view(lv_obj_t *parent)
{
    s_view[WIFI_VIEW_PASSWORD] = lv_obj_create(parent);
    if (s_view[WIFI_VIEW_PASSWORD] == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_view[WIFI_VIEW_PASSWORD]);
    lv_obj_set_size(s_view[WIFI_VIEW_PASSWORD], LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_view[WIFI_VIEW_PASSWORD], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_view[WIFI_VIEW_PASSWORD], 10, 0);
    lv_obj_remove_flag(s_view[WIFI_VIEW_PASSWORD], LV_OBJ_FLAG_SCROLLABLE);

    s_pass_ssid = lv_label_create(s_view[WIFI_VIEW_PASSWORD]);
    lv_obj_set_style_text_font(s_pass_ssid, ui_font(UI_FONT_TITLE), 0);
    lv_obj_set_style_text_color(s_pass_ssid, UI_COLOR_TEXT, 0);
    lv_label_set_text(s_pass_ssid, "连接到");

    s_pass_area = lv_textarea_create(s_view[WIFI_VIEW_PASSWORD]);
    lv_textarea_set_one_line(s_pass_area, true);
    lv_textarea_set_password_mode(s_pass_area, true);
    lv_textarea_set_max_length(s_pass_area, WIFI_MANAGER_PASS_MAX);
    lv_textarea_set_placeholder_text(s_pass_area, "密码（开放网络留空）");
    lv_obj_set_width(s_pass_area, LV_PCT(100));
    lv_obj_set_style_text_font(s_pass_area, ui_font(UI_FONT_BODY), 0);

    lv_obj_t *bar = lv_obj_create(s_view[WIFI_VIEW_PASSWORD]);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ok = lv_button_create(bar);
    lv_obj_set_height(ok, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(ok, 1);
    lv_obj_t *ok_label = lv_label_create(ok);
    lv_label_set_text(ok_label, "连接");
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok, connect_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_button_create(bar);
    lv_obj_set_height(back, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(back, 1);
    lv_obj_set_style_bg_color(back, UI_COLOR_SURFACE_ALT, 0);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, "返回列表");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back, pass_back_cb, LV_EVENT_CLICKED, NULL);

    /* Shared styling, so every keyboard in the app looks the same. */
    (void)ui_keyboard_create(s_view[WIFI_VIEW_PASSWORD], s_pass_area);

    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Status view                                                               */
/* ------------------------------------------------------------------------ */

static void disconnect_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (wifi_manager_disconnect() == ESP_OK) {
        ui_toast("已断开");
    }
}

/**
 * Drop the fixed address and go back to DHCP.
 *
 * The escape hatch: a fixed address that the network does not route would
 * make the web console unreachable, and the console is where the address was
 * set in the first place.
 */
static void dhcp_clicked_cb(lv_event_t *e)
{
    (void)e;
    const esp_err_t err = wifi_manager_set_static_ip("");
    if (err != ESP_OK) {
        ui_toast("改回 DHCP 失败：%s", esp_err_to_name(err));
        return;
    }
    ui_toast("已改回 DHCP，设备正在重新连接");
}

static void status_back_cb(lv_event_t *e)
{
    (void)e;
    show_view(WIFI_VIEW_SCAN);
}

static esp_err_t wifi_create_status_view(lv_obj_t *parent)
{
    s_view[WIFI_VIEW_STATUS] = lv_obj_create(parent);
    if (s_view[WIFI_VIEW_STATUS] == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_view[WIFI_VIEW_STATUS]);
    lv_obj_set_size(s_view[WIFI_VIEW_STATUS], LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_view[WIFI_VIEW_STATUS], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_view[WIFI_VIEW_STATUS], 10, 0);
    lv_obj_remove_flag(s_view[WIFI_VIEW_STATUS], LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *body = NULL;
    if (ui_card_create(s_view[WIFI_VIEW_STATUS], "连接状态", &body) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_status_detail = lv_label_create(body);
    lv_obj_set_width(s_status_detail, LV_PCT(100));
    lv_label_set_long_mode(s_status_detail, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_status_detail, ui_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(s_status_detail, UI_COLOR_TEXT, 0);
    lv_label_set_text(s_status_detail, "…");

    lv_obj_t *bar = lv_obj_create(s_view[WIFI_VIEW_STATUS]);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *disc = lv_button_create(bar);
    lv_obj_set_height(disc, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(disc, 1);
    lv_obj_set_style_bg_color(disc, UI_COLOR_DANGER, 0);
    lv_obj_t *disc_label = lv_label_create(disc);
    lv_label_set_text(disc_label, "断开");
    lv_obj_center(disc_label);
    lv_obj_add_event_cb(disc, disconnect_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_button_create(bar);
    lv_obj_set_height(back, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(back, 1);
    lv_obj_set_style_bg_color(back, UI_COLOR_SURFACE_ALT, 0);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, "切换网络");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back, status_back_cb, LV_EVENT_CLICKED, NULL);

    /*
     * The escape hatch for a fixed address.
     *
     * A fixed address makes the web console's URL stop changing, but if the
     * address is one the network does not route, the console can no longer be
     * reached - and then the only way out is this button, on the device
     * itself. So it lives here, not only in the web console.
     */
    s_dhcp_button = lv_button_create(bar);
    lv_obj_set_height(s_dhcp_button, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(s_dhcp_button, 1);
    lv_obj_set_style_bg_color(s_dhcp_button, UI_COLOR_SURFACE_ALT, 0);
    s_dhcp_label = lv_label_create(s_dhcp_button);
    lv_label_set_text(s_dhcp_label, "改回 DHCP");
    lv_obj_center(s_dhcp_label);
    lv_obj_add_event_cb(s_dhcp_button, dhcp_clicked_cb, LV_EVENT_CLICKED, NULL);
    /* Only shown when a fixed address is actually in use. */
    lv_obj_add_flag(s_dhcp_button, LV_OBJ_FLAG_HIDDEN);

    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Refresh                                                                   */
/* ------------------------------------------------------------------------ */

/**
 * Last rendered status text.
 *
 * refresh_status() runs on the 3 s tick *and* on every WiFi event, but the
 * six-line block it writes only changes when the link state, SSID, IP or host
 * name changes - RSSI is the one field that moves on its own. Writing identical
 * text still invalidates the wrapping label and re-measures six lines, so this
 * comparison keeps the 3 s tick nearly free (smoothness audit P1-10).
 */
static char s_status_text[256];

static void refresh_status(void)
{
    if (s_status_detail == NULL) {
        return;
    }

    /* The escape hatch is only useful (and only shown) with a fixed address. */
    if (s_dhcp_button != NULL) {
        const bool fixed = wifi_manager_get_static_ip()[0] != 0;
        if (fixed && lv_obj_has_flag(s_dhcp_button, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(s_dhcp_button, LV_OBJ_FLAG_HIDDEN);
        } else if (!fixed && !lv_obj_has_flag(s_dhcp_button, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_dhcp_button, LV_OBJ_FLAG_HIDDEN);
        }
    }

    wifi_mgr_state_t state = WIFI_MGR_DISCONNECTED;
    (void)wifi_manager_get_state(&state);

    char ip[16] = { 0 };
    char mac[18] = { 0 };
    char ssid[WIFI_MANAGER_SSID_MAX + 1] = { 0 };
    (void)wifi_manager_get_ip(ip, sizeof(ip));
    (void)wifi_manager_get_mac(mac, sizeof(mac));
    (void)wifi_manager_get_ssid(ssid, sizeof(ssid));

    char text[sizeof(s_status_text)];
    snprintf(text, sizeof(text),
             "状态：%s\n网络：%s\nIP：%s%s\n信号：%d dBm\nMAC：%s\n主机名：%s",
             wifi_manager_state_name(state),
             ssid[0] ? ssid : "（未连接）",
             ip,
             wifi_manager_get_static_ip()[0] ? "（固定）" : "",
             (int)wifi_manager_get_rssi(),
             mac,
             WIFI_MANAGER_HOSTNAME);

    if (strcmp(text, s_status_text) == 0) {
        return;
    }
    strlcpy(s_status_text, text, sizeof(s_status_text));
    lv_label_set_text(s_status_detail, s_status_text);
}

static void refresh_scan_hint(void)
{
    if (s_scan_status == NULL) {
        return;
    }

    wifi_mgr_state_t state = WIFI_MGR_DISCONNECTED;
    (void)wifi_manager_get_state(&state);

    if (state == WIFI_MGR_CONNECTED) {
        char ssid[WIFI_MANAGER_SSID_MAX + 1] = { 0 };
        (void)wifi_manager_get_ssid(ssid, sizeof(ssid));
        lv_label_set_text_fmt(s_scan_status, "已连接 %s，可重新扫描切换", ssid);
    } else if (state == WIFI_MGR_CONNECTING) {
        lv_label_set_text(s_scan_status, "连接中…");
    } else if (state == WIFI_MGR_FAILED) {
        lv_label_set_text(s_scan_status, "上次连接失败，请重试");
    } else {
        lv_label_set_text(s_scan_status, "选择要连接的网络");
    }
}

/* ------------------------------------------------------------------------ */
/* Page                                                                      */
/* ------------------------------------------------------------------------ */

static esp_err_t wifi_create(lv_obj_t *parent)
{
    (void)page_header_create(parent, "WiFi");

    esp_err_t err = wifi_create_scan_view(parent);
    if (err != ESP_OK) {
        return err;
    }
    err = wifi_create_password_view(parent);
    if (err != ESP_OK) {
        return err;
    }
    err = wifi_create_status_view(parent);
    if (err != ESP_OK) {
        return err;
    }

    show_view(WIFI_VIEW_SCAN);
    render_scan_list();
    refresh_scan_hint();
    refresh_status();

    /* Every WiFi event refreshes both the list hint and the status card. */
    (void)ui_event_bind_all(WIFI_MGR_EVENT, UI_PAGE_WIFI, 0);
    SYS_LOGD(TAG, "wifi page built");
    return ESP_OK;
}

static esp_err_t wifi_on_show(void)
{
    refresh_scan_hint();
    refresh_status();
    /* The page is on screen now, so the viewport height can finally be
     * measured: re-measure it and let the window follow the real viewport. */
    render_scan_window(true);
    /* A fresh scan on entry keeps the list current without a manual tap. */
    (void)wifi_manager_start_scan();
    return ESP_OK;
}

static esp_err_t wifi_on_event(int32_t event_id, void *data)
{
    (void)data;
    if (event_id == WIFI_MGR_EVENT_SCAN_DONE) {
        render_scan_list();
    }
    refresh_scan_hint();
    refresh_status();
    return ESP_OK;
}

static esp_err_t wifi_on_tick(void)
{
    refresh_status();
    return ESP_OK;
}

const ui_page_t page_wifi = {
    .id = UI_PAGE_WIFI,
    .name = "wifi",
    .create = wifi_create,
    .on_show = wifi_on_show,
    .on_event = wifi_on_event,
    .on_tick = wifi_on_tick,
    .tick_period_ms = 3000,
};
