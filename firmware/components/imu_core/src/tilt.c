/*
 * GYRO SNAKE — tilt angles to four steering directions.
 *
 * Copyright (c) 2026 GYRO SNAKE contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pipeline:
 *   effective_roll  = roll  - neutral_roll
 *   effective_pitch = pitch - neutral_pitch
 *   [swap_xy] [invert_roll] [invert_pitch]
 *   direction from the dominant axis:
 *       +roll  -> RIGHT        -roll  -> LEFT
 *       +pitch -> UP           -pitch -> DOWN
 *
 * The pitch sign above is this module's convention ("nose up" = UP). A board
 * whose axes disagree can flip it with invert_pitch; no other interpretation
 * is baked in. This module deliberately does not know about the current snake
 * direction: the game core ignores 180-degree reversals, tilt only requests.
 *
 * Hysteresis:
 *   - |dominant angle| < deadzone_deg: no direction (NONE).
 *   - the same direction must persist for stable_samples consecutive calls.
 *   - after a direction is accepted it stays locked; a different direction is
 *     ignored until the device returns below rearm_deg (re-arm).
 *   - while locked, the locked direction keeps being returned so a consumer
 *     that samples slower than the IMU (e-paper frame rate) cannot miss it.
 */
#include "imu_core.h"

#include <math.h>

const char *imu_dir_name(int8_t dir)
{
    switch (dir) {
    case IMU_DIR_UP:    return "UP";
    case IMU_DIR_RIGHT: return "RIGHT";
    case IMU_DIR_DOWN:  return "DOWN";
    case IMU_DIR_LEFT:  return "LEFT";
    default:            return "NONE";
    }
}

static void tilt_clear_dynamic(tilt_t *t)
{
    t->candidate = -1;
    t->count = 0;
    t->locked = -1;
}

void tilt_init(tilt_t *t, float deadzone_deg, float rearm_deg, int stable_samples)
{
    if (!t) return;

    t->deadzone_deg = deadzone_deg;
    t->rearm_deg = rearm_deg;
    t->stable_samples = stable_samples;
    t->neutral_roll_deg = 0.0f;
    t->neutral_pitch_deg = 0.0f;
    t->swap_xy = false;
    t->invert_roll = false;
    t->invert_pitch = false;
    tilt_clear_dynamic(t);
}

void tilt_set_neutral(tilt_t *t, float roll_deg, float pitch_deg)
{
    if (!t) return;

    t->neutral_roll_deg = roll_deg;
    t->neutral_pitch_deg = pitch_deg;
    /* A new zero pose invalidates any pending/held direction. */
    tilt_clear_dynamic(t);
}

void tilt_set_axis(tilt_t *t, bool swap_xy, bool invert_roll, bool invert_pitch)
{
    if (!t) return;

    t->swap_xy = swap_xy;
    t->invert_roll = invert_roll;
    t->invert_pitch = invert_pitch;
    /* A mapping change invalidates any pending/held direction. */
    tilt_clear_dynamic(t);
}

void tilt_reset(tilt_t *t)
{
    if (!t) return;
    tilt_clear_dynamic(t);
}

int8_t tilt_update(tilt_t *t, float roll_deg, float pitch_deg)
{
    if (!t) return IMU_DIR_NONE;

    float dr = roll_deg - t->neutral_roll_deg;
    float dp = pitch_deg - t->neutral_pitch_deg;

    if (t->swap_xy) {
        const float tmp = dr;
        dr = dp;
        dp = tmp;
    }
    if (t->invert_roll) dr = -dr;
    if (t->invert_pitch) dp = -dp;

    const float ar = fabsf(dr);
    const float ap = fabsf(dp);
    const float mag = (ar > ap) ? ar : ap;

    /* Back near the neutral pose: unlock for the next turn. */
    if (mag < t->rearm_deg) {
        tilt_clear_dynamic(t);
        return IMU_DIR_NONE;
    }

    /* Inside the deadzone but not re-armed: no direction, and the streak of
     * consecutive samples is broken. */
    if (mag < t->deadzone_deg) {
        t->candidate = -1;
        t->count = 0;
        return IMU_DIR_NONE;
    }

    int8_t dir;
    if (ar >= ap) {
        dir = (dr > 0.0f) ? IMU_DIR_RIGHT : IMU_DIR_LEFT;
    } else {
        dir = (dp > 0.0f) ? IMU_DIR_UP : IMU_DIR_DOWN;
    }

    if (t->locked >= 0) {
        /* Only the already accepted direction is reported; a reversal (or
         * any other turn) must wait for a re-arm. */
        return (dir == t->locked) ? (int8_t)t->locked : IMU_DIR_NONE;
    }

    const int need = (t->stable_samples > 0) ? t->stable_samples : 1;

    if (dir == t->candidate) {
        if (t->count < need) t->count++;
    } else {
        t->candidate = dir;
        t->count = 1;
    }

    if (t->count >= need) {
        t->locked = dir;
        return dir;
    }
    return IMU_DIR_NONE;
}