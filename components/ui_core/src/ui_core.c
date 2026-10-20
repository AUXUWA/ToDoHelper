/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_lv_adapter.h"

#include "ui_fonts.h"
#include "ui_internal.h"
#include "ui_theme.h"
#include "esp_attr.h"

static const char *TAG = "ui_core";

#define UI_NAV_STACK_MAX      8
#define UI_MAX_BINDINGS       40
#define UI_MAX_EVENT_BASES    8
#define UI_EVENT_PAYLOAD_MAX  256
/** Repaint triggers remembered per page while the page is not on display. */
#define UI_DEFERRED_MAX       4

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    const ui_page_t *desc;
    lv_obj_t *container;
    lv_timer_t *tick;
    /**
     * Repaint triggers that arrived while the page was not on display.
     *
     * Only the event id is kept: everything that can be deferred is a "please
     * repaint" event *without* a payload (see ui_event_drain_cb), so there is
     * nothing else worth carrying - and keeping the payload would mean holding
     * heap memory for a page that may not be shown for hours.
     */
    int32_t deferred_ids[UI_DEFERRED_MAX];
    uint8_t deferred_count;
} ui_slot_t;

typedef struct {
    esp_event_base_t base;
    int32_t event_id;       /* ESP_EVENT_ANY_ID acts as a wildcard */
    ui_page_id_t page;      /* UI_PAGE_MAX when this is a plain handler */
    ui_event_fn_t handler;
    size_t data_size;
    bool used;
} ui_binding_t;

/** Heap message used to move an event from the shared loop into the LVGL task. */
typedef struct ui_async_event {
    int32_t event_id;
    ui_page_id_t page;
    ui_event_fn_t handler;
    size_t data_size;
    /** Next pending event; the hand-off queue is a singly linked list. */
    struct ui_async_event *next;
    uint8_t payload[];
} ui_async_event_t;

static ui_slot_t s_slots[UI_PAGE_MAX];
static lv_obj_t *s_statusbar;
static lv_obj_t *s_content;
static ui_page_id_t s_current = UI_PAGE_MAX;
static ui_page_id_t s_nav[UI_NAV_STACK_MAX];
static size_t s_nav_depth;
static bool s_initialized;

/**
 * The 1 Hz clock timer.
 *
 * It writes the status bar's clock once a second, so it may only run while the
 * status bar is on display. The standby screen hides the whole status bar
 * (enter_idle()), and a timer that keeps running then writes into a hidden
 * label every second - which is exactly the "timer without an owner" that
 * AGENT.md 9.0.1.1 rule 3 forbids. The handle is kept here so the idle
 * transition can pause it and the wake-up can resume it.
 */
static lv_timer_t *s_clock_timer;

/** Page that was visible when the idle timer fired (FR6 11.4). */
static ui_page_id_t s_page_before_idle = UI_PAGE_HOME;

static EXT_RAM_BSS_ATTR ui_binding_t s_bindings[UI_MAX_BINDINGS];
static esp_event_base_t s_event_bases[UI_MAX_EVENT_BASES];
static size_t s_event_base_count;

/* ------------------------------------------------------------------------- */
/* Locking / marshalling                                                     */
/* ------------------------------------------------------------------------- */

bool ui_lock(int32_t timeout_ms)
{
    return th_bsp_lock(timeout_ms);
}

void ui_unlock(void)
{
    th_bsp_unlock();
}

/*
 * Cross-task hand-off queue.
 *
 * ui_post_event() runs on the *publisher's* task (the shared event loop, a
 * service worker), while the handlers must run in the LVGL task. It used to do
 * that by taking the adapter lock and calling lv_async_call() directly, which
 * coupled every publisher to the render task: a long LVGL callback (an NTP wait,
 * an SD write) would block the event loop on ui_lock(-1) = portMAX_DELAY, and a
 * busy event loop would in turn stall rendering.
 *
 * Now the publisher only appends to a singly linked list under a short spinlock
 * and wakes the drainer. The list is LIFO so a flood of notifications cannot
 * starve the newest one, and it is bounded so a stalled UI cannot exhaust the
 * heap (dropping a notification is acceptable, blocking the publisher is not).
 */
#define UI_EVENT_QUEUE_MAX 64
/** Depth of the pending queue; guarded by s_event_lock. */
static ui_async_event_t *s_event_head;
static size_t s_event_depth;
static portMUX_TYPE s_event_lock = portMUX_INITIALIZER_UNLOCKED;
/** LVGL timer that drains the queue inside the LVGL task. */
static lv_timer_t *s_event_timer;
/** Set by the publisher to ask the drainer for an immediate pass. */
static volatile bool s_event_pending;   /* written under s_event_lock */

/**
 * Remember a repaint request for a page that is not on display.
 *
 * The newest request wins: a repaint always renders the *current* state, so
 * an older one is covered by it. The list is bounded, because a page that is
 * never shown again must not accumulate requests.
 */
static void page_defer_repaint(ui_slot_t *slot, int32_t event_id)
{
    if (slot->deferred_count < UI_DEFERRED_MAX) {
        slot->deferred_ids[slot->deferred_count++] = event_id;
        return;
    }

    for (uint8_t i = 1; i < UI_DEFERRED_MAX; i++) {
        slot->deferred_ids[i - 1] = slot->deferred_ids[i];
    }
    slot->deferred_ids[UI_DEFERRED_MAX - 1] = event_id;
}

/**
 * Replay the repaint requests that arrived while the page was hidden.
 *
 * Called right after the page became visible. Most pages already refresh
 * themselves in on_show(), but not all of them do the *whole* job there (the
 * WiFi page, for instance, refreshes the status card but leaves the scan list
 * to its event handler), so the requests are replayed instead of being
 * dropped.
 */
static void page_replay_deferred(ui_slot_t *slot)
{
    if (slot->deferred_count == 0) {
        return;
    }

    const ui_page_t *desc = slot->desc;
    if (desc != NULL && desc->on_event != NULL) {
        for (uint8_t i = 0; i < slot->deferred_count; i++) {
            /* Deferred requests never carry a payload, so NULL is what the
             * handler would have received anyway. */
            (void)desc->on_event(slot->deferred_ids[i], NULL);
        }
    }
    slot->deferred_count = 0;
}

/** Runs in the LVGL task. Dispatches every queued event. */
static void ui_event_drain_cb(lv_timer_t *timer)
{
    (void)timer;

    taskENTER_CRITICAL(&s_event_lock);
    const bool pending = s_event_pending;
    s_event_pending = false;
    taskEXIT_CRITICAL(&s_event_lock);
    if (!pending) {
        return;
    }

    /*
     * Take the whole pending list in one critical section and dispatch it here,
     * rather than handing each node to lv_async_call().
     *
     * Two reasons, both learned the hard way on hardware:
     *
     *  1. lv_async_call() has its own small fixed pool. A notification burst
     *     (a DNS failure flood at boot, a WiFi reconnect) overflowed it and made
     *     this loop churn malloc/free while LVGL's internal lock was in play.
     *  2. Dispatching directly is what this timer is for: it already runs inside
     *     the LVGL task with the adapter lock held, which is exactly the context
     *     the handlers need. Going through lv_async_call() added a second
     *     deferral and a second lock for nothing.
     */
    for (;;) {
        taskENTER_CRITICAL(&s_event_lock);
        ui_async_event_t *ev = s_event_head;
        if (ev != NULL) {
            s_event_head = ev->next;
            s_event_depth--;
        }
        taskEXIT_CRITICAL(&s_event_lock);

        if (ev == NULL) {
            break;
        }

        /* Someone could have published while we were dispatching; keep the flag
         * consistent so the next tick still runs. */
        if (s_event_head != NULL) {
            s_event_pending = true;
        }

        ev->next = NULL;
        if (ev->handler != NULL) {
            ev->handler(ev->event_id, ev->payload);
        } else if (ev->page < UI_PAGE_MAX) {
            ui_slot_t *slot = &s_slots[ev->page];
            if (slot->desc != NULL && slot->desc->on_event != NULL) {
                if (ev->page == s_current || ev->data_size > 0) {
                    slot->desc->on_event(ev->event_id, ev->payload);
                } else {
                    /*
                     * The page is not on display, so repainting it would be
                     * work nobody can see: the todo list, for example, copies
                     * the whole list and rebuilds every row on each change,
                     * and the web console changes one row at a time.
                     *
                     * A repaint request (no payload) is therefore only
                     * *remembered* and replayed when the page comes back -
                     * see page_replay_deferred().
                     *
                     * An event that carries data is a different animal: it
                     * delivers content the page has to keep (the AI reply
                     * chunks, the AI state), and dropping it would lose data
                     * rather than save work. Those are handed over even to a
                     * hidden page.
                     */
                    page_defer_repaint(slot, ev->event_id);
                }
            }
        }
        free(ev);
    }
}

/** Runs on the publisher's task: no LVGL call, no blocking. */
static void ui_post_event(int32_t event_id, ui_page_id_t page, ui_event_fn_t handler,
                          size_t data_size, const void *data)
{
    if (data_size > UI_EVENT_PAYLOAD_MAX) {
        SYS_LOGW(TAG, "event %ld payload %u truncated", (long)event_id, (unsigned)data_size);
        data_size = UI_EVENT_PAYLOAD_MAX;
    }

    ui_async_event_t *ev = malloc(sizeof(*ev) + data_size + 1);
    if (ev == NULL) {
        return;
    }
    ev->event_id = event_id;
    ev->page = page;
    ev->handler = handler;
    ev->data_size = data_size;
    if (data_size > 0 && data != NULL) {
        memcpy(ev->payload, data, data_size);
    }
    /*
     * 载荷后面补一个结尾的 '\0'。
     *
     * 载荷会被截到 UI_EVENT_PAYLOAD_MAX（256），而**绑定声明的大小**是事件类型
     * 自己定的（AI 对话的流式文本声明的是 AI_MESSAGE_MAX = 1024）。
     * 处理器拿到的只是指针，它若按 C 字符串处理（strlen），
     * 遇到「正好被截断、没有结尾 0」的载荷就会**越过载荷读堆** ——
     * 界面上是乱码，最坏是越界读。
     * → 每个载荷都以 0 结尾，strlen 就一定停在载荷内。
     */
    ev->payload[data_size] = '\0';

    taskENTER_CRITICAL(&s_event_lock);
    const bool full = (s_event_depth >= UI_EVENT_QUEUE_MAX);
    if (!full) {
        ev->next = s_event_head;
        s_event_head = ev;
        s_event_depth++;
        s_event_pending = true;
    }
    taskEXIT_CRITICAL(&s_event_lock);

    if (full) {
        /* The UI is not draining (still booting, or wedged). Shedding the event
         * keeps a burst from growing without bound; the log is emitted outside
         * the critical section because it takes the console lock. */
        SYS_LOGW(TAG, "event queue full, dropping event %ld", (long)event_id);
        free(ev);
    }
}

/** Runs in the shared event loop task: must not touch LVGL directly. */
static void ui_loop_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    for (size_t i = 0; i < UI_MAX_BINDINGS; i++) {
        const ui_binding_t *b = &s_bindings[i];
        if (!b->used || b->base != base) {
            continue;
        }
        if (b->event_id != ESP_EVENT_ANY_ID && b->event_id != event_id) {
            continue;
        }
        ui_post_event(event_id, b->page, b->handler, b->data_size, data);
    }
}

static esp_err_t ensure_base_registered(esp_event_base_t base)
{
    for (size_t i = 0; i < s_event_base_count; i++) {
        if (s_event_bases[i] == base) {
            return ESP_OK;
        }
    }
    if (s_event_base_count >= UI_MAX_EVENT_BASES) {
        SYS_LOGE(TAG, "too many event bases (%u)", (unsigned)s_event_base_count);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sys_event_subscribe(base, ESP_EVENT_ANY_ID, ui_loop_handler, NULL, NULL);
    if (err != ESP_OK) {
        SYS_LOGE(TAG, "subscribe base %p failed: %s", (void *)base, esp_err_to_name(err));
        return err;
    }
    s_event_bases[s_event_base_count++] = base;
    return ESP_OK;
}

static esp_err_t binding_add(esp_event_base_t base, int32_t event_id,
                             ui_page_id_t page, ui_event_fn_t handler, size_t data_size)
{
    if (base == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (page >= UI_PAGE_MAX && handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < UI_MAX_BINDINGS; i++) {
        ui_binding_t *b = &s_bindings[i];
        if (b->used &&
            b->base == base && b->event_id == event_id &&
            b->page == page && b->handler == handler) {
            /* Re-binding the same triple refreshes the payload size. */
            b->data_size = data_size;
            return ESP_OK;
        }
    }

    for (size_t i = 0; i < UI_MAX_BINDINGS; i++) {
        ui_binding_t *b = &s_bindings[i];
        if (!b->used) {
            b->base = base;
            b->event_id = event_id;
            b->page = page;
            b->handler = handler;
            b->data_size = data_size;
            b->used = true;
            return ensure_base_registered(base);
        }
    }

    return ESP_ERR_NO_MEM;
}

esp_err_t ui_event_bind(esp_event_base_t base, int32_t event_id, ui_page_id_t page,
                        size_t data_size)
{
    if (page >= UI_PAGE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return binding_add(base, event_id, page, NULL, data_size);
}

esp_err_t ui_event_bind_all(esp_event_base_t base, ui_page_id_t page, size_t data_size)
{
    return ui_event_bind(base, ESP_EVENT_ANY_ID, page, data_size);
}

esp_err_t ui_event_subscribe(esp_event_base_t base, int32_t event_id, size_t data_size,
                             ui_event_fn_t handler)
{
    return binding_add(base, event_id, UI_PAGE_MAX, handler, data_size);
}

/** One-shot closure used by ui_run_async(). */
typedef struct {
    void (*fn)(void *);
    void *arg;
} ui_async_thunk_t;

static esp_err_t ui_run_async_handler(int32_t event_id, void *data)
{
    (void)event_id;
    ui_async_thunk_t *t = (ui_async_thunk_t *)data;
    if (t != NULL && t->fn != NULL) {
        t->fn(t->arg);
    }
    return ESP_OK;
}

esp_err_t ui_run_async(void (*fn)(void *arg), void *arg)
{
    if (fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const ui_async_thunk_t thunk = { .fn = fn, .arg = arg };
    ui_post_event(0, UI_PAGE_MAX, ui_run_async_handler, sizeof(thunk), &thunk);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Page registry                                                             */
/* ------------------------------------------------------------------------- */

esp_err_t ui_page_register(const ui_page_t *pages, size_t count)
{
    if (pages == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < count; i++) {
        const ui_page_t *p = &pages[i];
        if (p->id >= UI_PAGE_MAX || p->create == NULL) {
            SYS_LOGE(TAG, "invalid page entry %u", (unsigned)i);
            return ESP_ERR_INVALID_ARG;
        }
        if (s_slots[p->id].desc != NULL) {
            SYS_LOGE(TAG, "page id %d already registered ('%s')", p->id, p->name);
            return ESP_ERR_INVALID_STATE;
        }
        s_slots[p->id].desc = p;
    }

    SYS_LOGI(TAG, "registered %u pages", (unsigned)count);
    return ESP_OK;
}

const ui_page_t *ui_page_get(ui_page_id_t id)
{
    if (id >= UI_PAGE_MAX) {
        return NULL;
    }
    return s_slots[id].desc;
}

lv_obj_t *ui_page_parent(ui_page_id_t id)
{
    if (id >= UI_PAGE_MAX) {
        return NULL;
    }
    return s_slots[id].container;
}

static void page_tick_cb(lv_timer_t *timer)
{
    const ui_page_id_t id = (ui_page_id_t)(intptr_t)lv_timer_get_user_data(timer);
    if (id >= UI_PAGE_MAX) {
        return;
    }
    const ui_page_t *desc = s_slots[id].desc;
    if (desc != NULL && desc->on_tick != NULL) {
        desc->on_tick();
    }
}

static lv_obj_t *page_container_create(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    if (c == NULL) {
        return NULL;
    }

    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(c, UI_PAGE_PAD, 0);
    lv_obj_set_style_pad_row(c, UI_PAGE_GAP, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    return c;
}

/**
 * Page containers and the switch.
 *
 * A switch is a plain swap: the incoming container is shown and every other one
 * is hidden in the same frame.
 *
 * There used to be a slide + cross-fade here (220/240 ms of translate_x /
 * translate_y plus a full-screen `opa` animation on *both* containers). It was
 * removed for two reasons:
 *
 *   1. **Correctness.** Both containers are the same size and opaque, so any
 *      frame in which they only partially overlap renders the previous page
 *      through the gap; and if the exit animation was cancelled (switching away
 *      again before it finished, or re-entering that page), its completion
 *      callback - the only thing that hid the page - was deleted with it, leaving
 *      the old page visible for good. That is the "ghost of the previous page"
 *      the user reported.
 *   2. **Cost.** A full-screen alpha animation pushes every pixel of an 800x480
 *      tree through the software blend path for a dozen frames, which is the most
 *      expensive thing LVGL can be asked to do on this board.
 *
 * The idle face still gets its exit animation: it is driven by its own lv_timer
 * and by on_hide(), not by the container staying visible (see
 * idle_face_standby_exit()).
 */

/** Reset any leftover transform/opacity on @p page and hide it. */
static void page_reset_and_hide(lv_obj_t *page)
{
    if (page == NULL) {
        return;
    }
    lv_obj_set_style_translate_x(page, 0, 0);
    lv_obj_set_style_translate_y(page, 0, 0);
    lv_obj_set_style_opa(page, LV_OPA_COVER, 0);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

/**
 * Make @p keep the only visible page container.
 *
 * The invariant this enforces - exactly one page container is visible, and it is
 * the current one - is the one the animated version kept failing to hold.
 */
static void pages_show_only(ui_page_id_t keep)
{
    for (size_t i = 0; i < UI_PAGE_MAX; i++) {
        if ((ui_page_id_t)i == keep) {
            continue;
        }
        page_reset_and_hide(s_slots[i].container);
    }
}

static esp_err_t page_switch_locked(ui_page_id_t id)
{
    if (id >= UI_PAGE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_content == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ui_slot_t *slot = &s_slots[id];
    if (slot->desc == NULL) {
        SYS_LOGW(TAG, "page %d not registered", (int)id);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_current == id) {
        return ESP_OK;
    }

    if (s_current < UI_PAGE_MAX) {
        ui_slot_t *cur = &s_slots[s_current];
        if (cur->desc != NULL && cur->desc->on_hide != NULL) {
            cur->desc->on_hide();
        }
        if (cur->tick != NULL) {
            lv_timer_pause(cur->tick);
        }
    }

    if (slot->container == NULL) {
        slot->container = page_container_create(s_content);
        if (slot->container == NULL) {
            return ESP_ERR_NO_MEM;
        }

        esp_err_t err = (slot->desc->create != NULL) ? slot->desc->create(slot->container) : ESP_OK;
        if (err != ESP_OK) {
            /* Keep whatever was built instead of showing a blank screen. */
            SYS_LOGE(TAG, "page '%s' create failed: %s", slot->desc->name, esp_err_to_name(err));
        }

        if (slot->desc->on_tick != NULL && slot->desc->tick_period_ms > 0) {
            slot->tick = lv_timer_create(page_tick_cb, slot->desc->tick_period_ms,
                                         (void *)(intptr_t)id);
            if (slot->tick != NULL) {
                lv_timer_pause(slot->tick);
            }
        }
    }

    /* Show exactly this page, on top, with no leftover transform or opacity. */
    lv_obj_move_foreground(slot->container);
    lv_obj_set_style_translate_x(slot->container, 0, 0);
    lv_obj_set_style_translate_y(slot->container, 0, 0);
    lv_obj_set_style_opa(slot->container, LV_OPA_COVER, 0);
    lv_obj_remove_flag(slot->container, LV_OBJ_FLAG_HIDDEN);
    pages_show_only(id);

    s_current = id;

    if (slot->tick != NULL) {
        lv_timer_resume(slot->tick);
    }
    if (slot->desc->on_show != NULL) {
        slot->desc->on_show();
    }

    /*
     * Repaint requests that arrived while this page was off screen are
     * replayed now, so the page comes back showing the current state instead
     * of the state it had when it was left.
     */
    page_replay_deferred(slot);

    SYS_LOGD(TAG, "page -> %s", slot->desc->name);
    return ESP_OK;
}

esp_err_t ui_page_switch(ui_page_id_t id)
{
    if (!ui_lock(-1)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = page_switch_locked(id);
    ui_unlock();
    return err;
}

ui_page_id_t ui_page_current(void)
{
    return s_current;
}

esp_err_t ui_nav_push(ui_page_id_t id)
{
    if (id >= UI_PAGE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_nav_depth < UI_NAV_STACK_MAX) {
        s_nav[s_nav_depth++] = s_current;
    }
    return ui_page_switch(id);
}

esp_err_t ui_nav_back(void)
{
    if (s_nav_depth == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const ui_page_id_t target = s_nav[--s_nav_depth];
    if (target >= UI_PAGE_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    return ui_page_switch(target);
}

bool ui_nav_can_back(void)
{
    return s_nav_depth > 0;
}

/* ------------------------------------------------------------------------- */
/* System event reactions                                                    */
/* ------------------------------------------------------------------------- */

static esp_err_t on_status_changed(int32_t event_id, void *data)
{
    (void)event_id;
    (void)data;
    return ui_statusbar_refresh();
}

static esp_err_t on_button(int32_t event_id, void *data)
{
    (void)event_id;
    const sys_button_event_t *btn = (const sys_button_event_t *)data;
    if (btn == NULL) {
        return ESP_OK;
    }

    /* Wake first: a button press while idle must not also navigate. */
    if (th_bsp_is_idle()) {
        th_bsp_report_activity();
        return ESP_OK;
    }

    switch (btn->id) {
    case SYS_BUTTON_MODE:
        if (btn->action == SYS_BUTTON_PRESS) {
            (void)ui_page_switch(ui_page_current() == UI_PAGE_HOME ? UI_PAGE_TODO : UI_PAGE_HOME);
        }
        break;
    case SYS_BUTTON_SET:
        if (btn->action == SYS_BUTTON_PRESS && ui_nav_can_back()) {
            (void)ui_nav_back();
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/**
 * Start or stop the 1 Hz clock timer.
 *
 * Runs in the LVGL task (the idle/wake events are marshalled into it), so
 * touching the timer handle is safe.
 */
static void clock_timer_set_running(bool running)
{
    if (s_clock_timer == NULL) {
        return;
    }
    if (running) {
        lv_timer_resume(s_clock_timer);
    } else {
        lv_timer_pause(s_clock_timer);
    }
}

static esp_err_t enter_idle(void)
{
    if (s_current == UI_PAGE_IDLE_FACE) {
        /* Already on the standby screen: the status bar is hidden (and the
         * clock timer paused) since the first entry. */
        clock_timer_set_running(false);
        return ESP_OK;
    }

    s_page_before_idle = (s_current < UI_PAGE_MAX) ? s_current : UI_PAGE_HOME;

    /* The idle page belongs to the idle_face component, which ui_core must not
     * depend on: if it is absent we simply dim the current page. */
    esp_err_t err = ui_page_switch(UI_PAGE_IDLE_FACE);
    if (err != ESP_OK) {
        SYS_LOGD(TAG, "idle page unavailable (%s), dimming only", esp_err_to_name(err));
    } else if (s_statusbar != NULL) {
        /* Requirement 11.1: the idle screen is full black, so the status bar
         * (which lives outside the page container) has to go too. */
        lv_obj_add_flag(s_statusbar, LV_OBJ_FLAG_HIDDEN);
        /* The status bar is gone, so the clock has nobody to write to: stop
         * the 1 Hz timer instead of filling a hidden label once a second. */
        clock_timer_set_running(false);
    }

    /*
     * Standby brightness.
     *
     * Defaults to 100 (no dimming): the idle face is pure white on pure black, and
     * lowering the backlight turns those eyes grey, which the owner explicitly did
     * not want. A device that has never touched the slider therefore shows pure
     * white eyes.
     *
     * The settings slider still stores a value and it is honoured when present, so
     * dimming stays available as an opt-in.
     */
    int32_t dim = sys_config_get_i32_or(SYS_KEY_IDLE_BRIGHT, 100);    if (dim < 0) {
        dim = 0;
    }
    if (dim > 100) {
        dim = 100;
    }
    return th_bsp_set_brightness((uint8_t)dim);
}

static esp_err_t leave_idle(void)
{
    int32_t bright = sys_config_get_i32_or(SYS_KEY_BRIGHTNESS, 100);
    if (bright < 0) {
        bright = 0;
    }
    if (bright > 100) {
        bright = 100;
    }
    (void)th_bsp_set_brightness((uint8_t)bright);

    if (s_statusbar != NULL) {
        lv_obj_remove_flag(s_statusbar, LV_OBJ_FLAG_HIDDEN);
    }

    /* The status bar is back: the clock timer may run again. The refresh below
     * makes the clock correct on the very first visible frame instead of up to
     * a second later. */
    clock_timer_set_running(true);
    (void)ui_statusbar_refresh();

    if (s_current == UI_PAGE_IDLE_FACE) {
        (void)ui_page_switch(s_page_before_idle);
    }
    return ESP_OK;
}

static esp_err_t on_idle_timeout(int32_t event_id, void *data)
{
    (void)event_id;
    (void)data;
    return enter_idle();
}

static esp_err_t on_wakeup(int32_t event_id, void *data)
{
    (void)event_id;
    (void)data;
    return leave_idle();
}

static esp_err_t on_brightness(int32_t event_id, void *data)
{
    (void)event_id;
    (void)data;
    return ESP_OK;
}

static void clock_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Runs in the LVGL task, so touching the status bar is safe. */
    (void)ui_statusbar_refresh();

#if CONFIG_TODOHELPER_FPS_PROBE
    /* Temporary diagnostic: report the measured refresh rate once a second so
     * smoothness work can be judged from data instead of impression. Enable with
     * CONFIG_TODOHELPER_FPS_PROBE; it is off in normal builds. */
    if (th_bsp_display() != NULL) {
        uint32_t fps = 0;
        if (esp_lv_adapter_get_fps(th_bsp_display(), &fps) == ESP_OK) {
            SYS_LOGI(TAG, "fps %u", (unsigned)fps);
        }
    }
#endif
}

/* ------------------------------------------------------------------------- */
/* Init                                                                      */
/* ------------------------------------------------------------------------- */

esp_err_t ui_core_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (th_bsp_display() == NULL) {
        SYS_LOGE(TAG, "display not ready; call th_bsp_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    if (!ui_lock(-1)) {
        return ESP_ERR_TIMEOUT;
    }

    ui_fonts_init();
    ui_theme_apply();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_statusbar = ui_statusbar_create(scr);

    s_content = lv_obj_create(scr);
    if (s_content != NULL) {
        lv_obj_remove_style_all(s_content);
        lv_obj_set_width(s_content, LV_PCT(100));
        lv_obj_set_flex_grow(s_content, 1);
        lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(s_content, 0, 0);
        lv_obj_set_style_border_width(s_content, 0, 0);
        lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    }

    s_clock_timer = lv_timer_create(clock_tick_cb, 1000, NULL);
    if (s_clock_timer == NULL) {
        SYS_LOGW(TAG, "clock timer unavailable, the status bar clock will not tick");
    }

    /* Drains the cross-task event queue. 20 ms keeps a notification within about
     * one frame of being published without waking the LVGL task when idle: the
     * callback returns immediately unless s_event_pending is set. */
    s_event_timer = lv_timer_create(ui_event_drain_cb, 20, NULL);
    if (s_event_timer == NULL) {
        SYS_LOGW(TAG, "event drain timer unavailable, UI notifications will stall");
    }

#if CONFIG_TODOHELPER_FPS_PROBE
    /* Diagnostic build: start the adapter's FPS counter (clock_tick_cb logs it). */
    (void)esp_lv_adapter_fps_stats_enable(th_bsp_display(), true);
    SYS_LOGI(TAG, "FPS probe enabled");
#endif

    ui_unlock();

    if (s_content == NULL) {
        SYS_LOGE(TAG, "content area allocation failed");
        return ESP_ERR_NO_MEM;
    }

    /* System reactions. ui_core owns the idle policy (FR6 11.4) and the status
     * bar, so it is the only place that subscribes to these. */
    ui_event_subscribe(SYS_EVENT, SYS_EVENT_STATUS_CHANGED, 0, on_status_changed);
    ui_event_subscribe(SYS_EVENT, SYS_EVENT_BUTTON, sizeof(sys_button_event_t), on_button);
    ui_event_subscribe(SYS_EVENT, SYS_EVENT_IDLE_TIMEOUT, 0, on_idle_timeout);
    ui_event_subscribe(SYS_EVENT, SYS_EVENT_WAKEUP, 0, on_wakeup);
    ui_event_subscribe(SYS_EVENT, SYS_EVENT_BRIGHTNESS, sizeof(uint8_t), on_brightness);

    s_initialized = true;
    SYS_LOGI(TAG, "ready (%dx%d)", (int)ui_screen_width(), (int)ui_screen_height());
    return ESP_OK;
}

lv_obj_t *ui_content_area(void)
{
    return s_content;
}

/* ------------------------------------------------------------------------- */
/* Screen metrics                                                            */
/* ------------------------------------------------------------------------- */

int32_t ui_screen_width(void)
{
    lv_display_t *disp = th_bsp_display();
    if (disp == NULL) {
        return 0;
    }
    return lv_display_get_horizontal_resolution(disp);
}

int32_t ui_screen_height(void)
{
    lv_display_t *disp = th_bsp_display();
    if (disp == NULL) {
        return 0;
    }
    return lv_display_get_vertical_resolution(disp);
}

int32_t ui_statusbar_height(void)
{
    return UI_STATUSBAR_HEIGHT;
}
