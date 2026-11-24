/*
 * GYRO SNAKE - desktop simulator DLL boundary.
 *
 * Copyright (c) 2026 GYRO SNAKE contributors.
 * SPDX-License-Identifier: MIT
 *
 * Thin, ctypes-friendly wrapper around the frozen game_core API (snake.h).
 * The core owns no memory and is fully deterministic; this file only owns
 * static storage, serialises the frame handshake and exports a small C ABI.
 *
 * Contract:
 *   - one static snake_t, one 48,000 B mono frame, one 192,000 B gray4 pair;
 *   - no heap, no stdio, no logging, no threads, no locks;
 *   - every getter copies a field out of snake_status(), never returns a
 *     pointer into the core state;
 *   - booleans cross the boundary as int (0/1), never as C bool;
 *   - all calls are serialised by the caller (one DLL, one GUI thread).
 */

#include "snake.h"

#include <string.h>

/* The only board-side symbol the simulator links: pure 1 bpp pixel drawing. */
void board_draw_tilt_ball(uint8_t *mono, int roll_cdeg, int pitch_cdeg, int dir);

#if defined(_WIN32)
#  define GS_API __declspec(dllexport)
#else
#  define GS_API __attribute__((visibility("default")))
#endif

#define GS_ABI_VERSION 1

/* Caller-owned storage; the core borrows these for the process lifetime. */
static snake_t g_game;
static uint8_t g_mono[SNAKE_MONO_BYTES];   /* 48,000 B, 1 bpp       */
static uint8_t g_gray4[SNAKE_GRAY4_BYTES]; /* 192,000 B, two pages  */
static int     g_ready;

/* Status snapshot helper: NULL until a successful gs_init(). */
static const snake_status_t *gs_status(void)
{
    return g_ready ? snake_status(&g_game) : NULL;
}

/* ------------------------------------------------------------------ */
/* ABI / lifecycle                                                     */
/* ------------------------------------------------------------------ */

GS_API int gs_abi_version(void)
{
    return GS_ABI_VERSION;
}

GS_API int gs_init(uint32_t seed)
{
    snake_buffers_t bufs;

    memset(&g_game, 0, sizeof g_game);
    memset(g_mono, 0xFF, sizeof g_mono);
    memset(g_gray4, 0xFF, sizeof g_gray4);

    bufs.mono = g_mono;
    bufs.gray4 = g_gray4;
    bufs.gray4_page = 0;

    g_ready = snake_init(&g_game, &bufs, seed) ? 1 : 0;
    return g_ready;
}

GS_API int gs_tick(uint8_t buttons, int8_t tilt_dir)
{
    if (!g_ready) {
        return 0;
    }
    /* Contract guard: a pending frame would be overwritten by the core. */
    if (snake_dirty(&g_game)) {
        return 0;
    }
    snake_tick(&g_game, buttons, tilt_dir);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Calibration / run control                                           */
/* ------------------------------------------------------------------ */

GS_API void gs_begin_calibration(void)
{
    if (g_ready) {
        snake_begin_calibration(&g_game);
    }
}

GS_API int gs_calib_progress(uint8_t percent)
{
    const snake_status_t *s;

    if (!g_ready) {
        return 0;
    }
    snake_calibration_progress(&g_game, percent);
    s = snake_status(&g_game);
    return (s != NULL && s->state == SNAKE_STATE_CALIBRATING) ? 1 : 0;
}

GS_API int gs_calib_done(int ok)
{
    if (!g_ready) {
        return 0;
    }
    snake_calibration_done(&g_game, ok != 0);
    return (snake_status(&g_game)->state == SNAKE_STATE_PLAY) ? 1 : 0;
}

GS_API int gs_start_play(void)
{
    if (!g_ready) {
        return 0;
    }
    snake_start_play(&g_game);
    return (snake_status(&g_game)->state == SNAKE_STATE_PLAY) ? 1 : 0;
}

GS_API void gs_set_high(uint16_t high_score)
{
    if (g_ready) {
        snake_set_high_score(&g_game, high_score);
    }
}

/* ------------------------------------------------------------------ */
/* Framebuffers (borrowed pointers, copied immediately by the caller)  */
/* ------------------------------------------------------------------ */

GS_API const uint8_t *gs_mono(void)
{
    return g_ready ? snake_mono(&g_game) : NULL;
}

GS_API const uint8_t *gs_gray4(void)
{
    return g_ready ? snake_gray4(&g_game) : NULL;
}

/*
 * Stamp the live tilt ball onto the current MONO frame, exactly as the device
 * does before handing the frame to the panel. Call once per fresh frame and
 * before gs_mono(); the core recomposes every MONO frame from a cleared
 * buffer, so the stamp never accumulates.
 */
GS_API void gs_draw_ball(int roll_cdeg, int pitch_cdeg, int dir)
{
    const snake_status_t *s;

    if (!g_ready) {
        return;
    }
    s = snake_status(&g_game);
    if (!s || s->frame != SNAKE_FRAME_MONO) {
        return;
    }
    board_draw_tilt_ball(g_mono, roll_cdeg, pitch_cdeg, dir);
}

/* ------------------------------------------------------------------ */
/* Frame handshake                                                     */
/* ------------------------------------------------------------------ */

GS_API int gs_frame(void)
{
    const snake_status_t *s = gs_status();
    return s ? (int)s->frame : -1;
}

GS_API int gs_dirty(void)
{
    return (g_ready && snake_dirty(&g_game)) ? 1 : 0;
}

GS_API int gs_needs_full(void)
{
    return (g_ready && snake_needs_full_refresh(&g_game)) ? 1 : 0;
}

GS_API void gs_mark(int was_full)
{
    if (g_ready) {
        snake_mark_refreshed(&g_game, was_full != 0);
    }
}

/* ------------------------------------------------------------------ */
/* Status getters (field copies only)                                  */
/* ------------------------------------------------------------------ */

GS_API int gs_state(void)
{
    const snake_status_t *s = gs_status();
    return s ? (int)s->state : 0;
}

GS_API int gs_card(void)
{
    const snake_status_t *s = gs_status();
    return s ? (int)s->card : 0;
}

GS_API int gs_score(void)
{
    const snake_status_t *s = gs_status();
    return s ? (int)s->score : 0;
}

GS_API int gs_high(void)
{
    const snake_status_t *s = gs_status();
    return s ? (int)s->high_score : 0;
}

GS_API int gs_length(void)
{
    const snake_status_t *s = gs_status();
    return s ? (int)s->length : 0;
}

GS_API int gs_tick_index(void)
{
    const snake_status_t *s = gs_status();
    return s ? (int)s->tick : 0;
}

GS_API int gs_calib_percent(void)
{
    const snake_status_t *s = gs_status();
    return s ? (int)s->calib_percent : 0;
}

GS_API int gs_dir(void)
{
    const snake_status_t *s = gs_status();
    return s ? (int)s->dir : -1;
}

GS_API int gs_record_beaten(void)
{
    const snake_status_t *s = gs_status();
    return (s && s->record_beaten) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Extended introspection (GUI status card / logs)                     */
/* ------------------------------------------------------------------ */

GS_API int gs_calibrated(void)
{
    const snake_status_t *s = gs_status();
    return (s && s->calibrated) ? 1 : 0;
}

GS_API int gs_food_eaten(void)
{
    const snake_status_t *s = gs_status();
    return s ? (int)s->food_eaten : 0;
}

GS_API int gs_cells_total(void)
{
    const snake_status_t *s = gs_status();
    return s ? (int)s->cells_total : 0;
}

/* ------------------------------------------------------------------ */
/* Audio                                                               */
/* ------------------------------------------------------------------ */

GS_API int gs_take_sound(void)
{
    return g_ready ? (int)snake_take_sound(&g_game) : 0;
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

GS_API const char *gs_state_name(int state)
{
    return snake_state_name((snake_state_t)state);
}

GS_API const char *gs_card_name(int card)
{
    return snake_card_name((snake_card_t)card);
}

GS_API const char *gs_sound_name(int sound)
{
    return snake_sound_name((snake_sound_t)sound);
}