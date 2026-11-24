/*
 * GYRO SNAKE — portable game core, public API (FROZEN).
 *
 * Copyright (c) 2026 GYRO SNAKE contributors.
 * SPDX-License-Identifier: MIT
 *
 * This header is the only contract between the game core, the ESP-IDF port,
 * the host simulator and the desktop GUI. It is plain C99 and includes only
 * <stdbool.h>, <stdint.h>. No ESP-IDF, no FreeRTOS, no malloc, no I/O, no
 * wall clock, no libc rand.
 *
 * Display: the reTerminal Sticky panel is 480x800. The playfield is a
 * 15x23 grid of 32 px cells below a fixed 64 px HUD:
 *
 *      y=0   +--------------------------------+
 *            | HUD: score / high / length     |  64 px
 *      y=64  +--------------------------------+
 *            |                                |
 *            |  15 x 23 playfield, 32 px cell |
 *            |                                |
 *      y=800 +--------------------------------+
 *
 * Pixel formats are identical to bsp_epd.h:
 *   mono  480x800, 1 bpp, 60 B/row, MSB = leftmost, 1 = white, 0 = black.
 *   gray4 480x800, 2 bpp, 120 B/row, 0 = black .. 3 = white; the caller
 *         provides TWO 96,000 B pages (192,000 B total) so the core can
 *         compose the next card while the current one is on the panel.
 *
 * Refresh handshake (same shape as game.h):
 *   if (snake_dirty(g)) {
 *       kind = snake_status(g)->frame;
 *       push snake_mono() or snake_gray4();
 *       full = snake_needs_full_refresh(g);
 *       ... refresh ...
 *       snake_mark_refreshed(g, full);
 *   }
 *   snake_tick(g, buttons, tilt_dir);   // only when !snake_dirty(g)
 *
 * snake_tick() must not be called while a frame is pending. The port owns
 * that serialisation; the core does not detect it.
 */

#ifndef GYRO_SNAKE_H
#define GYRO_SNAKE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 1. Geometry
 * ====================================================================== */

#define SNAKE_SCREEN_W          480
#define SNAKE_SCREEN_H          800
#define SNAKE_HUD_H             64
#define SNAKE_CELL_PX           32
#define SNAKE_COLS              15
#define SNAKE_ROWS              23
#define SNAKE_PLAY_Y            SNAKE_HUD_H
#define SNAKE_PLAY_W            (SNAKE_COLS * SNAKE_CELL_PX)  /* 480 */
#define SNAKE_PLAY_H            (SNAKE_ROWS * SNAKE_CELL_PX)  /* 736 */
#define SNAKE_MAX_CELLS         (SNAKE_COLS * SNAKE_ROWS)     /* 345 */

#define SNAKE_MONO_ROW_BYTES    (SNAKE_SCREEN_W / 8)          /* 60    */
#define SNAKE_MONO_BYTES        (SNAKE_MONO_ROW_BYTES * SNAKE_SCREEN_H) /* 48000 */
#define SNAKE_GRAY4_ROW_BYTES   (SNAKE_SCREEN_W / 4)          /* 120   */
#define SNAKE_GRAY4_PAGE_BYTES  (SNAKE_GRAY4_ROW_BYTES * SNAKE_SCREEN_H) /* 96000 */
#define SNAKE_GRAY4_BYTES       (SNAKE_GRAY4_PAGE_BYTES * 2)  /* 192000 */

/* =========================================================================
 * 2. Buttons, directions, states, cards, frames, sounds
 * ====================================================================== */

/* Activity mask, exactly like game.h: a bit is set if the key produced a
 * press edge since the previous tick (or is held, for DOWN). */
#define SNAKE_BTN_OK   0x01u
#define SNAKE_BTN_UP   0x02u
#define SNAKE_BTN_DOWN 0x04u

/* Directions are also the tilt_core output values. NONE = -1. */
typedef enum {
    SNAKE_DIR_UP = 0,
    SNAKE_DIR_RIGHT = 1,
    SNAKE_DIR_DOWN = 2,
    SNAKE_DIR_LEFT = 3,
    SNAKE_DIR_NONE = -1
} snake_dir_t;

typedef enum {
    SNAKE_STATE_TITLE = 0,
    SNAKE_STATE_CALIBRATING,
    SNAKE_STATE_PLAY,
    SNAKE_STATE_PAUSE,
    SNAKE_STATE_DEAD,
    SNAKE_STATE_GAME_OVER
} snake_state_t;

typedef enum {
    SNAKE_CARD_NONE = 0,
    SNAKE_CARD_TITLE,
    SNAKE_CARD_CALIBRATING,
    SNAKE_CARD_PAUSED,
    SNAKE_CARD_GAME_OVER,
    SNAKE_CARD_WIN
} snake_card_t;

typedef enum {
    SNAKE_FRAME_MONO = 0,
    SNAKE_FRAME_GRAY4
} snake_frame_t;

typedef enum {
    SNAKE_SOUND_NONE = 0,
    SNAKE_SOUND_EAT,
    SNAKE_SOUND_TURN,
    SNAKE_SOUND_DEATH,
    SNAKE_SOUND_START,
    SNAKE_SOUND_CALIB_DONE,
    SNAKE_SOUND_CALIB_FAIL,
    SNAKE_SOUND_GAME_OVER,
    SNAKE_SOUND_WIN
} snake_sound_t;

/* =========================================================================
 * 3. Caller-owned buffers and opaque state
 * ====================================================================== */

typedef struct {
    uint8_t *mono;        /* SNAKE_MONO_BYTES                      */
    uint8_t *gray4;       /* SNAKE_GRAY4_BYTES, two pages          */
    uint8_t gray4_page;   /* 0 or 1: page currently on the panel   */
} snake_buffers_t;

/* Fixed-size storage; the layout is private to the core. 8 KiB is enough
 * for 345 body cells (2 bytes each) plus state and the RNG. */
#define SNAKE_STATE_BYTES 8192
typedef union snake {
    unsigned char bytes[SNAKE_STATE_BYTES];
    long long _align_ll;
    double _align_d;
    void *_align_ptr;
} snake_t;

/* Snapshot; the pointer points into the snake_t and is valid until the next
 * mutating call. */
typedef struct {
    snake_state_t state;
    snake_card_t card;
    snake_frame_t frame;
    snake_dir_t dir;          /* current movement direction             */
    uint8_t length;           /* body cells, 3..SNAKE_MAX_CELLS         */
    uint8_t calib_percent;    /* 0..100, meaningful while CALIBRATING   */
    uint8_t cells_total;      /* SNAKE_MAX_CELLS (for HUD)              */
    uint16_t score;
    uint16_t high_score;
    uint16_t tick;            /* ticks since the run started            */
    uint16_t food_eaten;
    bool calibrated;          /* a bias/neutral pose is available       */
    bool record_beaten;       /* score exceeded the high-water mark     */
} snake_status_t;

/* =========================================================================
 * 4. Lifecycle
 * ====================================================================== */

/*
 * Bind buffers, seed the RNG, compose the title card.
 * Preconditions: g, bufs, bufs->mono, bufs->gray4 non-NULL; page <= 1.
 * Postconditions: TITLE / CARD_TITLE / GRAY4 / dirty / needs full / score 0 /
 * high 0 / calibrated false.
 * Returns false on a precondition violation.
 */
bool snake_init(snake_t *g, const snake_buffers_t *bufs, uint32_t seed);

/* Adopt a persisted high score (monotonic high-water mark). */
void snake_set_high_score(snake_t *g, uint16_t high_score);
uint16_t snake_high_score(const snake_t *g);

/*
 * Enter the calibration card. The port then runs its calibration loop and
 * calls snake_calibration_progress() while it collects samples, followed by
 * snake_calibration_done(ok). The core composes the gray4 progress card.
 */
void snake_begin_calibration(snake_t *g);
void snake_calibration_progress(snake_t *g, uint8_t percent);
void snake_calibration_done(snake_t *g, bool ok);

/*
 * Start a fresh run using the already-calibrated bias/neutral pose.
 * If calibrated == false, this falls back to snake_begin_calibration().
 * Resets score, body, food, tick and queues SNAKE_SOUND_START.
 */
void snake_start_play(snake_t *g);

/* =========================================================================
 * 5. The tick
 * ====================================================================== */

/*
 * Advance exactly one displayed frame.
 *
 * `buttons` is the SNAKE_BTN_* activity mask for the observation window.
 * `tilt_dir` is the latest stable tilt direction (-1 = keep the current
 * direction, 0..3 = a request). The core ignores a 180-degree reversal.
 *
 * State rules:
 *   TITLE:       OK -> calibration if !calibrated, else start_play.
 *   CALIBRATING: no world; recomposes the progress card if percent changed.
 *   PLAY:        OK -> PAUSE. Otherwise apply tilt_dir, move one cell,
 *                handle food/growth/wall/self, compose a mono frame.
 *   PAUSE:       OK -> PLAY; UP -> begin_calibration (fresh run afterwards).
 *   DEAD:        one tick -> GAME_OVER (gray4 card, high score updated).
 *   GAME_OVER:   OK -> TITLE; UP -> begin_calibration.
 *
 * Precondition: snake_dirty(g) == false.
 */
void snake_tick(snake_t *g, uint8_t buttons, int8_t tilt_dir);

/* =========================================================================
 * 6. Framebuffer and refresh handshake
 * ====================================================================== */

const uint8_t *snake_mono(const snake_t *g);   /* SNAKE_MONO_BYTES          */
const uint8_t *snake_gray4(const snake_t *g);  /* one 96,000 B page          */
const snake_status_t *snake_status(const snake_t *g);

bool snake_dirty(const snake_t *g);
bool snake_needs_full_refresh(const snake_t *g);

/* Clear dirty; a gray4 refresh publishes the staging page and flips it.
 * `was_full` must be the mode actually used. */
void snake_mark_refreshed(snake_t *g, bool was_full);

/* =========================================================================
 * 7. Audio events
 * ====================================================================== */

snake_sound_t snake_take_sound(snake_t *g);

/* =========================================================================
 * 8. Introspection
 * ====================================================================== */

const char *snake_state_name(snake_state_t state);
const char *snake_card_name(snake_card_t card);
const char *snake_sound_name(snake_sound_t sound);

#ifdef __cplusplus
}
#endif

#endif /* GYRO_SNAKE_H */