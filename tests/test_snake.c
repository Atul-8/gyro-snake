/*
 * GYRO SNAKE — host unit tests.
 *
 * Copyright (c) 2026 GYRO SNAKE contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Build/run (from the repository root):
 *   gcc -std=c99 -O2 -Wall -Wextra \
 *       -I gyro-snake/firmware/components/game_core/include \
 *       gyro-snake/firmware/components/game_core/src/snake.c \
 *       gyro-snake/firmware/components/game_core/src/render.c \
 *       gyro-snake/firmware/components/game_core/src/rng.c \
 *       gyro-snake/firmware/components/game_core/src/font.c \
 *       gyro-snake/tests/test_snake.c -o /tmp/test_snake && /tmp/test_snake
 *
 * The tests only use the frozen public API. They read the real framebuffer to
 * find the apple and to verify that what the core reports matches what it
 * actually draws. The last line is always "TESTS passed=<n> failed=<m>".
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "snake.h"

/* =========================================================================
 * Harness
 * ====================================================================== */

static int t_run;
static int t_fail;

#define CHECK(cond)                                                         \
    do {                                                                    \
        ++t_run;                                                            \
        if (!(cond)) {                                                      \
            ++t_fail;                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                   \
    } while (0)

#define CHECK_EQ(expr, want)                                                \
    do {                                                                    \
        long _a = (long)(expr);                                             \
        long _w = (long)(want);                                             \
        ++t_run;                                                            \
        if (_a != _w) {                                                     \
            ++t_fail;                                                       \
            printf("FAIL %s:%d: %s = %ld, want %ld\n",                      \
                   __FILE__, __LINE__, #expr, _a, _w);                      \
        }                                                                   \
    } while (0)

/* =========================================================================
 * Framebuffer plumbing
 * ====================================================================== */

static uint8_t g_mono[SNAKE_MONO_BYTES];
static uint8_t g_gray[SNAKE_GRAY4_BYTES];
static uint8_t g_mono_b[SNAKE_MONO_BYTES];
static uint8_t g_gray_b[SNAKE_GRAY4_BYTES];

static snake_frame_t g_push_frame;
static uint32_t g_push_hash;
static bool g_push_full;
static int g_push_count;

static uint32_t hash32(const uint8_t *p, int n)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    int i;

    for (i = 0; i < n; ++i) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h;
}

/* Port-style refresh handshake: push the pending frame, then mark it. */
static void pump(snake_t *g)
{
    const snake_status_t *st;

    if (!snake_dirty(g)) {
        return;
    }
    st = snake_status(g);
    g_push_frame = st->frame;
    g_push_full = (st->frame == SNAKE_FRAME_GRAY4) ||
                  snake_needs_full_refresh(g);
    if (st->frame == SNAKE_FRAME_GRAY4) {
        g_push_hash = hash32(snake_gray4(g), SNAKE_GRAY4_PAGE_BYTES);
    } else {
        g_push_hash = hash32(snake_mono(g), SNAKE_MONO_BYTES);
    }
    ++g_push_count;
    snake_mark_refreshed(g, g_push_full);
}

static void step(snake_t *g, uint8_t buttons, int8_t tilt)
{
    pump(g);
    snake_tick(g, buttons, tilt);
    pump(g);
}

static void init_game(snake_t *g, uint8_t *mono, uint8_t *gray, uint32_t seed)
{
    snake_buffers_t b;

    b.mono = mono;
    b.gray4 = gray;
    b.gray4_page = 0;
    CHECK(snake_init(g, &b, seed));
    pump(g);
}

/* init + TITLE -> CALIBRATING -> 100% -> PLAY */
static void init_and_play(snake_t *g, uint8_t *mono, uint8_t *gray, uint32_t seed)
{
    init_game(g, mono, gray, seed);
    CHECK_EQ(snake_status(g)->state, SNAKE_STATE_TITLE);

    step(g, SNAKE_BTN_OK, -1);
    CHECK_EQ(snake_status(g)->state, SNAKE_STATE_CALIBRATING);

    snake_calibration_progress(g, 100);
    step(g, 0, -1);
    snake_calibration_done(g, true);

    CHECK_EQ(snake_status(g)->state, SNAKE_STATE_PLAY);
    pump(g);
}

/* =========================================================================
 * Pixel probes
 * ====================================================================== */

static int px_black(const uint8_t *m, int x, int y)
{
    if (x < 0 || y < 0 || x >= SNAKE_SCREEN_W || y >= SNAKE_SCREEN_H) {
        return 0;
    }
    return ((m[y * SNAKE_MONO_ROW_BYTES + (x >> 3)] >> (7 - (x & 7))) & 1u) == 0u;
}

/* A snake cell is solid black from +2 to +29 with 2 px rounded corners; the
 * probe at +5 is ink for both body and head and never for the round apple. */
static int cell_snake(const uint8_t *m, int cx, int cy)
{
    return px_black(m, cx * SNAKE_CELL_PX + 5,
                    SNAKE_PLAY_Y + cy * SNAKE_CELL_PX + 5);
}

static int cell_apple(const uint8_t *m, int cx, int cy)
{
    if (cell_snake(m, cx, cy)) {
        return 0;
    }
    return px_black(m, cx * SNAKE_CELL_PX + SNAKE_CELL_PX / 2,
                    SNAKE_PLAY_Y + cy * SNAKE_CELL_PX + SNAKE_CELL_PX / 2);
}

static int scan_apple(const uint8_t *m, int *ax, int *ay)
{
    int x;
    int y;

    for (y = 0; y < SNAKE_ROWS; ++y) {
        for (x = 0; x < SNAKE_COLS; ++x) {
            if (cell_apple(m, x, y)) {
                *ax = x;
                *ay = y;
                return 1;
            }
        }
    }
    return 0;
}

static void scan_body(const uint8_t *m, uint8_t *occ)
{
    int x;
    int y;

    for (y = 0; y < SNAKE_ROWS; ++y) {
        for (x = 0; x < SNAKE_COLS; ++x) {
            occ[y * SNAKE_COLS + x] = (uint8_t)cell_snake(m, x, y);
        }
    }
}

/* =========================================================================
 * Trail bookkeeping (the test's model of the snake)
 * ====================================================================== */

typedef struct {
    int x[SNAKE_MAX_CELLS];
    int y[SNAKE_MAX_CELLS];
    int len;
} trail_t;

static const int k_dx[4] = { 0, 1, 0, -1 };
static const int k_dy[4] = { -1, 0, 1, 0 };

static void trail_init(trail_t *t, int dir)
{
    t->len = 3;
    t->x[0] = SNAKE_COLS / 2;
    t->y[0] = SNAKE_ROWS / 2;
    t->x[1] = t->x[0] - 1;
    t->y[1] = t->y[0];
    t->x[2] = t->x[0] - 2;
    t->y[2] = t->y[0];
    (void)dir; /* the initial heading is always RIGHT */
}

static void trail_update(trail_t *t, int dir, int grew)
{
    int keep = grew ? t->len : t->len - 1;
    int i;

    for (i = keep; i > 0; --i) {
        t->x[i] = t->x[i - 1];
        t->y[i] = t->y[i - 1];
    }
    t->x[0] += k_dx[dir];
    t->y[0] += k_dy[dir];
    t->len = keep + 1;
}

static void trail_occ(const trail_t *t, uint8_t *occ)
{
    int i;

    memset(occ, 0, (size_t)SNAKE_MAX_CELLS);
    for (i = 0; i < t->len; ++i) {
        occ[t->y[i] * SNAKE_COLS + t->x[i]] = 1u;
    }
}

static int trail_matches_render(const trail_t *t, const uint8_t *m)
{
    uint8_t render[SNAKE_MAX_CELLS];
    int i;

    scan_body(m, render);
    for (i = 0; i < SNAKE_MAX_CELLS; ++i) {
        int on_trail = 0;
        int k;
        for (k = 0; k < t->len; ++k) {
            if (t->y[k] * SNAKE_COLS + t->x[k] == i) {
                on_trail = 1;
                break;
            }
        }
        if (render[i] != on_trail) {
            return 0;
        }
    }
    return 1;
}

/* =========================================================================
 * Tiny grid search used by the apple-chasing helpers
 * ====================================================================== */

static int bfs_first_step(const uint8_t *occ, int hx, int hy, int fx, int fy)
{
    static int16_t parent[SNAKE_MAX_CELLS];
    static uint8_t seen[SNAKE_MAX_CELLS];
    static int queue[SNAKE_MAX_CELLS];
    int qh = 0;
    int qt = 0;
    int start = hy * SNAKE_COLS + hx;
    int target = fy * SNAKE_COLS + fx;
    int i;

    if (occ[target]) {
        return -1;
    }
    memset(seen, 0, sizeof(seen));
    seen[start] = 1u;
    parent[start] = -1;
    queue[qt++] = start;

    while (qh < qt) {
        int cur = queue[qh++];
        int cx = cur % SNAKE_COLS;
        int cy = cur / SNAKE_COLS;
        int d;
        if (cur == target) {
            break;
        }
        for (d = 0; d < 4; ++d) {
            int nx = cx + k_dx[d];
            int ny = cy + k_dy[d];
            int ni;
            if (nx < 0 || nx >= SNAKE_COLS || ny < 0 || ny >= SNAKE_ROWS) {
                continue;
            }
            ni = ny * SNAKE_COLS + nx;
            if (seen[ni] || occ[ni]) {
                continue;
            }
            seen[ni] = 1u;
            parent[ni] = (int16_t)cur;
            queue[qt++] = ni;
        }
    }
    if (!seen[target]) {
        return -1;
    }
    i = target;
    while (parent[i] != start) {
        i = parent[i];
        if (i < 0) {
            return -1;
        }
    }
    {
        int x = i % SNAKE_COLS;
        int y = i / SNAKE_COLS;
        int d;
        for (d = 0; d < 4; ++d) {
            if (hx + k_dx[d] == x && hy + k_dy[d] == y) {
                return d;
            }
        }
    }
    return -1;
}

/* =========================================================================
 * 1. Initialisation
 * ====================================================================== */

static void test_init(void)
{
    snake_t g;
    snake_buffers_t b;
    const snake_status_t *st;

    memset(g_mono, 0xA5, sizeof(g_mono));
    memset(g_gray, 0xA5, sizeof(g_gray));

    b.mono = g_mono;
    b.gray4 = g_gray;
    b.gray4_page = 0;
    CHECK(snake_init(&g, &b, 42u));
    st = snake_status(&g);

    CHECK(st != NULL);
    CHECK_EQ(st->state, SNAKE_STATE_TITLE);
    CHECK_EQ(st->card, SNAKE_CARD_TITLE);
    CHECK_EQ(st->frame, SNAKE_FRAME_GRAY4);
    CHECK_EQ(st->score, 0);
    CHECK_EQ(st->high_score, 0);
    CHECK_EQ(st->tick, 0);
    CHECK_EQ(st->food_eaten, 0);
    /* uint8_t in the frozen header: 345 saturates to 255 */
    CHECK_EQ(st->cells_total, 255);
    CHECK_EQ(st->dir, SNAKE_DIR_RIGHT);
    CHECK_EQ(st->length, 3);
    CHECK_EQ(st->calibrated, false);
    CHECK_EQ(st->record_beaten, false);
    CHECK(snake_dirty(&g));
    CHECK(snake_needs_full_refresh(&g));

    /* gray4 staging page is the one NOT on the panel */
    CHECK(snake_gray4(&g) == g_gray + SNAKE_GRAY4_PAGE_BYTES);
    CHECK(snake_mono(&g) == g_mono);
    CHECK_EQ(snake_high_score(&g), 0);

    pump(&g);
    CHECK(!snake_dirty(&g));
    CHECK_EQ(g_push_frame, SNAKE_FRAME_GRAY4);
    CHECK(g_push_full);
    /* published page flipped: the composed page is now current */
    CHECK(snake_gray4(&g) == g_gray + SNAKE_GRAY4_PAGE_BYTES);
    CHECK(snake_needs_full_refresh(&g));

    /* page 1 argument asks for the other page */
    b.gray4_page = 1u;
    CHECK(snake_init(&g, &b, 7u));
    CHECK(snake_gray4(&g) == g_gray); /* staging is page 0 */

    /* rejected preconditions */
    CHECK(!snake_init(NULL, &b, 1u));
    CHECK(!snake_init(&g, NULL, 1u));
    b.mono = NULL;
    CHECK(!snake_init(&g, &b, 1u));
    b.mono = g_mono;
    b.gray4 = NULL;
    CHECK(!snake_init(&g, &b, 1u));
    b.gray4 = g_gray;
    b.gray4_page = 2u;
    CHECK(!snake_init(&g, &b, 1u));

    /* names */
    CHECK(strcmp(snake_state_name(SNAKE_STATE_PLAY), "PLAY") == 0);
    CHECK(strcmp(snake_card_name(SNAKE_CARD_PAUSED), "PAUSED") == 0);
    CHECK(strcmp(snake_sound_name(SNAKE_SOUND_EAT), "EAT") == 0);
    CHECK(strcmp(snake_card_name((snake_card_t)99), "?") == 0);
}

/* =========================================================================
 * 2. Movement, turning, reversal ignore
 * ====================================================================== */

static void test_movement_and_turn(void)
{
    snake_t g;
    const uint8_t *m;
    const snake_status_t *st;

    init_and_play(&g, g_mono, g_gray, 1001u);
    st = snake_status(&g);
    m = snake_mono(&g);
    CHECK_EQ(st->dir, SNAKE_DIR_RIGHT);
    CHECK(cell_snake(m, 7, 11));
    CHECK(cell_snake(m, 6, 11));
    CHECK(cell_snake(m, 5, 11));

    step(&g, 0, -1); /* keep heading, move one cell right */
    st = snake_status(&g);
    m = snake_mono(&g);
    CHECK_EQ(st->state, SNAKE_STATE_PLAY);
    CHECK_EQ(st->dir, SNAKE_DIR_RIGHT);
    CHECK_EQ(st->tick, 1);
    CHECK(cell_snake(m, 8, 11));
    CHECK(cell_snake(m, 7, 11));
    CHECK(cell_snake(m, 6, 11));
    CHECK(!cell_snake(m, 5, 11));

    step(&g, 0, SNAKE_DIR_UP);
    st = snake_status(&g);
    m = snake_mono(&g);
    CHECK_EQ(st->dir, SNAKE_DIR_UP);
    CHECK(cell_snake(m, 8, 10));
    CHECK(cell_snake(m, 8, 11));

    /* a 180-degree request must be ignored completely */
    step(&g, 0, SNAKE_DIR_DOWN);
    st = snake_status(&g);
    m = snake_mono(&g);
    CHECK_EQ(st->dir, SNAKE_DIR_UP);
    CHECK(cell_snake(m, 8, 9));
    CHECK(cell_snake(m, 8, 10));

    step(&g, 0, SNAKE_DIR_LEFT);
    st = snake_status(&g);
    m = snake_mono(&g);
    CHECK_EQ(st->dir, SNAKE_DIR_LEFT);
    CHECK(cell_snake(m, 7, 9));
    CHECK(cell_snake(m, 8, 9));
}

/* =========================================================================
 * 3. Eat, growth, score, apple relocation
 * ====================================================================== */

static void test_eat_grow(void)
{
    snake_t g;
    trail_t t;
    const snake_status_t *st;
    int fx = 0;
    int fy = 0;
    int steps = 0;
    int score0;
    uint8_t occ_render[SNAKE_MAX_CELLS];

    init_and_play(&g, g_mono, g_gray, 1001u);
    trail_init(&t, SNAKE_DIR_RIGHT);
    CHECK(trail_matches_render(&t, snake_mono(&g)));

    st = snake_status(&g);
    score0 = st->score;
    CHECK(scan_apple(snake_mono(&g), &fx, &fy));

    while (snake_status(&g)->score == score0 && steps < 200) {
        int d;
        int grew;
        uint8_t occ[SNAKE_MAX_CELLS];
        trail_occ(&t, occ);
        d = bfs_first_step(occ, t.x[0], t.y[0], fx, fy);
        CHECK(d >= 0);
        if (d < 0) {
            return;
        }
        step(&g, 0, (int8_t)d);
        grew = snake_status(&g)->score > score0;
        trail_update(&t, d, grew);
        ++steps;
    }

    st = snake_status(&g);
    CHECK_EQ(st->score, score0 + 10);
    CHECK_EQ(st->length, 4);
    CHECK_EQ(st->food_eaten, 1);
    CHECK_EQ(t.len, 4);
    CHECK(trail_matches_render(&t, snake_mono(&g)));
    CHECK(scan_apple(snake_mono(&g), &fx, &fy));

    /* the new apple is on a free cell */
    scan_body(snake_mono(&g), occ_render);
    CHECK(!occ_render[fy * SNAKE_COLS + fx]);
    CHECK_EQ(occ_render[t.y[0] * SNAKE_COLS + t.x[0]], 1);

}

/* =========================================================================
 * 4. Wall death -> DEAD -> GAME_OVER, sounds, high score
 * ====================================================================== */

static void drain_sounds(snake_t *g, int *eat, int *turn, int *start)
{
    snake_sound_t s;

    *eat = 0;
    *turn = 0;
    *start = 0;
    while ((s = snake_take_sound(g)) != SNAKE_SOUND_NONE) {
        if (s == SNAKE_SOUND_EAT) {
            ++*eat;
        } else if (s == SNAKE_SOUND_TURN) {
            ++*turn;
        } else if (s == SNAKE_SOUND_START) {
            ++*start;
        }
    }
}

static void test_wall_death(void)
{
    snake_t g;
    const snake_status_t *st;
    snake_sound_t s;
    int eat;
    int turn;
    int start;
    int i;

    init_and_play(&g, g_mono, g_gray, 2002u);
    drain_sounds(&g, &eat, &turn, &start);
    CHECK_EQ(start, 1);
    CHECK_EQ(turn, 0);

    /* head starts at x=7: seven moves reach x=14, the eighth hits the wall */
    for (i = 0; i < 7; ++i) {
        step(&g, 0, -1);
    }
    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_PLAY);
    CHECK(cell_snake(snake_mono(&g), 14, 11));

    step(&g, 0, -1);
    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_DEAD);
    CHECK_EQ(st->frame, SNAKE_FRAME_MONO);
    CHECK_EQ(st->card, SNAKE_CARD_NONE);
    /* the head did not teleport through the wall */
    CHECK(cell_snake(snake_mono(&g), 14, 11));
    s = snake_take_sound(&g);
    while (s != SNAKE_SOUND_NONE && s != SNAKE_SOUND_DEATH) {
        s = snake_take_sound(&g);
    }
    CHECK_EQ(s, SNAKE_SOUND_DEATH);

    step(&g, 0, -1); /* one tick in DEAD */
    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_GAME_OVER);
    CHECK_EQ(st->card, SNAKE_CARD_GAME_OVER);
    CHECK_EQ(st->frame, SNAKE_FRAME_GRAY4);
    CHECK(snake_needs_full_refresh(&g));
    s = snake_take_sound(&g);
    while (s != SNAKE_SOUND_NONE && s != SNAKE_SOUND_GAME_OVER) {
        s = snake_take_sound(&g);
    }
    CHECK_EQ(s, SNAKE_SOUND_GAME_OVER);

    /* OK on the game-over card returns to the title */
    step(&g, SNAKE_BTN_OK, -1);
    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_TITLE);
    CHECK_EQ(st->card, SNAKE_CARD_TITLE);
    CHECK_EQ(st->frame, SNAKE_FRAME_GRAY4);

    /* UP on the game-over card starts a fresh calibration */
    init_and_play(&g, g_mono, g_gray, 2002u);
    for (i = 0; i < 8; ++i) {
        step(&g, 0, -1);
    }
    step(&g, 0, -1); /* DEAD -> GAME_OVER */
    CHECK_EQ(snake_status(&g)->state, SNAKE_STATE_GAME_OVER);
    step(&g, SNAKE_BTN_UP, -1);
    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_CALIBRATING);
    CHECK_EQ(st->card, SNAKE_CARD_CALIBRATING);
    CHECK_EQ(st->calibrated, false);
}

/* =========================================================================
 * 5. Pause / resume
 * ====================================================================== */

static void test_pause_resume(void)
{
    snake_t g;
    const snake_status_t *st;
    const uint8_t *m;
    trail_t t;

    init_and_play(&g, g_mono, g_gray, 3003u);
    trail_init(&t, SNAKE_DIR_RIGHT);
    m = snake_mono(&g);
    CHECK(trail_matches_render(&t, m));

    step(&g, SNAKE_BTN_OK, -1);
    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_PAUSE);
    CHECK_EQ(st->card, SNAKE_CARD_PAUSED);
    CHECK_EQ(st->frame, SNAKE_FRAME_MONO);
    CHECK_EQ(st->tick, 0);
    {
        uint32_t pause_hash = g_push_hash;

        /* tilting while paused must not move the world or repaint */
        step(&g, 0, SNAKE_DIR_UP);
        st = snake_status(&g);
        CHECK_EQ(st->state, SNAKE_STATE_PAUSE);
        CHECK_EQ(st->dir, SNAKE_DIR_RIGHT);
        CHECK_EQ(st->tick, 0);
        CHECK_EQ(g_push_hash, pause_hash);
        CHECK(!snake_dirty(&g));
    }

    step(&g, SNAKE_BTN_OK, -1);
    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_PLAY);
    CHECK_EQ(st->card, SNAKE_CARD_NONE);
    CHECK_EQ(st->tick, 0);

    step(&g, 0, -1);
    st = snake_status(&g);
    CHECK_EQ(st->tick, 1);
    CHECK(cell_snake(snake_mono(&g), 8, 11));

    /* UP while paused restarts calibration */
    step(&g, SNAKE_BTN_OK, -1);
    CHECK_EQ(snake_status(&g)->state, SNAKE_STATE_PAUSE);
    step(&g, SNAKE_BTN_UP, -1);
    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_CALIBRATING);
    CHECK_EQ(st->card, SNAKE_CARD_CALIBRATING);
}

/* =========================================================================
 * 6. Calibration cards and the calibrated gate
 * ====================================================================== */

static void test_calibration(void)
{
    snake_t g;
    const snake_status_t *st;
    uint32_t h0;
    uint32_t h1;

    init_game(&g, g_mono, g_gray, 4004u);
    snake_calibration_progress(&g, 40); /* ignored outside CALIBRATING */
    CHECK_EQ(snake_status(&g)->calib_percent, 0);

    /* TITLE OK without a calibration must not start the run */
    step(&g, SNAKE_BTN_OK, -1);
    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_CALIBRATING);
    CHECK_EQ(st->card, SNAKE_CARD_CALIBRATING);
    CHECK_EQ(st->frame, SNAKE_FRAME_GRAY4);
    CHECK_EQ(st->calib_percent, 0);
    CHECK(snake_needs_full_refresh(&g));

    pump(&g);
    h0 = g_push_hash;
    CHECK(!snake_dirty(&g));

    snake_calibration_progress(&g, 40);
    CHECK_EQ(snake_status(&g)->calib_percent, 40);
    CHECK(!snake_dirty(&g)); /* progress alone does not repaint */

    step(&g, 0, -1); /* tick repaints the changed percentage */
    CHECK(snake_dirty(&g) == false); /* step() already pumped it */
    h1 = g_push_hash;
    CHECK(h0 != h1);
    CHECK_EQ(g_push_frame, SNAKE_FRAME_GRAY4);

    /* the same percentage again is a no-op */
    snake_calibration_progress(&g, 40);
    step(&g, 0, -1);
    CHECK_EQ(g_push_hash, h1);

    snake_calibration_progress(&g, 250); /* clamps */
    CHECK_EQ(snake_status(&g)->calib_percent, 100);
    step(&g, 0, -1);

    snake_calibration_done(&g, true);
    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_PLAY);
    CHECK_EQ(st->calibrated, true);
    CHECK_EQ(st->score, 0);
    CHECK(snake_take_sound(&g) == SNAKE_SOUND_CALIB_DONE);
    CHECK(snake_take_sound(&g) == SNAKE_SOUND_START);
    pump(&g);

    /* a failed calibration goes back to the title, still uncalibrated */
    snake_begin_calibration(&g);
    CHECK_EQ(snake_status(&g)->state, SNAKE_STATE_CALIBRATING);
    snake_calibration_done(&g, false);
    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_TITLE);
    CHECK_EQ(st->card, SNAKE_CARD_TITLE);
    CHECK_EQ(st->calibrated, false);
    CHECK(snake_take_sound(&g) == SNAKE_SOUND_CALIB_FAIL);

    /* snake_start_play falls back to calibration when uncalibrated */
    pump(&g);
    snake_start_play(&g);
    CHECK_EQ(snake_status(&g)->state, SNAKE_STATE_CALIBRATING);
}

/* =========================================================================
 * 7. Refresh handshake: dirty / needs_full / page flip / 20 fast frames
 * ====================================================================== */

static void test_refresh_handshake(void)
{
    snake_t g;
    const snake_status_t *st;
    int i;
    int fast_seen = 0;
    int saw_full = 0;

    init_game(&g, g_mono, g_gray, 5005u);
    st = snake_status(&g);
    CHECK_EQ(st->frame, SNAKE_FRAME_GRAY4);
    /* init_game() already pushed the title card */
    CHECK(!snake_dirty(&g));
    CHECK(snake_needs_full_refresh(&g));
    CHECK(snake_gray4(&g) == g_gray + SNAKE_GRAY4_PAGE_BYTES);
    pump(&g);
    CHECK(!snake_dirty(&g));
    CHECK(snake_needs_full_refresh(&g)); /* gray4 forces the next mono full */

    /* calibration card flips to page 0 */
    snake_begin_calibration(&g);
    CHECK(snake_gray4(&g) == g_gray);
    pump(&g);
    CHECK(snake_gray4(&g) == g_gray);

    /* start a run: first mono frame must be full */
    snake_calibration_done(&g, true);
    CHECK(snake_dirty(&g));
    CHECK_EQ(snake_status(&g)->frame, SNAKE_FRAME_MONO);
    CHECK(snake_needs_full_refresh(&g));
    pump(&g);
    CHECK(!snake_needs_full_refresh(&g));

    /* 20 fast mono frames (pause/resume toggles) request a full refresh */
    for (i = 0; i < 20; ++i) {
        pump(&g);
        snake_tick(&g, SNAKE_BTN_OK, -1);
        pump(&g);
        if (g_push_frame == SNAKE_FRAME_MONO) {
            if (g_push_full) {
                saw_full = 1;
            } else {
                ++fast_seen;
            }
        }
        if (snake_needs_full_refresh(&g)) {
            break;
        }
    }
    CHECK_EQ(fast_seen, 20);
    CHECK_EQ(saw_full, 0);
    CHECK(snake_needs_full_refresh(&g));

    /* the requested full refresh clears the flag and the counter */
    pump(&g);
    step(&g, SNAKE_BTN_OK, -1);
    if (g_push_full) {
        CHECK(!snake_needs_full_refresh(&g));
    }

    /* a gray4 card after mono forces the next mono to be full again */
    snake_begin_calibration(&g);
    CHECK(snake_needs_full_refresh(&g));
    pump(&g);
    CHECK(snake_needs_full_refresh(&g));
}

/* =========================================================================
 * 8. High score
 * ====================================================================== */

static void test_high_score(void)
{
    snake_t g;
    trail_t t;
    const snake_status_t *st;
    int fx = 0;
    int fy = 0;
    int i;

    init_game(&g, g_mono, g_gray, 6006u);
    snake_set_high_score(&g, 120);
    CHECK_EQ(snake_high_score(&g), 120);
    snake_set_high_score(&g, 50); /* monotonic: never lowers */
    CHECK_EQ(snake_high_score(&g), 120);
    pump(&g);

    /* a fresh core starts at zero, so the bar can be set low for one apple */
    init_game(&g, g_mono, g_gray, 6007u);
    CHECK_EQ(snake_high_score(&g), 0);
    snake_set_high_score(&g, 5);
    CHECK_EQ(snake_high_score(&g), 5);
    pump(&g);

    step(&g, SNAKE_BTN_OK, -1); /* TITLE -> CALIBRATING */
    snake_calibration_progress(&g, 100);
    step(&g, 0, -1);
    snake_calibration_done(&g, true);
    pump(&g);
    CHECK_EQ(snake_status(&g)->state, SNAKE_STATE_PLAY);

    trail_init(&t, SNAKE_DIR_RIGHT);
    CHECK(scan_apple(snake_mono(&g), &fx, &fy));
    for (i = 0; i < 200; ++i) {
        uint8_t occ[SNAKE_MAX_CELLS];
        int d;
        trail_occ(&t, occ);
        d = bfs_first_step(occ, t.x[0], t.y[0], fx, fy);
        CHECK(d >= 0);
        if (d < 0) {
            return;
        }
        step(&g, 0, (int8_t)d);
        trail_update(&t, d, 0);
        if (snake_status(&g)->score > 0) {
            break;
        }
    }
    st = snake_status(&g);
    CHECK_EQ(st->score, 10);
    CHECK_EQ(st->record_beaten, true);
    CHECK_EQ(snake_high_score(&g), 5); /* still the old mark while playing */

    /* now crash into a wall: high score is adopted on GAME_OVER */
    for (i = 0; i < 40 && snake_status(&g)->state == SNAKE_STATE_PLAY; ++i) {
        step(&g, 0, -1);
    }
    CHECK_EQ(snake_status(&g)->state, SNAKE_STATE_DEAD);
    step(&g, 0, -1);
    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_GAME_OVER);
    CHECK_EQ(st->high_score, 10);
    CHECK_EQ(snake_high_score(&g), 10);
    CHECK_EQ(st->record_beaten, true);

    /* monotonic even after the game */
    snake_set_high_score(&g, 7);
    CHECK_EQ(snake_high_score(&g), 10);
}

/* =========================================================================
 * 9. Self collision (needs length >= 5, so eat two apples first)
 * ====================================================================== */

static int chase_one_apple(snake_t *g, trail_t *t, int max_steps)
{
    const snake_status_t *st = snake_status(g);
    int fx = 0;
    int fy = 0;
    int score0 = st->score;
    int steps = 0;

    if (!scan_apple(snake_mono(g), &fx, &fy)) {
        return 0;
    }
    while (snake_status(g)->score == score0 && steps < max_steps) {
        uint8_t occ[SNAKE_MAX_CELLS];
        int d;
        int grew;
        trail_occ(t, occ);
        d = bfs_first_step(occ, t->x[0], t->y[0], fx, fy);
        if (d < 0) {
            return 0;
        }
        step(g, 0, (int8_t)d);
        grew = snake_status(g)->score > score0;
        trail_update(t, d, grew);
        ++steps;
    }
    return snake_status(g)->score == score0 + 10;
}

static void test_self_collision(void)
{
    snake_t g;
    trail_t t;
    const snake_status_t *st;
    int i;

    init_and_play(&g, g_mono, g_gray, 7007u);
    trail_init(&t, SNAKE_DIR_RIGHT);

    CHECK(chase_one_apple(&g, &t, 400));
    CHECK_EQ(snake_status(&g)->length, 4);
    CHECK(chase_one_apple(&g, &t, 400));
    CHECK_EQ(snake_status(&g)->length, 5);
    CHECK(trail_matches_render(&t, snake_mono(&g)));

    /* 3 right turns describe a 2x2 loop whose last cell is the body cell
     * right behind the head; with length 5 that is a self collision. */
    {
    int did_loop = 0;
    for (i = 0; i < 80 && snake_status(&g)->state == SNAKE_STATE_PLAY; ++i) {
        int dir = snake_status(&g)->dir;
        int turn = (dir + 1) & 3;
        int ax = t.x[0] + k_dx[turn];
        int ay = t.y[0] + k_dy[turn];
        int bx = ax + k_dx[(turn + 1) & 3];
        int by = ay + k_dy[(turn + 1) & 3];
        uint8_t occ[SNAKE_MAX_CELLS];
        int free_a;

        trail_occ(&t, occ);
        free_a = ax >= 0 && ax < SNAKE_COLS && ay >= 0 && ay < SNAKE_ROWS &&
                 !occ[ay * SNAKE_COLS + ax];
        {
            int free_b = bx >= 0 && bx < SNAKE_COLS && by >= 0 && by < SNAKE_ROWS &&
                         !occ[by * SNAKE_COLS + bx];
            if (free_a && free_b) {
                /* turn, turn, turn */
                int d1 = turn;
                int d2 = (turn + 1) & 3;
                int d3 = (turn + 2) & 3;
                int score0 = snake_status(&g)->score;
                step(&g, 0, (int8_t)d1);
                trail_update(&t, d1, snake_status(&g)->score > score0);
                score0 = snake_status(&g)->score;
                step(&g, 0, (int8_t)d2);
                trail_update(&t, d2, snake_status(&g)->score > score0);
                step(&g, 0, (int8_t)d3);
                did_loop = 1;
                break;
            }
        }
        /* no room for the loop here: drift straight and try again */
        step(&g, 0, (int8_t)dir);
        trail_update(&t, dir, 0);
    }
    CHECK(did_loop);
    }

    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_DEAD);
    CHECK(st->length >= 5);
    CHECK(st->tick > 0);

    step(&g, 0, -1);
    CHECK_EQ(snake_status(&g)->state, SNAKE_STATE_GAME_OVER);
    CHECK_EQ(snake_status(&g)->card, SNAKE_CARD_GAME_OVER);
}

/* =========================================================================
 * 10. Determinism: same seed + same inputs -> identical frames
 * ====================================================================== */

static int status_equal(const snake_status_t *a, const snake_status_t *b)
{
    return a->state == b->state && a->card == b->card && a->frame == b->frame &&
           a->dir == b->dir && a->length == b->length &&
           a->calib_percent == b->calib_percent &&
           a->cells_total == b->cells_total && a->score == b->score &&
           a->high_score == b->high_score && a->tick == b->tick &&
           a->food_eaten == b->food_eaten && a->calibrated == b->calibrated &&
           a->record_beaten == b->record_beaten;
}

static void test_determinism(void)
{
    snake_t a;
    snake_t b;
    int i;

    init_and_play(&a, g_mono, g_gray, 909090u);
    init_and_play(&b, g_mono_b, g_gray_b, 909090u);
    CHECK(status_equal(snake_status(&a), snake_status(&b)));
    CHECK_EQ(hash32(snake_mono(&a), SNAKE_MONO_BYTES),
             hash32(snake_mono(&b), SNAKE_MONO_BYTES));

    for (i = 0; i < 240; ++i) {
        uint8_t buttons = (i % 31 == 0) ? SNAKE_BTN_OK : 0u;
        int8_t tilt = (int8_t)((i * 5 + 1) % 4);

        step(&a, buttons, tilt);
        step(&b, buttons, tilt);
        CHECK(status_equal(snake_status(&a), snake_status(&b)));
        if (t_fail != 0) {
            break;
        }
    }

    /* replay a fresh pair with a plain right run and compare full frames */
    {
        uint32_t ha;
        uint32_t hb;

        init_and_play(&a, g_mono, g_gray, 123456u);
        init_and_play(&b, g_mono_b, g_gray_b, 123456u);
        ha = g_push_hash;
        hb = hash32(snake_mono(&b), SNAKE_MONO_BYTES);
        CHECK_EQ(ha, hb);

        for (i = 0; i < 5; ++i) {
            step(&a, 0, (int8_t)(i % 2 ? SNAKE_DIR_DOWN : -1));
        }
        init_and_play(&b, g_mono_b, g_gray_b, 123456u);
        for (i = 0; i < 5; ++i) {
            step(&b, 0, (int8_t)(i % 2 ? SNAKE_DIR_DOWN : -1));
        }
        CHECK_EQ(hash32(snake_mono(&a), SNAKE_MONO_BYTES),
                 hash32(snake_mono(&b), SNAKE_MONO_BYTES));
        CHECK(status_equal(snake_status(&a), snake_status(&b)));
    }

    /* different seeds must diverge (apple placement) */
    init_and_play(&a, g_mono, g_gray, 1u);
    init_and_play(&b, g_mono_b, g_gray_b, 2u);
    CHECK(hash32(snake_mono(&a), SNAKE_MONO_BYTES) !=
          hash32(snake_mono(&b), SNAKE_MONO_BYTES));
}

/* =========================================================================
 * 11. WIN
 *
 * The public API cannot fabricate a near-full board, and a genuine 342-apple
 * run is far too slow for a unit test. snake_test_setup_win() (internal test
 * support in snake.c, not part of snake.h) builds a legal one-apple-from-
 * victory board; the tick below then walks the real eat -> grow -> WIN path.
 * ====================================================================== */

extern bool snake_test_setup_win(snake_t *g);

static void test_win(void)
{
    snake_t g;
    const snake_status_t *st;

    init_game(&g, g_mono, g_gray, 777u);
    CHECK_EQ(snake_status(&g)->state, SNAKE_STATE_TITLE);
    CHECK(snake_test_setup_win(&g));
    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_PLAY);
    CHECK_EQ(st->frame, SNAKE_FRAME_MONO);
    CHECK_EQ(st->score, 3410);
    CHECK_EQ(st->food_eaten, 341);
    CHECK_EQ(st->dir, SNAKE_DIR_RIGHT);
    pump(&g);

    /* one tick: head enters (14,22), eats the 342nd apple, board is full */
    step(&g, 0, -1);
    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_GAME_OVER);
    CHECK_EQ(st->card, SNAKE_CARD_WIN);
    CHECK_EQ(st->frame, SNAKE_FRAME_GRAY4);
    CHECK_EQ(st->score, 3420);
    CHECK_EQ(st->food_eaten, 342);
    CHECK_EQ(st->high_score, 3420);
    CHECK_EQ(st->record_beaten, true);
    CHECK(snake_needs_full_refresh(&g));
    CHECK_EQ(snake_take_sound(&g), SNAKE_SOUND_EAT);
    CHECK_EQ(snake_take_sound(&g), SNAKE_SOUND_WIN);
    CHECK_EQ(snake_take_sound(&g), SNAKE_SOUND_NONE);

    /* the win page is the staging page and it is not a blank sheet */
    /* title flipped to page 1, the win card then flipped back to page 0 */
    CHECK(snake_gray4(&g) == g_gray);
    CHECK(g_push_frame == SNAKE_FRAME_GRAY4);
    CHECK(g_push_full);
    CHECK(g_push_hash != 0u);

    /* the high score is adopted and stays monotonic afterwards */
    snake_set_high_score(&g, 1000);
    CHECK_EQ(snake_high_score(&g), 3420);

    /* OK on the win card returns to the title, which now shows the record */
    step(&g, SNAKE_BTN_OK, -1);
    st = snake_status(&g);
    CHECK_EQ(st->state, SNAKE_STATE_TITLE);
    CHECK_EQ(st->card, SNAKE_CARD_TITLE);
    CHECK_EQ(st->high_score, 3420);
    CHECK_EQ(st->record_beaten, true); /* cleared by the next start_run */

    /* the card renderer itself is pure: the win status can be re-rendered */
    CHECK(!snake_test_setup_win(NULL));
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    printf("gyro-snake core tests\n");
    test_init();
    test_movement_and_turn();
    test_eat_grow();
    test_wall_death();
    test_pause_resume();
    test_calibration();
    test_refresh_handshake();
    test_high_score();
    test_self_collision();
    test_determinism();
    test_win();

    printf("TESTS passed=%d failed=%d\n", t_run - t_fail, t_fail);
    return (t_fail == 0) ? 0 : 1;
}