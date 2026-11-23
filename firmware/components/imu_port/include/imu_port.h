/*
 * GYRO SNAKE — LSM6DS3TR-C port (ESP-IDF i2c_master).
 *
 * Bus: I2C1, SDA = GPIO1, SCL = GPIO0, 100 kHz, 7-bit address 0x6A.
 *
 * A single 100 Hz task owns the sensor and the IMU algorithms:
 *   normal mode:  raw -> subtract gyro bias only -> mahony_update ->
 *                 (roll,pitch) - neutral -> tilt_update -> atomic direction
 *   calibration:  400 samples at 100 Hz -> imu_calib_feed(raw, raw) +
 *                 mahony_update(raw); on completion imu_calib_result()
 *                 (gyro bias), mahony_get_angles() (neutral pose), NVS save,
 *                 then the result is published to the game task.
 *
 * The accelerometer is always passed to Mahony with gravity intact; only the
 * gyro is bias-corrected. This is the deliberate difference from the
 * reference implementation documented in docs/imu-calibration.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_port.h"
#include "esp_err.h"
#include "imu_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Calibration window: 400 samples at 100 Hz (4 s) on every start. */
#define IMU_PORT_CALIB_SAMPLES 400
#define IMU_PORT_SAMPLE_HZ 100

/*
 * Board mounting. The LSM6DS3TR-C axes on the Sticky are NOT verified on
 * hardware yet; these three compile-time switches are the one-line fix after
 * the four-direction physical check. Defaults are the screen-natural mapping
 * used by the desktop simulator.
 */
#define IMU_PORT_AXIS_SWAP_XY 0
#define IMU_PORT_AXIS_INVERT_ROLL 0
#define IMU_PORT_AXIS_INVERT_PITCH 0

/* Result of one calibration window, published by the IMU task and consumed by
 * the game task. */
typedef struct {
    bool ok;
    imu_bias_t bias;        /* valid == true on success; ax/ay/az diagnostics */
    float neutral_roll;     /* degrees                                     */
    float neutral_pitch;    /* degrees                                     */
} imu_port_calib_result_t;

/* Create the I2C bus, probe WHO_AM_I and configure the sensor. A failure is
 * recoverable: the task keeps re-probing every 500 ms. */
esp_err_t imu_port_init(void);

/* Adopt a calibration loaded from NVS. Call after imu_port_init() and before
 * imu_port_start()/the game task. */
void imu_port_apply_saved(const board_imu_calib_t *calib);

/* Start the 100 Hz task. Idempotent. */
esp_err_t imu_port_start(void);

/* Ask for a fresh 400-sample calibration. Never blocks. If the task is not
 * running, an immediate failure result is published instead. */
void imu_port_request_calibration(void);

/* Make the current Mahony roll/pitch the neutral pose immediately. Intended
 * for PLAY: hold the device the way you are actually playing, press UP, and
 * the tilt baseline is re-zeroed without restarting the run. */
void imu_port_request_rezero(void);

/* Runtime axis mapping (the compile-time IMU_PORT_AXIS_* defaults can be
 * overridden live from the serial console while the four directions are
 * checked on hardware). Applied by the IMU task at its next sample. */
void imu_port_set_axis(bool swap_xy, bool invert_roll, bool invert_pitch);

/* Runtime deadzone / re-arm / stable-sample count. Non-positive values keep
 * the current setting. */
void imu_port_set_tilt_params(float deadzone_deg, float rearm_deg,
                              int stable_samples);

/* Telemetry rate in Hz (1..50, default 10). Values outside the range are
 * clamped. */
void imu_port_set_telemetry_hz(int hz);

/* Snapshot of the runtime configuration, for the serial console. */
typedef struct {
    bool swap_xy;
    bool invert_roll;
    bool invert_pitch;
    float deadzone_deg;
    float rearm_deg;
    int stable_samples;
    int telemetry_hz;
    float neutral_roll;
    float neutral_pitch;
    bool ready;
} imu_port_config_t;

void imu_port_get_config(imu_port_config_t *out);

/* 0..100 progress of the current calibration window. */
uint8_t imu_port_calib_progress(void);

/* Take the pending calibration result, if any. */
bool imu_port_take_calib_result(imu_port_calib_result_t *out);

/* Latest stable tilt direction (IMU_DIR_*), published atomically. */
int8_t imu_port_direction(void);

/* Latched direction since the previous call (-1 if none). A quick tilt that
 * returns to center before the next e-paper tick is still delivered once. */
int8_t imu_port_take_direction(void);

/* Latest relative tilt in deci-degrees, for the on-screen ball. */
void imu_port_get_tilt(int *roll_cdeg, int *pitch_cdeg);

/* True while the sensor answers and is configured. */
bool imu_port_ready(void);

#ifdef __cplusplus
}
#endif