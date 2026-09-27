/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file idle_face.h
 * @brief FR6: the black-screen white-line "eyes" idle animation.
 *
 * Everything is drawn with LVGL primitives - no bitmaps and no SD assets - per
 * requirement 11.1 ("程序化绘制优先") and 11.5 (P0/P1 must not need frame
 * material). Every state shares one set of objects whose properties are tweened,
 * so switching expressions never allocates (requirement 11.3).
 *
 * Animation model:
 *   - switching between two expressions cross-fades from the *current* visual
 *     state to the new table row through an ease curve (never a snap);
 *   - blinking, glancing, pupil drift and breathing are each randomised every
 *     cycle, so nothing repeats on a fixed period;
 *   - on top of those, three slower layers keep a long standby session alive:
 *     spontaneous moods (a brief random expression every 13-34 s), pupil
 *     dilation, whole-face drift, and a drowsiness value that builds up while
 *     nobody interacts and is wiped out by any real activity;
 *   - four continuous/reflexive layers (v1.2.0) remove the "posing between
 *     events" feel: a slow 2-D gaze wander so the pupils are never perfectly
 *     still, a lid settle that springs past the resting height after a blink,
 *     an occasional attention perk (eyes widen, pupils constrict - no expression
 *     change), and asymmetric blinks where the two lids differ;
 *   - entering standby keeps the live NORMAL face (see
 *     idle_face_standby_enter()) and leaving it widens the eyes briefly
 *     (idle_face_standby_exit(), driven by the idle page's on_show/on_hide).
 *
 * State definitions live in exactly three places and must stay in sync:
 *   - idle_face_state_t below,
 *   - the effect table in idle_face.c,
 *   - docs/TODOHELPER_REQUIREMENTS.md FR6 / 11.2.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#include "lvgl.h"

#include "sys_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(IDLE_FACE_EVENT);

typedef enum {
    IDLE_FACE_EVENT_STATE_CHANGED = 0, /**< payload: idle_face_state_t */
} idle_face_event_id_t;

typedef enum {
    IDLE_FACE_NORMAL = 0,   /**< default: oval eyes, randomised breathing */
    IDLE_FACE_BLINK,        /**< eyes close and reopen (random 2.6-7.5 s) */
    IDLE_FACE_LOOK_LEFT,    /**< pupils move left (kept for the API/trigger table) */
    IDLE_FACE_LOOK_RIGHT,   /**< pupils move right (kept for the API/trigger table) */
    IDLE_FACE_HAPPY,        /**< arcs: todo done, WiFi connected */
    IDLE_FACE_SLEEPY,       /**< half closed: night, long inactivity, *standby pose* */
    IDLE_FACE_ERROR,        /**< crossed eyes: WiFi / sync failure */
    IDLE_FACE_SYNCING,      /**< pupils circling: sync in progress */
    IDLE_FACE_THINKING,     /**< one pupil looks up: network request */
    IDLE_FACE_LISTENING,    /**< eyes enlarged: voice wake, AI recording (P2) */
    IDLE_FACE_SPEAKING,     /**< rhythmic arcs: AI is talking (P2) */
    IDLE_FACE_MAX,
} idle_face_state_t;

/**
 * @brief Standby entry: hand the face over to the standby screen.
 *
 * Called by the idle page's @c on_show, so it runs every time the standby screen
 * is (re-)entered. It deliberately does **not** ease the eyes shut: the standby
 * screen is the one place where the face is on display, and freezing it in a
 * SLEEPY pose suppressed blinking, glancing, saccades and everything else that
 * makes it look alive. Instead the face stays on its live NORMAL parameters,
 * restarts its playbook (blink/glance/mood/dilation schedules) and then winds
 * down on its own through the drowsiness layer.
 *
 * Refuses to run (and returns @c ESP_ERR_INVALID_STATE) when a notification or
 * AI expression is live; the caller does not need to check anything.
 */
esp_err_t idle_face_standby_enter(void);

/**
 * @brief Standby exit: a brief wide-eyed "I am back" before the screen goes away.
 *
 * Called by the idle page's @c on_hide, i.e. when the screen is woken. Also wipes
 * the drowsiness and any spontaneous mood, because the user is back.
 *
 * @note Since v1.2.0 the page switch is an atomic same-frame swap (the cross-fade
 *       was removed - it caused the previous page to ghost, see AGENT.md
 *       9.0.1.1), so this overshoot is **no longer visible on screen**: the idle
 *       page is gone in the same frame. The envelope is kept because it is
 *       harmless and correct, and because a future host may render the face
 *       somewhere it can be seen (for example as an overlay on lv_layer_top()).
 *       Returns @c ESP_ERR_INVALID_STATE when an expression owns the face (which
 *       must not be replaced by a yawn).
 */
esp_err_t idle_face_standby_exit(void);

/**
 * @brief Component entry point: register the idle page and start the animator.
 *
 * Called from the app module table. The page itself is registered with ui_core
 * so this component owns UI_PAGE_IDLE_FACE (frontend layering note in
 * docs/TODOHELPER_FRAMEWORK.md 2.1).
 */
esp_err_t idle_face_init(void);

/**
 * @brief Build the eyes inside @p parent and start drawing.
 *
 * Idempotent. Called by the idle page when it is created; exposed so the face
 * can also be embedded in another page (for example a compact AI chat header).
 */
esp_err_t idle_face_attach(lv_obj_t *parent);

/** @brief Switch expression. Safe to call from any task. */
esp_err_t idle_face_set_state(idle_face_state_t state);

/** @brief Current expression. */
idle_face_state_t idle_face_get_state(void);

/**
 * @brief Show @p state for @p duration_ms, then return to the previous one.
 *
 * This is how other components make the face react ("WiFi failed", "syncing")
 * without idle_face needing to know about them: they call this, or publish an
 * L0 notification that the trigger table maps to a state.
 */
esp_err_t idle_face_express(idle_face_state_t state, uint32_t duration_ms);

/**
 * @brief Advance the animation. Called by the internal 33 ms timer; exposed so
 *        a host that drives its own loop can tick it manually.
 *
 * @param now_ms Monotonic milliseconds.
 */
esp_err_t idle_face_tick(uint32_t now_ms);

/**
 * @brief Dim or restore the panel.
 *
 * ui_core owns *when* this happens (idle policy); the face only forwards it.
 */
esp_err_t idle_face_set_brightness(uint8_t percent);

/** @brief True once the eyes have been built. */
bool idle_face_is_attached(void);

/**
 * @brief The idle page descriptor as an opaque pointer, so a host can compose
 *        it without including ui_core's page internals.
 */
extern const void *idle_face_page_descriptor(void);

#ifdef __cplusplus
}
#endif
