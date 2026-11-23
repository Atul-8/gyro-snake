/*
 * GYRO SNAKE — serial console for IMU debugging.
 *
 * Reads "$"-prefixed command lines from UART0 (the logging port) and applies
 * them to the IMU port, so the four tilt directions can be checked on
 * hardware without reflashing:
 *
 *   $?                 print the runtime config ("CFG ..." line)
 *   $zero              set the current pose as tilt zero
 *   $cal               run the 4 s calibration window
 *   $map S R P         axis map: swap_xy, invert_roll, invert_pitch (0/1)
 *   $dead <deci-deg>   deadzone (e.g. $dead 80 = 8.0 deg)
 *   $rearm <deci-deg>  re-arm threshold
 *   $stable <samples>  consecutive samples a direction must hold
 *   $hz <1..50>        telemetry rate
 *
 * Every applied command answers with a "CMD <name> ok|err" log line so the
 * desktop debug page can confirm it.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install the UART0 driver and start the reader task. Safe to call once. */
esp_err_t console_port_init(void);

#ifdef __cplusplus
}
#endif
