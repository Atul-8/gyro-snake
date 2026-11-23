/*
 * GYRO SNAKE — live tilt ball in the bottom-right corner of a mono frame.
 *
 * Pure 1 bpp pixel pushing: 480x800, 60 B/row, MSB leftmost, 1 = white.
 *
 * The instrument is drawn as outlines, not as a filled disc: the bottom-right
 * corner of the play field is legal snake territory, so a solid ball would
 * hide cells. Every stroke gets a white halo first, which keeps the ring and
 * the dot visible over a black snake body as well as over the white field.
 *
 *   outer ring   full scale (30 deg)
 *   inner ring   the 8 deg deadzone: inside it no direction is reported
 *   dot          current relative roll/pitch
 *   side tick    the direction the tilt filter currently reports
 *
 * Drawn by the game task after the core composed a MONO frame, just before
 * the frame is handed to the panel.
 */

#include "board_port.h"

#include <math.h>
#include <stdint.h>

#include "imu_core.h"

#define BALL_CX 450
#define BALL_CY 770
#define BALL_R 26
#define BALL_DEADZONE_DEG 8.0f
#define BALL_FULL_SCALE_DEG 20.0f

static void put(uint8_t *b, int x, int y, int white)
{
    if (x < 0 || x >= SNAKE_SCREEN_W || y < 0 || y >= SNAKE_SCREEN_H) return;
    uint8_t *p = &b[y * (SNAKE_SCREEN_W / 8) + x / 8];
    const uint8_t mask = (uint8_t)(0x80u >> (x & 7));
    if (white) {
        *p |= mask;
    } else {
        *p &= (uint8_t)~mask;
    }
}

static void disc(uint8_t *b, int cx, int cy, int r, int white)
{
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            if (x * x + y * y <= r * r) put(b, cx + x, cy + y, white);
        }
    }
}

static void ring(uint8_t *b, int cx, int cy, int r, int thickness, int white)
{
    if (r <= 0) return;
    const int inner = r - thickness;
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            const int d2 = x * x + y * y;
            if (d2 <= r * r && d2 >= inner * inner) put(b, cx + x, cy + y, white);
        }
    }
}

/* White halo + black core: legible on both paper and ink. */
static void orb(uint8_t *b, int cx, int cy, int r)
{
    disc(b, cx, cy, r + 2, 1);
    disc(b, cx, cy, r, 0);
}

static void tick(uint8_t *b, int x0, int y0, int x1, int y1)
{
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int t = 0; t <= 100; ++t) {
                const int x = x0 + (x1 - x0) * t / 100 + dx;
                const int y = y0 + (y1 - y0) * t / 100 + dy;
                put(b, x, y, (dx == 0 && dy == 0) ? 0 : 1);
            }
        }
    }
}

void board_draw_tilt_ball(uint8_t *mono, int roll_cdeg, int pitch_cdeg, int dir)
{
    if (!mono) return;

    /* White halo first, black stroke inside it: the ring reads on paper and on
     * a black snake body alike. */
    ring(mono, BALL_CX, BALL_CY, BALL_R, 4, 1);
    ring(mono, BALL_CX, BALL_CY, BALL_R - 4, 2, 0);

    const int dz = (int)lroundf((float)BALL_R * BALL_DEADZONE_DEG / BALL_FULL_SCALE_DEG);
    ring(mono, BALL_CX, BALL_CY, dz, 3, 1);
    ring(mono, BALL_CX, BALL_CY, dz - 2, 1, 0);

    /* Four ticks mark the ring's axes inside the stroke; the active one is
     * drawn twice so it reads as a heavier mark. */
    const int tick_in = BALL_R - 10;
    const int tick_out = BALL_R - 6;
    tick(mono, BALL_CX, BALL_CY - tick_in, BALL_CX, BALL_CY - tick_out);
    tick(mono, BALL_CX, BALL_CY + tick_in, BALL_CX, BALL_CY + tick_out);
    tick(mono, BALL_CX - tick_in, BALL_CY, BALL_CX - tick_out, BALL_CY);
    tick(mono, BALL_CX + tick_in, BALL_CY, BALL_CX + tick_out, BALL_CY);

    switch (dir) {
    case IMU_DIR_UP:
        tick(mono, BALL_CX - 3, BALL_CY - tick_in, BALL_CX - 3, BALL_CY - tick_out);
        tick(mono, BALL_CX + 3, BALL_CY - tick_in, BALL_CX + 3, BALL_CY - tick_out);
        break;
    case IMU_DIR_DOWN:
        tick(mono, BALL_CX - 3, BALL_CY + tick_in, BALL_CX - 3, BALL_CY + tick_out);
        tick(mono, BALL_CX + 3, BALL_CY + tick_in, BALL_CX + 3, BALL_CY + tick_out);
        break;
    case IMU_DIR_LEFT:
        tick(mono, BALL_CX - tick_in, BALL_CY - 3, BALL_CX - tick_out, BALL_CY - 3);
        tick(mono, BALL_CX - tick_in, BALL_CY + 3, BALL_CX - tick_out, BALL_CY + 3);
        break;
    case IMU_DIR_RIGHT:
        tick(mono, BALL_CX + tick_in, BALL_CY - 3, BALL_CX + tick_out, BALL_CY - 3);
        tick(mono, BALL_CX + tick_in, BALL_CY + 3, BALL_CX + tick_out, BALL_CY + 3);
        break;
    default:
        break;
    }

    /* Relative tilt in degrees (deci-degrees / 10). +roll = RIGHT, +pitch =
     * UP, matching the direction filter in imu_core/tilt.c. The dot stays
     * inside the black ring at full scale. */
    float dx = (float)roll_cdeg / 10.0f / BALL_FULL_SCALE_DEG * (float)(BALL_R - 9);
    float dy = -(float)pitch_cdeg / 10.0f / BALL_FULL_SCALE_DEG * (float)(BALL_R - 9);
    const float len = sqrtf(dx * dx + dy * dy);
    const float max_len = (float)(BALL_R - 9);
    if (len > max_len && len > 0.0f) {
        dx = dx * max_len / len;
        dy = dy * max_len / len;
    }

    orb(mono, BALL_CX + (int)lroundf(dx), BALL_CY + (int)lroundf(dy), 3);
}