/*
 * Host preview: compose a real PLAY frame with game_core, stamp the tilt ball
 * that board_port draws on the device, and write the result as a PGM so the
 * layout can be inspected without flashing.
 *
 * Build (from the project root, any host gcc):
 *   gcc -std=c99 -O2 -Ihost/preview \
 *       -Ifirmware/components/game_core/include \
 *       -Ifirmware/components/imu_core/include \
 *       -Ifirmware/components/board_port/include \
 *       host/preview/play_preview.c \
 *       firmware/components/game_core/src/*.c \
 *       firmware/components/board_port/src/tilt_ball.c \
 *       -lm -o host/preview/play_preview
 *
 * Usage: play_preview out_centre.pgm out_tilt.pgm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_port.h"
#include "snake.h"

static uint8_t s_mono[SNAKE_MONO_BYTES];
static uint8_t s_gray4[SNAKE_GRAY4_BYTES];
static snake_t s_game;

static int write_pgm(const char *path, const uint8_t *mono)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 1;
    }
    fprintf(f, "P5\n%d %d\n255\n", SNAKE_SCREEN_W, SNAKE_SCREEN_H);
    for (int y = 0; y < SNAKE_SCREEN_H; ++y) {
        const uint8_t *row = mono + (size_t)y * (SNAKE_SCREEN_W / 8);
        for (int x = 0; x < SNAKE_SCREEN_W; ++x) {
            const uint8_t bit = (uint8_t)(0x80u >> (x & 7));
            const uint8_t v = (row[x / 8] & bit) ? 255 : 0;
            fputc(v, f);
        }
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s out_centre.pgm out_tilt.pgm\n", argv[0]);
        return 2;
    }

    const snake_buffers_t bufs = {s_mono, s_gray4, 0};
    if (!snake_init(&s_game, &bufs, 0x4753u)) {
        fprintf(stderr, "snake_init failed\n");
        return 1;
    }

    /* Straight to a play frame: calibration window, then PLAY. */
    snake_begin_calibration(&s_game);
    snake_calibration_progress(&s_game, 100);
    snake_calibration_done(&s_game, true);
    for (int i = 0; i < 6; ++i) {
        snake_tick(&s_game, 0, -1);
        snake_mark_refreshed(&s_game, false);
    }

    /* Frame 1: at rest, no direction. */
    board_draw_tilt_ball(s_mono, 0, 0, -1);
    if (write_pgm(argv[1], s_mono) != 0) return 1;

    /* Frame 2: right + up tilt, RIGHT reported. The core will recompose on the
     * next real tick; here the ball is stamped over the same frame. */
    board_draw_tilt_ball(s_mono, 120, 60, 1);
    if (write_pgm(argv[2], s_mono) != 0) return 1;

    puts("ok");
    return 0;
}