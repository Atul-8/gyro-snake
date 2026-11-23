#ifndef GYRO_SNAKE_TOUCH_PORT_H
#define GYRO_SNAKE_TOUCH_PORT_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GT911 capacitive touch on I2C0 (SDA=GPIO3, SCL=GPIO2, INT=GPIO21,
 * RST=GPIO41, EN=GPIO42). Powers the panel, resets/probes 0x14 and 0x5D,
 * reads the product ID ("911") and starts a 30 ms polling task.
 *
 * A tap anywhere produces a one-shot press event; the game task maps that to
 * the OK button (start / pause / resume / title). The driver only needs the
 * touch count, not calibrated coordinates.
 */
esp_err_t touch_port_init(void);

/* True exactly once per touch-down transition. Safe from any task. */
bool touch_port_take_press(void);

/* Same, but also returns the touch-down point mapped to the 480x800 logical
 * screen. x/y may be NULL. The GT911 on this panel reports a 480x800 range,
 * so the mapping is a scale of each axis; axis flips are not applied until a
 * tap in a known corner confirms them (every touch-down logs raw and mapped
 * points). */
bool touch_port_take_press_xy(int *x, int *y);

/* For diagnostics/logging. */
bool touch_port_ready(void);
int  touch_port_address(void);

#ifdef __cplusplus
}
#endif

#endif /* GYRO_SNAKE_TOUCH_PORT_H */