/*
 * GYRO SNAKE — host tests for the portable IMU core.
 *
 * Copyright (c) 2026 GYRO SNAKE contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Build with gcc (C99, -lm):
 *   gcc -std=c99 -O2 -Wall -Wextra \
 *       -I gyro-snake/firmware/components/imu_core/include \
 *       gyro-snake/firmware/components/imu_core/src/imu_calib.c \
 *       gyro-snake/firmware/components/imu_core/src/mahony.c \
 *       gyro-snake/firmware/components/imu_core/src/tilt.c \
 *       gyro-snake/tests/test_imu.c -lm -o /tmp/test_imu
 */
#include "imu_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_passed;
static int g_failed;

static int near_f(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) {                                                        \
            g_passed++;                                                    \
        } else {                                                           \
            g_failed++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                              \
    CHECK(near_f((float)(a), (float)(b), (float)(tol)))

/* Three consecutive samples of the same tilt must yield `want`. */
#define CHECK_DIR3(t, roll, pitch, want)                                   \
    do {                                                                   \
        CHECK(tilt_update((t), (roll), (pitch)) == IMU_DIR_NONE);          \
        CHECK(tilt_update((t), (roll), (pitch)) == IMU_DIR_NONE);          \
        CHECK(tilt_update((t), (roll), (pitch)) == (want));                \
    } while (0)

static imu_sample_t sample(float ax, float ay, float az,
                           float gx, float gy, float gz)
{
    imu_sample_t s;
    s.ax = ax;
    s.ay = ay;
    s.az = az;
    s.gx = gx;
    s.gy = gy;
    s.gz = gz;
    return s;
}

static int feed_n(imu_calib_t *c, const imu_sample_t *s,
                  const imu_sample_t *raw, int n)
{
    int completions = 0;
    for (int i = 0; i < n; ++i) {
        completions += imu_calib_feed(c, s, raw);
    }
    return completions;
}

/* -------------------------------------------------------------------------
 * Calibration: 300 samples of a known gyro bias; accel mean keeps gravity.
 * ---------------------------------------------------------------------- */
static void test_calib_gyro_and_accel(void)
{
    imu_calib_t cal;
    imu_bias_t b = {0};
    const imu_sample_t rest = sample(0.0f, 0.0f, 1.0f,
                                     1.5f, -0.5f, 0.25f);

    imu_calib_reset(&cal, 300);
    CHECK(cal.state == IMU_CALIB_IDLE);
    CHECK(cal.grav_axis == -1);
    CHECK(cal.n == 0u);
    CHECK(!imu_calib_ready(&cal));

    imu_calib_start(&cal);
    CHECK(cal.state == IMU_CALIB_COLLECTING);
    CHECK(cal.grav_axis == -1);

    CHECK(feed_n(&cal, &rest, &rest, 299) == 0);
    CHECK(!imu_calib_ready(&cal));
    CHECK(feed_n(&cal, &rest, &rest, 1) == 1);
    CHECK(imu_calib_ready(&cal));
    CHECK(cal.state == IMU_CALIB_IDLE);
    CHECK(cal.n == 300u);

    imu_calib_result(&cal, &b);
    CHECK_NEAR(b.gx, 1.5f, 1e-3f);
    CHECK_NEAR(b.gy, -0.5f, 1e-3f);
    CHECK_NEAR(b.gz, 0.25f, 1e-3f);

    /* Gravity axis from the raw mean: +Z. */
    CHECK(b.grav_axis == 2);
    CHECK(b.grav_sign == 1);

    /* The accel mean keeps gravity: az stays ~1, it is NOT reduced to 0. */
    CHECK_NEAR(b.ax, 0.0f, 1e-6f);
    CHECK_NEAR(b.ay, 0.0f, 1e-6f);
    CHECK_NEAR(b.az, 1.0f, 1e-6f);

    /* Caller owns validity. */
    CHECK(!b.valid);

    /* Runtime path: only the gyro is corrected; accel reaches Mahony intact. */
    imu_sample_t live = rest;
    imu_apply_gyro_bias(&live, &b);
    CHECK_NEAR(live.gx, 0.0f, 1e-6f);
    CHECK_NEAR(live.gy, 0.0f, 1e-6f);
    CHECK_NEAR(live.gz, 0.0f, 1e-6f);
    CHECK_NEAR(live.ax, 0.0f, 1e-6f);
    CHECK_NEAR(live.ay, 0.0f, 1e-6f);
    CHECK_NEAR(live.az, 1.0f, 1e-6f);

    /* Diagnostic path: this one does remove gravity, so it must not be used
     * for the sample that feeds Mahony. */
    imu_sample_t diag = rest;
    imu_apply_all_bias(&diag, &b);
    CHECK_NEAR(diag.gx, 0.0f, 1e-6f);
    CHECK_NEAR(diag.ax, 0.0f, 1e-6f);
    CHECK_NEAR(diag.ay, 0.0f, 1e-6f);
    CHECK_NEAR(diag.az, 0.0f, 1e-6f);
}

/* -------------------------------------------------------------------------
 * Gravity axis: |raw mean| is largest on Z then on -X.
 * ---------------------------------------------------------------------- */
static void test_gravity_axis(void)
{
    imu_calib_t cal;
    imu_bias_t b = {0};
    const imu_sample_t z_up = sample(0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
    const imu_sample_t x_neg = sample(-1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    const imu_sample_t y_neg = sample(0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f);

    imu_calib_reset(&cal, 100);
    imu_calib_start(&cal);
    CHECK(feed_n(&cal, &z_up, &z_up, 100) == 1);
    imu_calib_result(&cal, &b);
    CHECK(b.grav_axis == 2);
    CHECK(b.grav_sign == 1);
    CHECK_NEAR(b.az, 1.0f, 1e-6f);

    imu_calib_reset(&cal, 100);
    imu_calib_start(&cal);
    CHECK(feed_n(&cal, &x_neg, &x_neg, 100) == 1);
    imu_calib_result(&cal, &b);
    CHECK(b.grav_axis == 0);
    CHECK(b.grav_sign == -1);
    CHECK_NEAR(b.ax, -1.0f, 1e-6f);

    imu_calib_reset(&cal, 100);
    imu_calib_start(&cal);
    CHECK(feed_n(&cal, &y_neg, &y_neg, 100) == 1);
    imu_calib_result(&cal, &b);
    CHECK(b.grav_axis == 1);
    CHECK(b.grav_sign == -1);
}

/* -------------------------------------------------------------------------
 * `s` drives the gyro mean (residual scheme), `raw` drives the accel mean
 * and the gravity axis. This is the separation the runtime needs.
 * ---------------------------------------------------------------------- */
static void test_calib_raw_accel_and_residual_gyro(void)
{
    imu_calib_t cal;
    imu_bias_t b = {0};
    const imu_sample_t s   = sample(0.0f, 0.0f, 0.0f,
                                    1.0f, -2.0f, 0.5f);
    const imu_sample_t raw = sample(0.0f, 0.0f, 1.0f,
                                    1.0f, -2.0f, 0.5f);

    imu_calib_reset(&cal, 50);
    imu_calib_start(&cal);
    CHECK(feed_n(&cal, &s, &raw, 50) == 1);

    imu_calib_result(&cal, &b);
    CHECK_NEAR(b.gx, 1.0f, 1e-6f);
    CHECK_NEAR(b.gy, -2.0f, 1e-6f);
    CHECK_NEAR(b.gz, 0.5f, 1e-6f);

    /* The accel mean comes from raw, so gravity survives even though the
     * corrected sample `s` had it removed. */
    CHECK_NEAR(b.ax, 0.0f, 1e-6f);
    CHECK_NEAR(b.ay, 0.0f, 1e-6f);
    CHECK_NEAR(b.az, 1.0f, 1e-6f);
    CHECK(b.grav_axis == 2);
    CHECK(b.grav_sign == 1);
}

/* -------------------------------------------------------------------------
 * State machine: idle feed, idempotent start, incomplete result is all zero.
 * ---------------------------------------------------------------------- */
static void test_calib_state_and_zero_result(void)
{
    imu_calib_t cal;
    imu_bias_t b = {0};
    const imu_sample_t s = sample(0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);

    imu_calib_reset(&cal, 10);
    CHECK(cal.state == IMU_CALIB_IDLE);
    CHECK(imu_calib_feed(&cal, &s, &s) == 0);   /* not collecting */
    CHECK(cal.n == 0u);

    imu_calib_result(&cal, &b);
    CHECK(!b.valid);
    CHECK(b.gx == 0.0f && b.gy == 0.0f && b.gz == 0.0f);
    CHECK(b.ax == 0.0f && b.ay == 0.0f && b.az == 0.0f);
    CHECK(b.grav_axis == 0 && b.grav_sign == 0);

    imu_calib_start(&cal);
    CHECK(cal.state == IMU_CALIB_COLLECTING);
    CHECK(feed_n(&cal, &s, &s, 3) == 0);
    CHECK(cal.n == 3u);

    /* start() while collecting is idempotent: it must not restart the sums. */
    imu_calib_start(&cal);
    CHECK(cal.state == IMU_CALIB_COLLECTING);
    CHECK(cal.n == 3u);
    CHECK(feed_n(&cal, &s, &s, 7) == 1);
    CHECK(cal.state == IMU_CALIB_IDLE);
    CHECK(imu_calib_ready(&cal));

    /* A new window invalidates the previous result immediately. */
    imu_calib_start(&cal);
    CHECK(cal.state == IMU_CALIB_COLLECTING);
    CHECK(cal.n == 0u);
    CHECK(cal.grav_axis == -1);
    CHECK(!imu_calib_ready(&cal));

    imu_calib_result(&cal, &b);
    CHECK(!b.valid);
    CHECK(b.gx == 0.0f && b.gy == 0.0f && b.gz == 0.0f);
    CHECK(b.ax == 0.0f && b.ay == 0.0f && b.az == 0.0f);
    CHECK(b.grav_axis == 0 && b.grav_sign == 0);
}

/* -------------------------------------------------------------------------
 * Mahony: level and stationary -> zero roll/pitch, identity quaternion.
 * ---------------------------------------------------------------------- */
static void test_mahony_static(void)
{
    mahony_t m;
    const imu_sample_t level = sample(0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
    float roll = 0.0f, pitch = 0.0f, yaw = 0.0f;
    float w = 0.0f, x = 0.0f, y = 0.0f, z = 0.0f;

    mahony_init(&m, 2.0f, 0.0f, 0.01f);
    CHECK(m.kp == 2.0f);
    CHECK(m.ki == 0.0f);
    CHECK(m.dt == 0.01f);
    CHECK(m.qw == 1.0f && m.qx == 0.0f && m.qy == 0.0f && m.qz == 0.0f);
    CHECK(m.integral[0] == 0.0f && m.integral[1] == 0.0f &&
          m.integral[2] == 0.0f);

    for (int i = 0; i < 200; ++i) {
        mahony_update(&m, &level);
    }

    mahony_get_angles(&m, &roll, &pitch, &yaw);
    CHECK_NEAR(roll, 0.0f, 1e-6f);
    CHECK_NEAR(pitch, 0.0f, 1e-6f);
    CHECK_NEAR(yaw, 0.0f, 1e-6f);

    mahony_get_quat(&m, &w, &x, &y, &z);
    CHECK_NEAR(w, 1.0f, 1e-6f);
    CHECK_NEAR(x, 0.0f, 1e-6f);
    CHECK_NEAR(y, 0.0f, 1e-6f);
    CHECK_NEAR(z, 0.0f, 1e-6f);

    /* A zero acceleration sample must be ignored (free fall). */
    const imu_sample_t invalid = sample(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    float w0 = 0.0f, x0 = 0.0f, y0 = 0.0f, z0 = 0.0f;
    mahony_get_quat(&m, &w0, &x0, &y0, &z0);
    mahony_update(&m, &invalid);
    CHECK(m.qw == w0 && m.qx == x0 && m.qy == y0 && m.qz == z0);
}

/* -------------------------------------------------------------------------
 * Mahony: acceleration +X converges to pitch -30 deg (ZYX sign convention).
 * ---------------------------------------------------------------------- */
static void test_mahony_accel_tilt(void)
{
    mahony_t m;
    const imu_sample_t tilted = sample(0.5f, 0.0f, 0.8660254f,
                                       0.0f, 0.0f, 0.0f);
    float roll = 0.0f, pitch = 0.0f, yaw = 0.0f;

    mahony_init(&m, 2.0f, 0.0f, 0.01f);
    for (int i = 0; i < 200; ++i) {
        mahony_update(&m, &tilted);
    }

    mahony_get_angles(&m, &roll, &pitch, &yaw);
    CHECK_NEAR(roll, 0.0f, 1e-3f);
    CHECK(fabsf(pitch) > 0.1f);          /* non-zero */
    CHECK(pitch < 0.0f);                 /* +X accel -> negative pitch */
    CHECK_NEAR(pitch, -0.5235988f, 0.02f); /* asin(-0.5) = -30 deg */
    CHECK_NEAR(yaw, 0.0f, 1e-3f);
}

/* -------------------------------------------------------------------------
 * Mahony: 90 dps about Z for 1 s -> yaw = +pi/2 (dps -> rad/s, dt).
 * ---------------------------------------------------------------------- */
static void test_mahony_yaw_rate(void)
{
    mahony_t m;
    const imu_sample_t spin = sample(0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 90.0f);
    float roll = 0.0f, pitch = 0.0f, yaw = 0.0f;

    mahony_init(&m, 2.0f, 0.0f, 0.01f);
    for (int i = 0; i < 100; ++i) {
        mahony_update(&m, &spin);
    }

    mahony_get_angles(&m, &roll, &pitch, &yaw);
    CHECK_NEAR(roll, 0.0f, 1e-3f);
    CHECK_NEAR(pitch, 0.0f, 1e-3f);
    CHECK_NEAR(yaw, 1.5707963f, 0.01f);
}

/* -------------------------------------------------------------------------
 * Tilt: neutral, four directions, hold, and re-arm.
 * ---------------------------------------------------------------------- */
static void test_tilt_basic(void)
{
    tilt_t t;

    tilt_init(&t, 12.0f, 6.0f, 3);
    tilt_set_neutral(&t, 0.0f, 0.0f);

    CHECK(tilt_update(&t, 0.0f, 0.0f) == IMU_DIR_NONE);

    CHECK_DIR3(&t, 20.0f, 0.0f, IMU_DIR_RIGHT);
    /* While held, the locked direction keeps being reported. */
    CHECK(tilt_update(&t, 20.0f, 0.0f) == IMU_DIR_RIGHT);

    CHECK(tilt_update(&t, 0.0f, 0.0f) == IMU_DIR_NONE);
    CHECK_DIR3(&t, -20.0f, 0.0f, IMU_DIR_LEFT);
    CHECK(tilt_update(&t, -20.0f, 0.0f) == IMU_DIR_LEFT);

    CHECK(tilt_update(&t, 0.0f, 0.0f) == IMU_DIR_NONE);
    CHECK_DIR3(&t, 0.0f, 20.0f, IMU_DIR_UP);
    CHECK(tilt_update(&t, 0.0f, 20.0f) == IMU_DIR_UP);

    CHECK(tilt_update(&t, 0.0f, 0.0f) == IMU_DIR_NONE);
    CHECK_DIR3(&t, 0.0f, -20.0f, IMU_DIR_DOWN);
    CHECK(tilt_update(&t, 0.0f, -20.0f) == IMU_DIR_DOWN);
}

/* -------------------------------------------------------------------------
 * Tilt: a tilt inside the 12 deg deadzone never produces a direction, and
 * deadzone samples do not pre-charge the consecutive-sample counter.
 * ---------------------------------------------------------------------- */
static void test_tilt_deadzone(void)
{
    tilt_t t;

    tilt_init(&t, 12.0f, 6.0f, 3);
    tilt_set_neutral(&t, 0.0f, 0.0f);

    for (int i = 0; i < 5; ++i) {
        CHECK(tilt_update(&t, 11.0f, 0.0f) == IMU_DIR_NONE);
    }
    for (int i = 0; i < 5; ++i) {
        CHECK(tilt_update(&t, 0.0f, -11.0f) == IMU_DIR_NONE);
    }

    CHECK(tilt_update(&t, 20.0f, 0.0f) == IMU_DIR_NONE);
    CHECK(tilt_update(&t, 20.0f, 0.0f) == IMU_DIR_NONE);
    CHECK(tilt_update(&t, 20.0f, 0.0f) == IMU_DIR_RIGHT);

    /* A dip into the deadzone breaks the streak before a lock is taken. */
    tilt_reset(&t);
    CHECK(tilt_update(&t, 20.0f, 0.0f) == IMU_DIR_NONE);
    CHECK(tilt_update(&t, 11.0f, 0.0f) == IMU_DIR_NONE);
    CHECK(tilt_update(&t, 20.0f, 0.0f) == IMU_DIR_NONE);
    CHECK(tilt_update(&t, 20.0f, 0.0f) == IMU_DIR_NONE);
    CHECK(tilt_update(&t, 20.0f, 0.0f) == IMU_DIR_RIGHT);

    /* Once locked, a deadzone dip (still above re-arm) keeps the lock; the
     * same direction is reported again immediately. */
    CHECK(tilt_update(&t, 11.0f, 0.0f) == IMU_DIR_NONE);
    CHECK(tilt_update(&t, 20.0f, 0.0f) == IMU_DIR_RIGHT);
}

/* -------------------------------------------------------------------------
 * Tilt: 8 deg is inside the deadzone but not a re-arm; reversals before the
 * 6 deg re-arm are ignored.
 * ---------------------------------------------------------------------- */
static void test_tilt_rearm(void)
{
    tilt_t t;

    tilt_init(&t, 12.0f, 6.0f, 3);
    tilt_set_neutral(&t, 0.0f, 0.0f);

    CHECK_DIR3(&t, 20.0f, 0.0f, IMU_DIR_RIGHT);

    /* 8 deg: deadzone but above the 6 deg re-arm threshold. */
    CHECK(tilt_update(&t, 8.0f, 0.0f) == IMU_DIR_NONE);

    /* A reversal without re-arming must not be accepted. */
    CHECK(tilt_update(&t, -20.0f, 0.0f) == IMU_DIR_NONE);
    CHECK(tilt_update(&t, -20.0f, 0.0f) == IMU_DIR_NONE);
    CHECK(tilt_update(&t, -20.0f, 0.0f) == IMU_DIR_NONE);

    /* The accepted direction is still reported. */
    CHECK(tilt_update(&t, 20.0f, 0.0f) == IMU_DIR_RIGHT);

    /* Below 6 deg the lock is released, and a new direction needs 3 samples. */
    CHECK(tilt_update(&t, 5.0f, 0.0f) == IMU_DIR_NONE);
    CHECK(tilt_update(&t, -20.0f, 0.0f) == IMU_DIR_NONE);
    CHECK(tilt_update(&t, -20.0f, 0.0f) == IMU_DIR_NONE);
    CHECK(tilt_update(&t, -20.0f, 0.0f) == IMU_DIR_LEFT);
}

/* -------------------------------------------------------------------------
 * Tilt: neutral subtraction and the swap/invert axis mapping.
 * ---------------------------------------------------------------------- */
static void test_tilt_neutral_and_axis(void)
{
    tilt_t t;

    tilt_init(&t, 12.0f, 6.0f, 3);
    tilt_set_neutral(&t, 20.0f, -10.0f);

    CHECK(tilt_update(&t, 20.0f, -10.0f) == IMU_DIR_NONE);
    CHECK_DIR3(&t, 40.0f, -10.0f, IMU_DIR_RIGHT);   /* +20 effective roll */

    tilt_set_neutral(&t, 0.0f, 0.0f);

    /* swap_xy: sensor pitch drives the horizontal axis. */
    tilt_set_axis(&t, true, false, false);
    CHECK_DIR3(&t, 0.0f, 20.0f, IMU_DIR_RIGHT);

    /* invert_roll: +roll now means LEFT. */
    tilt_set_axis(&t, false, true, false);
    CHECK_DIR3(&t, 20.0f, 0.0f, IMU_DIR_LEFT);

    /* invert_pitch: +pitch now means DOWN. */
    tilt_set_axis(&t, false, false, true);
    CHECK_DIR3(&t, 0.0f, 20.0f, IMU_DIR_DOWN);

    /* swap happens first, invert_roll then flips the swapped value. */
    tilt_set_axis(&t, true, true, false);
    CHECK_DIR3(&t, 0.0f, 20.0f, IMU_DIR_LEFT);

    /* Default mapping: +pitch is UP, -pitch is DOWN. */
    tilt_set_axis(&t, false, false, false);
    CHECK_DIR3(&t, 0.0f, 20.0f, IMU_DIR_UP);
    CHECK(tilt_update(&t, 0.0f, 0.0f) == IMU_DIR_NONE);
    CHECK_DIR3(&t, 0.0f, -20.0f, IMU_DIR_DOWN);

    /* Neutral subtraction still applies after a mapping change. */
    tilt_set_neutral(&t, -15.0f, 0.0f);
    tilt_set_axis(&t, false, false, false);
    CHECK(tilt_update(&t, -15.0f, 0.0f) == IMU_DIR_NONE);
    CHECK_DIR3(&t, -35.0f, 0.0f, IMU_DIR_LEFT);
}

static void test_names(void)
{
    CHECK(strcmp(imu_dir_name(IMU_DIR_UP), "UP") == 0);
    CHECK(strcmp(imu_dir_name(IMU_DIR_RIGHT), "RIGHT") == 0);
    CHECK(strcmp(imu_dir_name(IMU_DIR_DOWN), "DOWN") == 0);
    CHECK(strcmp(imu_dir_name(IMU_DIR_LEFT), "LEFT") == 0);
    CHECK(strcmp(imu_dir_name(IMU_DIR_NONE), "NONE") == 0);
}

int main(void)
{
    test_calib_gyro_and_accel();
    test_gravity_axis();
    test_calib_raw_accel_and_residual_gyro();
    test_calib_state_and_zero_result();
    test_mahony_static();
    test_mahony_accel_tilt();
    test_mahony_yaw_rate();
    test_tilt_basic();
    test_tilt_deadzone();
    test_tilt_rearm();
    test_tilt_neutral_and_axis();
    test_names();

    printf("TESTS passed=%d failed=%d\n", g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}