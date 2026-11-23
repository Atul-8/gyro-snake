/*
 * GYRO SNAKE — portable IMU calibration, Mahony attitude and tilt direction.
 *
 * Copyright (c) 2026 GYRO SNAKE contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain C99 + <math.h>. No ESP-IDF, no I2C, no malloc, no wall clock.
 *
 * Algorithm provenance
 * --------------------
 * The calibration and Mahony code is a clean re-implementation of the
 * *actual* code paths reached from app_main() in the local reference
 * project MagicRobot_V0.1/embedded
 * (src/core/calibration/imu_calib.c, src/core/attitude/mahony.c,
 *  apps/espidf/main/runtime/app_stream.c), not of its comments.
 *
 * One deliberate difference from that reference:
 *   MagicRobot's imu_calib_result() subtracts sign*1g from the gravity axis
 *   and stores that in the accelerometer bias. Its do_sample() then subtracts
 *   the same bias before mahony_update(), so after a calibration the accel
 *   input to Mahony is almost zero and mahony_update() returns early
 *   (n < 1e-6). DEF_SYS_FLAGS includes SYSF_APPLY_BIAS_EN, so this is the
 *   default path, not a dead branch.
 *
 *   This module therefore separates the two concerns:
 *     - the gyro bias (mean angular rate at rest) is subtracted from the gyro;
 *     - the accelerometer is passed to Mahony **with gravity intact**;
 *     - the neutral pose is captured as the Mahony roll/pitch after the
 *       calibration window, and subtracted from later angles.
 *   imu_calib_result() still reports the mean raw accel vector for
 *   diagnostics, but it does NOT subtract 1g and does NOT feed it to Mahony.
 *
 * Board mounting
 * --------------
 * The LSM6DS3TR-C axes are board-specific. tilt_set_axis() lets the port
 * choose swap_xy / invert_roll / invert_pitch after a one-time physical
 * check; the desktop simulator uses the screen-natural mapping.
 *
 * Units: acceleration in g, angular rate in dps, angles in degrees at the
 * tilt interface and radians inside Mahony.
 */

#ifndef GYRO_SNAKE_IMU_CORE_H
#define GYRO_SNAKE_IMU_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Direction values match snake_dir_t in snake.h. */
#define IMU_DIR_NONE  (-1)
#define IMU_DIR_UP     0
#define IMU_DIR_RIGHT  1
#define IMU_DIR_DOWN   2
#define IMU_DIR_LEFT   3

/* =========================================================================
 * 1. Samples and bias
 * ====================================================================== */

typedef struct {
    float ax, ay, az;   /* g   */
    float gx, gy, gz;   /* dps */
} imu_sample_t;

typedef struct {
    float gx, gy, gz;   /* gyro bias, dps                         */
    float ax, ay, az;   /* mean raw accel, g (diagnostics/neutral) */
    bool  valid;        /* set by the caller after a calibration  */
    int8_t grav_axis;   /* 0..2, or -1 when not detected          */
    int8_t grav_sign;   /* +1 / -1                                */
} imu_bias_t;

/* =========================================================================
 * 2. Static calibration (mean gyro bias + gravity-axis detection)
 * ====================================================================== */

typedef enum {
    IMU_CALIB_IDLE = 0,
    IMU_CALIB_COLLECTING = 1
} imu_calib_state_t;

typedef struct {
    double gx_sum, gy_sum, gz_sum;
    double ax_sum, ay_sum, az_sum;
    uint32_t n;
    int target_n;
    int state;        /* imu_calib_state_t */
    int grav_axis;    /* -1 until the window completes */
    int grav_sign;
} imu_calib_t;

/* Reset to IDLE, clear sums, grav_axis = -1, keep target_n. */
void imu_calib_reset(imu_calib_t *c, int target_n);

/* Start a collection window (idempotent while collecting). */
void imu_calib_start(imu_calib_t *c);

/*
 * Feed one sample. `s` is averaged into the bias (pass raw at boot, or
 * bias-corrected if the caller implements the residual/accumulate scheme);
 * `raw` is used ONLY for gravity-axis detection and may be NULL (= s).
 * Returns 1 when the window just completed, 0 otherwise.
 */
int imu_calib_feed(imu_calib_t *c, const imu_sample_t *s, const imu_sample_t *raw);

bool imu_calib_ready(const imu_calib_t *c);

/*
 * Read the mean gyro bias and the mean raw accel vector. No 1g subtraction.
 * The caller sets bias->valid = true after a successful calibration.
 * If not ready, all fields are zero and valid is false.
 */
void imu_calib_result(const imu_calib_t *c, imu_bias_t *bias);

/* Subtract the gyro bias only. This is the sample Mahony must receive. */
void imu_apply_gyro_bias(imu_sample_t *s, const imu_bias_t *b);

/* Subtract gyro and accel bias. Diagnostics/stream only; NOT for Mahony. */
void imu_apply_all_bias(imu_sample_t *s, const imu_bias_t *b);

/* =========================================================================
 * 3. Mahony attitude filter (float)
 * ====================================================================== */

typedef struct {
    float qw, qx, qy, qz;
    float integral[3];
    float kp, ki, dt;   /* dt in seconds */
} mahony_t;

/* kp = 2.0, ki = 0.0, dt = 0.01 is the reference default (104 Hz). */
void mahony_init(mahony_t *m, float kp, float ki, float dt);
void mahony_update(mahony_t *m, const imu_sample_t *s);
void mahony_get_angles(const mahony_t *m, float *roll, float *pitch, float *yaw);
void mahony_get_quat(const mahony_t *m, float *w, float *x, float *y, float *z);

/* =========================================================================
 * 4. Tilt -> 4-direction steering
 * ====================================================================== */

typedef struct {
    float deadzone_deg;   /* e.g. 8: below this magnitude, no direction      */
    float rearm_deg;      /* boundary hysteresis (deg) + unlock threshold    */
    int stable_samples;   /* consecutive samples to accept a new direction   */
    int candidate;        /* -1 or 0..3                                      */
    int count;
    int locked;           /* held direction, -1 or 0..3 (continuous)         */
    float neutral_roll_deg;
    float neutral_pitch_deg;
    bool swap_xy;
    bool invert_roll;
    bool invert_pitch;
} tilt_t;

/*
 * Neutral pose is captured after calibration. deadzone/rearm are degrees.
 * Default axis mapping is screen-natural (roll = X, pitch = Y); the port can
 * override it with tilt_set_axis().
 */
void tilt_init(tilt_t *t, float deadzone_deg, float rearm_deg, int stable_samples);
void tilt_set_neutral(tilt_t *t, float roll_deg, float pitch_deg);
void tilt_set_axis(tilt_t *t, bool swap_xy, bool invert_roll, bool invert_pitch);
void tilt_reset(tilt_t *t);

/*
 * Update with the latest roll/pitch (degrees, screen frame). The tilt vector
 * is split into four 45-degree sectors around the neutral pose: anything
 * within +-45 deg of straight up reports UP, and likewise for the other
 * directions, so a diagonal tilt still steers toward its dominant side.
 * The direction follows the sensor in real time: changing sectors switches
 * the direction without returning to center. deadzone_deg silences the
 * center; rearm_deg widens the held sector (hysteresis) and unlocks below
 * it; a new direction must hold stable_samples calls (with the previous
 * direction still reported in the meantime). Returns IMU_DIR_NONE (-1) when
 * near center; otherwise 0..3. The core ignores 180-degree reversals.
 */
int8_t tilt_update(tilt_t *t, float roll_deg, float pitch_deg);

/* "UP"/"RIGHT"/"DOWN"/"LEFT"/"NONE" */
const char *imu_dir_name(int8_t dir);

#ifdef __cplusplus
}
#endif

#endif /* GYRO_SNAKE_IMU_CORE_H */