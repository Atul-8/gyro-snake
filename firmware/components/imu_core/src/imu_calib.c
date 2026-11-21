/*
 * GYRO SNAKE — static IMU calibration (mean gyro bias + gravity axis).
 *
 * Copyright (c) 2026 GYRO SNAKE contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reference code path (the code actually reached, not its comments):
 *   MagicRobot_V0.1/embedded/src/core/calibration/imu_calib.c
 * called from app_main() -> app_imu.c -> poll_calib() in app_stream.c.
 *
 * Deliberate differences from that reference:
 *   - imu_calib_result() reports the mean RAW accelerometer vector and never
 *     subtracts 1g on the gravity axis. Gravity must stay in the sample that
 *     feeds Mahony; only the gyro bias is applied there (see imu_core.h).
 *   - The accelerometer mean is accumulated from `raw`, while the gyro mean
 *     is accumulated from `s`. imu_calib_t has a single accelerometer
 *     accumulator set and its documented output is the mean raw accel, so
 *     this keeps gravity intact even when the caller feeds a bias-corrected
 *     `s` (the residual/accumulate scheme used for the gyro).
 */
#include "imu_core.h"

#include <math.h>

/* target_n <= 0 is not a usable window; treat it as one sample. */
static uint32_t calib_target_n(const imu_calib_t *c)
{
    return (c->target_n > 0) ? (uint32_t)c->target_n : 1u;
}

void imu_calib_reset(imu_calib_t *c, int target_n)
{
    if (!c) return;

    c->gx_sum = 0.0;
    c->gy_sum = 0.0;
    c->gz_sum = 0.0;
    c->ax_sum = 0.0;
    c->ay_sum = 0.0;
    c->az_sum = 0.0;
    c->n = 0;
    c->target_n = target_n;
    c->state = IMU_CALIB_IDLE;
    c->grav_axis = -1;
    c->grav_sign = 1;
}

void imu_calib_start(imu_calib_t *c)
{
    if (!c) return;
    if (c->state == IMU_CALIB_COLLECTING) return;   /* idempotent */

    c->gx_sum = 0.0;
    c->gy_sum = 0.0;
    c->gz_sum = 0.0;
    c->ax_sum = 0.0;
    c->ay_sum = 0.0;
    c->az_sum = 0.0;
    c->n = 0;
    c->state = IMU_CALIB_COLLECTING;
    c->grav_axis = -1;
    c->grav_sign = 1;
}

int imu_calib_feed(imu_calib_t *c, const imu_sample_t *s, const imu_sample_t *raw)
{
    if (!c || !s) return 0;
    if (c->state != IMU_CALIB_COLLECTING) return 0;

    c->gx_sum += (double)s->gx;
    c->gy_sum += (double)s->gy;
    c->gz_sum += (double)s->gz;

    /* Gravity axis and the reported accel mean come from the raw sample. */
    const imu_sample_t *r = raw ? raw : s;
    c->ax_sum += (double)r->ax;
    c->ay_sum += (double)r->ay;
    c->az_sum += (double)r->az;

    c->n++;
    if (c->n < calib_target_n(c)) return 0;

    c->state = IMU_CALIB_IDLE;

    const double inv = 1.0 / (double)c->n;
    const double a[3] = {
        c->ax_sum * inv,
        c->ay_sum * inv,
        c->az_sum * inv,
    };
    const double aa[3] = { fabs(a[0]), fabs(a[1]), fabs(a[2]) };

    int axis = 0;
    if (aa[1] > aa[0]) axis = 1;
    if (aa[2] > aa[axis]) axis = 2;

    c->grav_axis = axis;
    /* Exact zero is treated as +1 (the reset default); real rest data never
     * hits it. The sign follows the raw mean, not the gyro-corrected one. */
    c->grav_sign = (a[axis] >= 0.0) ? 1 : -1;
    return 1;
}

bool imu_calib_ready(const imu_calib_t *c)
{
    return c && c->grav_axis >= 0 && c->n >= calib_target_n(c);
}

void imu_calib_result(const imu_calib_t *c, imu_bias_t *bias)
{
    if (!bias) return;

    if (!imu_calib_ready(c)) {
        bias->gx = 0.0f;
        bias->gy = 0.0f;
        bias->gz = 0.0f;
        bias->ax = 0.0f;
        bias->ay = 0.0f;
        bias->az = 0.0f;
        bias->valid = false;
        bias->grav_axis = 0;
        bias->grav_sign = 0;
        return;
    }

    const double inv = 1.0 / (double)c->n;
    bias->gx = (float)(c->gx_sum * inv);
    bias->gy = (float)(c->gy_sum * inv);
    bias->gz = (float)(c->gz_sum * inv);
    bias->ax = (float)(c->ax_sum * inv);
    bias->ay = (float)(c->ay_sum * inv);
    bias->az = (float)(c->az_sum * inv);

    /* The caller owns validity: it sets this true after a successful
     * calibration (and only then is the bias persisted / applied). */
    bias->valid = false;
    bias->grav_axis = (int8_t)c->grav_axis;
    bias->grav_sign = (int8_t)c->grav_sign;
}

void imu_apply_gyro_bias(imu_sample_t *s, const imu_bias_t *b)
{
    if (!s || !b) return;

    s->gx -= b->gx;
    s->gy -= b->gy;
    s->gz -= b->gz;
}

void imu_apply_all_bias(imu_sample_t *s, const imu_bias_t *b)
{
    if (!s || !b) return;

    s->gx -= b->gx;
    s->gy -= b->gy;
    s->gz -= b->gz;
    s->ax -= b->ax;
    s->ay -= b->ay;
    s->az -= b->az;
}