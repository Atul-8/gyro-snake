/*
 * GYRO SNAKE — portable game core.
 *
 * Copyright (c) 2026 GYRO SNAKE contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implements the frozen contract in snake.h. The whole game state lives inside
 * the caller-owned snake_t union; no allocation, no I/O, no wall clock and no
 * libc rand are used, so a session replays byte-identically from the same seed
 * and the same input sequence.
 *
 * Refresh policy
 *   - a gray4 card is always a full refresh, and it forces the next mono frame
 *     to be full as well (the panel needs a clean base after grayscale);
 *   - 20 consecutive fast mono refreshes force one full mono refresh;
 *   - the very first frame is full.
 */

#include <stdbool.h>
#include <stdint.h>

#include "snake.h"

/* render.c */
extern void snake_render_play(uint8_t *mono, const snake_status_t *st,
                              const uint8_t *body_xy, uint16_t length,
                              uint8_t food_x, uint8_t food_y, uint8_t state);
extern void snake_render_card(uint8_t *page, snake_card_t card,
                              const snake_status_t *st);

/* rng.c */
extern void snake_rng_seed(uint32_t *state, uint32_t seed);
extern uint32_t snake_rng_next(uint32_t *state);

#define SNAKE_MAGIC 0x47534E4Bu /* "GSNK" */

#define SNAKE_FAST_LIMIT 20
#define SNAKE_SOUND_DEPTH 8
#define SNAKE_SCORE_MAX 9999u
#define SNAKE_SCORE_STEP 10u

#define F_DIRTY  0x01u
#define F_FULL   0x02u
#define F_CALIB  0x04u
#define F_RECORD 0x08u
#define F_WON    0x10u

typedef struct {
    uint8_t x;
    uint8_t y;
} snake_cell_t;

typedef struct {
    uint32_t magic;
    uint32_t rng;
    uint16_t score;
    uint16_t high;
    uint16_t tick;
    uint16_t eaten;
    uint16_t fast_count;
    uint8_t state;
    uint8_t card;
    uint8_t frame;
    uint8_t dir;
    uint16_t length; /* board has 345 cells; the public status field is 8-bit */
    uint8_t calib;
    uint8_t calib_shown;
    uint8_t gray4_page;
    uint8_t flags;
    uint8_t food_x;
    uint8_t food_y;
    uint8_t snd[SNAKE_SOUND_DEPTH];
    uint8_t snd_head;
    uint8_t snd_len;
    uint8_t *mono;
    uint8_t *gray4;
    snake_cell_t body[SNAKE_MAX_CELLS];
    snake_status_t status;
} snake_impl_t;

typedef char snake_impl_size_check[(sizeof(snake_impl_t) <= SNAKE_STATE_BYTES) ? 1 : -1];

static const int8_t k_dx[4] = { 0, 1, 0, -1 };
static const int8_t k_dy[4] = { -1, 0, 1, 0 };

static snake_impl_t *impl_of(snake_t *g)
{
    return (snake_impl_t *)(void *)g->bytes;
}

static const snake_impl_t *impl_of_c(const snake_t *g)
{
    return (const snake_impl_t *)(const void *)g->bytes;
}

static bool impl_ok(const snake_t *g)
{
    return g != 0 && impl_of_c(g)->magic == SNAKE_MAGIC;
}

static bool is_reverse(int from, int to)
{
    return (int)((from + 2) & 3) == to;
}

static bool body_has(const snake_impl_t *s, int x, int y)
{
    int i;

    for (i = 0; i < (int)s->length; ++i) {
        if ((int)s->body[i].x == x && (int)s->body[i].y == y) {
            return true;
        }
    }
    return false;
}

static void push_sound(snake_impl_t *s, snake_sound_t sound)
{
    if (s->snd_len >= SNAKE_SOUND_DEPTH) {
        return;
    }
    s->snd[(s->snd_head + s->snd_len) % SNAKE_SOUND_DEPTH] = (uint8_t)sound;
    ++s->snd_len;
}

static uint8_t *gray4_staging(snake_impl_t *s)
{
    return s->gray4 + (size_t)(1u - s->gray4_page) * SNAKE_GRAY4_PAGE_BYTES;
}

static void update_status(snake_impl_t *s)
{
    s->status.state = (snake_state_t)s->state;
    s->status.card = (snake_card_t)s->card;
    s->status.frame = (snake_frame_t)s->frame;
    s->status.dir = (snake_dir_t)(int8_t)s->dir;
    s->status.length = (uint8_t)(s->length > 255u ? 255u : s->length);
    s->status.calib_percent = s->calib;
    /* The frozen header types cells_total as uint8_t; 345 does not fit, so
     * saturate rather than wrap (the HUD reads the real constant). */
    s->status.cells_total = (SNAKE_MAX_CELLS > 255) ? 255u : (uint8_t)SNAKE_MAX_CELLS;
    s->status.score = s->score;
    s->status.high_score = s->high;
    s->status.tick = s->tick;
    s->status.food_eaten = s->eaten;
    s->status.calibrated = (s->flags & F_CALIB) != 0u;
    s->status.record_beaten = (s->flags & F_RECORD) != 0u;
}

static void compose_mono(snake_impl_t *s)
{
    s->frame = (uint8_t)SNAKE_FRAME_MONO;
    update_status(s);
    snake_render_play(s->mono, &s->status, (const uint8_t *)s->body,
                      s->length, s->food_x, s->food_y, s->state);
    s->flags |= F_DIRTY;
}

static void compose_card(snake_impl_t *s, snake_card_t card)
{
    s->card = (uint8_t)card;
    s->frame = (uint8_t)SNAKE_FRAME_GRAY4;
    update_status(s);
    snake_render_card(gray4_staging(s), card, &s->status);
    s->flags |= F_DIRTY | F_FULL;
}

static void reset_body(snake_impl_t *s)
{
    s->length = 3;
    s->dir = (uint8_t)SNAKE_DIR_RIGHT;
    s->body[0].x = (uint8_t)(SNAKE_COLS / 2);
    s->body[0].y = (uint8_t)(SNAKE_ROWS / 2);
    s->body[1].x = (uint8_t)(SNAKE_COLS / 2 - 1);
    s->body[1].y = (uint8_t)(SNAKE_ROWS / 2);
    s->body[2].x = (uint8_t)(SNAKE_COLS / 2 - 2);
    s->body[2].y = (uint8_t)(SNAKE_ROWS / 2);
}

/* Uniform pick among the cells the body does not occupy. */
static void place_food(snake_impl_t *s)
{
    int free_n = SNAKE_MAX_CELLS - (int)s->length;
    uint32_t pick;
    int x;
    int y;

    s->food_x = 0xFFu;
    s->food_y = 0xFFu;
    if (free_n <= 0) {
        return;
    }
    pick = snake_rng_next(&s->rng) % (uint32_t)free_n;

    for (y = 0; y < SNAKE_ROWS; ++y) {
        for (x = 0; x < SNAKE_COLS; ++x) {
            if (!body_has(s, x, y)) {
                if (pick == 0u) {
                    s->food_x = (uint8_t)x;
                    s->food_y = (uint8_t)y;
                    return;
                }
                --pick;
            }
        }
    }
}

static void update_high(snake_impl_t *s)
{
    if (s->score > s->high) {
        s->high = s->score;
    }
}

static void start_run(snake_impl_t *s)
{
    reset_body(s);
    s->score = 0;
    s->tick = 0;
    s->eaten = 0;
    s->state = (uint8_t)SNAKE_STATE_PLAY;
    s->card = (uint8_t)SNAKE_CARD_NONE;
    s->flags &= (uint8_t)~(F_RECORD | F_WON);
    place_food(s);
    push_sound(s, SNAKE_SOUND_START);
    compose_mono(s);
}

static void die(snake_impl_t *s)
{
    push_sound(s, SNAKE_SOUND_DEATH);
    s->state = (uint8_t)SNAKE_STATE_DEAD;
    s->card = (uint8_t)SNAKE_CARD_NONE;
    compose_mono(s);
}

static void win(snake_impl_t *s)
{
    update_high(s);
    push_sound(s, SNAKE_SOUND_WIN);
    s->state = (uint8_t)SNAKE_STATE_GAME_OVER;
    s->card = (uint8_t)SNAKE_CARD_WIN;
    s->flags |= F_WON;
    compose_card(s, SNAKE_CARD_WIN);
}

static void enter_game_over(snake_impl_t *s)
{
    update_high(s);
    push_sound(s, SNAKE_SOUND_GAME_OVER);
    s->state = (uint8_t)SNAKE_STATE_GAME_OVER;
    s->card = (uint8_t)SNAKE_CARD_GAME_OVER;
    compose_card(s, SNAKE_CARD_GAME_OVER);
}

static void tick_play(snake_impl_t *s, uint8_t buttons, int8_t tilt_dir)
{
    int nx;
    int ny;
    int grow;
    int i;

    (void)buttons; /* OK in PLAY is handled by the caller */

    if (tilt_dir >= 0 && tilt_dir <= 3) {
        int want = (int)tilt_dir;
        if (!is_reverse((int)s->dir, want) && want != (int)s->dir) {
            s->dir = (uint8_t)want;
            push_sound(s, SNAKE_SOUND_TURN);
        }
    }

    ++s->tick;
    s->card = (uint8_t)SNAKE_CARD_NONE;

    nx = (int)s->body[0].x + (int)k_dx[s->dir];
    ny = (int)s->body[0].y + (int)k_dy[s->dir];

    if (nx < 0 || nx >= SNAKE_COLS || ny < 0 || ny >= SNAKE_ROWS) {
        die(s);
        return;
    }

    grow = (nx == (int)s->food_x && ny == (int)s->food_y);
    /* The tail vacates its cell on a non-growing move, so it is not an
     * obstacle then. */
    for (i = 0; i < (int)s->length - (grow ? 0 : 1); ++i) {
        if ((int)s->body[i].x == nx && (int)s->body[i].y == ny) {
            die(s);
            return;
        }
    }

    if (grow) {
        for (i = (int)s->length; i > 0; --i) {
            s->body[i] = s->body[i - 1];
        }
        ++s->length;
    } else {
        for (i = (int)s->length - 1; i > 0; --i) {
            s->body[i] = s->body[i - 1];
        }
    }
    s->body[0].x = (uint8_t)nx;
    s->body[0].y = (uint8_t)ny;

    if (grow) {
        push_sound(s, SNAKE_SOUND_EAT);
        ++s->eaten;
        if (s->score > SNAKE_SCORE_MAX - SNAKE_SCORE_STEP) {
            s->score = (uint16_t)SNAKE_SCORE_MAX;
        } else {
            s->score = (uint16_t)(s->score + SNAKE_SCORE_STEP);
        }
        if (s->score > s->high) {
            s->flags |= F_RECORD;
        }
        if (s->length == (uint16_t)SNAKE_MAX_CELLS) {
            win(s);
            return;
        }
        place_food(s);
    }

    compose_mono(s);
}

/* =========================================================================
 * Public API
 * ====================================================================== */

bool snake_init(snake_t *g, const snake_buffers_t *bufs, uint32_t seed)
{
    snake_impl_t *s;
    int i;

    if (g == 0 || bufs == 0 || bufs->mono == 0 || bufs->gray4 == 0 ||
        bufs->gray4_page > 1u) {
        return false;
    }

    for (i = 0; i < SNAKE_STATE_BYTES; ++i) {
        g->bytes[i] = 0;
    }

    s = impl_of(g);
    s->magic = SNAKE_MAGIC;
    s->mono = bufs->mono;
    s->gray4 = bufs->gray4;
    s->gray4_page = (uint8_t)(bufs->gray4_page & 1u);
    snake_rng_seed(&s->rng, seed);

    s->state = (uint8_t)SNAKE_STATE_TITLE;
    s->card = (uint8_t)SNAKE_CARD_TITLE;
    s->frame = (uint8_t)SNAKE_FRAME_GRAY4;
    s->flags = 0;
    s->score = 0;
    s->high = 0;
    s->tick = 0;
    s->eaten = 0;
    s->fast_count = 0;
    s->calib = 0;
    s->calib_shown = 0;
    s->snd_head = 0;
    s->snd_len = 0;

    reset_body(s);
    place_food(s);
    compose_card(s, SNAKE_CARD_TITLE);
    update_status(s);
    return true;
}

void snake_set_high_score(snake_t *g, uint16_t high_score)
{
    snake_impl_t *s;

    if (!impl_ok(g)) {
        return;
    }
    s = impl_of(g);
    if (high_score <= s->high) {
        return;
    }
    s->high = high_score;
    if (s->state == (uint8_t)SNAKE_STATE_TITLE) {
        compose_card(s, SNAKE_CARD_TITLE);
    } else {
        update_status(s);
    }
}

uint16_t snake_high_score(const snake_t *g)
{
    if (!impl_ok(g)) {
        return 0;
    }
    return impl_of_c(g)->high;
}

void snake_begin_calibration(snake_t *g)
{
    snake_impl_t *s;

    if (!impl_ok(g)) {
        return;
    }
    s = impl_of(g);
    s->state = (uint8_t)SNAKE_STATE_CALIBRATING;
    s->card = (uint8_t)SNAKE_CARD_CALIBRATING;
    s->calib = 0;
    s->calib_shown = 0;
    s->flags &= (uint8_t)~F_CALIB;
    compose_card(s, SNAKE_CARD_CALIBRATING);
}

void snake_calibration_progress(snake_t *g, uint8_t percent)
{
    snake_impl_t *s;

    if (!impl_ok(g)) {
        return;
    }
    s = impl_of(g);
    if (s->state != (uint8_t)SNAKE_STATE_CALIBRATING) {
        return;
    }
    s->calib = (percent > 100u) ? 100u : percent;
    update_status(s);
    /* tick() recomposes the card when the displayed value changes, so a
     * 100 Hz sampling loop does not repaint 100 times a second. */
}

void snake_calibration_done(snake_t *g, bool ok)
{
    snake_impl_t *s;

    if (!impl_ok(g)) {
        return;
    }
    s = impl_of(g);
    if (s->state != (uint8_t)SNAKE_STATE_CALIBRATING) {
        return;
    }
    if (ok) {
        s->flags |= F_CALIB;
        s->calib = 100;
        s->calib_shown = 100;
        push_sound(s, SNAKE_SOUND_CALIB_DONE);
        start_run(s);
    } else {
        s->flags &= (uint8_t)~F_CALIB;
        push_sound(s, SNAKE_SOUND_CALIB_FAIL);
        s->state = (uint8_t)SNAKE_STATE_TITLE;
        s->card = (uint8_t)SNAKE_CARD_TITLE;
        compose_card(s, SNAKE_CARD_TITLE);
    }
}

void snake_start_play(snake_t *g)
{
    snake_impl_t *s;

    if (!impl_ok(g)) {
        return;
    }
    s = impl_of(g);
    if ((s->flags & F_CALIB) == 0u) {
        snake_begin_calibration(g);
        return;
    }
    start_run(s);
}

void snake_tick(snake_t *g, uint8_t buttons, int8_t tilt_dir)
{
    snake_impl_t *s;

    if (!impl_ok(g)) {
        return;
    }
    s = impl_of(g);
    if ((s->flags & F_DIRTY) != 0u) {
        /* Contract violation: a frame is still pending. Never overwrite it. */
        return;
    }

    switch ((snake_state_t)s->state) {
    case SNAKE_STATE_TITLE:
        if ((buttons & SNAKE_BTN_OK) != 0u) {
            if ((s->flags & F_CALIB) != 0u) {
                start_run(s);
            } else {
                snake_begin_calibration(g);
            }
        }
        break;

    case SNAKE_STATE_CALIBRATING:
        if (s->calib != s->calib_shown) {
            s->calib_shown = s->calib;
            compose_card(s, SNAKE_CARD_CALIBRATING);
        }
        break;

    case SNAKE_STATE_PLAY:
        if ((buttons & SNAKE_BTN_OK) != 0u) {
            s->state = (uint8_t)SNAKE_STATE_PAUSE;
            s->card = (uint8_t)SNAKE_CARD_PAUSED;
            compose_mono(s);
        } else {
            tick_play(s, buttons, tilt_dir);
        }
        break;

    case SNAKE_STATE_PAUSE:
        if ((buttons & SNAKE_BTN_OK) != 0u) {
            s->state = (uint8_t)SNAKE_STATE_PLAY;
            s->card = (uint8_t)SNAKE_CARD_NONE;
            compose_mono(s);
        } else if ((buttons & SNAKE_BTN_UP) != 0u) {
            snake_begin_calibration(g);
        }
        break;

    case SNAKE_STATE_DEAD:
        enter_game_over(s);
        break;

    case SNAKE_STATE_GAME_OVER:
        if ((buttons & SNAKE_BTN_OK) != 0u) {
            s->state = (uint8_t)SNAKE_STATE_TITLE;
            s->card = (uint8_t)SNAKE_CARD_TITLE;
            compose_card(s, SNAKE_CARD_TITLE);
        } else if ((buttons & SNAKE_BTN_UP) != 0u) {
            snake_begin_calibration(g);
        }
        break;

    default:
        break;
    }
}

const uint8_t *snake_mono(const snake_t *g)
{
    if (!impl_ok(g)) {
        return 0;
    }
    return impl_of_c(g)->mono;
}

const uint8_t *snake_gray4(const snake_t *g)
{
    const snake_impl_t *s;

    if (!impl_ok(g)) {
        return 0;
    }
    s = impl_of_c(g);
    if ((s->flags & F_DIRTY) != 0u && s->frame == (uint8_t)SNAKE_FRAME_GRAY4) {
        return s->gray4 + (size_t)(1u - s->gray4_page) * SNAKE_GRAY4_PAGE_BYTES;
    }
    return s->gray4 + (size_t)s->gray4_page * SNAKE_GRAY4_PAGE_BYTES;
}

const snake_status_t *snake_status(const snake_t *g)
{
    if (!impl_ok(g)) {
        return 0;
    }
    return &impl_of_c(g)->status;
}

bool snake_dirty(const snake_t *g)
{
    return impl_ok(g) && (impl_of_c(g)->flags & F_DIRTY) != 0u;
}

bool snake_needs_full_refresh(const snake_t *g)
{
    return impl_ok(g) && (impl_of_c(g)->flags & F_FULL) != 0u;
}

void snake_mark_refreshed(snake_t *g, bool was_full)
{
    snake_impl_t *s;

    if (!impl_ok(g)) {
        return;
    }
    s = impl_of(g);
    if ((s->flags & F_DIRTY) == 0u) {
        return;
    }

    if (s->frame == (uint8_t)SNAKE_FRAME_GRAY4) {
        /* Publish the staging page; gray4 is always a full panel update. */
        s->gray4_page = (uint8_t)(1u - s->gray4_page);
        s->flags &= (uint8_t)~F_DIRTY;
        s->flags |= F_FULL;
        s->fast_count = 0;
    } else {
        s->flags &= (uint8_t)~F_DIRTY;
        if (was_full) {
            s->flags &= (uint8_t)~F_FULL;
            s->fast_count = 0;
        } else {
            if (s->fast_count < SNAKE_FAST_LIMIT) {
                ++s->fast_count;
            }
            if (s->fast_count >= SNAKE_FAST_LIMIT) {
                s->flags |= F_FULL;
            }
        }
    }
    update_status(s);
}

snake_sound_t snake_take_sound(snake_t *g)
{
    snake_impl_t *s;
    snake_sound_t sound;

    if (!impl_ok(g)) {
        return SNAKE_SOUND_NONE;
    }
    s = impl_of(g);
    if (s->snd_len == 0u) {
        return SNAKE_SOUND_NONE;
    }
    sound = (snake_sound_t)s->snd[s->snd_head];
    s->snd_head = (uint8_t)((s->snd_head + 1u) % SNAKE_SOUND_DEPTH);
    --s->snd_len;
    return sound;
}

/* =========================================================================
 * Test support (NOT part of the frozen snake.h contract)
 *
 * A real winning run needs 342 random apples, which is far too slow for a
 * host unit test. This helper builds a legal "one apple from victory" board:
 * 344 body cells (every cell except the head and the apple), the head at
 * (13,22) facing right, the apple at (14,22), score/eaten consistent with 341
 * apples already eaten. One normal tick then exercises the real eat -> grow ->
 * WIN transition, high-score adoption, WIN card and jingle.
 *
 * The firmware never calls this; only tests/test_snake.c does.
 * ====================================================================== */
bool snake_test_setup_win(snake_t *g)
{
    snake_impl_t *s;
    int x;
    int y;

    if (!impl_ok(g)) {
        return false;
    }
    s = impl_of(g);

    s->length = 1;
    s->body[0].x = 13;
    s->body[0].y = 22;
    for (y = 0; y < SNAKE_ROWS; ++y) {
        for (x = 0; x < SNAKE_COLS; ++x) {
            if ((x == 13 && y == 22) || (x == 14 && y == 22)) {
                continue;
            }
            s->body[s->length].x = (uint8_t)x;
            s->body[s->length].y = (uint8_t)y;
            ++s->length;
        }
    }
    if (s->length != (uint16_t)(SNAKE_MAX_CELLS - 1)) {
        return false;
    }

    s->dir = (uint8_t)SNAKE_DIR_RIGHT;
    s->food_x = 14;
    s->food_y = 22;
    s->score = 3410;
    s->eaten = 341;
    s->high = 0;
    s->tick = 500;
    s->state = (uint8_t)SNAKE_STATE_PLAY;
    s->card = (uint8_t)SNAKE_CARD_NONE;
    s->flags = F_CALIB | F_RECORD;
    s->snd_head = 0;
    s->snd_len = 0;
    compose_mono(s);
    return true;
}

/* =========================================================================
 * Names
 * ====================================================================== */

const char *snake_state_name(snake_state_t state)
{
    switch (state) {
    case SNAKE_STATE_TITLE:       return "TITLE";
    case SNAKE_STATE_CALIBRATING: return "CALIBRATING";
    case SNAKE_STATE_PLAY:        return "PLAY";
    case SNAKE_STATE_PAUSE:       return "PAUSE";
    case SNAKE_STATE_DEAD:        return "DEAD";
    case SNAKE_STATE_GAME_OVER:   return "GAME_OVER";
    default:                      return "?";
    }
}

const char *snake_card_name(snake_card_t card)
{
    switch (card) {
    case SNAKE_CARD_NONE:        return "NONE";
    case SNAKE_CARD_TITLE:       return "TITLE";
    case SNAKE_CARD_CALIBRATING: return "CALIBRATING";
    case SNAKE_CARD_PAUSED:      return "PAUSED";
    case SNAKE_CARD_GAME_OVER:   return "GAME_OVER";
    case SNAKE_CARD_WIN:         return "WIN";
    default:                     return "?";
    }
}

const char *snake_sound_name(snake_sound_t sound)
{
    switch (sound) {
    case SNAKE_SOUND_NONE:        return "NONE";
    case SNAKE_SOUND_EAT:         return "EAT";
    case SNAKE_SOUND_TURN:        return "TURN";
    case SNAKE_SOUND_DEATH:       return "DEATH";
    case SNAKE_SOUND_START:       return "START";
    case SNAKE_SOUND_CALIB_DONE:  return "CALIB_DONE";
    case SNAKE_SOUND_CALIB_FAIL:  return "CALIB_FAIL";
    case SNAKE_SOUND_GAME_OVER:   return "GAME_OVER";
    case SNAKE_SOUND_WIN:         return "WIN";
    default:                      return "?";
    }
}