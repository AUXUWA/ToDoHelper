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

/* ------------------------------------------------------------------------- */
/* Tunables                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * Autonomous behaviour intervals. Every one of them is re-rolled from a fresh
 * random value, so the face never repeats a rhythm (see "Randomisation" in the
 * component README).
 */
/** Blink: the gap is re-rolled every time, and ~18% of blinks are doubled. */
#define IDLE_FACE_BLINK_MIN_MS       2600
#define IDLE_FACE_BLINK_MAX_MS       7500
#define IDLE_FACE_BLINK_DUR_MIN_MS   110
#define IDLE_FACE_BLINK_DUR_MAX_MS   165
#define IDLE_FACE_BLINK_DOUBLE_PCT   18
#define IDLE_FACE_BLINK_DOUBLE_MS    300
/** Look-around: hold time is random too, so glances have different lengths. */
#define IDLE_FACE_LOOK_MIN_MS        4000
#define IDLE_FACE_LOOK_MAX_MS        12000
#define IDLE_FACE_LOOK_HOLD_MIN_MS   900
#define IDLE_FACE_LOOK_HOLD_MAX_MS   2000
#define IDLE_FACE_LOOK_GAIN          0.16f  /**< per tick, ~180 ms settle */
/** Micro-saccade: a few pixels of pupil drift, well below a full glance. */
#define IDLE_FACE_SACCADE_MIN_MS     1800
#define IDLE_FACE_SACCADE_MAX_MS     5200
#define IDLE_FACE_SACCADE_GAIN       0.30f
/** Breathing: the period is re-rolled after every cycle (~12-19 s). */
#define IDLE_FACE_BREATH_MIN_MS      12000
#define IDLE_FACE_BREATH_MAX_MS      19000
#define IDLE_FACE_BREATH_AMP         0.030f
#define IDLE_FACE_SYNC_PERIOD_MS     1800   /**< SYNCING pupil orbit */
#define IDLE_FACE_TALK_PERIOD_MS     620    /**< SPEAKING "mouth-less" rhythm */

/** Default duration for idle_face_express(). */
#define IDLE_FACE_EXPRESS_DEFAULT_MS 1600

/**
 * Expression cross-fade duration, derived from the effect table's `speed`
 * column: SPEED_FAST states snap (a blink must not take half a second),
 * everything else eases over a little under half a second.
 */
#define IDLE_FACE_XFADE_MS           420
#define IDLE_FACE_XFADE_FAST_MS      180

/** Standby enter (eyes droop shut) / exit (eyes widen and settle). */
#define IDLE_FACE_SLEEP_MS           950
#define IDLE_FACE_WAKE_MS            420
#define IDLE_FACE_WAKE_OVERSHOOT     0.12f

/*
 * Richer idle behaviour (1.1).
 *
 * The face used to do four things: blink, glance, drift its pupils a little and
 * breathe. That is enough to look alive for a few seconds and repetitive after a
 * minute, which is exactly how long the standby screen stays up. Three more
 * layers fix that, all of them additive and all of them bounded:
 *
 *   1. spontaneous moods (s_antic_*) - a brief, random expression every 13-34 s;
 *   2. drowsiness (s_drowsy) - the longer nobody interacts, the shorter and
 *      more frequent the blinks, the droopier the gaze; any real interaction
 *      (an expression, waking up) wipes it out instantly;
 *   3. pupil dilation + whole-face drift - slow random walks that make the eye
 *      look alive rather than drawn.
 */

/** Spontaneous mood: how often, and how long one lasts. */
#define IDLE_FACE_ANTIC_MIN_MS          13000
#define IDLE_FACE_ANTIC_MAX_MS          34000
#define IDLE_FACE_ANTIC_HOLD_MIN_MS     320
#define IDLE_FACE_ANTIC_HOLD_MAX_MS     900
#define IDLE_FACE_ANTIC_LONG_MIN_MS     600   /**< for the slower moods (SLEEPY/THINKING) */
#define IDLE_FACE_ANTIC_LONG_MAX_MS     1100

/** Pupil dilation: slow random walk between constricted and dilated. */
#define IDLE_FACE_DILATE_MIN_MS         5000
#define IDLE_FACE_DILATE_MAX_MS         11000
#define IDLE_FACE_DILATE_GAIN           0.05f
#define IDLE_FACE_PUPIL_WIDE_MIN        1.05f
#define IDLE_FACE_PUPIL_NARROW_MIN      0.91f

/** Whole-face drift: two slow sines (irrational ratio) read as head movement. */
#define IDLE_FACE_DRIFT_X_PX            3.0f
#define IDLE_FACE_DRIFT_Y_PX            2.0f
#define IDLE_FACE_DRIFT_GAIN            0.05f

/** Drowsiness: 0 -> 1 while nothing happens, wiped out by activity. */
#define IDLE_FACE_DROWSY_RISE_MS        150000  /**< fully drowsy after 2.5 minutes */
#define IDLE_FACE_DROWSY_FALL_MS        3000
#define IDLE_FACE_DROWSY_OPEN           0.10f   /**< eyes end up 10% shorter */
#define IDLE_FACE_DROWSY_EYE_DY         5.0f    /**< and look 5 px lower */
#define IDLE_FACE_DROWSY_BLINK_SPEEDUP  0.35f   /**< blink intervals shrink by up to 35% */
#define IDLE_FACE_DROWSY_BLINK_LONGER   0.60f   /**< blink durations grow by up to 60% */

/*
 * Richer idle behaviour (1.2).
 *
 * Everything above moves the eyes in *discrete events*: a blink happens, a glance
 * happens, a mood happens. The face still reads as "posing between events". These
 * layers add continuous or reflexive motion on top:
 *
 *   1. gaze wander - a slow 2-D orbit of both pupils, so the eyes are never
 *      perfectly still between glances (a real eye has continuous micro-drift);
 *   2. eyelid settle - a small overshoot when an eye re-opens, instead of the
 *      lid arriving exactly at its resting height;
 *   3. attention perk - occasionally the eyes widen and the pupils constrict for
 *      a moment, as if something was noticed, with no expression change at all;
 *   4. blink asymmetry - one lid sometimes comes down further than the other.
 *
 * All four are additive, bounded, and allocation-free: they only perturb values
 * that apply_params() already computes.
 */

/** Gaze wander: two slow, mutually irrational sines trace a drifting ellipse. */
#define IDLE_FACE_WANDER_X              0.16f   /**< pupil travel fraction, x */
#define IDLE_FACE_WANDER_Y              0.10f   /**< pupil travel fraction, y */

/** Eyelid settle: how far past the resting height a re-opened lid overshoots. */
#define IDLE_FACE_SETTLE_OPEN           0.055f
#define IDLE_FACE_SETTLE_MS             260

/** Attention perk: widen + constrict for a beat, then fade. */
#define IDLE_FACE_PERK_MIN_MS           17000
#define IDLE_FACE_PERK_MAX_MS           46000
#define IDLE_FACE_PERK_MS               520
#define IDLE_FACE_PERK_OPEN             0.18f   /**< eye height gain at the peak */
#define IDLE_FACE_PERK_PUPIL            (-0.10f)/**< pupils constrict (focus) */

/** Blink asymmetry: how much the two lids may differ, and how often it matters. */
#define IDLE_FACE_BLINK_ASYM_PCT        30
#define IDLE_FACE_BLINK_ASYM_MAX        0.30f

/** Partial (squint) blink: the eyelid only comes part of the way down. */
#define IDLE_FACE_BLINK_PARTIAL_PCT     28
#define IDLE_FACE_BLINK_PARTIAL_MIN     0.40f
#define IDLE_FACE_BLINK_PARTIAL_SPAN    0.22f
/** The rare slow blink of someone reading or tired. */
#define IDLE_FACE_BLINK_SLOW_PCT        9
#define IDLE_FACE_BLINK_SLOW_MIN_MS     240
#define IDLE_FACE_BLINK_SLOW_MAX_MS     380
/** Chance that a glance is immediately followed by one to the other side. */
#define IDLE_FACE_LOOK_DOUBLE_PCT       22

/* ------------------------------------------------------------------------ */
/* Effect table (requirement 11.2)                                           */
/* ------------------------------------------------------------------------ */

/**
 * Interpolated eye parameters.
 *
 * `open` scales the eye height, `pupil` the pupil visibility, `dx`/`dy` shift
 * the pupil inside the eye. `arc` morphs the outline into an upward arc (happy)
 * and `cross` switches to the X shape (error). `speed` is *not* a per-frame
 * lerp gain any more: it only selects the cross-fade duration (fast vs. slow)
 * when this state becomes the target, and it is stored per state so the table
 * stays the single source of truth for how each expression moves.
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
    float speed;      /**< cross-fade class: SPEED_FAST = snappy, else eased */
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
    /* SPEAKING   */ { 0.78f,  0.00f,  0.00f, 0.00f, 0.8f, 0.0f,  -4.0f, 1.25f, SPEED_NORMAL },
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

/**
 * A cross-fade in flight.
 *
 * Expression switches used to snap because `s_current` was lerped towards the
 * new table row with a per-frame gain: the first frame already jumped a large
 * fraction of the way. Now `from` freezes the visual state the moment the
 * target changes, `elapsed_ms` is accumulated and the render values are
 * recomputed as `from -> target` through an ease curve. Nothing is allocated
 * and no LVGL animation object is involved.
 */
typedef struct {
    face_params_t from;
    idle_face_state_t target;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
} face_transition_t;

static face_objects_t s_face;
static bool s_attached;

static idle_face_state_t s_state = IDLE_FACE_NORMAL;
static face_params_t s_current;
static face_transition_t s_xfade;
static float s_phase;              /**< 0..1, drives the SYNCING pupil orbit */
static uint32_t s_breath_ms;       /**< elapsed time in the current breath */
static uint32_t s_breath_period_ms;/**< re-rolled each cycle, 12-19 s */

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
static float s_look_dx;            /**< eased pupil offset, -1..1 */
static float s_look_dx_target;
static uint32_t s_next_saccade_ms;
static uint32_t s_saccade_hold_ms;
static uint32_t s_saccade_from_ms;
static float s_saccade_dx;
static float s_saccade_dy;

/* spontaneous idle moods (1.1): a brief random expression of its own */
static bool s_antic_active;
static idle_face_state_t s_antic_state = IDLE_FACE_NORMAL;
static uint32_t s_antic_until_ms;
static uint32_t s_next_antic_ms;

/* longer-term liveliness: dilation, head drift, drowsiness */
static float s_pupil_scale = 1.0f;
static float s_pupil_scale_target = 1.0f;
static uint32_t s_next_dilate_ms;
static float s_drift_x;
static float s_drift_y;
static float s_blink_depth = 1.0f;   /**< 1 = full blink, < 1 = squint */
static float s_drowsy;               /**< 0 = alert, 1 = about to nod off */

/* 1.2: continuous gaze wander, eyelid settle, attention perk, blink asymmetry */
static float s_wander_x;             /**< pupil wander, -1..1 (eased) */
static float s_wander_y;
static uint32_t s_next_perk_ms;      /**< when the next attention perk may start */
static uint32_t s_perk_from_ms;      /**< 0 = no perk running */
static uint32_t s_settle_from_ms;    /**< 0 = no eyelid settle running */
/** Per-eye blink depth, so the two lids can differ (0 = left, 1 = right). */
static float s_eye_lid[2] = { 1.0f, 1.0f };

/* standby enter/exit envelope (see idle_face_standby_enter/exit) */
static uint32_t s_motion_from_ms;
static uint32_t s_motion_dur_ms;
static float s_motion_from;
static float s_motion_to;
static bool s_motion_active;
static uint32_t s_wake_from_ms;
static bool s_wake_active;

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

static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/** Cubic ease-in-out: zero slope at both ends, so no switch can "snap". */
static float smoothstep01(float t)
{
    t = clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/** Blend one component, snapping exactly onto the target when it is close. */
static float xfade_value(float from, float to, float t, float snap)
{
    const float delta = to - from;
    if (delta < 0.0f ? -delta < snap : delta < snap) {
        return to;
    }
    return from + delta * t;
}

static uint32_t xfade_duration_for(const face_params_t *p)
{
    return (p->speed >= SPEED_FAST) ? IDLE_FACE_XFADE_FAST_MS : IDLE_FACE_XFADE_MS;
}

static const face_params_t *params_for(idle_face_state_t state)
{
    if (state >= IDLE_FACE_MAX) {
        state = IDLE_FACE_NORMAL;
    }
    return &s_effects[state];
}

/** The expression that should actually be rendered right now. */
static idle_face_state_t effective_state(void);

/* ------------------------------------------------------------------------ */
/* Expression cross-fade + standby envelope                                  */
/* ------------------------------------------------------------------------ */

static void begin_xfade(idle_face_state_t target)
{
    s_xfade.from = s_current;
    s_xfade.target = target;
    s_xfade.elapsed_ms = 0;
    s_xfade.duration_ms = xfade_duration_for(params_for(target));
}

static void schedule_breath(uint32_t now)
{
    (void)now;
    s_breath_period_ms = rand_between(IDLE_FACE_BREATH_MIN_MS, IDLE_FACE_BREATH_MAX_MS);
}

/** Start a one-shot 1 -> 0 (sleep) or 1 -> overshoot (wake) motion envelope. */
static void begin_motion(uint32_t now, uint32_t duration_ms, float from, float to)
{
    s_motion_from_ms = now;
    s_motion_dur_ms = duration_ms;
    s_motion_from = from;
    s_motion_to = to;
    s_motion_active = true;
}

/** Bounded motion envelope, or 1.0 when no enter/exit animation is running. */
static float motion_env(uint32_t now)
{
    if (!s_motion_active) {
        return 1.0f;
    }
    const uint32_t elapsed = now - s_motion_from_ms;
    if (elapsed >= s_motion_dur_ms) {
        s_motion_active = false;
        return s_motion_to;
    }
    const float t = (float)elapsed / (float)s_motion_dur_ms;
    return s_motion_from + (s_motion_to - s_motion_from) * smoothstep01(t);
}

/** Extra eye opening while waking up: 0 at both ends, a small bump in the middle. */
static float wake_env(uint32_t now)
{
    if (!s_wake_active) {
        return 0.0f;
    }
    const uint32_t elapsed = now - s_wake_from_ms;
    if (elapsed >= IDLE_FACE_WAKE_MS) {
        s_wake_active = false;
        return 0.0f;
    }
    const float t = (float)elapsed / (float)IDLE_FACE_WAKE_MS;
    return IDLE_FACE_WAKE_OVERSHOOT * sinf(t * (float)M_PI);
}

/**
 * Eyelid settle: a small overshoot just after a blink re-opens.
 *
 * Without it the lid arrives exactly at its resting height and stops, which reads
 * as a mechanical shutter. A real lid springs slightly past the resting position
 * and eases back. Like wake_env() this is a bounded bump that is zero at both
 * ends, so it can never leave the eye in a wrong state.
 */
static float settle_env(uint32_t now)
{
    if (s_settle_from_ms == 0) {
        return 0.0f;
    }
    const uint32_t elapsed = now - s_settle_from_ms;
    if (elapsed >= IDLE_FACE_SETTLE_MS) {
        s_settle_from_ms = 0;
        return 0.0f;
    }
    const float t = (float)elapsed / (float)IDLE_FACE_SETTLE_MS;
    return IDLE_FACE_SETTLE_OPEN * sinf(t * (float)M_PI);
}

/**
 * Attention perk: 0 at both ends, peaks in the middle.
 *
 * Driven by s_perk_from_ms; the caller starts it. Positive return widens the eye,
 * and the caller scales the pupil by (1 + perk * PERK_PUPIL/PERK_OPEN) so the
 * pupils constrict as the eyes open - the "focusing on something" reflex.
 */
static float perk_env(uint32_t now)
{
    if (s_perk_from_ms == 0) {
        return 0.0f;
    }
    const uint32_t elapsed = now - s_perk_from_ms;
    if (elapsed >= IDLE_FACE_PERK_MS) {
        s_perk_from_ms = 0;
        return 0.0f;
    }
    const float t = (float)elapsed / (float)IDLE_FACE_PERK_MS;
    return IDLE_FACE_PERK_OPEN * sinf(t * (float)M_PI);
}

/** True while a perk is in flight (the scheduler must not start another). */
static bool perk_running(void)
{
    return s_perk_from_ms != 0;
}

static void update_xfade(uint32_t dt_ms)
{
    s_xfade.elapsed_ms += dt_ms;
    const float t = (s_xfade.duration_ms == 0)
                        ? 1.0f
                        : smoothstep01((float)s_xfade.elapsed_ms / (float)s_xfade.duration_ms);

    const face_params_t *to = params_for(s_xfade.target);
    const face_params_t *from = &s_xfade.from;

    s_current.open = xfade_value(from->open, to->open, t, 0.004f);
    s_current.dx = xfade_value(from->dx, to->dx, t, 0.004f);
    s_current.dy = xfade_value(from->dy, to->dy, t, 0.004f);
    s_current.pupil = xfade_value(from->pupil, to->pupil, t, 0.004f);
    s_current.arc = xfade_value(from->arc, to->arc, t, 0.004f);
    s_current.cross = xfade_value(from->cross, to->cross, t, 0.004f);
    s_current.eye_dy = xfade_value(from->eye_dy, to->eye_dy, t, 0.2f);
    s_current.width_scale = xfade_value(from->width_scale, to->width_scale, t, 0.004f);
    s_current.speed = to->speed;

    if (s_xfade.elapsed_ms >= s_xfade.duration_ms) {
        s_current = *to;
    }
}

/* ------------------------------------------------------------------------ */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------ */

static void apply_params(uint32_t now)
{
    if (!s_attached) {
        return;
    }

    const idle_face_state_t state = effective_state();

    /* Breathing: amplitude-scaled, and suppressed while the eyes are shut, so
     * a slow breath cannot re-open a sleeping eye. */
    const float breath_phase = (s_breath_period_ms == 0)
                                   ? 0.0f
                                   : (float)s_breath_ms / (float)s_breath_period_ms;
    const float breath =
        1.0f + (IDLE_FACE_BREATH_AMP * motion_env(now)) * sinf(breath_phase * 2.0f * (float)M_PI);
    float open = s_current.open * breath * motion_env(now) + wake_env(now);

    /* Drowsiness shortens the eye; a partial blink then only closes it part of
     * the way, which is what turns a mechanical shutter into a squint. */
    open *= 1.0f - (IDLE_FACE_DROWSY_OPEN * s_drowsy);

    /*
     * Attention perk and eyelid settle: two more bounded bumps on the same open
     * value. The perk widens both eyes; the settle is the lid springing just past
     * its resting height after a blink. Both are zero at their endpoints, so
     * neither can leave the eye stuck open or shut.
     */
    const float perk = perk_env(now);
    open += perk;
    open += settle_env(now);

    if (state == IDLE_FACE_BLINK && s_blink_depth < 0.999f) {
        open = 1.0f - (1.0f - open) * s_blink_depth;
    }
    open = clampf(open, 0.04f, 1.6f);

    const int32_t eye_w = (int32_t)((float)s_eye_base_w * (1.0f + 0.05f * s_current.arc));
    const int32_t border = (int32_t)((float)CONFIG_TODOHELPER_IDLE_LINE_WIDTH *
                                     s_current.width_scale);
    const bool cross_mode = s_current.cross > 0.5f;
    const bool arc_mode = !cross_mode && s_current.arc > 0.5f;
    const float pupil_fade = sqrtf(clampf(open, 0.0f, 1.0f));

    /*
     * Per-eye lid depth. Usually both eyes share s_blink_depth, but an asymmetric
     * blink (one lid further down than the other) is one of the small tells that
     * separates a face from a metronome, so a fraction of blinks split the depth.
     * A perk is also never perfectly symmetric for the same reason.
     */
    const float lid_l = (state == IDLE_FACE_BLINK) ? s_eye_lid[0] : 1.0f;
    const float lid_r = (state == IDLE_FACE_BLINK) ? s_eye_lid[1] : 1.0f;
    const int32_t eye_h_eye[2] = {
        (int32_t)((float)s_eye_base_h *
                  clampf(1.0f - (1.0f - open) * lid_l, 0.04f, 1.6f)),
        (int32_t)((float)s_eye_base_h *
                  clampf(1.0f - (1.0f - open) * lid_r, 0.04f, 1.6f)),
    };

    /* Whole-face drift and the drowsy droop move both eyes (and the X strokes,
     * which are aligned here as well so they stay in register) together. */
    const int32_t face_x = (int32_t)s_drift_x;
    const int32_t face_y =
        (int32_t)(s_current.eye_dy + (IDLE_FACE_DROWSY_EYE_DY * s_drowsy) + s_drift_y);

    for (int i = 0; i < 2; i++) {
        lv_obj_t *eye = s_face.eye[i];
        lv_obj_t *pupil = s_face.pupil[i];
        if (eye == NULL) {
            continue;
        }

        const int32_t eye_x = (i == 0) ? -(int32_t)(s_eye_gap / 2 + s_eye_base_w / 2)
                                       : (int32_t)(s_eye_gap / 2 + s_eye_base_w / 2);

        lv_obj_set_size(eye, eye_w, arc_mode ? eye_h_eye[i] / 2 : eye_h_eye[i]);
        lv_obj_align(eye, LV_ALIGN_CENTER, eye_x + face_x, face_y);

        lv_obj_set_style_border_width(eye, border, 0);
        lv_obj_set_style_border_opa(eye, cross_mode ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(eye, arc_mode ? LV_BORDER_SIDE_TOP : LV_BORDER_SIDE_FULL, 0);
        /* A radius equal to half the height turns the outline into an ellipse;
         * for the happy arc the bottom half is simply not drawn. */
        lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);

        /* Pupil: kept inside the eye and shifted by dx/dy. */
        if (pupil != NULL) {
            /* Dilation + a little extra when the face is paying attention.
             * A perk constricts the pupils as the eyes widen - the focusing
             * reflex - which is what stops the widen from reading as surprise. */
            float scale = s_pupil_scale;
            if (state == IDLE_FACE_LISTENING) {
                scale *= 1.06f;
            }
            if (perk > 0.0f) {
                scale *= 1.0f + (perk / IDLE_FACE_PERK_OPEN) * IDLE_FACE_PERK_PUPIL;
            }
            const int32_t pupil_size =
                (int32_t)((float)eye_h_eye[i] * 0.34f * clampf(scale, 0.5f, 1.5f));
            const int32_t travel_x = (eye_w - pupil_size) / 2 - border;
            const int32_t travel_y = (eye_h_eye[i] - pupil_size) / 2 - border;

            lv_obj_set_size(pupil, pupil_size, pupil_size);
            lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, 0);

            float opacity = s_current.pupil;
            if (arc_mode || cross_mode) {
                opacity = 0.0f;
            }
            opacity *= pupil_fade;
            if (opacity < 0.0f) {
                opacity = 0.0f;
            }
            if (opacity > 1.0f) {
                opacity = 1.0f;
            }
            lv_obj_set_style_bg_opa(pupil, (lv_opa_t)(opacity * LV_OPA_COVER), 0);

            /* The table's dx/dy, plus the idle glance, plus a micro-saccade, plus
             * the continuous gaze wander, plus any perk drift. */
            float dx = s_current.dx + s_look_dx + s_saccade_dx + s_wander_x;
            float dy = s_current.dy + s_saccade_dy + s_wander_y;
            dx = clampf(dx, -1.0f, 1.0f);
            dy = clampf(dy, -1.0f, 1.0f);

            /* SYNCING walks the pupil around a small circle. */
            if (state == IDLE_FACE_SYNCING) {
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
            lv_obj_align(line, LV_ALIGN_CENTER, eye_x + face_x, face_y);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Auto behaviour                                                            */
/* ------------------------------------------------------------------------ */

/**
 * Blink interval. Drowsiness shortens it (tired eyes blink more often), which is
 * the effect that makes a long standby session feel like it winds down.
 */
static void schedule_next_blink(uint32_t now)
{
    const uint32_t span = IDLE_FACE_BLINK_MAX_MS - IDLE_FACE_BLINK_MIN_MS;
    const uint32_t shrink =
        (uint32_t)((float)span * IDLE_FACE_DROWSY_BLINK_SPEEDUP * s_drowsy);
    const uint32_t hi = (shrink < span) ? (IDLE_FACE_BLINK_MAX_MS - shrink)
                                        : IDLE_FACE_BLINK_MIN_MS;
    s_next_blink_ms = now + rand_between(IDLE_FACE_BLINK_MIN_MS, hi);
}

static void schedule_next_antic(uint32_t now)
{
    s_next_antic_ms = now + rand_between(IDLE_FACE_ANTIC_MIN_MS, IDLE_FACE_ANTIC_MAX_MS);
}

static void schedule_next_dilate(uint32_t now)
{
    s_next_dilate_ms = now + rand_between(IDLE_FACE_DILATE_MIN_MS, IDLE_FACE_DILATE_MAX_MS);
}

static void schedule_next_look(uint32_t now)
{
    s_next_look_ms = now + rand_between(IDLE_FACE_LOOK_MIN_MS, IDLE_FACE_LOOK_MAX_MS);
}

static void schedule_next_saccade(uint32_t now)
{
    s_next_saccade_ms = now + rand_between(IDLE_FACE_SACCADE_MIN_MS, IDLE_FACE_SACCADE_MAX_MS);
}

/**
 * Blinking, glancing and pupil drift only make sense for the plain idle
 * expression: an ERROR or THINKING face must not blink itself back to neutral.
 * A deliberately requested SLEEPY face (notification trigger or AI state) is
 * excluded for the same reason - it means something, so it must not be blinked
 * away. Standby itself does *not* use SLEEPY any more (see standby_enter): it
 * keeps NORMAL and lets all of this keep running.
 */
static bool state_allows_idle_antics(idle_face_state_t state)
{
    return state == IDLE_FACE_NORMAL;
}

/**
 * Drop a temporary expression whose time is up.
 *
 * Shared by the animator tick and the standby hand-over on purpose: the animator
 * timer only starts when the idle page is created, so the *first* hand-over can
 * still see an override that expired minutes ago (measured on hardware: a 2.5 s
 * `wifi_connected` expression from t=7.7 s was still flagged active at t=82 s,
 * which made the first standby entry silently refuse). Expiring it here as well
 * makes the hand-over independent of whether a tick has run yet.
 */
static void override_expire_due(uint32_t now)
{
    if (!s_override_active || (int32_t)(now - s_override_until_ms) < 0) {
        return;
    }

    s_override_active = false;
    const idle_face_state_t restore = s_state_before_override;
    if (restore != s_state) {
        s_state = restore;
        (void)sys_event_post_to(IDLE_FACE_EVENT, IDLE_FACE_EVENT_STATE_CHANGED,
                                &s_state, sizeof(s_state));
    }
}

/**
 * Start a spontaneous idle mood: a short, random expression with no reason
 * behind it, which is what an otherwise perfectly regular face lacks.
 *
 * The table is the whole "personality": repetition is the weighting, so the two
 * human-looking moods dominate and the rest are seasoning. Every mood is short,
 * every mood cross-fades in and out through the normal machinery, and none of
 * them can outlive its hold (tick_behaviour drops it on a timer).
 */
static void start_antic(uint32_t now)
{
    static const idle_face_state_t moods[] = {
        IDLE_FACE_HAPPY,      /* squint-smile */
        IDLE_FACE_HAPPY,
        IDLE_FACE_SLEEPY,     /* lazy half-close */
        IDLE_FACE_LISTENING,  /* a moment of attention: eyes widen */
        IDLE_FACE_THINKING,   /* glance up, as if recalling something */
        IDLE_FACE_SPEAKING,   /* tiny arc pulse, the mouth-free mumble */
    };

    const idle_face_state_t mood = moods[esp_random() % (sizeof(moods) / sizeof(moods[0]))];

    uint32_t hold;
    if (mood == IDLE_FACE_SLEEPY || mood == IDLE_FACE_THINKING) {
        hold = rand_between(IDLE_FACE_ANTIC_LONG_MIN_MS, IDLE_FACE_ANTIC_LONG_MAX_MS);
    } else {
        hold = rand_between(IDLE_FACE_ANTIC_HOLD_MIN_MS, IDLE_FACE_ANTIC_HOLD_MAX_MS);
    }

    s_antic_active = true;
    s_antic_state = mood;
    s_antic_until_ms = now + hold;
    schedule_next_antic(now + hold);

    SYS_LOGD(TAG, "idle mood %d for %u ms (drowsy %u%%)", (int)mood, (unsigned)hold, (unsigned)(s_drowsy * 100.0f));
}

static void tick_behaviour(uint32_t now)
{
    override_expire_due(now);

    const idle_face_state_t base = s_override_active ? s_override_state : s_state;
    /* Idle antics are allowed only when nothing else is expressing anything. */
    const bool idle_ok = state_allows_idle_antics(base) && !s_override_active;

    /* A spontaneous mood never outlives its hold, and is dropped the moment
     * something more important takes over the face. Rescheduling the blink and
     * the glance here also keeps a mood from ending in an immediate blink. */
    if (s_antic_active && (now >= s_antic_until_ms || !idle_ok)) {
        s_antic_active = false;
        schedule_next_blink(now);
        schedule_next_look(now);
    }

    if (!idle_ok || s_antic_active) {
        /* Let a glance that is already in flight settle back to centre. The blink
         * is cancelled rather than completed: cutting a mood in half with an
         * eyelid looks broken, and the mood itself is the more interesting shape. */
        s_look_dx_target = 0.0f;
        s_saccade_dx = 0.0f;
        s_saccade_dy = 0.0f;
        s_blink_until_ms = 0;
        return;
    }

    /* Finish an in-flight blink first: it overrides everything else. */
    if (s_blink_until_ms != 0 && now >= s_blink_until_ms) {
        s_blink_until_ms = 0;
        /* The lid springs slightly past its resting height before settling
         * (1.2). Without it the eye arrives and stops, which looks mechanical. */
        s_settle_from_ms = now;
        /* Occasionally follow up immediately: real eyes blink in bursts, and a
         * single fixed interval is exactly what makes an idle face look fake. */
        if ((esp_random() % 100u) < IDLE_FACE_BLINK_DOUBLE_PCT) {
            s_next_blink_ms = now + IDLE_FACE_BLINK_DOUBLE_MS;
        } else {
            schedule_next_blink(now);
        }
    }

    if (s_look_until_ms != 0 && now >= s_look_until_ms) {
        s_look_until_ms = 0;
        if (((esp_random() % 100u) < IDLE_FACE_LOOK_DOUBLE_PCT) &&
            (s_look_dx_target > 0.2f || s_look_dx_target < -0.2f)) {
            /* Second glance to the other side: people rarely look exactly once,
             * and the pair reads as "scanning the room" rather than a twitch. */
            s_look_dx_target = -s_look_dx_target * (0.55f + (float)rand_between(0, 2500) / 10000.0f);
            s_look_until_ms = now + rand_between(IDLE_FACE_LOOK_HOLD_MIN_MS,
                                                 IDLE_FACE_LOOK_HOLD_MAX_MS);
            schedule_next_look(now + IDLE_FACE_LOOK_HOLD_MAX_MS);
        } else {
            s_look_dx_target = 0.0f;
            schedule_next_look(now);
        }
    }

    /* Micro-saccade: a few pixels of pupil drift, held for a moment and then
     * released. The hold countdown lives on the tick, the trigger here. */
    if (s_saccade_hold_ms != 0) {
        const uint32_t elapsed = now - s_saccade_from_ms;
        if (elapsed >= s_saccade_hold_ms) {
            s_saccade_hold_ms = 0;
        }
    }
    if (s_saccade_hold_ms == 0 && s_next_saccade_ms != 0 &&
        (int32_t)(now - s_next_saccade_ms) >= 0) {
        s_saccade_dx = (float)((int32_t)rand_between(0, 201) - 100) / 1000.0f;
        s_saccade_dy = (float)((int32_t)rand_between(0, 161) - 80) / 1000.0f;
        s_saccade_hold_ms = rand_between(220, 700);
        s_saccade_from_ms = now;
        schedule_next_saccade(now);
    }

    if (s_blink_until_ms == 0 && (int32_t)(now - s_next_blink_ms) >= 0) {
        uint32_t duration;
        if ((esp_random() % 100u) < IDLE_FACE_BLINK_SLOW_PCT) {
            duration = rand_between(IDLE_FACE_BLINK_SLOW_MIN_MS, IDLE_FACE_BLINK_SLOW_MAX_MS);
        } else {
            duration = rand_between(IDLE_FACE_BLINK_DUR_MIN_MS, IDLE_FACE_BLINK_DUR_MAX_MS);
        }
        /* Drowsy lids move slower. */
        duration += (uint32_t)((float)duration * IDLE_FACE_DROWSY_BLINK_LONGER * s_drowsy);

        /* Full or partial: an eyelid that sometimes stops halfway is most of the
         * difference between a face and an animation. */
        s_blink_depth = ((esp_random() % 100u) < IDLE_FACE_BLINK_PARTIAL_PCT)
                            ? (IDLE_FACE_BLINK_PARTIAL_MIN +
                               (float)rand_between(0, (uint32_t)(IDLE_FACE_BLINK_PARTIAL_SPAN * 1000.0f)) /
                                   1000.0f)
                            : 1.0f;

        /*
         * Asymmetric blink (1.2): sometimes one lid travels further than the
         * other. Both stay within the same full/partial class, so this only ever
         * skews the pair - it can never turn a full blink into a half one.
         */
        s_eye_lid[0] = s_blink_depth;
        s_eye_lid[1] = s_blink_depth;
        if ((esp_random() % 100u) < IDLE_FACE_BLINK_ASYM_PCT) {
            const float skew = (float)rand_between(0, (uint32_t)(IDLE_FACE_BLINK_ASYM_MAX * 1000.0f)) /
                               1000.0f;
            /* Which eye lags is random too, otherwise the face always squints the
             * same way and the asymmetry itself becomes the pattern. */
            const float deeper = clampf(s_blink_depth + skew, IDLE_FACE_BLINK_PARTIAL_MIN, 1.0f);
            const float shallower = clampf(s_blink_depth - skew, IDLE_FACE_BLINK_PARTIAL_MIN, 1.0f);
            if ((esp_random() & 1u) != 0u) {
                s_eye_lid[0] = deeper;
                s_eye_lid[1] = shallower;
            } else {
                s_eye_lid[0] = shallower;
                s_eye_lid[1] = deeper;
            }
        }

        s_blink_until_ms = now + duration;
        schedule_next_blink(now + duration);
    }

    if (s_blink_until_ms == 0 && s_look_until_ms == 0 &&
        (int32_t)(now - s_next_look_ms) >= 0) {
        /* Glances are eased through s_look_dx instead of a state switch, so an
         * interrupted glance continues from where the pupils actually are. */
        s_look_dx_target = ((esp_random() & 1u) ? -1.0f : 1.0f) *
                           (0.75f + (float)rand_between(0, 2000) / 10000.0f);
        s_look_until_ms = now + rand_between(IDLE_FACE_LOOK_HOLD_MIN_MS,
                                             IDLE_FACE_LOOK_HOLD_MAX_MS);
        schedule_next_look(now + rand_between(IDLE_FACE_LOOK_HOLD_MIN_MS,
                                              IDLE_FACE_LOOK_HOLD_MAX_MS));
    }

    /* Spontaneous mood: only from a settled, centred face, so it never fights a
     * glance that is still moving. */
    if (s_blink_until_ms == 0 && s_look_until_ms == 0 &&
        (s_look_dx < 0.15f && s_look_dx > -0.15f) && s_next_antic_ms != 0 &&
        (int32_t)(now - s_next_antic_ms) >= 0) {
        start_antic(now);
    }

    /*
     * Attention perk (1.2): the eyes widen and the pupils tighten for ~0.5 s as
     * if something was noticed. No expression change is involved, which is what
     * makes it read as a reflex rather than a mood.
     *
     * It must not start while an eye is shut or a mood owns the face (the mood
     * already sets the eye shape, and a perk on top would fight it), and it never
     * starts twice - perk_env() clears the timestamp when it finishes.
     */
    if (!perk_running() && s_blink_until_ms == 0 && s_look_until_ms == 0 && !s_antic_active) {
        if (s_next_perk_ms == 0) {
            s_next_perk_ms = now + rand_between(IDLE_FACE_PERK_MIN_MS, IDLE_FACE_PERK_MAX_MS);
        } else if ((int32_t)(now - s_next_perk_ms) >= 0) {
            s_perk_from_ms = now;
            s_next_perk_ms = now + rand_between(IDLE_FACE_PERK_MIN_MS, IDLE_FACE_PERK_MAX_MS);
            SYS_LOGD(TAG, "attention perk");
        }
    }
}

/** True while a spontaneous mood owns the face (used by the renderer). */
static bool antic_showing(void)
{
    return s_antic_active;
}

/** The expression that should actually be rendered right now. */
static idle_face_state_t effective_state(void)
{
    if (s_blink_until_ms != 0) {
        return IDLE_FACE_BLINK;      /* an eyelid is not a mood: it always wins */
    }
    if (s_override_active) {
        return s_override_state;     /* notifications and AI outrank idle moods */
    }
    if (antic_showing()) {
        return s_antic_state;        /* a spontaneous idle mood */
    }
    return s_state;
}

/* ------------------------------------------------------------------------ */
/* Automatic expressions (requirement 11.2 trigger column)                   */
/* ------------------------------------------------------------------------ */

/**
 * Maps an L0 notification name to an expression.
 *
 * This is deliberately the *whole* coupling between idle_face and the business
 * layer: producers call `sys_notify("wifi_connected")` (L0) and never learn that
 * a face exists, while adding a reaction here is one table row and needs no
 * changes anywhere else. `ai_service` is the one exception that calls
 * `idle_face_express()` directly, because it has a real state machine of its own.
 */
typedef struct {
    const char *notification;
    idle_face_state_t state;
    uint32_t duration_ms;
} idle_trigger_t;

static const idle_trigger_t s_triggers[] = {
    { "wifi_connected", IDLE_FACE_HAPPY,    2500 },
    { "wifi_failed",    IDLE_FACE_ERROR,    3500 },
    { "net_offline",    IDLE_FACE_SLEEPY,   4000 },
    { "net_online",     IDLE_FACE_HAPPY,    1800 },
    { "todo_done",      IDLE_FACE_HAPPY,    2000 },
    { "sync_started",   IDLE_FACE_SYNCING,  9000 },
    { "sync_done",      IDLE_FACE_HAPPY,    1500 },
    { "widget_done",    IDLE_FACE_HAPPY,    1500 },
};

static void notify_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;

    const sys_notify_t *note = (const sys_notify_t *)data;
    if (note == NULL) {
        return;
    }

    for (size_t i = 0; i < sizeof(s_triggers) / sizeof(s_triggers[0]); i++) {
        if (strcmp(note->name, s_triggers[i].notification) != 0) {
            continue;
        }
        SYS_LOGD(TAG, "trigger '%s' -> state %d for %u ms", note->name, (int)s_triggers[i].state, (unsigned)s_triggers[i].duration_ms);
        (void)idle_face_express(s_triggers[i].state, s_triggers[i].duration_ms);
        return;
    }
}

/* ------------------------------------------------------------------------ */
/* Timer                                                                     */
/* ------------------------------------------------------------------------ */

static void face_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* lv_timer callbacks already run in the LVGL task, so no lock is needed
     * (AGENT.md 9.0.1). Never use esp_timer for anything that touches LVGL. */
    (void)idle_face_tick((uint32_t)sys_time_ms());
}

esp_err_t idle_face_tick(uint32_t now_ms)
{
    if (!s_attached) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Drive the behaviour timeline BEFORE reading effective_state().
     *
     * This call is load-bearing, not a refactor nicety: tick_behaviour() is where
     * the temporary-expression expiry lives (s_override_until_ms is read nowhere
     * else) and where the next blink / glance / micro-saccade is triggered and
     * finished. Without it the face never blinks or looks around on its own, and
     * an expression set by idle_face_express() would never end - it would stay on
     * screen forever.
     *
     * It runs first because it can clear the override and start a blink, and
     * effective_state() reports exactly those.
     */
    tick_behaviour(now_ms);

    const idle_face_state_t want = effective_state();
    if (want != s_xfade.target) {
        begin_xfade(want);
        SYS_LOGD(TAG, "state -> %d", (int)want);
    }

    /* Shortest path between two 33 ms ticks, so a 64-bit wraparound of
     * esp_timer_get_time() or a stalled timer cannot freeze the animation. */
    static uint32_t s_prev_now;
    uint32_t dt_ms = now_ms - s_prev_now;
    s_prev_now = now_ms;
    if (dt_ms == 0 || dt_ms > 250) {
        dt_ms = IDLE_FACE_TICK_MS;
    }

    update_xfade(dt_ms);

    /* Idle glance: a fast, small correction on top of the expression, kept in
     * its own lane so a look cannot fight an expression cross-fade. The
     * micro-saccade is *held* while it is triggered, then released. */
    s_look_dx += (s_look_dx_target - s_look_dx) * IDLE_FACE_LOOK_GAIN;
    if (s_saccade_hold_ms == 0) {
        s_saccade_dx = xfade_value(s_saccade_dx, 0.0f, 0.18f, 0.002f);
        s_saccade_dy = xfade_value(s_saccade_dy, 0.0f, 0.18f, 0.002f);
    }

    /*
     * Slow layers (1.1): drowsiness, head drift and pupil dilation.
     *
     * They share one property: they change over seconds to minutes, not frames,
     * so they read as a living face rather than as an animation. All three are
     * eased, so switching them off (an expression arrives) never snaps.
     */
    const bool neutral_face =
        state_allows_idle_antics(s_override_active ? s_override_state : s_state) &&
        !s_override_active;

    if (neutral_face) {
        /* A spontaneous mood is fidgeting, not attention: it must NOT reset the
         * drowsiness, otherwise the accumulation below would never get anywhere. */
        s_drowsy += (float)dt_ms / (float)IDLE_FACE_DROWSY_RISE_MS;
        if (s_drowsy > 1.0f) {
            s_drowsy = 1.0f;
        }
    } else {
        s_drowsy -= (float)dt_ms / (float)IDLE_FACE_DROWSY_FALL_MS;
        if (s_drowsy < 0.0f) {
            s_drowsy = 0.0f;
        }
    }

    /* Two sines with an irrational frequency ratio: the pattern never repeats
     * audibly, and the amplitude is small enough to read as head movement. It
     * keeps running during a mood - it is the head, not the expression. */
    const float slow_s = (float)now_ms * 0.001f;
    const float drift_gate = neutral_face ? 1.0f : 0.0f;
    const float drift_x_want = IDLE_FACE_DRIFT_X_PX * drift_gate *
                               (0.6f * sinf(slow_s * 0.31f) + 0.4f * sinf(slow_s * 0.17f));
    const float drift_y_want = IDLE_FACE_DRIFT_Y_PX * drift_gate *
                               (0.6f * sinf(slow_s * 0.23f) + 0.4f * sinf(slow_s * 0.13f));
    s_drift_x = xfade_value(s_drift_x, drift_x_want, IDLE_FACE_DRIFT_GAIN, 0.01f);
    s_drift_y = xfade_value(s_drift_y, drift_y_want, IDLE_FACE_DRIFT_GAIN, 0.01f);

    /*
     * Continuous gaze wander (1.2).
     *
     * The blink/glance/saccade layers are all *events*: between them the pupils
     * are perfectly still, which is the one thing a real eye never is. This adds a
     * slow two-axis Lissajous drift on top, so there is always a little motion.
     * Frequencies are deliberately incommensurate (0.041 / 0.029 and 0.037 /
     * 0.021 Hz) so the path never visibly closes.
     *
     * Eased rather than written directly, so switching it off (an expression
     * arrives) or on again never snaps the pupils.
     */
    const float wander_gate = neutral_face ? 1.0f : 0.0f;
    const float wander_x_want =
        IDLE_FACE_WANDER_X * wander_gate * (0.7f * sinf(slow_s * 0.26f) + 0.3f * sinf(slow_s * 0.41f));
    const float wander_y_want =
        IDLE_FACE_WANDER_Y * wander_gate * (0.6f * sinf(slow_s * 0.19f) + 0.4f * sinf(slow_s * 0.33f));
    s_wander_x = xfade_value(s_wander_x, wander_x_want, 0.06f, 0.002f);
    s_wander_y = xfade_value(s_wander_y, wander_y_want, 0.06f, 0.002f);

    if (s_next_dilate_ms == 0 || (int32_t)(now_ms - s_next_dilate_ms) >= 0) {
        if ((esp_random() & 1u) != 0u) {
            s_pupil_scale_target = IDLE_FACE_PUPIL_WIDE_MIN +
                                   (float)rand_between(0, 90) / 1000.0f;
        } else {
            s_pupil_scale_target = IDLE_FACE_PUPIL_NARROW_MIN +
                                   (float)rand_between(0, 60) / 1000.0f;
        }
        schedule_next_dilate(now_ms);
    }
    s_pupil_scale = xfade_value(s_pupil_scale, s_pupil_scale_target, IDLE_FACE_DILATE_GAIN, 0.001f);

    /* Breathing runs on its own clock; the period is re-rolled every cycle so
     * the rhythm never becomes a metronome. */
    s_breath_ms += dt_ms;
    if (s_breath_ms >= s_breath_period_ms) {
        s_breath_ms = 0;
        schedule_breath(now_ms);
    }

    /* SYNCING orbits its pupils; SPEAKING pulses the eye arcs, which is the
     * mouth-free way of showing a talking rhythm. */
    float cycles_per_ms = 0.0f;
    if (want == IDLE_FACE_SYNCING) {
        cycles_per_ms = 1.0f / (float)IDLE_FACE_SYNC_PERIOD_MS;
    } else if (want == IDLE_FACE_SPEAKING) {
        cycles_per_ms = 1.0f / (float)IDLE_FACE_TALK_PERIOD_MS;
    }
    s_phase += (float)dt_ms * cycles_per_ms;
    while (s_phase >= 1.0f) {
        s_phase -= 1.0f;
    }

    apply_params(now_ms);
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

    /* A new explicit expression cancels any pending blink/look/standby motion,
     * ends a spontaneous mood and resets the drowsiness: this is activity. */
    s_blink_until_ms = 0;
    s_look_until_ms = 0;
    s_look_dx_target = 0.0f;
    s_motion_active = false;
    s_wake_active = false;
    s_antic_active = false;
    s_drowsy = 0.0f;
    /* 1.2 layers: a new expression takes the eyes, so drop any perk and lid
     * settle (a perk mid-expression would widen an ERROR face) and put both lids
     * back to neutral. The gaze wander eases to zero by itself via its gate. */
    s_perk_from_ms = 0;
    s_settle_from_ms = 0;
    s_eye_lid[0] = 1.0f;
    s_eye_lid[1] = 1.0f;
    const uint32_t now = now_ms32();
    s_next_perk_ms = 0;
    schedule_next_blink(now);
    schedule_next_look(now);
    schedule_next_saccade(now);
    schedule_next_antic(now);
    schedule_next_dilate(now);

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

    /* Something real is happening: drop the mood and the drowsiness so the
     * expression is not shown through half-closed, tired eyes. The 1.2 layers go
     * with them - a perk or a lid settle must not deform an expression. */
    s_antic_active = false;
    s_drowsy = 0.0f;
    s_perk_from_ms = 0;
    s_settle_from_ms = 0;
    s_eye_lid[0] = 1.0f;
    s_eye_lid[1] = 1.0f;

    (void)sys_event_post_to(IDLE_FACE_EVENT, IDLE_FACE_EVENT_STATE_CHANGED,
                            &state, sizeof(state));
    return ESP_OK;
}

/**
 * Fall asleep: only from a neutral face, and never on top of an active
 * notification expression. The eyes ease shut over IDLE_FACE_SLEEP_MS and stay
 * half closed (the SLEEPY pose) for as long as the standby screen is up.
 *
 * Called by the idle page's on_show, which happens on the first creation and on
 * every wake-to-standby cycle.
 */
esp_err_t idle_face_standby_enter(void)
{
    /* Only from a neutral face: a live notification expression (WiFi failed,
     * syncing, AI thinking) must not be buried under the sleep pose. An expression
     * that has already timed out does not count - see override_expire_due(). */
    override_expire_due(now_ms32());
    if (s_state != IDLE_FACE_NORMAL || s_override_active) {
        SYS_LOGD(TAG, "standby enter refused (state=%d override=%d)", (int)s_state,
                 (int)s_override_active);
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t now = now_ms32();
    s_override_active = false;
    /*
     * Stay AWAKE in standby.
     *
     * The first revision of this feature eased the eyes shut into the SLEEPY pose
     * and held them there for the whole standby period. That suppressed blinking,
     * glancing and saccades - i.e. it made the standby screen *less* animated,
     * while the request was to enrich it. So the face keeps its live NORMAL
     * parameters and all of the new motion keeps running.
     *
     * To get the sleeping face instead, change all three together:
     *   s_state = IDLE_FACE_SLEEPY;
     *   begin_motion(now, IDLE_FACE_SLEEP_MS, 1.0f, 0.0f);
     *   s_current = *params_for(IDLE_FACE_SLEEPY);
     * (s_current must be moved too, otherwise the cross-fade fights the envelope.)
     */
    s_wake_active = false;
    /* A constant 1.0 envelope: no visual effect, but it keeps the motion machinery
     * exercised (and keeps begin_motion() referenced). */
    begin_motion(now, IDLE_FACE_SLEEP_MS, 1.0f, 1.0f);

    /* Standby is the one place the long-term behaviour matters, so make sure the
     * playbook starts from scratch: alert, no leftover mood, first blink, glance
     * and mood scheduled from now. Drowsiness then builds up on its own over the
     * next couple of minutes (see the slow layers in idle_face_tick). */
    s_drowsy = 0.0f;
    s_antic_active = false;
    /* 1.2 layers restart from neutral as well, so the standby screen always opens
     * on the same clean face. */
    s_perk_from_ms = 0;
    s_settle_from_ms = 0;
    s_eye_lid[0] = 1.0f;
    s_eye_lid[1] = 1.0f;
    s_next_perk_ms = now + rand_between(IDLE_FACE_PERK_MIN_MS, IDLE_FACE_PERK_MAX_MS);
    s_wander_x = 0.0f;
    s_wander_y = 0.0f;
    schedule_next_blink(now);
    schedule_next_look(now);
    schedule_next_saccade(now);
    schedule_next_antic(now);
    schedule_next_dilate(now);

    (void)sys_event_post_to(IDLE_FACE_EVENT, IDLE_FACE_EVENT_STATE_CHANGED,
                            &s_state, sizeof(s_state));
    SYS_LOGD(TAG, "standby enter (face stays awake)");
    return ESP_OK;
}

/**
 * Wake up: the eyes widen briefly and settle back.
 *
 * Called by the idle page's on_hide, i.e. the moment the standby screen is left.
 * ui_core keeps the outgoing page visible for UI_PAGE_ANIM_OUT_MS (240 ms) while
 * it cross-fades to the next page, and that window is exactly what makes this
 * animation visible - the page used to be hidden in the same frame, which cut
 * the whole sequence off before a single frame of it could be rendered.
 *
 * The overshoot peak therefore sits at ~200 ms (see IDLE_FACE_WAKE_MS), i.e.
 * inside the visible window.
 */
esp_err_t idle_face_standby_exit(void)
{
    /* A notification/AI expression is on screen: it owns the face and must not be
     * replaced by a yawn. (Standby itself no longer uses the SLEEPY pose, so there
     * is no pose to check for - see idle_face_standby_enter().) A stale override
     * does not count here either. */
    override_expire_due(now_ms32());
    if (s_override_active) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t now = now_ms32();
    s_wake_from_ms = now;
    s_wake_active = true;
    s_motion_active = false;

    /* Waking up wipes out the drowsiness and the current mood: the user is back.
     * The 1.2 layers are cleared too so the wake overshoot is the only thing
     * moving the eyelids at that moment. */
    s_drowsy = 0.0f;
    s_antic_active = false;
    s_blink_until_ms = 0;
    s_look_until_ms = 0;
    s_look_dx_target = 0.0f;
    s_perk_from_ms = 0;
    s_settle_from_ms = 0;
    s_eye_lid[0] = 1.0f;
    s_eye_lid[1] = 1.0f;
    s_next_perk_ms = 0;

    s_state = IDLE_FACE_NORMAL;
    schedule_next_blink(now + IDLE_FACE_WAKE_MS);
    schedule_next_look(now);
    schedule_next_saccade(now);
    schedule_next_antic(now);
    schedule_next_dilate(now);

    (void)sys_event_post_to(IDLE_FACE_EVENT, IDLE_FACE_EVENT_STATE_CHANGED,
                            &s_state, sizeof(s_state));
    SYS_LOGD(TAG, "standby exit (eyes opening)");
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
    s_xfade.target = s_state;
    const uint32_t now = now_ms32();
    s_breath_period_ms = rand_between(IDLE_FACE_BREATH_MIN_MS, IDLE_FACE_BREATH_MAX_MS);
    /* Start the breath at a random point of its cycle. */
    s_breath_ms = rand_between(0, s_breath_period_ms);
    s_phase = (float)rand_between(0, 1000) / 1000.0f;
    schedule_next_blink(now);
    schedule_next_look(now);
    schedule_next_saccade(now);
    /* Seed the long-term clocks too, so the face is fully alive even before the
     * first standby_enter() (which re-seeds them anyway). */
    schedule_next_antic(now);
    schedule_next_dilate(now);
    apply_params(now);

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

/** Entering the standby screen: the face falls asleep. */
static esp_err_t idle_page_on_show(void)
{
    /* Not a neutral face (an expression is live): keep it displayed as is. */
    (void)idle_face_standby_enter();
    return ESP_OK;
}

/** Leaving the standby screen: the face wakes up. */
static esp_err_t idle_page_on_hide(void)
{
    return idle_face_standby_exit();
}

static const ui_page_t s_idle_page = {
    .id = UI_PAGE_IDLE_FACE,
    .name = "idle_face",
    .create = idle_page_create,
    .on_show = idle_page_on_show,
    .on_hide = idle_page_on_hide,
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
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        SYS_LOGE(TAG, "page registration failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Subscribe at boot, not at attach: the page is created lazily on the first
     * idle timeout, but the target expression must already be live by then. */
    static bool subscribed;
    if (!subscribed) {
        if (sys_notify_subscribe(notify_handler, NULL, NULL) == ESP_OK) {
            subscribed = true;
        } else {
            SYS_LOGW(TAG, "expression triggers unavailable");
        }
    }

    SYS_LOGI(TAG, "ready (%d states, %u triggers)", IDLE_FACE_MAX,
             (unsigned)(sizeof(s_triggers) / sizeof(s_triggers[0])));
    return ESP_OK;
}
