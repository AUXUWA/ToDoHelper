/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "idle_face.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "th_bsp.h"
#include "ui_core.h"
#include "ui_theme.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "idle_face";

ESP_EVENT_DEFINE_BASE(IDLE_FACE_EVENT);

#ifndef CONFIG_TODOHELPER_IDLE_EYE_WIDTH
#define CONFIG_TODOHELPER_IDLE_EYE_WIDTH 150
#endif
#ifndef CONFIG_TODOHELPER_IDLE_EYE_HEIGHT
#define CONFIG_TODOHELPER_IDLE_EYE_HEIGHT 150
#endif
#ifndef CONFIG_TODOHELPER_IDLE_EYE_GAP
#define CONFIG_TODOHELPER_IDLE_EYE_GAP 96
#endif
#ifndef CONFIG_TODOHELPER_IDLE_LINE_WIDTH
#define CONFIG_TODOHELPER_IDLE_LINE_WIDTH 8
#endif

/** Animator period: ~30 fps, and 15 fps is acceptable for an idle screen. */
#define IDLE_FACE_TICK_MS 33
/** Blink duration and the interval it is repeated on. */
#define IDLE_FACE_BLINK_MS 130
#define IDLE_FACE_BLINK_MIN_MS 3000
#define IDLE_FACE_BLINK_MAX_MS 8000
/** Look-around, only while the face is idle (NORMAL). */
#define IDLE_FACE_LOOK_MIN_MS 4000
#define IDLE_FACE_LOOK_MAX_MS 12000
#define IDLE_FACE_LOOK_HOLD_MS 1400
/** Default duration for idle_face_express(). */
#define IDLE_FACE_EXPRESS_DEFAULT_MS 1600

/* ------------------------------------------------------------------------ */
/* Effect table (requirement 11.2)                                           */
/* ------------------------------------------------------------------------ */

/**
 * Interpolated eye parameters.
 *
 * `open` scales the eye height, `pupil` the pupil visibility, `dx`/`dy` shift
 * the pupil inside the eye. `arc` morphs the outline into an upward arc (happy)
 * and `cross` switches to the X shape (error).
 */
typedef struct {
    float open;
    float dx;
    float dy;
    float pupil;
    float arc;
    float cross;
    float eye_dy;
    float width_scale;
    float speed;      /**< interpolation gain per tick, 0..1 */
} face_params_t;

#define SPEED_FAST   0.55f
#define SPEED_NORMAL 0.22f
#define SPEED_SLOW   0.10f

static const face_params_t s_effects[IDLE_FACE_MAX] = {
    /* NORMAL     */ { 1.00f,  0.00f,  0.00f, 0.85f, 0.0f, 0.0f,   0.0f, 1.00f, SPEED_NORMAL },
    /* BLINK      */ { 0.06f,  0.00f,  0.00f, 0.00f, 0.0f, 0.0f,   0.0f, 1.40f, SPEED_FAST },
    /* LOOK_LEFT  */ { 1.00f, -0.95f,  0.00f, 1.00f, 0.0f, 0.0f,   0.0f, 1.00f, SPEED_NORMAL },
    /* LOOK_RIGHT */ { 1.00f,  0.95f,  0.00f, 1.00f, 0.0f, 0.0f,   0.0f, 1.00f, SPEED_NORMAL },
    /* HAPPY      */ { 0.55f,  0.00f,  0.00f, 0.00f, 1.0f, 0.0f,  -8.0f, 1.30f, SPEED_NORMAL },
    /* SLEEPY     */ { 0.38f,  0.00f,  0.55f, 0.30f, 0.0f, 0.0f,  22.0f, 1.10f, SPEED_SLOW },
    /* ERROR      */ { 1.00f,  0.00f,  0.00f, 0.00f, 0.0f, 1.0f,   0.0f, 1.10f, SPEED_NORMAL },
    /* SYNCING    */ { 0.92f,  0.00f,  0.00f, 1.00f, 0.0f, 0.0f,   0.0f, 1.00f, SPEED_NORMAL },
    /* THINKING   */ { 0.92f,  0.00f, -0.75f, 1.00f, 0.0f, 0.0f, -10.0f, 1.00f, SPEED_NORMAL },
    /* LISTENING  */ { 1.28f,  0.00f,  0.00f, 1.00f, 0.0f, 0.0f,   0.0f, 1.20f, SPEED_NORMAL },
    /* SPEAKING   */ { 0.78f,  0.00f,  0.00f, 0.00f, 0.8f, 0.0f,  -4.0f, 1.25f, SPEED_FAST },
};

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
    lv_obj_t *container;
    lv_obj_t *eye[2];
    lv_obj_t *pupil[2];
    lv_obj_t *cross[2][2];   /**< two strokes per eye for the X shape */
    lv_timer_t *timer;
} face_objects_t;

static face_objects_t s_face;
static bool s_attached;

static idle_face_state_t s_state = IDLE_FACE_NORMAL;
static face_params_t s_current;
static float s_phase;              /**< 0..1, drives breathing and circling */

/* transient override (idle_face_express) */
static bool s_override_active;
static idle_face_state_t s_override_state;
static idle_face_state_t s_state_before_override;
static uint32_t s_override_until_ms;

/* auto behaviour timers */
static uint32_t s_next_blink_ms;
static uint32_t s_blink_until_ms;
static uint32_t s_next_look_ms;
static uint32_t s_look_until_ms;
static idle_face_state_t s_look_state = IDLE_FACE_LOOK_LEFT;

static const uint32_t s_eye_base_w = CONFIG_TODOHELPER_IDLE_EYE_WIDTH;
static const uint32_t s_eye_base_h = CONFIG_TODOHELPER_IDLE_EYE_HEIGHT;
static const uint32_t s_eye_gap = CONFIG_TODOHELPER_IDLE_EYE_GAP;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static uint32_t now_ms32(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t rand_between(uint32_t lo, uint32_t hi)
{
    if (hi <= lo) {
        return lo;
    }
    return lo + (esp_random() % (hi - lo));
}

static float lerp(float from, float to, float gain)
{
    return from + (to - from) * gain;
}

static const face_params_t *params_for(idle_face_state_t state)
{
    if (state >= IDLE_FACE_MAX) {
        state = IDLE_FACE_NORMAL;
    }
    return &s_effects[state];
}

static void apply_params(void)
{
    if (!s_attached) {
        return;
    }

    /* Breathing: a small, slow amplitude change on top of the state's value. */
    const float breath = 1.0f + 0.035f * sinf(s_phase * 2.0f * (float)M_PI);
    float open = s_current.open * breath;
    if (open < 0.04f) {
        open = 0.04f;
    }
    if (open > 1.6f) {
        open = 1.6f;
    }

    const int32_t eye_h = (int32_t)((float)s_eye_base_h * open);
    const int32_t eye_w = (int32_t)((float)s_eye_base_w * (1.0f + 0.05f * s_current.arc));
    const int32_t border = (int32_t)((float)CONFIG_TODOHELPER_IDLE_LINE_WIDTH *
                                     s_current.width_scale);
    const bool cross_mode = s_current.cross > 0.5f;
    const bool arc_mode = !cross_mode && s_current.arc > 0.5f;

    for (int i = 0; i < 2; i++) {
        lv_obj_t *eye = s_face.eye[i];
        lv_obj_t *pupil = s_face.pupil[i];
        if (eye == NULL) {
            continue;
        }

        lv_obj_set_size(eye, eye_w, arc_mode ? eye_h / 2 : eye_h);
        lv_obj_align(eye, LV_ALIGN_CENTER,
                     (i == 0) ? -(int32_t)(s_eye_gap / 2 + s_eye_base_w / 2)
                              : (int32_t)(s_eye_gap / 2 + s_eye_base_w / 2),
                     (int32_t)s_current.eye_dy);

        lv_obj_set_style_border_width(eye, border, 0);
        lv_obj_set_style_border_opa(eye, cross_mode ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(eye, arc_mode ? LV_BORDER_SIDE_TOP : LV_BORDER_SIDE_FULL, 0);
        /* A radius equal to half the height turns the outline into an ellipse;
         * for the happy arc the bottom half is simply not drawn. */
        lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);

        /* Pupil: kept inside the eye and shifted by dx/dy. */
        if (pupil != NULL) {
            const int32_t pupil_size = (int32_t)(eye_h * 0.34f);
            const int32_t travel_x = (eye_w - pupil_size) / 2 - border;
            const int32_t travel_y = (eye_h - pupil_size) / 2 - border;

            lv_obj_set_size(pupil, pupil_size, pupil_size);
            lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, 0);

            float opacity = s_current.pupil;
            if (arc_mode || cross_mode) {
                opacity = 0.0f;
            }
            lv_obj_set_style_bg_opa(pupil, (lv_opa_t)(opacity * LV_OPA_COVER), 0);

            /* SYNCING walks the pupil around a small circle. */
            float dx = s_current.dx;
            float dy = s_current.dy;
            if (s_state == IDLE_FACE_SYNCING) {
                dx = 0.75f * cosf(s_phase * 2.0f * (float)M_PI);
                dy = 0.75f * sinf(s_phase * 2.0f * (float)M_PI);
            }

            lv_obj_align(pupil, LV_ALIGN_CENTER,
                         (int32_t)(dx * (float)(travel_x > 0 ? travel_x : 0)),
                         (int32_t)(dy * (float)(travel_y > 0 ? travel_y : 0)));
        }

        /* X strokes use the full eye box; show them only in ERROR. */
        for (int k = 0; k < 2; k++) {
            lv_obj_t *line = s_face.cross[i][k];
            if (line == NULL) {
                continue;
            }
            lv_obj_set_style_line_opa(line, cross_mode ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_line_width(line, border, 0);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Auto behaviour                                                            */
/* ------------------------------------------------------------------------ */

static void schedule_next_blink(uint32_t now)
{
    s_next_blink_ms = now + rand_between(IDLE_FACE_BLINK_MIN_MS, IDLE_FACE_BLINK_MAX_MS);
}

static void schedule_next_look(uint32_t now)
{
    s_next_look_ms = now + rand_between(IDLE_FACE_LOOK_MIN_MS, IDLE_FACE_LOOK_MAX_MS);
}

/**
 * Blinking and looking around only make sense for the plain idle expression:
 * an ERROR or THINKING face must not blink itself back to neutral.
 */
static bool state_allows_idle_antics(idle_face_state_t state)
{
    return state == IDLE_FACE_NORMAL || state == IDLE_FACE_LOOK_LEFT ||
           state == IDLE_FACE_LOOK_RIGHT || state == IDLE_FACE_BLINK;
}

static void tick_behaviour(uint32_t now)
{
    if (s_override_active && now >= s_override_until_ms) {
        s_override_active = false;
        const idle_face_state_t restore = s_state_before_override;
        s_state = restore;
        (void)sys_event_post_to(IDLE_FACE_EVENT, IDLE_FACE_EVENT_STATE_CHANGED,
                                &s_state, sizeof(s_state));
    }

    const idle_face_state_t base = s_override_active ? s_override_state : s_state;

    if (!state_allows_idle_antics(base)) {
        s_blink_until_ms = 0;
        return;
    }

    /* Finish an in-flight blink first: it overrides the look target. */
    if (s_blink_until_ms != 0 && now >= s_blink_until_ms) {
        s_blink_until_ms = 0;
        schedule_next_blink(now);
    }

    if (s_look_until_ms != 0 && now >= s_look_until_ms) {
        s_look_until_ms = 0;
        if (!s_override_active && s_state != IDLE_FACE_BLINK) {
            s_state = IDLE_FACE_NORMAL;
            (void)sys_event_post_to(IDLE_FACE_EVENT, IDLE_FACE_EVENT_STATE_CHANGED,
                                    &s_state, sizeof(s_state));
        }
        schedule_next_look(now);
    }

    if (s_blink_until_ms == 0 && (int32_t)(now - s_next_blink_ms) >= 0) {
        s_blink_until_ms = now + IDLE_FACE_BLINK_MS;
        schedule_next_blink(now + IDLE_FACE_BLINK_MS);
    }

    if (s_blink_until_ms == 0 && s_look_until_ms == 0 &&
        (int32_t)(now - s_next_look_ms) >= 0) {
        s_look_state = (esp_random() & 1) ? IDLE_FACE_LOOK_LEFT : IDLE_FACE_LOOK_RIGHT;
        s_look_until_ms = now + IDLE_FACE_LOOK_HOLD_MS;
        schedule_next_look(now + IDLE_FACE_LOOK_HOLD_MS);
    }
}

/** The expression that should actually be rendered right now. */
static idle_face_state_t effective_state(void)
{
    if (s_blink_until_ms != 0) {
        return IDLE_FACE_BLINK;
    }
    if (s_look_until_ms != 0) {
        return s_look_state;
    }
    return s_override_active ? s_override_state : s_state;
}

/* ------------------------------------------------------------------------ */
/* Timer                                                                     */
/* ------------------------------------------------------------------------ */

static void face_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    (void)idle_face_tick((uint32_t)sys_time_ms());
}

esp_err_t idle_face_tick(uint32_t now_ms)
{
    if (!s_attached) {
        return ESP_ERR_INVALID_STATE;
    }

    tick_behaviour(now_ms);

    const face_params_t *target = params_for(effective_state());
    s_current.open = lerp(s_current.open, target->open, target->speed);
    s_current.dx = lerp(s_current.dx, target->dx, target->speed);
    s_current.dy = lerp(s_current.dy, target->dy, target->speed);
    s_current.pupil = lerp(s_current.pupil, target->pupil, target->speed);
    s_current.arc = lerp(s_current.arc, target->arc, target->speed);
    s_current.cross = lerp(s_current.cross, target->cross, target->speed);
    s_current.eye_dy = lerp(s_current.eye_dy, target->eye_dy, target->speed);
    s_current.width_scale = lerp(s_current.width_scale, target->width_scale, target->speed);
    s_current.speed = target->speed;

    /* Breathing runs at ~15 s per cycle; SYNCING needs a faster orbit. */
    const float cycles_per_ms = (effective_state() == IDLE_FACE_SYNCING)
                                    ? (1.0f / 1800.0f)
                                    : (1.0f / 15000.0f);
    s_phase += (float)IDLE_FACE_TICK_MS * cycles_per_ms;
    if (s_phase >= 1.0f) {
        s_phase -= 1.0f;
    }

    apply_params();
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Public state control                                                      */
/* ------------------------------------------------------------------------ */

esp_err_t idle_face_set_state(idle_face_state_t state)
{
    if (state >= IDLE_FACE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state == state && !s_override_active) {
        return ESP_OK;
    }

    s_override_active = false;
    s_state = state;

    /* A new explicit expression cancels any pending blink/look. */
    s_blink_until_ms = 0;
    s_look_until_ms = 0;
    const uint32_t now = now_ms32();
    schedule_next_blink(now);
    schedule_next_look(now);

    SYS_LOGD(TAG, "state -> %d", (int)state);
    (void)sys_event_post_to(IDLE_FACE_EVENT, IDLE_FACE_EVENT_STATE_CHANGED,
                            &s_state, sizeof(s_state));
    return ESP_OK;
}

idle_face_state_t idle_face_get_state(void)
{
    return s_state;
}

esp_err_t idle_face_express(idle_face_state_t state, uint32_t duration_ms)
{
    if (state >= IDLE_FACE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (duration_ms == 0) {
        duration_ms = IDLE_FACE_EXPRESS_DEFAULT_MS;
    }

    if (!s_override_active) {
        s_state_before_override = s_state;
    }
    s_override_active = true;
    s_override_state = state;
    s_override_until_ms = now_ms32() + duration_ms;

    (void)sys_event_post_to(IDLE_FACE_EVENT, IDLE_FACE_EVENT_STATE_CHANGED,
                            &state, sizeof(state));
    return ESP_OK;
}

esp_err_t idle_face_set_brightness(uint8_t percent)
{
    return th_bsp_set_brightness(percent);
}

bool idle_face_is_attached(void)
{
    return s_attached;
}

/* ------------------------------------------------------------------------ */
/* Construction                                                              */
/* ------------------------------------------------------------------------ */

/** Build the two X strokes of one eye and return them through @p out. */
static esp_err_t build_cross_lines(lv_obj_t *parent, lv_obj_t *out[2],
                                   lv_point_precise_t points[2][2])
{
    for (int k = 0; k < 2; k++) {
        out[k] = lv_line_create(parent);
        if (out[k] == NULL) {
            return ESP_ERR_NO_MEM;
        }

        if (k == 0) {
            points[k][0] = (lv_point_precise_t){ .x = 0, .y = 0 };
            points[k][1] = (lv_point_precise_t){ .x = (lv_value_precise_t)s_eye_base_w,
                                                 .y = (lv_value_precise_t)s_eye_base_h };
        } else {
            points[k][0] = (lv_point_precise_t){ .x = (lv_value_precise_t)s_eye_base_w, .y = 0 };
            points[k][1] = (lv_point_precise_t){ .x = 0,
                                                 .y = (lv_value_precise_t)s_eye_base_h };
        }

        lv_line_set_points(out[k], points[k], 2);
        lv_obj_set_style_line_color(out[k], UI_COLOR_IDLE_FG, 0);
        lv_obj_set_style_line_width(out[k], CONFIG_TODOHELPER_IDLE_LINE_WIDTH, 0);
        lv_obj_set_style_line_rounded(out[k], true, 0);
        lv_obj_set_style_line_opa(out[k], LV_OPA_TRANSP, 0);
        lv_obj_align(out[k], LV_ALIGN_CENTER, 0, 0);
    }
    return ESP_OK;
}

/**
 * Point storage for lv_line must outlive the call, hence file scope.
 * Index 0/1 = left/right eye, then stroke, then the two endpoints.
 */
static lv_point_precise_t s_cross_points[2][2][2];

esp_err_t idle_face_attach(lv_obj_t *parent)
{
    if (parent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_attached) {
        return ESP_OK;
    }

    /* Full-bleed black background (requirement 11.1). */
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *canvas = lv_obj_create(parent);
    if (canvas == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(canvas);
    lv_obj_set_size(canvas, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(canvas, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(canvas, LV_OPA_COVER, 0);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 2; i++) {
        /* Outline-only object: a white ellipse drawn with the border. */
        s_face.eye[i] = lv_obj_create(canvas);
        if (s_face.eye[i] == NULL) {
            return ESP_ERR_NO_MEM;
        }
        lv_obj_remove_style_all(s_face.eye[i]);
        lv_obj_set_size(s_face.eye[i], s_eye_base_w, s_eye_base_h);
        lv_obj_set_style_bg_opa(s_face.eye[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_face.eye[i], UI_COLOR_IDLE_FG, 0);
        lv_obj_set_style_border_width(s_face.eye[i], CONFIG_TODOHELPER_IDLE_LINE_WIDTH, 0);
        lv_obj_set_style_border_side(s_face.eye[i], LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_radius(s_face.eye[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(s_face.eye[i], 0, 0);
        lv_obj_remove_flag(s_face.eye[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_face.eye[i], LV_OBJ_FLAG_CLICKABLE);

        /* Pupil, a child of the eye so it moves with it. */
        s_face.pupil[i] = lv_obj_create(s_face.eye[i]);
        if (s_face.pupil[i] == NULL) {
            return ESP_ERR_NO_MEM;
        }
        lv_obj_remove_style_all(s_face.pupil[i]);
        lv_obj_set_style_bg_color(s_face.pupil[i], UI_COLOR_IDLE_FG, 0);
        lv_obj_set_style_bg_opa(s_face.pupil[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_face.pupil[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_remove_flag(s_face.pupil[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_face.pupil[i], LV_OBJ_FLAG_CLICKABLE);

        /* X strokes live on the canvas, aligned with the eye box. */
        if (build_cross_lines(canvas, s_face.cross[i], s_cross_points[i]) != ESP_OK) {
            return ESP_ERR_NO_MEM;
        }
        for (int k = 0; k < 2; k++) {
            lv_obj_align(s_face.cross[i][k], LV_ALIGN_CENTER,
                         (i == 0) ? -(int32_t)(s_eye_gap / 2 + s_eye_base_w / 2)
                                  : (int32_t)(s_eye_gap / 2 + s_eye_base_w / 2),
                         0);
        }
    }

    s_face.container = canvas;
    s_attached = true;

    /* Start from the current state without a visible jump. */
    s_current = *params_for(s_state);
    s_current.arc = 0.0f;
    s_current.cross = 0.0f;
    const uint32_t now = now_ms32();
    schedule_next_blink(now);
    schedule_next_look(now);
    apply_params();

    s_face.timer = lv_timer_create(face_timer_cb, IDLE_FACE_TICK_MS, NULL);
    if (s_face.timer == NULL) {
        SYS_LOGW(TAG, "animator timer unavailable");
    }

    SYS_LOGI(TAG, "idle face attached (%ux%u eyes, gap %u)",
             (unsigned)s_eye_base_w, (unsigned)s_eye_base_h, (unsigned)s_eye_gap);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Page + module entry                                                       */
/* ------------------------------------------------------------------------ */

static esp_err_t idle_page_create(lv_obj_t *parent)
{
    return idle_face_attach(parent);
}

static esp_err_t idle_page_on_show(void)
{
    return idle_face_set_state(IDLE_FACE_NORMAL);
}

static const ui_page_t s_idle_page = {
    .id = UI_PAGE_IDLE_FACE,
    .name = "idle_face",
    .create = idle_page_create,
    .on_show = idle_page_on_show,
};

const void *idle_face_page_descriptor(void)
{
    return &s_idle_page;
}

esp_err_t idle_face_init(void)
{
    esp_err_t err = ui_page_register(&s_idle_page, 1);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Already registered by a previous init: harmless. */
        return ESP_OK;
    }
    if (err != ESP_OK) {
        SYS_LOGE(TAG, "page registration failed: %s", esp_err_to_name(err));
        return err;
    }

    SYS_LOGI(TAG, "ready (%d states)", IDLE_FACE_MAX);
    return ESP_OK;
}
