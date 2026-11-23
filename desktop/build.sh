#!/usr/bin/env bash
#
# GYRO SNAKE - desktop simulator DLL build.
#
# Compiles the portable game core (firmware/components/game_core/src/*.c)
# together with desktop/sim_api.c into desktop/build/gyro_snake.dll.
# No ESP-IDF, no LVGL, no board; only the C99 core plus the ctypes boundary.
#
# Usage:
#   bash gyro-snake/desktop/build.sh            # from any working directory
#   CC=clang bash gyro-snake/desktop/build.sh   # alternate compiler
#
# Exit codes: 0 built, 2 sources missing, other = compiler/linker error.

set -eu

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/.." && pwd)"
CORE_DIR="$ROOT/firmware/components/game_core"
CORE_SRC="$CORE_DIR/src"
BALL_DIR="$ROOT/firmware/components/board_port"
IMU_DIR="$ROOT/firmware/components/imu_core"
STUB_DIR="$ROOT/host/preview"
BUILD_DIR="$HERE/build"
OUT="$BUILD_DIR/gyro_snake.dll"

CC="${CC:-gcc}"
CFLAGS="-std=c99 -O2 -Wall -Wextra"
LDFLAGS="-shared -static-libgcc"

mkdir -p "$BUILD_DIR"

# The task pins the four core translation units; globbing keeps the build
# working if the frozen core ever gains another .c file.
core_sources=("$CORE_SRC"/*.c)
if [ ! -e "${core_sources[0]}" ]; then
    echo "build.sh: error: no game_core sources in $CORE_SRC" >&2
    exit 2
fi

if [ ! -f "$HERE/sim_api.c" ]; then
    echo "build.sh: error: $HERE/sim_api.c is missing" >&2
    exit 2
fi

# One link step: every core translation unit, the tilt-ball renderer shared
# with the firmware, and the DLL boundary. host/preview supplies the esp_err_t
# stand-in that board_port.h needs on a host build.
echo "[cc] core  ${#core_sources[@]} file(s) + tilt_ball.c + sim_api.c -> $OUT"
# shellcheck disable=SC2086
"$CC" $CFLAGS $LDFLAGS \
    -I"$CORE_DIR/include" -I"$IMU_DIR/include" -I"$BALL_DIR/include" -I"$STUB_DIR" \
    "${core_sources[@]}" "$BALL_DIR/src/tilt_ball.c" "$HERE/sim_api.c" -o "$OUT"

if [ ! -s "$OUT" ]; then
    echo "build.sh: error: $OUT was not produced" >&2
    exit 1
fi

echo "build ok"
echo "  $OUT"