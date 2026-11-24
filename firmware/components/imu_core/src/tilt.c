/*
 * GYRO SNAKE — tilt angles to four steering directions.
 *
 * Copyright (c) 2026 GYRO SNAKE contributors.
 * SPDX-License-Identifier: MIT
 *
 * Pipeline:
 *   effective_roll  = roll  - neutral_roll
 *   effective_pitch = pitch - neutral_pitch
 *   [swap_xy] [invert_roll] [invert_pitch]
 *   sector of the tilt vector -> direction (real-time following)
 *
 * The pitch sign above is this module's convention ("nose up" = UP). A board
 * whose axes disagree can flip it with invert_pitch; no other interpretation
 * is baked in.
 *
 * Direction model (2026-11-23 rework, per on-hardware feedback):
 *   - the tilt vector (dr, dp) owns four +-45 deg sectors around up / right /
 *     down / left; anything within the sector steers that way, so a diagonal
 *     tilt follows its dominant side;
 *   - the direction follows the sensor continuously: moving from one sector
 *     into another switches without returning to center;
 *   - |vector| < deadzone_deg: no direction;
 *   - |vector| < rearm_deg: releases the held direction;
 *   - the held sector is widened by rearm_deg so jitter at a 45 deg boundary
 *     does not flicker;
 *   - a new sector must persist for stable_samples calls before the held
 *     direction switches; the previous direction stays reported meanwhile.
 *
 * This module deliberately does not know about the current snake direction:
 * the game core ignores 180-degree reversals, tilt only requests.
 */
#include "imu_core.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TILT_RAD_TO_DEG (180.0f / (float)M_PI)

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

/* Plain 45-degree sectors: 0 deg = UP, +90 = RIGHT, +-180 = DOWN, -90 = LEFT. */
static int8_t tilt_sector_plain(float ang)
{
    if (ang >= -45.0f && ang < 45.0f) return IMU_DIR_UP;
    if (ang >= 45.0f && ang < 135.0f) return IMU_DIR_RIGHT;
    if (ang >= 135.0f || ang < -135.0f) return IMU_DIR_DOWN;
    return IMU_DIR_LEFT;
}

/* Sector of the tilt vector. A held direction keeps its sector widened by
 * `margin` degrees on both sides (boundary hysteresis). */
static int8_t tilt_sector(float ang, float margin, int8_t held)
{
    if (held >= 0) {
        const float m = margin;
        switch (held) {
        case IMU_DIR_UP:
            if (ang >= -(45.0f + m) && ang < (45.0f + m)) return IMU_DIR_UP;
            break;
        case IMU_DIR_RIGHT:
            if (ang >= (45.0f - m) && ang < (135.0f + m)) return IMU_DIR_RIGHT;
            break;
        case IMU_DIR_DOWN:
            if (ang >= (135.0f - m) || ang < -(135.0f - m)) return IMU_DIR_DOWN;
            break;
        case IMU_DIR_LEFT:
            if (ang >= -(135.0f + m) && ang < -(45.0f - m)) return IMU_DIR_LEFT;
            break;
        default:
            break;
        }
    }
    return tilt_sector_plain(ang);
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

    const float mag = sqrtf(dr * dr + dp * dp);

    /* Fully released: below the unlock threshold everything resets. */
    if (mag < t->rearm_deg) {
        tilt_clear_dynamic(t);
        return IMU_DIR_NONE;
    }

    /* Hush zone: no direction is reported, but the held direction survives so
     * a brief dip does not force a fresh stable-sample run afterwards. */
    if (mag < t->deadzone_deg) {
        t->candidate = -1;
        t->count = 0;
        return IMU_DIR_NONE;
    }

    const float ang = atan2f(dr, dp) * TILT_RAD_TO_DEG;
    const int8_t dir = tilt_sector(ang, t->rearm_deg, (int8_t)t->locked);
    const int need = (t->stable_samples > 0) ? t->stable_samples : 1;

    /* Same sector as held: real-time, report at once. */
    if (dir == (int8_t)t->locked) {
        t->candidate = -1;
        t->count = 0;
        return (int8_t)t->locked;
    }

    /* New sector: debounce it, but keep the held direction reported so the
     * output never stutters while the hand moves. */
    if (dir == (int8_t)t->candidate) {
        if (t->count < need) t->count++;
    } else {
        t->candidate = dir;
        t->count = 1;
    }
    if (t->count >= need) {
        t->locked = dir;
        t->candidate = -1;
        t->count = 0;
        return dir;
    }
    return (t->locked >= 0) ? (int8_t)t->locked : IMU_DIR_NONE;
}