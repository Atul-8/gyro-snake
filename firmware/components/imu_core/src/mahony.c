/*
 * GYRO SNAKE — Mahony attitude filter, float implementation.
 *
 * Copyright (c) 2026 GYRO SNAKE contributors.
 * SPDX-License-Identifier: MIT
 *
 * Clean re-implementation of the code actually reached in
 *   MagicRobot_V0.1/embedded/src/core/attitude/mahony.c
 * (HAL_USE_FPU == 1 path). The algorithm is unchanged:
 *
 *   1. normalize the measured acceleration (return early below 1e-6)
 *   2. estimate gravity in body coordinates from the quaternion
 *   3. error = measured x estimated
 *   4. integrate the error with 2*ki
 *   5. gyro dps -> rad/s, plus 2*kp*error + integral
 *   6. first-order quaternion integration
 *   7. normalize the quaternion
 *
 * Angles are radians and use the ZYX convention of quat_to_euler():
 *   roll  = atan2(2(wx + yz), 1 - 2(x^2 + y^2))
 *   pitch = asin(2(wy - zx))
 *   yaw   = atan2(2(wz + xy), 1 - 2(y^2 + z^2))
 */
#include "imu_core.h"

#include <math.h>

#define MAHONY_PI_F 3.14159265358979323846f

static void mahony_set_identity(mahony_t *m)
{
    m->qw = 1.0f;
    m->qx = 0.0f;
    m->qy = 0.0f;
    m->qz = 0.0f;
}

void mahony_init(mahony_t *m, float kp, float ki, float dt)
{
    if (!m) return;

    mahony_set_identity(m);
    m->integral[0] = 0.0f;
    m->integral[1] = 0.0f;
    m->integral[2] = 0.0f;
    m->kp = kp;
    m->ki = ki;
    m->dt = dt;
}

void mahony_update(mahony_t *m, const imu_sample_t *s)
{
    if (!m || !s) return;

    const float dt    = m->dt;
    const float twoKp = 2.0f * m->kp;
    const float twoKi = 2.0f * m->ki;

    /* 1. Normalize the acceleration. A (near) free-fall sample carries no
     * gravity direction, so keep the previous attitude. */
    float ax = s->ax;
    float ay = s->ay;
    float az = s->az;
    const float n = sqrtf(ax * ax + ay * ay + az * az);
    if (n < 1e-6f) return;
    ax /= n;
    ay /= n;
    az /= n;

    /* 2. Estimated gravity direction in body coordinates. */
    const float q0 = m->qw;
    const float q1 = m->qx;
    const float q2 = m->qy;
    const float q3 = m->qz;
    const float vx = 2.0f * (q1 * q3 - q0 * q2);
    const float vy = 2.0f * (q0 * q1 + q2 * q3);
    const float vz = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3;

    /* 3. Error = measured accel x estimated gravity. */
    const float ex = ay * vz - az * vy;
    const float ey = az * vx - ax * vz;
    const float ez = ax * vy - ay * vx;

    /* 4. Error integral (zero when ki == 0). */
    m->integral[0] += twoKi * ex * dt;
    m->integral[1] += twoKi * ey * dt;
    m->integral[2] += twoKi * ez * dt;

    /* 5. Gyro dps -> rad/s, plus the PI correction. */
    const float d2r = MAHONY_PI_F / 180.0f;
    const float gx = s->gx * d2r + twoKp * ex + m->integral[0];
    const float gy = s->gy * d2r + twoKp * ey + m->integral[1];
    const float gz = s->gz * d2r + twoKp * ez + m->integral[2];

    /* 6. First-order quaternion integration. */
    const float hdt = 0.5f * dt;
    m->qw += hdt * (-q1 * gx - q2 * gy - q3 * gz);
    m->qx += hdt * ( q0 * gx + q2 * gz - q3 * gy);
    m->qy += hdt * ( q0 * gy - q1 * gz + q3 * gx);
    m->qz += hdt * ( q0 * gz + q1 * gy - q2 * gx);

    /* 7. Normalize; a degenerate quaternion falls back to identity. */
    const float qn = sqrtf(m->qw * m->qw + m->qx * m->qx +
                           m->qy * m->qy + m->qz * m->qz);
    if (qn < 1e-6f) {
        mahony_set_identity(m);
        return;
    }
    m->qw /= qn;
    m->qx /= qn;
    m->qy /= qn;
    m->qz /= qn;
}

void mahony_get_angles(const mahony_t *m, float *roll, float *pitch, float *yaw)
{
    if (!m) {
        if (roll) *roll = 0.0f;
        if (pitch) *pitch = 0.0f;
        if (yaw) *yaw = 0.0f;
        return;
    }

    const float w = m->qw;
    const float x = m->qx;
    const float y = m->qy;
    const float z = m->qz;

    const float sinr_cosp = 2.0f * (w * x + y * z);
    const float cosr_cosp = 1.0f - 2.0f * (x * x + y * y);
    const float r = atan2f(sinr_cosp, cosr_cosp);

    const float sinp = 2.0f * (w * y - z * x);
    float p;
    if (fabsf(sinp) >= 1.0f) p = copysignf(MAHONY_PI_F / 2.0f, sinp);
    else                     p = asinf(sinp);

    const float siny_cosp = 2.0f * (w * z + x * y);
    const float cosy_cosp = 1.0f - 2.0f * (y * y + z * z);
    const float yw = atan2f(siny_cosp, cosy_cosp);

    if (roll) *roll = r;
    if (pitch) *pitch = p;
    if (yaw) *yaw = yw;
}

void mahony_get_quat(const mahony_t *m, float *w, float *x, float *y, float *z)
{
    if (!m) {
        if (w) *w = 0.0f;
        if (x) *x = 0.0f;
        if (y) *y = 0.0f;
        if (z) *z = 0.0f;
        return;
    }

    if (w) *w = m->qw;
    if (x) *x = m->qx;
    if (y) *y = m->qy;
    if (z) *z = m->qz;
}