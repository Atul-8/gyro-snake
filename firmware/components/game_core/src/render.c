/*
 * GYRO SNAKE — framebuffer painter.
 *
 * Copyright (c) 2026 GYRO SNAKE contributors.
 * SPDX-License-Identifier: MIT
 *
 * Everything here is pure pixel pushing into caller-owned buffers:
 *   mono   480x800, 1 bpp, 60 B/row, MSB leftmost, 1 = white, 0 = black.
 *   gray4  480x800, 2 bpp, 120 B/row, 0 = black .. 3 = white (one page).
 *
 * No libc, no allocation, no floating point: the same code runs on the host
 * simulator and on the ESP32-S3 and draws identical pixels.
 *
 * Visual language
 *   - paper-white sheet, black ink, no gradients (e-paper friendly);
 *   - snake segments are 28x28 px inside each 32 px cell (2 px inset);
 *   - the head carries two eyes that point where the snake is going;
 *   - the apple is a round berry with a stem, a leaf and one highlight;
 *   - cards use level 2 gray for hairlines and level 1 for secondary text.
 */

#include <stdbool.h>
#include <stdint.h>

#include "snake.h"

/* font.c */
extern const uint8_t *font5x7_glyph(char c);

#define GLYPH_W 5
#define GLYPH_H 7
#define GLYPH_ADV 6 /* 5 columns + 1 column gap */

/* =========================================================================
 * Small text helpers
 * ====================================================================== */

static int str_len(const char *s)
{
    int n = 0;

    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

/* Right-aligned zero padded unsigned value, at most 5 digits. */
static void fmt_u16(char *dst, uint16_t v, int width)
{
    int i;

    if (width < 1) {
        width = 1;
    }
    if (width > 5) {
        width = 5;
    }
    for (i = width - 1; i >= 0; --i) {
        dst[i] = (char)('0' + (int)(v % 10u));
        v = (uint16_t)(v / 10u);
    }
    dst[width] = '\0';
}

static void fmt_udec(char *dst, uint16_t v)
{
    char tmp[6];
    int n = 0;
    int i;

    if (v == 0u) {
        dst[0] = '0';
        dst[1] = (char)0;
        return;
    }
    while (v > 0u) {
        tmp[n++] = (char)('0' + (int)(v % 10u));
        v = (uint16_t)(v / 10u);
    }
    for (i = 0; i < n; ++i) {
        dst[i] = tmp[n - 1 - i];
    }
    dst[n] = (char)0;
}

static int text_width(const char *s, int scale)
{
    int n = str_len(s);

    if (n <= 0) {
        return 0;
    }
    return (n * GLYPH_ADV - 1) * scale;
}

/* =========================================================================
 * Mono canvas
 * ====================================================================== */

static void mono_clear(uint8_t *b, int white)
{
    uint8_t v = white ? 0xFFu : 0x00u;
    int i;

    for (i = 0; i < SNAKE_MONO_BYTES; ++i) {
        b[i] = v;
    }
}

static void mono_px(uint8_t *b, int x, int y, int white)
{
    uint8_t *p;
    uint8_t m;

    if (x < 0 || y < 0 || x >= SNAKE_SCREEN_W || y >= SNAKE_SCREEN_H) {
        return;
    }
    p = b + y * SNAKE_MONO_ROW_BYTES + (x >> 3);
    m = (uint8_t)(0x80u >> (x & 7));
    if (white) {
        *p |= m;
    } else {
        *p &= (uint8_t)~m;
    }
}

/* Clipped solid rectangle; one pass per row with byte masks. */
static void mono_rect(uint8_t *b, int x, int y, int w, int h, int white)
{
    int yy;

    if (w <= 0 || h <= 0) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > SNAKE_SCREEN_W) {
        w = SNAKE_SCREEN_W - x;
    }
    if (y + h > SNAKE_SCREEN_H) {
        h = SNAKE_SCREEN_H - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    for (yy = y; yy < y + h; ++yy) {
        uint8_t *row = b + yy * SNAKE_MONO_ROW_BYTES;
        int x0 = x;
        int x1 = x + w - 1;
        int b0 = x0 >> 3;
        int b1 = x1 >> 3;
        uint8_t m0 = (uint8_t)(0xFFu >> (x0 & 7));
        uint8_t m1 = (uint8_t)(0xFFu << (7 - (x1 & 7)));

        if (b0 == b1) {
            uint8_t m = (uint8_t)(m0 & m1);
            if (white) {
                row[b0] |= m;
            } else {
                row[b0] &= (uint8_t)~m;
            }
        } else {
            int k;
            if (white) {
                row[b0] |= m0;
                row[b1] |= m1;
            } else {
                row[b0] &= (uint8_t)~m0;
                row[b1] &= (uint8_t)~m1;
            }
            for (k = b0 + 1; k < b1; ++k) {
                row[k] = white ? 0xFFu : 0x00u;
            }
        }
    }
}

static void mono_text(uint8_t *b, int x, int y, const char *s, int scale, int white)
{
    int cx = x;

    for (; *s != '\0'; ++s) {
        const uint8_t *g = font5x7_glyph(*s);
        int c;

        for (c = 0; c < GLYPH_W; ++c) {
            uint8_t col = g[c];
            int r;
            for (r = 0; r < GLYPH_H; ++r) {
                if ((col & (uint8_t)(1u << r)) != 0u) {
                    mono_rect(b, cx + c * scale, y + r * scale, scale, scale, white);
                }
            }
        }
        cx += GLYPH_ADV * scale;
    }
}

static void mono_text_centered(uint8_t *b, int y, const char *s, int scale, int white)
{
    mono_text(b, (SNAKE_SCREEN_W - text_width(s, scale)) / 2, y, s, scale, white);
}

/* Filled ellipse; used for the apple leaf. */
static void mono_ellipse(uint8_t *b, int cx, int cy, int rx, int ry, int white)
{
    int dy;

    if (rx <= 0 || ry <= 0) {
        return;
    }
    for (dy = -ry; dy <= ry; ++dy) {
        int dx;
        int lim;
        /* dx^2 / rx^2 + dy^2 / ry^2 <= 1 */
        int lhs = rx * rx * ry * ry - dy * dy * rx * rx;
        if (lhs < 0) {
            continue;
        }
        /* dx <= sqrt(lhs) / ry */
        lim = 0;
        while ((lim + 1) * (lim + 1) * ry * ry <= lhs) {
            ++lim;
        }
        for (dx = -lim; dx <= lim; ++dx) {
            mono_px(b, cx + dx, cy + dy, white);
        }
    }
}

static void mono_disc(uint8_t *b, int cx, int cy, int r, int white)
{
    int dy;

    for (dy = -r; dy <= r; ++dy) {
        int dx;
        for (dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy <= r * r) {
                mono_px(b, cx + dx, cy + dy, white);
            }
        }
    }
}

/* =========================================================================
 * Gray4 canvas
 * ====================================================================== */

static void g4_clear(uint8_t *p, int level)
{
    uint8_t v = (uint8_t)((level & 3) * 0x55);
    int i;

    for (i = 0; i < SNAKE_GRAY4_PAGE_BYTES; ++i) {
        p[i] = v;
    }
}

static void g4_px(uint8_t *p, int x, int y, int level)
{
    uint8_t *q;
    int sh;

    if (x < 0 || y < 0 || x >= SNAKE_SCREEN_W || y >= SNAKE_SCREEN_H) {
        return;
    }
    q = p + y * SNAKE_GRAY4_ROW_BYTES + (x >> 2);
    sh = 6 - 2 * (x & 3);
    *q = (uint8_t)((*q & (uint8_t)~(3u << sh)) | (uint8_t)((level & 3) << sh));
}

static void g4_rect(uint8_t *p, int x, int y, int w, int h, int level)
{
    int yy;

    for (yy = y; yy < y + h; ++yy) {
        int xx;
        for (xx = x; xx < x + w; ++xx) {
            g4_px(p, xx, yy, level);
        }
    }
}

static void g4_frame(uint8_t *p, int x, int y, int w, int h, int t, int level)
{
    g4_rect(p, x, y, w, t, level);
    g4_rect(p, x, y + h - t, w, t, level);
    g4_rect(p, x, y, t, h, level);
    g4_rect(p, x + w - t, y, t, h, level);
}

static void g4_text(uint8_t *p, int x, int y, const char *s, int scale, int level)
{
    int cx = x;

    for (; *s != '\0'; ++s) {
        const uint8_t *g = font5x7_glyph(*s);
        int c;

        for (c = 0; c < GLYPH_W; ++c) {
            uint8_t col = g[c];
            int r;
            for (r = 0; r < GLYPH_H; ++r) {
                if ((col & (uint8_t)(1u << r)) != 0u) {
                    g4_rect(p, cx + c * scale, y + r * scale, scale, scale, level);
                }
            }
        }
        cx += GLYPH_ADV * scale;
    }
}

static void g4_text_centered(uint8_t *p, int y, const char *s, int scale, int level)
{
    g4_text(p, (SNAKE_SCREEN_W - text_width(s, scale)) / 2, y, s, scale, level);
}

static void g4_disc(uint8_t *p, int cx, int cy, int r, int level)
{
    int dy;

    for (dy = -r; dy <= r; ++dy) {
        int dx;
        for (dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy <= r * r) {
                g4_px(p, cx + dx, cy + dy, level);
            }
        }
    }
}

static void g4_ring(uint8_t *p, int cx, int cy, int r, int t, int level)
{
    int dy;

    if (t < 1) {
        t = 1;
    }
    for (dy = -r; dy <= r; ++dy) {
        int dx;
        for (dx = -r; dx <= r; ++dx) {
            int d2 = dx * dx + dy * dy;
            int ri = r - t;
            if (d2 <= r * r && d2 >= ri * ri) {
                g4_px(p, cx + dx, cy + dy, level);
            }
        }
    }
}

static void g4_ellipse(uint8_t *p, int cx, int cy, int rx, int ry, int level)
{
    int dy;

    if (rx <= 0 || ry <= 0) {
        return;
    }
    for (dy = -ry; dy <= ry; ++dy) {
        int lhs = rx * rx * ry * ry - dy * dy * rx * rx;
        int lim = 0;
        if (lhs < 0) {
            continue;
        }
        while ((lim + 1) * (lim + 1) * ry * ry <= lhs) {
            ++lim;
        }
        g4_rect(p, cx - lim, cy + dy, 2 * lim + 1, 1, level);
    }
}

/* =========================================================================
 * Applause-free apples and eyes
 * ====================================================================== */

/* Apple centred on (cx, cy); scale 1 is the playfield size (r = 9 px). */
static void mono_apple(uint8_t *b, int cx, int cy)
{
    mono_disc(b, cx, cy, 10, 0);                /* berry            */
    mono_rect(b, cx - 1, cy - 14, 2, 5, 0);     /* stem             */
    mono_ellipse(b, cx + 7, cy - 12, 7, 3, 0);  /* leaf             */
    mono_disc(b, cx - 4, cy - 4, 2, 1);         /* highlight        */
}

static void g4_apple(uint8_t *p, int cx, int cy, int scale)
{
    int r = 9 * scale;

    g4_disc(p, cx, cy, r, 0);
    g4_rect(p, cx - 1 * scale, cy - 14 * scale, 2 * scale, 5 * scale, 0);
    g4_ellipse(p, cx + 6 * scale, cy - 12 * scale, 6 * scale, 3 * scale, 0);
    g4_disc(p, cx - 4 * scale, cy - 4 * scale, 2 * scale, 3);
}

/* Eye rectangles (6x6, cell relative) for each heading. */
static void eye_boxes(snake_dir_t dir, int *bx, int *by)
{
    switch (dir) {
    case SNAKE_DIR_UP:
        bx[0] = 7;  by[0] = 5;
        bx[1] = 17; by[1] = 5;
        break;
    case SNAKE_DIR_DOWN:
        bx[0] = 7;  by[0] = 17;
        bx[1] = 17; by[1] = 17;
        break;
    case SNAKE_DIR_LEFT:
        bx[0] = 5;  by[0] = 7;
        bx[1] = 5;  by[1] = 17;
        break;
    case SNAKE_DIR_RIGHT:
    default:
        bx[0] = 18; by[0] = 7;
        bx[1] = 18; by[1] = 17;
        break;
    }
}

static void pupil_offset(snake_dir_t dir, int *dx, int *dy)
{
    *dx = 0;
    *dy = 0;
    switch (dir) {
    case SNAKE_DIR_UP:    *dy = -1; break;
    case SNAKE_DIR_DOWN:  *dy = 1;  break;
    case SNAKE_DIR_LEFT:  *dx = -1; break;
    case SNAKE_DIR_RIGHT:
    default:              *dx = 1;  break;
    }
}

/* Head block occupying the 28x28 inset of the cell at (x0, y0). */
static void mono_head(uint8_t *b, int x0, int y0, snake_dir_t dir, int dead)
{
    int bx[2];
    int by[2];
    int i;

    mono_rect(b, x0 + 2, y0 + 2, 28, 28, 0);
    /* 2 px rounded corners */
    mono_rect(b, x0 + 2, y0 + 2, 2, 2, 1);
    mono_rect(b, x0 + 28, y0 + 2, 2, 2, 1);
    mono_rect(b, x0 + 2, y0 + 28, 2, 2, 1);
    mono_rect(b, x0 + 28, y0 + 28, 2, 2, 1);

    eye_boxes(dir, bx, by);
    if (dead) {
        for (i = 0; i < 2; ++i) {
            int k;
            for (k = 0; k < GLYPH_ADV; ++k) {
                mono_rect(b, x0 + bx[i] + k, y0 + by[i] + k, 2, 2, 1);
                mono_rect(b, x0 + bx[i] + 5 - k, y0 + by[i] + k, 2, 2, 1);
            }
        }
    } else {
        int dx;
        int dy;
        pupil_offset(dir, &dx, &dy);
        for (i = 0; i < 2; ++i) {
            mono_rect(b, x0 + bx[i], y0 + by[i], 6, 6, 1);
            mono_rect(b, x0 + bx[i] + 2 + dx, y0 + by[i] + 2 + dy, 2, 2, 0);
        }
    }
}

static void g4_head(uint8_t *p, int x0, int y0, snake_dir_t dir, int dead)
{
    int bx[2];
    int by[2];
    int i;

    g4_rect(p, x0 + 2, y0 + 2, 28, 28, 0);
    g4_rect(p, x0 + 2, y0 + 2, 2, 2, 3);
    g4_rect(p, x0 + 28, y0 + 2, 2, 2, 3);
    g4_rect(p, x0 + 2, y0 + 28, 2, 2, 3);
    g4_rect(p, x0 + 28, y0 + 28, 2, 2, 3);

    eye_boxes(dir, bx, by);
    if (dead) {
        for (i = 0; i < 2; ++i) {
            int k;
            for (k = 0; k < GLYPH_ADV; ++k) {
                g4_rect(p, x0 + bx[i] + k, y0 + by[i] + k, 2, 2, 3);
                g4_rect(p, x0 + bx[i] + 5 - k, y0 + by[i] + k, 2, 2, 3);
            }
        }
    } else {
        int dx;
        int dy;
        pupil_offset(dir, &dx, &dy);
        for (i = 0; i < 2; ++i) {
            g4_rect(p, x0 + bx[i], y0 + by[i], 6, 6, 3);
            g4_rect(p, x0 + bx[i] + 2 + dx, y0 + by[i] + 2 + dy, 2, 2, 0);
        }
    }
}

/* =========================================================================
 * HUD
 * ====================================================================== */

static void mono_arrow(uint8_t *b, int cx, int cy, snake_dir_t dir, int white)
{
    int i;

    for (i = 0; i <= 15; ++i) {
        int h = (15 - i) * 6 / 15; /* 6 .. 0 */
        if (dir == SNAKE_DIR_UP) {
            mono_rect(b, cx - h, cy + 8 - i, 2 * h + 1, 1, white);
        } else if (dir == SNAKE_DIR_DOWN) {
            mono_rect(b, cx - h, cy - 8 + i, 2 * h + 1, 1, white);
        } else if (dir == SNAKE_DIR_LEFT) {
            mono_rect(b, cx + 8 - i, cy - h, 1, 2 * h + 1, white);
        } else {
            mono_rect(b, cx - 8 + i, cy - h, 1, 2 * h + 1, white);
        }
    }
}

static const char *dir_name(snake_dir_t dir)
{
    switch (dir) {
    case SNAKE_DIR_UP:    return "UP";
    case SNAKE_DIR_RIGHT: return "RIGHT";
    case SNAKE_DIR_DOWN:  return "DOWN";
    case SNAKE_DIR_LEFT:  return "LEFT";
    default:              return "-";
    }
}

static void draw_hud(uint8_t *b, const snake_status_t *st, uint16_t true_len)
{
    char num[8];
    char len[12];
    const int adv = GLYPH_ADV * 2; /* HUD is 2x */

    /* row 1 */
    mono_text(b, 12, 5, "SCORE", 2, 0);
    fmt_u16(num, st->score, 4);
    mono_text(b, 12 + 6 * adv, 5, num, 2, 0);

    mono_text(b, 250, 5, "HIGH", 2, 0);
    fmt_u16(num, st->high_score, 4);
    mono_text(b, 250 + 5 * adv, 5, num, 2, 0);

    /* row 2 */
    mono_text(b, 12, 33, "LEN", 2, 0);
    fmt_u16(len, true_len, 3);
    len[3] = '/';
    fmt_u16(len + 4, SNAKE_MAX_CELLS, 3);
    mono_text(b, 12 + 4 * adv, 33, len, 2, 0);

    mono_text(b, 250, 33, "DIR", 2, 0);
    mono_text(b, 250 + 4 * adv, 33, dir_name(st->dir), 2, 0);

    mono_arrow(b, 438, 42, st->dir, 0);

    /* separator above the playfield */
    mono_rect(b, 0, 62, SNAKE_SCREEN_W, 2, 0);
}

/* =========================================================================
 * Playfield
 * ====================================================================== */

void snake_render_play(uint8_t *mono, const snake_status_t *st,
                       const uint8_t *body_xy, uint16_t length,
                       uint8_t food_x, uint8_t food_y, uint8_t state)
{
    int i;

    if (mono == 0 || st == 0 || body_xy == 0) {
        return;
    }

    mono_clear(mono, 1);
    draw_hud(mono, st, length);

    /* cell-boundary dots: a quiet lattice that helps judge distances */
    for (int gy = 0; gy <= SNAKE_ROWS; ++gy) {
        int py = SNAKE_PLAY_Y + gy * SNAKE_CELL_PX;
        if (py >= SNAKE_SCREEN_H) py = SNAKE_SCREEN_H - 1;
        for (int gx = 0; gx <= SNAKE_COLS; ++gx) {
            int px = gx * SNAKE_CELL_PX;
            if (px >= SNAKE_SCREEN_W) px = SNAKE_SCREEN_W - 1;
            mono_rect(mono, px, py, 1, 1, 0);
        }
    }

    /* food first so a (never happening) overlap would hide it */
    if (food_x < SNAKE_COLS && food_y < SNAKE_ROWS) {
        mono_apple(mono, food_x * SNAKE_CELL_PX + SNAKE_CELL_PX / 2,
                   SNAKE_PLAY_Y + food_y * SNAKE_CELL_PX + SNAKE_CELL_PX / 2);
    }

    /* body, tail to neck, then the head on top */
    for (i = (int)length - 1; i >= 1; --i) {
        int x0 = body_xy[i * 2] * SNAKE_CELL_PX;
        int y0 = SNAKE_PLAY_Y + body_xy[i * 2 + 1] * SNAKE_CELL_PX;

        mono_rect(mono, x0 + 2, y0 + 2, 28, 28, 0);
        mono_rect(mono, x0 + 2, y0 + 2, 2, 2, 1);
        mono_rect(mono, x0 + 28, y0 + 2, 2, 2, 1);
        mono_rect(mono, x0 + 2, y0 + 28, 2, 2, 1);
        mono_rect(mono, x0 + 28, y0 + 28, 2, 2, 1);
    }
    if (length > 0) {
        int x0 = body_xy[0] * SNAKE_CELL_PX;
        int y0 = SNAKE_PLAY_Y + body_xy[1] * SNAKE_CELL_PX;
        mono_head(mono, x0, y0, st->dir, state == SNAKE_STATE_DEAD);
    }

    if (state == SNAKE_STATE_PAUSE) {
        int pw = 352;
        int ph = 186;
        int px = (SNAKE_SCREEN_W - pw) / 2;
        int py = 300;

        mono_rect(mono, px, py, pw, ph, 0);
        mono_rect(mono, px + 4, py + 4, pw - 8, ph - 8, 1);
        mono_text_centered(mono, py + 22, "PAUSED", 3, 0);
        mono_text_centered(mono, py + 88, "PRESS OK", 2, 0);
        mono_text_centered(mono, py + 126, "UP TO CALIBRATE", 2, 0);
    }
}

/* =========================================================================
 * Cards (4-level gray)
 * ====================================================================== */

static void card_frame(uint8_t *p)
{
    g4_clear(p, 3);
    g4_frame(p, 16, 16, 448, 768, 4, 0);
    g4_frame(p, 24, 24, 432, 752, 2, 2);
}

static void card_button(uint8_t *p, int y, const char *label)
{
    int w = 240;
    int h = 56;
    int x = (SNAKE_SCREEN_W - w) / 2;

    g4_rect(p, x, y, w, h, 0);
    g4_rect(p, x + 3, y + 3, w - 6, h - 6, 1);
    g4_rect(p, x + 5, y + 5, w - 10, h - 10, 0);
    g4_text_centered(p, y + (h - GLYPH_H * 3) / 2, label, 3, 3);
}

static void card_title(uint8_t *p, const snake_status_t *st)
{
    int i;
    int seg_x[6] = { 96, 130, 164, 198, 198, 232 };
    int seg_y[6] = { 230, 230, 230, 230, 264, 264 };

    card_frame(p);

    g4_text_centered(p, 64, "GYRO SNAKE", 3, 0);
    g4_rect(p, 160, 100, 160, 3, 0);
    g4_text_centered(p, 130, "TILT TO STEER", 2, 1);

    /* decorative snake with a bend, head and apple */
    for (i = 0; i < 6; ++i) {
        g4_rect(p, seg_x[i], seg_y[i], 28, 28, 0);
        g4_rect(p, seg_x[i], seg_y[i], 2, 2, 3);
        g4_rect(p, seg_x[i] + 26, seg_y[i], 2, 2, 3);
        g4_rect(p, seg_x[i], seg_y[i] + 26, 2, 2, 3);
        g4_rect(p, seg_x[i] + 26, seg_y[i] + 26, 2, 2, 3);
    }
    g4_head(p, 266, 264, SNAKE_DIR_RIGHT, 0);
    g4_apple(p, 336, 280, 2);

    card_button(p, 380, "TAP TO START");
    g4_text_centered(p, 450, "OR PRESS OK", 2, 1);

    {
        char full[10];
        full[0] = 'H'; full[1] = 'I'; full[2] = 'G'; full[3] = 'H'; full[4] = ' ';
        fmt_u16(&full[5], st->high_score, 4);
        g4_text_centered(p, 490, full, 2, 0);
    }

    g4_text_centered(p, 520, "AUTO-CALIBRATES ON START", 2, 1);
    g4_text_centered(p, 566, "FILL THE BOARD TO WIN", 2, 1);
    g4_text_centered(p, 610, "DOWN = SET ZERO", 2, 1);
    g4_text_centered(p, 640, "UP = RE-CAL IN PAUSE", 2, 1);

    for (i = 60; i < 420; i += 24) {
        g4_rect(p, i, 700, 12, 6, 2);
    }
}

static void card_calibrating(uint8_t *p, const snake_status_t *st)
{
    char pct[6];
    int bx = 60;
    int by = 330;
    int bw = 360;
    int bh = 64;
    int inner_w = bw - 8;
    int fill;

    card_frame(p);

    g4_text_centered(p, 70, "HOLD STILL", 3, 0);
    g4_text_centered(p, 110, "CALIBRATING", 2, 1);

    /* gyro mark */
    g4_ring(p, 240, 215, 52, 4, 0);
    g4_rect(p, 239, 173, 2, 84, 0);
    g4_rect(p, 198, 214, 84, 2, 0);
    g4_disc(p, 240, 215, 6, 0);

    /* progress track + fill */
    g4_rect(p, bx, by, bw, bh, 0);
    g4_rect(p, bx + 4, by + 4, inner_w, bh - 8, 2);
    fill = inner_w * (int)st->calib_percent / 100;
    if (fill > 0) {
        g4_rect(p, bx + 4, by + 4, fill, bh - 8, 0);
    }

    {
        int n;
        fmt_udec(pct, st->calib_percent);
        n = str_len(pct);
        pct[n] = '%';
        pct[n + 1] = (char)0;
        g4_text_centered(p, 420, pct, 3, 0);
    }

    g4_text_centered(p, 500, "KEEP YOUR PLAY POSE", 2, 1);
    g4_text_centered(p, 546, "DO NOT MOVE FOR 4 S", 2, 2);
    g4_text_centered(p, 640, "SAMPLES 400", 2, 2);
}

static void card_game_over(uint8_t *p, const snake_status_t *st, int win)
{
    char num[6];
    int i;
    int seg_x[4] = { 155, 189, 223, 257 };

    card_frame(p);

    if (win) {
        g4_text_centered(p, 90, "YOU WIN", 3, 0);
        g4_text_centered(p, 130, "BOARD FULL", 2, 1);
        g4_apple(p, 240, 240, 3);
        /* confetti */
        g4_rect(p, 120, 200, 6, 6, 2);
        g4_rect(p, 350, 190, 8, 8, 2);
        g4_rect(p, 330, 300, 6, 6, 2);
        g4_rect(p, 140, 300, 8, 8, 2);
    } else {
        g4_text_centered(p, 90, "GAME OVER", 3, 0);
        g4_rect(p, 160, 124, 160, 3, 0);
        for (i = 0; i < 4; ++i) {
            g4_rect(p, seg_x[i], 180, 28, 28, 0);
            g4_rect(p, seg_x[i], 180, 2, 2, 3);
            g4_rect(p, seg_x[i] + 26, 180, 2, 2, 3);
            g4_rect(p, seg_x[i], 206, 2, 2, 3);
            g4_rect(p, seg_x[i] + 26, 206, 2, 2, 3);
        }
        g4_head(p, 291, 180, SNAKE_DIR_RIGHT, 1);
    }

    g4_text_centered(p, 340, "SCORE", 2, 1);
    fmt_u16(num, st->score, 4);
    g4_text_centered(p, 366, num, 3, 0);

    g4_text_centered(p, 430, "HIGH", 2, 1);
    fmt_u16(num, st->high_score, 4);
    g4_text_centered(p, 456, num, 3, 0);

    if (st->record_beaten) {
        int bw = 260;
        int bx = (SNAKE_SCREEN_W - bw) / 2;
        g4_rect(p, bx, 520, bw, 44, 0);
        g4_text_centered(p, 534, "NEW RECORD", 2, 3);
    } else if (win) {
        int bw = 260;
        int bx = (SNAKE_SCREEN_W - bw) / 2;
        g4_rect(p, bx, 520, bw, 44, 0);
        g4_text_centered(p, 534, "PERFECT RUN", 2, 3);
    } else {
        g4_text_centered(p, 530, "TRY AGAIN", 2, 2);
    }

    card_button(p, 620, "PRESS OK");
    g4_text_centered(p, 710, "UP TO CALIBRATE", 2, 2);
}

void snake_render_card(uint8_t *page, snake_card_t card, const snake_status_t *st)
{
    if (page == 0 || st == 0) {
        return;
    }

    switch (card) {
    case SNAKE_CARD_TITLE:
        card_title(page, st);
        break;
    case SNAKE_CARD_CALIBRATING:
        card_calibrating(page, st);
        break;
    case SNAKE_CARD_GAME_OVER:
        card_game_over(page, st, 0);
        break;
    case SNAKE_CARD_WIN:
        card_game_over(page, st, 1);
        break;
    case SNAKE_CARD_PAUSED:
    case SNAKE_CARD_NONE:
    default:
        card_frame(page);
        g4_text_centered(page, 360, "GYRO SNAKE", 2, 1);
        break;
    }
}