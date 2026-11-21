#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GYRO SNAKE - desktop DLL smoke test.

Loads desktop/build/gyro_snake.dll with ctypes, initialises the frozen core,
walks the title -> calibration -> play -> pause flow, reads every status
getter and prints ``DLL_SMOKE ok``.  No window, no sound, no GUI import; safe
to run on a build machine.

Usage:
    python desktop/test_dll.py [--dll PATH] [--seed N]

Exit codes: 0 on success, 1 on any failed check or a missing DLL.
"""

from __future__ import annotations

import argparse
import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DLL = os.path.join(HERE, "build", "gyro_snake.dll")

ABI_VERSION = 1

# snake.h numeric contracts
STATE_TITLE = 0
STATE_CALIBRATING = 1
STATE_PLAY = 2
STATE_PAUSE = 3
STATE_DEAD = 4
STATE_GAME_OVER = 5

CARD_NONE = 0
CARD_TITLE = 1
CARD_CALIBRATING = 2
CARD_PAUSED = 3
CARD_GAME_OVER = 4
CARD_WIN = 5

FRAME_MONO = 0
FRAME_GRAY4 = 1

DIR_UP = 0
DIR_RIGHT = 1
DIR_DOWN = 2
DIR_LEFT = 3
DIR_NONE = -1

BTN_OK = 0x01
BTN_UP = 0x02
BTN_DOWN = 0x04

MONO_BYTES = 48000
GRAY4_PAGE_BYTES = 96000


def fail(check, detail):
    print("DLL_SMOKE fail %s: %s" % (check, detail), file=sys.stderr)
    raise SystemExit(1)


def check(cond, name, detail=""):
    if not cond:
        fail(name, detail)


def pe_machine(path):
    """Return the PE COFF machine field, or None when the file is not PE."""
    try:
        with open(path, "rb") as f:
            dos = f.read(0x40)
            if len(dos) < 0x40 or dos[:2] != b"MZ":
                return None
            offset = int.from_bytes(dos[0x3C:0x40], "little")
            f.seek(offset)
            header = f.read(6)
    except OSError:
        return None
    if len(header) < 6 or header[:4] != b"PE\0\0":
        return None
    return int.from_bytes(header[4:6], "little")


def bind(dll):
    """Declare the whole gs_* ABI before any call is made."""
    sig = {
        "gs_abi_version": (ctypes.c_int, []),
        "gs_init": (ctypes.c_int, [ctypes.c_uint32]),
        "gs_tick": (ctypes.c_int, [ctypes.c_uint8, ctypes.c_int8]),
        "gs_begin_calibration": (None, []),
        "gs_calib_progress": (ctypes.c_int, [ctypes.c_uint8]),
        "gs_calib_done": (ctypes.c_int, [ctypes.c_int]),
        "gs_start_play": (ctypes.c_int, []),
        "gs_set_high": (None, [ctypes.c_uint16]),
        "gs_mono": (ctypes.c_void_p, []),
        "gs_gray4": (ctypes.c_void_p, []),
        "gs_frame": (ctypes.c_int, []),
        "gs_state": (ctypes.c_int, []),
        "gs_card": (ctypes.c_int, []),
        "gs_dirty": (ctypes.c_int, []),
        "gs_needs_full": (ctypes.c_int, []),
        "gs_mark": (None, [ctypes.c_int]),
        "gs_take_sound": (ctypes.c_int, []),
        "gs_score": (ctypes.c_int, []),
        "gs_high": (ctypes.c_int, []),
        "gs_length": (ctypes.c_int, []),
        "gs_tick_index": (ctypes.c_int, []),
        "gs_calib_percent": (ctypes.c_int, []),
        "gs_dir": (ctypes.c_int, []),
        "gs_record_beaten": (ctypes.c_int, []),
        "gs_calibrated": (ctypes.c_int, []),
        "gs_food_eaten": (ctypes.c_int, []),
        "gs_cells_total": (ctypes.c_int, []),
        "gs_state_name": (ctypes.c_char_p, [ctypes.c_int]),
        "gs_card_name": (ctypes.c_char_p, [ctypes.c_int]),
        "gs_sound_name": (ctypes.c_char_p, [ctypes.c_int]),
    }
    for name, (restype, argtypes) in sig.items():
        try:
            fn = getattr(dll, name)
        except AttributeError:
            fail("export", "%s is missing from %s" % (name, dll._name))
        fn.restype = restype
        fn.argtypes = argtypes


def resolve_dll(cli_path):
    if cli_path:
        return os.path.abspath(cli_path)
    env_path = os.environ.get("GYRO_SNAKE_DLL")
    if env_path:
        return os.path.abspath(env_path)
    return DEFAULT_DLL


def drain_sounds(dll):
    out = []
    while True:
        snd = int(dll.gs_take_sound())
        if snd == 0:
            return out
        check(0 <= snd <= 8, "sound range", repr(snd))
        out.append(snd)


def main(argv=None):
    parser = argparse.ArgumentParser(description="GYRO SNAKE DLL smoke test")
    parser.add_argument("--dll", default=None, help="path to gyro_snake.dll")
    parser.add_argument("--seed", type=int, default=1, help="init seed (default 1)")
    args = parser.parse_args(argv)

    path = resolve_dll(args.dll)
    if not os.path.isfile(path):
        fail("dll missing", path)
    if os.path.getsize(path) == 0:
        fail("dll empty", path)

    if os.name == "nt":
        machine = pe_machine(path)
        check(machine in (0x8664, 0x14C), "pe machine", repr(machine))
        expected = 0x8664 if ctypes.sizeof(ctypes.c_void_p) == 8 else 0x14C
        check(machine == expected, "pe bitness",
              "machine=0x%x python=%d-bit" %
              (machine, ctypes.sizeof(ctypes.c_void_p) * 8))

    try:
        dll = ctypes.CDLL(path)
    except OSError as exc:
        fail("load", "%s: %s" % (path, exc))
    bind(dll)

    check(dll.gs_abi_version() == ABI_VERSION, "abi",
          "got %d" % dll.gs_abi_version())

    # --- init -----------------------------------------------------------
    check(dll.gs_init(args.seed) == 1, "init", "gs_init returned 0")
    check(dll.gs_dirty() == 1, "init dirty", "expected 1")
    check(dll.gs_frame() == FRAME_GRAY4, "init frame",
          "got %d" % dll.gs_frame())
    check(dll.gs_state() == STATE_TITLE, "init state",
          "got %d" % dll.gs_state())
    check(dll.gs_card() == CARD_TITLE, "init card",
          "got %d" % dll.gs_card())
    check(dll.gs_needs_full() == 1, "init needs full", "expected 1")
    check(dll.gs_score() == 0, "init score", "got %d" % dll.gs_score())
    check(dll.gs_high() == 0, "init high", "got %d" % dll.gs_high())
    check(dll.gs_length() == 3, "init length", "got %d" % dll.gs_length())
    check(dll.gs_tick_index() == 0, "init tick", "got %d" % dll.gs_tick_index())
    check(dll.gs_dir() == DIR_RIGHT, "init dir", "got %d" % dll.gs_dir())
    check(dll.gs_calib_percent() == 0, "init calib", "got %d" % dll.gs_calib_percent())
    check(dll.gs_calibrated() == 0, "init calibrated", "expected 0")
    check(dll.gs_record_beaten() == 0, "init record", "expected 0")
    check(dll.gs_cells_total() == 255, "init cells total",
          "got %d" % dll.gs_cells_total())

    mono = dll.gs_mono()
    gray4 = dll.gs_gray4()
    check(mono is not None, "mono null", "gs_mono returned NULL")
    check(gray4 is not None, "gray4 null", "gs_gray4 returned NULL")
    check(len(ctypes.string_at(mono, MONO_BYTES)) == MONO_BYTES,
          "mono size", "not 48000 B")
    check(len(ctypes.string_at(gray4, GRAY4_PAGE_BYTES)) == GRAY4_PAGE_BYTES,
          "gray4 size", "not 96000 B (must be one staging page)")

    # A pending frame forbids a tick; the guard must not mutate the game.
    tick_before = dll.gs_tick_index()
    check(dll.gs_tick(BTN_OK, DIR_NONE) == 0, "tick guard",
          "gs_tick must refuse while dirty")
    check(dll.gs_tick_index() == tick_before, "tick guard index",
          "tick_index changed while dirty")
    dll.gs_mark(1)
    check(dll.gs_dirty() == 0, "mark title", "dirty still set")

    # --- title -> calibration (TITLE OK, uncalibrated) ------------------
    check(dll.gs_tick(BTN_OK, DIR_NONE) == 1, "title ok", "tick refused")
    check(dll.gs_state() == STATE_CALIBRATING, "calib state",
          "got %d" % dll.gs_state())
    check(dll.gs_card() == CARD_CALIBRATING, "calib card",
          "got %d" % dll.gs_card())
    check(dll.gs_frame() == FRAME_GRAY4, "calib frame",
          "got %d" % dll.gs_frame())
    check(dll.gs_dirty() == 1, "calib dirty", "expected 1")
    dll.gs_mark(1)

    # --- progress -------------------------------------------------------
    check(dll.gs_calib_progress(50) == 1, "progress", "not CALIBRATING")
    check(dll.gs_calib_percent() == 50, "progress value",
          "got %d" % dll.gs_calib_percent())
    check(dll.gs_tick(0, DIR_NONE) == 1, "progress tick", "tick refused")
    check(dll.gs_dirty() == 1, "progress frame", "card was not recomposed")
    check(dll.gs_card() == CARD_CALIBRATING, "progress card",
          "got %d" % dll.gs_card())
    dll.gs_mark(1)

    check(dll.gs_calib_progress(100) == 1, "progress 100", "not CALIBRATING")
    check(dll.gs_tick(0, DIR_NONE) == 1, "progress 100 tick", "tick refused")
    dll.gs_mark(1)

    # --- calibration done -> PLAY --------------------------------------
    check(dll.gs_calib_done(1) == 1, "calib done", "did not enter PLAY")
    check(dll.gs_state() == STATE_PLAY, "play state", "got %d" % dll.gs_state())
    check(dll.gs_card() == CARD_NONE, "play card", "got %d" % dll.gs_card())
    check(dll.gs_frame() == FRAME_MONO, "play frame", "got %d" % dll.gs_frame())
    check(dll.gs_calibrated() == 1, "calibrated flag", "expected 1")
    check(dll.gs_length() == 3, "play length", "got %d" % dll.gs_length())
    sounds = drain_sounds(dll)
    check(5 in sounds and 4 in sounds, "calib sounds",
          "expected CALIB_DONE(5)+START(4), got %r" % sounds)
    check(dll.gs_dirty() == 1, "play dirty", "expected 1")
    dll.gs_mark(1)
    check(dll.gs_dirty() == 0, "play mark", "dirty still set")

    # --- ticks, directions, pause/resume -------------------------------
    for i, tilt in enumerate((DIR_UP, DIR_UP, DIR_RIGHT, DIR_RIGHT,
                              DIR_DOWN, DIR_LEFT)):
        check(dll.gs_dirty() == 0, "tick %d pre" % i, "dirty before tick")
        check(dll.gs_tick(0, tilt) == 1, "tick %d" % i, "tick refused")
        check(dll.gs_state() == STATE_PLAY, "tick %d state" % i,
              "got %d" % dll.gs_state())
        check(isinstance(dll.gs_score(), int), "tick %d score" % i, "not int")
        check(3 <= dll.gs_length() <= 255, "tick %d length" % i,
              "got %d" % dll.gs_length())
        check(dll.gs_dirty() == 1, "tick %d frame" % i, "no frame")
        check(dll.gs_frame() == FRAME_MONO, "tick %d frame kind" % i,
              "got %d" % dll.gs_frame())
        dll.gs_mark(0)

    check(dll.gs_tick_index() == 6, "tick index", "got %d" % dll.gs_tick_index())
    check(dll.gs_dir() in (DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT),
          "dir", "got %d" % dll.gs_dir())

    # OK in PLAY -> PAUSE, OK in PAUSE -> PLAY.
    check(dll.gs_tick(BTN_OK, DIR_NONE) == 1, "pause", "tick refused")
    check(dll.gs_state() == STATE_PAUSE, "pause state",
          "got %d" % dll.gs_state())
    check(dll.gs_card() == CARD_PAUSED, "pause card",
          "got %d" % dll.gs_card())
    check(dll.gs_frame() == FRAME_MONO, "pause frame",
          "got %d" % dll.gs_frame())
    dll.gs_mark(0)
    check(dll.gs_tick(BTN_OK, DIR_NONE) == 1, "resume", "tick refused")
    check(dll.gs_state() == STATE_PLAY, "resume state",
          "got %d" % dll.gs_state())
    dll.gs_mark(0)

    # --- high score is monotonic ---------------------------------------
    dll.gs_set_high(1234)
    check(dll.gs_high() >= 1234, "high set", "got %d" % dll.gs_high())
    dll.gs_set_high(1)
    check(dll.gs_high() >= 1234, "high monotonic", "got %d" % dll.gs_high())

    # --- start_play after calibration restarts the run -----------------
    check(dll.gs_start_play() == 1, "start play", "did not enter PLAY")
    check(dll.gs_state() == STATE_PLAY, "start play state",
          "got %d" % dll.gs_state())
    check(dll.gs_tick_index() == 0, "start play tick",
          "got %d" % dll.gs_tick_index())
    check(dll.gs_score() == 0, "start play score", "got %d" % dll.gs_score())
    check(dll.gs_dirty() == 1, "start play dirty", "expected 1")
    dll.gs_mark(1)

    # --- names ----------------------------------------------------------
    for name, fn, arg in (("state", dll.gs_state_name, dll.gs_state()),
                          ("card", dll.gs_card_name, dll.gs_card()),
                          ("sound", dll.gs_sound_name, 4)):
        raw = fn(arg)
        check(raw is not None, "%s name null" % name, "returned NULL")
        text = raw.decode("ascii")
        check(text != "" and "?" not in text, "%s name" % name, repr(text))

    print("DLL_SMOKE ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())