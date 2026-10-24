/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file word_ssp.h
 * @brief SSP-MMC scheduling primitives, internal to word_service (v1.15.0).
 *
 * The memory state per word is (difficulty, halflife_days). halflife_days == 0
 * means "never reviewed with this model"; the first review seeds it from the
 * difficulty alone. All formulas and constants come from maimemo/SSP-MMC
 * (MIT, KDD 2022); see word_ssp.c.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Difficulty range of the SSP-MMC model; lapses add 2, capped at the max. */
#define WORD_SSP_D_MIN     1
#define WORD_SSP_D_MAX     18
/** Difficulty a word starts at (SSP-MMC grades words 1..10; the middle is the
 *  honest choice when the book carries no difficulty of its own). */
#define WORD_SSP_D_DEFAULT 5

/**
 * @brief Apply one review to the memory state.
 *
 * @param difficulty     In/out, clamped to [1, 18].
 * @param halflife_days  In/out, days; 0 on the first review.
 * @param known          true for 认识, false for 模糊/不认识 (SSP-MMC's own
 *                       three-way mapping folds the middle answer onto a lapse,
 *                       responses_dict '2' -> 0 - this follows their data).
 * @param elapsed_days   Days since the previous review (ignored when 0).
 */
void word_ssp_review(uint8_t *difficulty, float *halflife_days,
                     bool known, float elapsed_days);

/** @brief Optimal next review interval for the state, in whole days (>= 1). */
int32_t word_ssp_next_interval_days(uint8_t difficulty, float halflife_days);

#ifdef __cplusplus
}
#endif
