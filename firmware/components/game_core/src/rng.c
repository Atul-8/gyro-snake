/*
 * GYRO SNAKE — deterministic internal PRNG (xorshift32).
 *
 * Copyright (c) 2026 GYRO SNAKE contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The core never calls rand()/random()/time(): food placement is a pure
 * function of the seed handed to snake_init() and of the input sequence, so a
 * replay of the same session produces byte-identical frames.
 *
 * The state word is caller-owned (it lives inside snake_t); this module only
 * advances it.
 */

#include <stdint.h>

#define RNG_DEFAULT_SEED 0x9E3779B9u
#define RNG_ALT_SEED     0x6D2B79F5u

void snake_rng_seed(uint32_t *state, uint32_t seed)
{
    uint32_t s = seed ^ 0xA341316Cu; /* splitmix-style salt */

    if (s == 0u) {
        s = RNG_DEFAULT_SEED;
    }
    if (state != 0) {
        *state = s;
    }
}

uint32_t snake_rng_next(uint32_t *state)
{
    uint32_t x;

    if (state == 0) {
        return RNG_DEFAULT_SEED;
    }
    x = *state;
    if (x == 0u) {
        x = RNG_ALT_SEED;
    }
    /* Marsaglia xorshift32: full cycle over the 2^32-1 non-zero states. */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}