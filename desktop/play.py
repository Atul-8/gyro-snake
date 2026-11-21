#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GYRO SNAKE 桌面硬件模拟器（Tkinter + Pillow + ctypes）。

设计契约见 gyro-snake/docs/requirements.md 与
firmware/components/game_core/include/snake.h。本文件只做三件事：

  1. 通过 ctypes 调用 desktop/build/gyro_snake.dll（所有 gs_* 调用都在 Tk
     主线程；DLL 有静态状态，不是线程安全的）；
  2. 把 mono / gray4 帧缓冲解码成 480x800 的纸色面板，按 0.75 缩到
     360x600，并按 snake.h 的刷新握手推进 tick（dirty == 0 才允许 tick）；
  3. 提供窗口模式、无窗口自测（--selftest）与窗口截图自测
     （--window-test）。

模块顶层刻意不导入 tkinter / PIL.ImageGrab：--selftest 必须能在没有显示
服务的环境里运行。窗口相关导入全部延迟到 run_window() /
run_window_test() 内部。

所有 550 / 1800 / 1500 / 120 / 450 / 40 / 160 ms 都是**模拟值**，不是真机
实测；界面明确标注 SIMULATED TILT / 模拟倾斜。桌面倾斜盘是虚拟 IMU，
不是 LSM6DS3TR-C 的真实数据。
"""

from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
import queue
import sys
import threading
import time
import traceback
from datetime import datetime, timezone
from pathlib import Path

from PIL import Image, ImageOps

try:  # 只用于启动时的版本门禁；正常环境一定有 Pillow
    import PIL
except Exception:  # pragma: no cover
    PIL = None


# =========================================================================
# 1. 路径与常量
# =========================================================================

BASE_DIR = Path(__file__).resolve().parent          # gyro-snake/desktop
PROJECT_ROOT = BASE_DIR.parent                      # gyro-snake
BUILD_DIR = BASE_DIR / "build"
OUT_DIR = BASE_DIR / "out"
HISCORE_PATH = BASE_DIR / "highscore.json"
SETTINGS_PATH = BASE_DIR / "settings.json"
DLL_PATH = BUILD_DIR / "gyro_snake.dll"

SCREEN_W = 480
SCREEN_H = 800
WIN_W = 520
WIN_H = 1000
PANEL_SCALE = 0.75
PANEL_W = 360
PANEL_H = 600
PANEL_X = 80
PANEL_Y = 62

# 深炭机身 + 纸色屏幕 + 绿色强调（与固件墨水屏的视觉语言一致）。
SHELL = "#1b1a17"
SHELL_DECK = "#211f1a"
SHELL_FACE = "#2b2822"
SHELL_FACE_ACTIVE = "#2f5f42"
SHELL_LINE = "#3a362d"
SHELL_LINE_SOFT = "#302d26"
PAPER = "#f3efe4"
INK = "#141310"
TEXT = "#e9e2d1"
MUTED = "#9a9383"
FAINT = "#736d60"
GREEN = "#3f8f5a"
GREEN_BRIGHT = "#6fce8d"
GREEN_DARK = "#2b6a43"
GREEN_PALE = "#c9ead3"
AMBER = "#c9a45c"

# snake.h 冻结的按键位（activity mask）
BTN_OK = 0x01
BTN_UP = 0x02
BTN_DOWN = 0x04

# snake.h 冻结的状态 / 卡片 / 帧 / 音效编号
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

SOUND_NONE = 0
SOUND_EAT = 1
SOUND_TURN = 2
SOUND_DEATH = 3
SOUND_START = 4
SOUND_CALIB_DONE = 5
SOUND_CALIB_FAIL = 6
SOUND_GAME_OVER = 7
SOUND_WIN = 8

DIR_UP = 0
DIR_RIGHT = 1
DIR_DOWN = 2
DIR_LEFT = 3
DIR_NONE = -1

MONO_FRAME_BYTES = 48000
GRAY4_PAGE_BYTES = 96000

SPEED_LEVELS = (1, 2, 4)
# 时序档位：EPD 保留真机节奏；FAST/TURBO 只用于桌面试玩，不是真机时序。
TIMING_PROFILES = {
    "epd":   {"fast": 550, "full": 1800, "gray4": 1800, "think": 1500, "lock": 180},
    "fast":  {"fast": 120, "full": 450,  "gray4": 450,  "think": 120,  "lock": 20},
    "turbo": {"fast": 40,  "full": 160,  "gray4": 160,  "think": 40,   "lock": 10},
}
TIMING_ORDER = ("epd", "fast", "turbo")
DEFAULT_TIMING = "fast"

CALIB_SECONDS = 3.0
CALIB_POLL_MS = 60
DEMO_MAX_TICKS = 600
RECORD_MAX_FRAMES = 600

# 原创短音（频率 Hz, 时长 ms）。不是任何现有游戏的旋律。
SOUND_PATTERNS = {
    SOUND_EAT: ((880, 22), (1175, 28)),
    SOUND_TURN: ((440, 12),),
    SOUND_DEATH: ((392, 35), (294, 35), (196, 55)),
    SOUND_START: ((392, 28), (523, 28), (659, 35)),
    SOUND_CALIB_DONE: ((659, 24), (880, 30)),
    SOUND_CALIB_FAIL: ((311, 35), (233, 45)),
    SOUND_GAME_OVER: ((330, 35), (262, 50), (196, 60)),
    SOUND_WIN: ((523, 26), (659, 26), (784, 26), (1047, 36)),
}

DIR_NAMES = {DIR_UP: "UP", DIR_RIGHT: "RIGHT", DIR_DOWN: "DOWN", DIR_LEFT: "LEFT"}
EXIT_PAIRS = {DIR_UP: DIR_DOWN, DIR_DOWN: DIR_UP, DIR_LEFT: DIR_RIGHT, DIR_RIGHT: DIR_LEFT}

# =========================================================================
# 2. 界面文案（游戏画面内的 HUD 由 C 核心渲染，保持英文）
# =========================================================================

LANG = {
    "zh": {
        "subtitle": "reTerminal Sticky 硬件模拟器 · 480×800 电子墨水",
        "sim_tilt": "模拟倾斜 · 虚拟 IMU",
        "drag_hint": "鼠标拖动 / 方向键",
        "start": "开始 START",
        "pause": "暂停 PAUSE",
        "resume": "继续 RESUME",
        "restart": "重新开始 RESTART",
        "calibrating_btn": "校准中 ...",
        "calibrate": "校准 CALIBRATE",
        "center": "回中 CENTER",
        "speed": "SPEED 速度",
        "timing": "TIMING 时序",
        "sound_on": "音效 ON",
        "sound_off": "音效 OFF",
        "shot": "截图 SHOT",
        "record": "录制 REC",
        "stop_rec": "停止 REC",
        "calib_yes": "IMU 已校准",
        "calib_no": "IMU 未校准",
        "tilt_none": "TILT —",
        "rev": "REV",
        "state_names": {
            STATE_TITLE: "标题",
            STATE_CALIBRATING: "校准中",
            STATE_PLAY: "游戏中",
            STATE_PAUSE: "暂停",
            STATE_DEAD: "死亡",
            STATE_GAME_OVER: "结束",
        },
        "sim_note": "刷新时序为模拟值 · 虚拟倾斜不是真机 IMU",
        "no_frame": "还没有画面",
        "shot_saved": "截图已保存",
        "shot_failed": "截图失败",
        "rec_started": "录制中",
        "rec_saved": "录制已保存",
        "rec_failed": "录制失败",
        "high_saved": "最高分已保存",
        "calibrating_status": "校准中，请保持倾斜盘居中",
    },
    "en": {
        "subtitle": "reTerminal Sticky hardware simulator · 480×800 e-paper",
        "sim_tilt": "SIMULATED TILT · virtual IMU",
        "drag_hint": "mouse drag / arrow keys",
        "start": "START",
        "pause": "PAUSE",
        "resume": "RESUME",
        "restart": "RESTART",
        "calibrating_btn": "CALIBRATING ...",
        "calibrate": "CALIBRATE",
        "center": "CENTER",
        "speed": "SPEED",
        "timing": "TIMING",
        "sound_on": "SOUND ON",
        "sound_off": "SOUND OFF",
        "shot": "SHOT",
        "record": "REC",
        "stop_rec": "STOP REC",
        "calib_yes": "IMU CALIBRATED",
        "calib_no": "IMU NOT CALIBRATED",
        "tilt_none": "TILT —",
        "rev": "REV",
        "state_names": {
            STATE_TITLE: "TITLE",
            STATE_CALIBRATING: "CALIBRATING",
            STATE_PLAY: "PLAY",
            STATE_PAUSE: "PAUSE",
            STATE_DEAD: "DEAD",
            STATE_GAME_OVER: "GAME OVER",
        },
        "sim_note": "refresh timings are simulated · tilt is a virtual IMU",
        "no_frame": "NO FRAME YET",
        "shot_saved": "SHOT SAVED",
        "shot_failed": "SHOT FAILED",
        "rec_started": "RECORDING",
        "rec_saved": "REC SAVED",
        "rec_failed": "REC FAILED",
        "high_saved": "HIGH SCORE SAVED",
        "calibrating_status": "CALIBRATING - keep the tilt puck centered",
    },
}


# =========================================================================
# 3. 基础工具
# =========================================================================

class SnakeError(RuntimeError):
    """DLL 缺失、ABI 不符、帧缓冲异常或输出路径不合法。"""


def _iso_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _unique_path(path: Path) -> Path:
    """同秒冲突时追加 _1、_2 ……，不覆盖已有文件/目录。"""
    if not path.exists():
        return path
    stem = path.stem
    suffix = path.suffix
    for i in range(1, 1000):
        cand = path.with_name(f"{stem}_{i}{suffix}")
        if not cand.exists():
            return cand
    raise SnakeError(f"cannot find a free name for {path}")


def _resolve_out(value, default: Path) -> Path:
    """解析 --out。

    任务书里的示例是 ``--out desktop/out/selftest``，既可能从项目根
    （gyro-snake/）执行，也可能从仓库根（codecraft/）执行。若相对路径
    以 ``desktop`` 开头，一律钉到 gyro-snake/desktop 下，因而两种 cwd
    都得到 gyro-snake/desktop/out/...；其他相对路径按 cwd 常规解析。
    """
    if value is None:
        return default
    p = Path(str(value)).expanduser()
    if p.is_absolute():
        return p.resolve()
    parts = p.parts
    if parts and parts[0].lower() == "desktop":
        rest = Path(*parts[1:]) if len(parts) > 1 else Path()
        return (BASE_DIR / rest).resolve()
    return (Path.cwd() / p).resolve()


def _approx_green(pixel) -> bool:
    r, g, b = pixel[0], pixel[1], pixel[2]
    return g > r + 18 and g > b + 12 and g > 80


# =========================================================================
# 4. 帧解码（core 像素 -> 纸色面板）
# =========================================================================

_GRAY4_LUT = (0, 85, 170, 255)
_GRAY4_TABLE = tuple(
    bytes((
        _GRAY4_LUT[b >> 6],
        _GRAY4_LUT[(b >> 4) & 3],
        _GRAY4_LUT[(b >> 2) & 3],
        _GRAY4_LUT[b & 3],
    ))
    for b in range(256)
)
_GRAY4_INDEX = [
    0 if v < 43 else 1 if v < 128 else 2 if v < 213 else 3 for v in range(256)
]
_GRAY4_PALETTE = [
    20, 19, 16,        # 0   -> #141310
    90, 86, 77,        # 85  -> #5a564d
    176, 170, 155,     # 170 -> #b0aa9b
    243, 239, 228,     # 255 -> #f3efe4 (PAPER)
]


def decode_mono(buf: bytes) -> Image.Image:
    """mono 1 bpp -> Pillow "1"（1 = white, 0 = black）。"""
    if len(buf) != MONO_FRAME_BYTES:
        raise SnakeError(f"mono frame is {len(buf)} bytes, expected {MONO_FRAME_BYTES}")
    return Image.frombytes("1", (SCREEN_W, SCREEN_H), buf)


def decode_gray4(buf: bytes) -> Image.Image:
    """gray4 2 bpp -> Pillow "L"，像素值严格为 0/85/170/255。"""
    if len(buf) != GRAY4_PAGE_BYTES:
        raise SnakeError(
            f"gray4 frame is {len(buf)} bytes, expected {GRAY4_PAGE_BYTES}"
        )
    row_bytes = SCREEN_W // 4  # 120
    get = _GRAY4_TABLE.__getitem__
    rows = []
    for y in range(SCREEN_H):
        row = buf[y * row_bytes:(y + 1) * row_bytes]
        rows.append(b"".join(map(get, row)))
    return Image.frombytes("L", (SCREEN_W, SCREEN_H), b"".join(rows))


def to_paper_mono(im: Image.Image) -> Image.Image:
    """mono 显示映射：0 -> INK，1 -> PAPER。"""
    return ImageOps.colorize(im.convert("L"), black=INK, white=PAPER).convert("RGB")


def to_paper_gray4(im: Image.Image) -> Image.Image:
    """gray4 显示映射：4 级纸色索引调色板。"""
    idx = im.point(_GRAY4_INDEX)
    p = Image.frombytes("P", (SCREEN_W, SCREEN_H), idx.tobytes())
    p.putpalette(_GRAY4_PALETTE)
    return p.convert("RGB")


def panel_image(kind: int, core_im: Image.Image) -> Image.Image:
    """480x800 core 像素 -> 0.75 缩放后的 360x600 纸色面板。"""
    paper = to_paper_mono(core_im) if kind == FRAME_MONO else to_paper_gray4(core_im)
    return paper.resize((PANEL_W, PANEL_H), Image.Resampling.NEAREST)


# =========================================================================
# 5. SnakeDLL —— 唯一出现 ctypes / gs_* 的地方
# =========================================================================

class SnakeDLL:
    """冻结 ABI v1 的薄封装。所有调用必须由同一线程串行执行。"""

    _PROTOTYPES = (
        ("gs_abi_version", ctypes.c_int, []),
        ("gs_init", ctypes.c_int, [ctypes.c_uint32]),
        ("gs_tick", ctypes.c_int, [ctypes.c_uint8, ctypes.c_int8]),
        ("gs_begin_calibration", None, []),
        ("gs_calib_progress", ctypes.c_int, [ctypes.c_uint8]),
        ("gs_calib_done", ctypes.c_int, [ctypes.c_int]),
        ("gs_start_play", ctypes.c_int, []),
        ("gs_set_high", None, [ctypes.c_uint16]),
        ("gs_mono", ctypes.c_void_p, []),
        ("gs_gray4", ctypes.c_void_p, []),
        ("gs_frame", ctypes.c_int, []),
        ("gs_state", ctypes.c_int, []),
        ("gs_card", ctypes.c_int, []),
        ("gs_dirty", ctypes.c_int, []),
        ("gs_needs_full", ctypes.c_int, []),
        ("gs_mark", None, [ctypes.c_int]),
        ("gs_take_sound", ctypes.c_int, []),
        ("gs_score", ctypes.c_int, []),
        ("gs_high", ctypes.c_int, []),
        ("gs_length", ctypes.c_int, []),
        ("gs_tick_index", ctypes.c_int, []),
        ("gs_calib_percent", ctypes.c_int, []),
        ("gs_dir", ctypes.c_int, []),
        ("gs_record_beaten", ctypes.c_int, []),
        ("gs_calibrated", ctypes.c_int, []),
        ("gs_food_eaten", ctypes.c_int, []),
        ("gs_cells_total", ctypes.c_int, []),
        ("gs_state_name", ctypes.c_char_p, [ctypes.c_int]),
        ("gs_card_name", ctypes.c_char_p, [ctypes.c_int]),
        ("gs_sound_name", ctypes.c_char_p, [ctypes.c_int]),
    )

    def __init__(self, path: Path):
        self.path = Path(path)
        if not self.path.is_file():
            raise SnakeError(f"dll missing: {self.path}")
        try:
            self.lib = ctypes.CDLL(str(self.path))
        except OSError as exc:
            raise SnakeError(
                f"cannot load {self.path}: {exc}. "
                "Check that Python and the DLL are both 64-bit, and that the "
                "toolchain used -static-libgcc."
            ) from exc

        for name, restype, argtypes in self._PROTOTYPES:
            try:
                fn = getattr(self.lib, name)
            except AttributeError as exc:
                raise SnakeError(f"dll is missing export '{name}'") from exc
            fn.restype = restype
            fn.argtypes = argtypes

        self.abi = int(self.lib.gs_abi_version())
        if self.abi != 1:
            raise SnakeError(f"unsupported ABI {self.abi}, expected 1")

    # ---- 生命周期 ----------------------------------------------------
    def init(self, seed: int) -> int:
        return int(self.lib.gs_init(int(seed) & 0xFFFFFFFF))

    def tick(self, buttons: int, tilt_dir: int = DIR_NONE) -> int:
        return int(self.lib.gs_tick(int(buttons) & 0xFF, int(tilt_dir)))

    def begin_calibration(self) -> None:
        self.lib.gs_begin_calibration()

    def calib_progress(self, percent: int) -> int:
        return int(self.lib.gs_calib_progress(max(0, min(100, int(percent)))))

    def calib_done(self, ok: bool) -> int:
        return int(self.lib.gs_calib_done(1 if ok else 0))

    def start_play(self) -> int:
        return int(self.lib.gs_start_play())

    def set_high(self, value: int) -> None:
        self.lib.gs_set_high(max(0, min(9999, int(value))))

    def mark_refreshed(self, full: bool) -> None:
        self.lib.gs_mark(1 if full else 0)

    # ---- 查询 --------------------------------------------------------
    def dirty(self) -> bool:
        return int(self.lib.gs_dirty()) != 0

    def needs_full(self) -> bool:
        return int(self.lib.gs_needs_full()) != 0

    def frame_kind(self) -> int:
        return int(self.lib.gs_frame())

    def state(self) -> int:
        return int(self.lib.gs_state())

    def card(self) -> int:
        return int(self.lib.gs_card())

    def score(self) -> int:
        return int(self.lib.gs_score())

    def high(self) -> int:
        return int(self.lib.gs_high())

    def length(self) -> int:
        return int(self.lib.gs_length())

    def tick_index(self) -> int:
        return int(self.lib.gs_tick_index())

    def calib_percent(self) -> int:
        return int(self.lib.gs_calib_percent())

    def direction(self) -> int:
        return int(self.lib.gs_dir())

    def record_beaten(self) -> bool:
        return int(self.lib.gs_record_beaten()) != 0

    def calibrated(self) -> bool:
        return int(self.lib.gs_calibrated()) != 0

    def food_eaten(self) -> int:
        return int(self.lib.gs_food_eaten())

    def cells_total(self) -> int:
        return int(self.lib.gs_cells_total())

    def take_sound(self) -> int:
        return int(self.lib.gs_take_sound())

    def drain_sounds(self):
        out = []
        while True:
            snd = self.take_sound()
            if snd == SOUND_NONE:
                return out
            out.append(snd)

    @staticmethod
    def _name(fn, value: int) -> str:
        raw = fn(int(value))
        if not raw:
            return "?"
        try:
            return raw.decode("ascii")
        except Exception:
            return "?"

    def state_name(self, value: int) -> str:
        return self._name(self.lib.gs_state_name, value)

    def card_name(self, value: int) -> str:
        return self._name(self.lib.gs_card_name, value)

    def sound_name(self, value: int) -> str:
        return self._name(self.lib.gs_sound_name, value)

    def snapshot(self) -> dict:
        st = self.state()
        card = self.card()
        return {
            "state": st,
            "card": card,
            "frame": self.frame_kind(),
            "dir": self.direction(),
            "length": self.length(),
            "calib": self.calib_percent(),
            "score": self.score(),
            "high": self.high(),
            "tick": self.tick_index(),
            "eaten": self.food_eaten(),
            "calibrated": self.calibrated(),
            "record": self.record_beaten(),
            "dirty": self.dirty(),
            "needs_full": self.needs_full(),
            "state_name": self.state_name(st),
            "card_name": self.card_name(card),
        }

    # ---- 帧缓冲（立即拷贝，不跨 tick 缓存内容） -----------------------
    def _copy(self, ptr, size: int, what: str) -> bytes:
        if not ptr:
            raise SnakeError(f"null {what} framebuffer")
        buf = ctypes.string_at(ptr, size)
        if len(buf) != size:
            raise SnakeError(f"short {what} framebuffer: {len(buf)} != {size}")
        return buf

    def mono_bytes(self) -> bytes:
        return self._copy(self.lib.gs_mono(), MONO_FRAME_BYTES, "mono")

    def gray4_bytes(self) -> bytes:
        return self._copy(self.lib.gs_gray4(), GRAY4_PAGE_BYTES, "gray4")

    def frame_bytes(self):
        kind = self.frame_kind()
        if kind == FRAME_MONO:
            return kind, self.mono_bytes()
        if kind == FRAME_GRAY4:
            return kind, self.gray4_bytes()
        raise SnakeError(f"gs_frame() returned {kind}")


# =========================================================================
# 6. 最高分 / 设置
# =========================================================================

class HighScoreStore:
    """desktop/highscore.json 的读写；失败只记日志，不影响游戏。"""

    MAX_SCORE = 9999

    def __init__(self, path: Path):
        candidate = Path(path)
        if candidate.name != "highscore.json":
            raise SnakeError(f"highscore path must be named highscore.json: {candidate}")
        self.path = candidate
        self._warned = False

    def load(self) -> int:
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
            if not isinstance(data, dict):
                raise ValueError("not a JSON object")
            if data.get("schema") != 1:
                raise ValueError("schema != 1")
            value = data.get("high_score")
            if isinstance(value, bool) or not isinstance(value, int):
                raise ValueError("high_score is not an integer")
            return max(0, min(self.MAX_SCORE, int(value)))
        except FileNotFoundError:
            return 0
        except Exception as exc:
            if not self._warned:
                print(f"highscore: ignored ({exc})", file=sys.stderr)
                self._warned = True
            return 0

    def save(self, value: int) -> bool:
        value = max(0, min(self.MAX_SCORE, int(value)))
        payload = {
            "schema": 1,
            "high_score": value,
            "updated": _iso_now(),
            "source": "gyro-snake desktop simulator",
        }
        tmp = self.path.with_name(self.path.name + ".tmp")
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with tmp.open("w", encoding="utf-8") as fh:
                json.dump(payload, fh, ensure_ascii=False, indent=2)
                fh.write("\n")
                fh.flush()
                os.fsync(fh.fileno())
            os.replace(tmp, self.path)
            return True
        except Exception as exc:
            print(f"highscore: save failed ({exc})", file=sys.stderr)
            try:
                tmp.unlink(missing_ok=True)
            except Exception:
                pass
            return False


class SettingsStore:
    """desktop/settings.json：语言/速度/时序/音效，失败不影响游戏。"""

    def __init__(self, path: Path):
        self.path = Path(path)

    def load(self) -> dict:
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
            if isinstance(data, dict) and data.get("schema") == 1:
                return data
            return {}
        except Exception:
            return {}

    def save(self, data: dict) -> bool:
        payload = dict(data)
        payload["schema"] = 1
        payload["updated"] = _iso_now()
        tmp = self.path.with_name(self.path.name + ".tmp")
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with tmp.open("w", encoding="utf-8") as fh:
                json.dump(payload, fh, ensure_ascii=False, indent=2)
                fh.write("\n")
                fh.flush()
                os.fsync(fh.fileno())
            os.replace(tmp, self.path)
            return True
        except Exception:
            try:
                tmp.unlink(missing_ok=True)
            except Exception:
                pass
            return False


# =========================================================================
# 7. 音效（daemon 线程 + winsound.Beep，不碰 Tk / DLL）
# =========================================================================

class SoundPlayer:
    def __init__(self):
        self.available = False
        self.enabled = True
        self._winsound = None
        self._q = queue.Queue(maxsize=32)
        self._thread = None
        self._beep_error_reported = False
        try:
            import winsound  # Windows only
            self._winsound = winsound
            self.available = True
        except Exception:
            self.available = False
        if self.available:
            self._thread = threading.Thread(
                target=self._worker, name="gyro-snake-sound", daemon=True
            )
            self._thread.start()

    def _worker(self):
        while True:
            item = self._q.get()
            try:
                if item is None:
                    break
                if not self.enabled:
                    continue
                for snd in item:
                    pattern = SOUND_PATTERNS.get(snd)
                    if not pattern:
                        continue
                    for freq, ms in pattern:
                        if not self.enabled:
                            break
                        try:
                            self._winsound.Beep(int(freq), int(ms))
                        except Exception as exc:  # RDP 静音、设备占用等
                            if not self._beep_error_reported:
                                print(f"sound: beep failed ({exc})", file=sys.stderr)
                                self._beep_error_reported = True
                            break
                        time.sleep(0.004)
                    time.sleep(0.070)
            finally:
                self._q.task_done()

    def submit(self, events):
        if not self.available or not self.enabled or not events:
            return
        try:
            self._q.put_nowait(list(events))
        except queue.Full:
            pass  # 丢弃新事件，绝不阻塞游戏

    def set_enabled(self, enabled: bool):
        self.enabled = bool(enabled)
        if not self.enabled:
            self.clear()

    def clear(self):
        while True:
            try:
                item = self._q.get_nowait()
            except queue.Empty:
                break
            try:
                self._q.task_done()
            except Exception:
                pass
            if item is None:
                break

    def close(self):
        try:
            self._q.put_nowait(None)
        except queue.Full:
            pass
        if self._thread is not None:
            self._thread.join(timeout=0.5)


# =========================================================================
# 8. 录制
# =========================================================================

class Recorder:
    """把每张呈现的面板写成 360x600 PNG，停止时合成 GIF + JSON。"""

    def __init__(self):
        self.active = False
        self.dir = None
        self.paths = []
        self.times = []
        self.started_iso = ""
        self.last_snapshot = {}

    def start(self, out_root: Path) -> Path:
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        out_root.mkdir(parents=True, exist_ok=True)
        d = _unique_path(out_root / f"record_{stamp}")
        d.mkdir(parents=True, exist_ok=False)
        self.dir = d
        self.paths = []
        self.times = []
        self.started_iso = _iso_now()
        self.last_snapshot = {}
        self.active = True
        return d

    def add(self, im: Image.Image, st: dict) -> bool:
        """写一帧；达到 600 帧上限时返回 False（由调用方停止）。"""
        if not self.active or self.dir is None:
            return True
        if len(self.paths) >= RECORD_MAX_FRAMES:
            return False
        name = f"frame_{len(self.paths):04d}.png"
        im.save(self.dir / name, "PNG")
        self.paths.append(name)
        self.times.append(time.monotonic())
        self.last_snapshot = dict(st)
        return True

    def stop(self, speed: int):
        if not self.active or self.dir is None:
            return None, None
        self.active = False
        d = self.dir

        frames = []
        for name in self.paths:
            try:
                with Image.open(d / name) as src:
                    frames.append(
                        src.convert("RGB").resize((240, 400), Image.Resampling.NEAREST)
                    )
            except Exception as exc:
                print(f"record: cannot read {name} ({exc})", file=sys.stderr)

        durations = []
        for i in range(len(self.paths)):
            if i + 1 < len(self.times):
                ms = int(round((self.times[i + 1] - self.times[i]) * 1000.0))
                durations.append(max(40, min(1000, ms)))
            else:
                durations.append(300)
        if not durations:
            durations = [300]

        gif_path = d / "record.gif"
        if frames:
            try:
                if len(frames) == 1:
                    frames[0].save(gif_path, "GIF", duration=300, loop=0)
                else:
                    frames[0].save(
                        gif_path,
                        "GIF",
                        save_all=True,
                        append_images=frames[1:],
                        duration=durations,
                        loop=0,
                        optimize=False,
                    )
            except Exception as exc:
                print(f"record: gif failed ({exc})", file=sys.stderr)
                gif_path = None
        else:
            gif_path = None

        st = self.last_snapshot or {}
        meta = {
            "schema": 1,
            "frames": len(self.paths),
            "ticks": int(st.get("tick", 0)),
            "score": int(st.get("score", 0)),
            "state": str(st.get("state_name", "?")),
            "speed": int(speed),
            "started": self.started_iso,
            "ended": _iso_now(),
            "timing": "wall-clock, capped 40..1000 ms",
            "frames_dir": "frame_%04d.png",
            "note": "panel frames, 360x600; GIF is 240x400",
        }
        json_path = d / "record.json"
        try:
            json_path.write_text(
                json.dumps(meta, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
        except Exception as exc:
            print(f"record: json failed ({exc})", file=sys.stderr)
            json_path = None
        return gif_path, json_path


# =========================================================================
# 9. Demo / selftest 方向脚本（确定性）
# =========================================================================

def initial_food(seed: int) -> tuple:
    """按 core 的 xorshift32 + 空格均匀取样复算开局苹果格。

    只用于 selftest 的确定性脚本：core 的初始蛇在 (7,11),(6,11),(5,11)，
    snake_init 先放一次食物，start_run 再放一次（共 2 次 xorshift），
    pick = rnd % 342，然后按 y/x 扫描空格。
    """
    mask = 0xFFFFFFFF
    x = (int(seed) ^ 0xA341316C) & mask
    if x == 0:
        x = 0x9E3779B9
    # snake_init() places the title-screen food (draw 1); start_run() resets
    # the body and places the food actually visible in PLAY (draw 2).
    for _ in range(2):
        x ^= (x << 13) & mask
        x ^= x >> 17
        x ^= (x << 5) & mask
        x &= mask
    pick = x % (15 * 23 - 3)
    body = {(7, 11), (6, 11), (5, 11)}
    for y in range(23):
        for bx in range(15):
            if (bx, y) in body:
                continue
            if pick == 0:
                return bx, y
            pick -= 1
    return 0, 0


def plan_to_food(seed: int, tail: int = 32) -> tuple:
    """只用 90 度转向走到开局苹果，然后沿最后方向直走撞墙。

    返回 (food_x, food_y, directions)。路径先竖直再水平（或同行时用
    一格上绕），不会请求 180 度反向，因此 core 不会忽略脚本输入。
    """
    fx, fy = initial_food(seed)
    dirs = []
    if fy == 11:
        if fx > 7:
            dirs += [DIR_RIGHT] * (fx - 7)
        elif fx < 7:
            dirs += [DIR_UP]
            dirs += [DIR_LEFT] * (7 - fx)
            dirs += [DIR_DOWN]
    else:
        vertical = DIR_DOWN if fy > 11 else DIR_UP
        dirs += [vertical] * abs(fy - 11)
        if fx > 7:
            dirs += [DIR_RIGHT] * (fx - 7)
        elif fx < 7:
            dirs += [DIR_LEFT] * (7 - fx)
    if not dirs:
        dirs = [DIR_RIGHT]
    dirs += [dirs[-1]] * tail
    return fx, fy, dirs


def demo_direction(step: int) -> int:
    """窗口测试用的方形绕圈：每 5 个 tick 顺时针转 90 度。"""
    block = (step // 5) % 4
    order = (DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT)
    return order[block]


# =========================================================================
# 10. 窗口 UI
# =========================================================================

# 按钮几何：name -> (x, y, w, h)
BUTTON_GEOMETRY = {
    "primary": (206, 692, 298, 54),
    "calibrate": (206, 752, 142, 30),
    "lang": (352, 752, 68, 30),
    "sound": (426, 752, 78, 30),
    "speed1": (206, 800, 94, 26),
    "speed2": (308, 800, 94, 26),
    "speed4": (410, 800, 94, 26),
    "timing_epd": (206, 843, 94, 26),
    "timing_fast": (308, 843, 94, 26),
    "timing_turbo": (410, 843, 94, 26),
    "shot": (206, 875, 142, 30),
    "rec": (362, 875, 142, 30),
    "center": (45, 880, 120, 28),
}

TILT_CX = 105
TILT_CY = 796
TILT_R = 82
TILT_TRAVEL = 38
PUCK_R = 10
TILT_DEADZONE = 12


def _round_points(x, y, w, h, r):
    return [
        x + r, y,
        x + w - r, y,
        x + w, y,
        x + w, y + r,
        x + w, y + h - r,
        x + w, y + h,
        x + w - r, y + h,
        x + r, y + h,
        x, y + h,
        x, y + h - r,
        x, y + r,
        x, y,
    ]


def _round_rect(canvas, x, y, w, h, r, **kwargs):
    return canvas.create_polygon(
        _round_points(x, y, w, h, r), smooth=True, splinesteps=24, **kwargs
    )


def _opposite(a: int, b: int) -> bool:
    return (a + 2) % 4 == b


def _clamp_vec(dx: float, dy: float):
    length = math.hypot(dx, dy)
    if length <= TILT_TRAVEL:
        return dx, dy
    k = TILT_TRAVEL / length
    return dx * k, dy * k


def _dir_from_vec(dx: float, dy: float) -> int:
    if math.hypot(dx, dy) < TILT_DEADZONE:
        return DIR_NONE
    if abs(dx) >= abs(dy):
        return DIR_RIGHT if dx > 0 else DIR_LEFT
    return DIR_DOWN if dy > 0 else DIR_UP


class GameUI:
    """窗口模式的全部 UI 与状态机。只在 Tk 主线程运行。"""

    def __init__(self, root, tk, lib: SnakeDLL, args):
        from PIL import ImageTk

        self.root = root
        self.tk = tk
        self.lib = lib
        self.args = args
        self.seed = int(args.seed)
        self.window_test = bool(getattr(args, "window_test", None) is not None)
        self.persist = not self.window_test and not bool(getattr(args, "selftest", False))
        self.quiet = bool(getattr(args, "quiet", False))

        self._ImageTk = ImageTk
        self._photo = None
        self.panel_item = None
        self.last_paper_image = None
        self.last_core_image = None
        self.last_refresh_mode = "—"
        self._after_id = None
        self._closed = False
        self.phase = "INIT"
        self.startup_ok = False
        self.fatal_reported = False
        self._tool_after_id = None
        self._tool_default = ""

        self.pending_buttons = 0
        self.restart_pending = False
        self.demo_mode = False
        self.demo_step = 0
        self.hiscore_saved = False
        self.calib_active = False
        self.calib_started = 0.0
        self.dragging = False
        self.held_keys = set()

        self.tilt_vec = (0.0, 0.0)
        self.tilt_dir = DIR_NONE

        self.settings_store = SettingsStore(SETTINGS_PATH)
        saved = self.settings_store.load()
        self.lang = saved.get("lang") if saved.get("lang") in LANG else "zh"
        self.speed = saved.get("speed") if saved.get("speed") in SPEED_LEVELS else 1
        self.timing = saved.get("timing") if saved.get("timing") in TIMING_PROFILES else DEFAULT_TIMING
        saved_sound = saved.get("sound")
        self.sound_enabled = bool(saved_sound) if isinstance(saved_sound, bool) else True

        if getattr(args, "_lang_override", None) in LANG:
            self.lang = args._lang_override
        if getattr(args, "_speed_override", None) in SPEED_LEVELS:
            self.speed = args._speed_override
        if getattr(args, "_timing_override", None) in TIMING_PROFILES:
            self.timing = args._timing_override

        if self.window_test:
            # 自动化窗口截图：英文界面、快速节奏、固定 demo，不写设置。
            self.lang = "en"
            self.speed = 2
            self.timing = "fast"
            self.sound_enabled = False

        self.L = LANG[self.lang]
        self.sound_player = SoundPlayer()
        self.sound_player.set_enabled(self.sound_enabled)
        self.recorder = Recorder()
        self.hiscore = HighScoreStore(HISCORE_PATH)

        self._canvas = None
        self._button_items = {}
        self._ui_sig = None
        self._status_line1 = None
        self._status_line2 = None
        self._footer_item = None
        self._caption_mode = None
        self._tilt_puck = None
        self._tilt_shadow = None
        self._tilt_vector = None
        self._tilt_readout = None
        self._tilt_dir_items = {}
        self._error_item = None

        self._build_ui()

    # ---- 字体 --------------------------------------------------------
    def _ui_font(self, size: int, bold: bool = False):
        family = "Microsoft YaHei UI" if self.lang == "zh" else "Consolas"
        return (family, size, "bold") if bold else (family, size)

    def _mono_font(self, size: int, bold: bool = False):
        return ("Consolas", size, "bold") if bold else ("Consolas", size)

    # ---- UI 构建 -----------------------------------------------------
    def _build_ui(self):
        tk = self.tk
        root = self.root
        L = self.L
        root.configure(bg=SHELL)
        if self._tool_after_id is not None:
            try:
                self.root.after_cancel(self._tool_after_id)
            except Exception:
                pass
            self._tool_after_id = None

        if self._canvas is None:
            self._canvas = tk.Canvas(
                root, width=WIN_W, height=WIN_H, bg=SHELL,
                highlightthickness=0, bd=0,
            )
            self._canvas.place(x=0, y=0, width=WIN_W, height=WIN_H)
            self._bind_canvas()
        c = self._canvas
        c.delete("all")
        self._button_items = {}
        self._tilt_dir_items = {}
        self.panel_item = None
        self._photo = None
        self._status_line1 = None
        self._status_line2 = None
        self._footer_item = None
        self._caption_mode = None
        self._tilt_puck = None
        self._tilt_shadow = None
        self._tilt_vector = None
        self._tilt_readout = None
        self._error_item = None

        # ---- 头部 ----------------------------------------------------
        c.create_rectangle(18, 10, 22, 40, fill=GREEN, outline="")
        c.create_text(
            30, 8, anchor="nw", text="GYRO SNAKE",
            font=self._mono_font(-20, True), fill=TEXT,
        )
        c.create_text(
            31, 33, anchor="nw", text=L["subtitle"],
            font=self._ui_font(-8), fill=MUTED,
        )
        c.create_line(0, 52, WIN_W, 52, fill=SHELL_LINE)

        # ---- 480x800 屏幕（0.75 -> 360x600） -------------------------
        c.create_rectangle(
            PANEL_X + 4, PANEL_Y + 4,
            PANEL_X + PANEL_W + 4, PANEL_Y + PANEL_H + 4,
            fill="#0e0d0b", outline="",
        )
        _round_rect(
            c, PANEL_X - 4, PANEL_Y - 4, PANEL_W + 8, PANEL_H + 8, 6,
            fill="#100f0d", outline=SHELL_LINE, width=1,
        )
        c.create_rectangle(
            PANEL_X, PANEL_Y, PANEL_X + PANEL_W, PANEL_Y + PANEL_H,
            fill=PAPER, outline=INK, width=1,
        )
        c.create_text(
            PANEL_X, PANEL_Y + PANEL_H + 7, anchor="nw",
            text="SIMULATED E-PAPER · 480×800 · 0.75×",
            font=self._mono_font(-8), fill=FAINT,
        )
        self._caption_mode = c.create_text(
            PANEL_X + PANEL_W, PANEL_Y + PANEL_H + 7, anchor="ne",
            text="", font=self._mono_font(-8), fill=GREEN,
        )

        # ---- 控制面板背景 --------------------------------------------
        _round_rect(
            c, 12, 684, 496, 304, 14,
            fill=SHELL_DECK, outline=SHELL_LINE, width=1,
        )

        # ---- 虚拟倾斜盘 ----------------------------------------------
        c.create_text(
            26, 694, anchor="nw", text=L["sim_tilt"],
            font=self._ui_font(-8, True), fill=GREEN,
        )
        c.create_oval(
            TILT_CX - TILT_R - 1, TILT_CY - TILT_R - 1,
            TILT_CX + TILT_R + 1, TILT_CY + TILT_R + 1,
            fill="#151410", outline=GREEN, width=2,
        )
        c.create_oval(
            TILT_CX - TILT_R + 10, TILT_CY - TILT_R + 10,
            TILT_CX + TILT_R - 10, TILT_CY + TILT_R - 10,
            fill="#1b1a17", outline=SHELL_LINE_SOFT, width=1,
        )
        c.create_line(
            TILT_CX - 58, TILT_CY, TILT_CX + 58, TILT_CY,
            fill="#3a362e", width=1,
        )
        c.create_line(
            TILT_CX, TILT_CY - 58, TILT_CX, TILT_CY + 58,
            fill="#3a362e", width=1,
        )
        c.create_oval(
            TILT_CX - 2, TILT_CY - 2, TILT_CX + 2, TILT_CY + 2,
            fill=FAINT, outline="",
        )
        dir_pos = {
            DIR_UP: (TILT_CX, TILT_CY - 66),
            DIR_RIGHT: (TILT_CX + 66, TILT_CY),
            DIR_DOWN: (TILT_CX, TILT_CY + 66),
            DIR_LEFT: (TILT_CX - 66, TILT_CY),
        }
        for d, name in DIR_NAMES.items():
            x, y = dir_pos[d]
            self._tilt_dir_items[d] = c.create_text(
                x, y, text=name, font=self._mono_font(-8, True), fill=FAINT,
            )

        self._tilt_vector = c.create_line(
            TILT_CX, TILT_CY, TILT_CX, TILT_CY,
            fill=GREEN_BRIGHT, width=2,
        )
        self._tilt_shadow = c.create_oval(
            0, 0, 0, 0, fill="#0d0c0a", outline="",
        )
        self._tilt_puck = c.create_oval(
            0, 0, 0, 0, fill=GREEN, outline=GREEN_BRIGHT, width=1,
        )
        self._tilt_readout = c.create_text(
            TILT_CX, 912, anchor="n", text=L["tilt_none"],
            font=self._mono_font(-9, True), fill=GREEN_BRIGHT,
        )
        c.create_text(
            TILT_CX, 930, anchor="n", text=L["drag_hint"],
            font=self._ui_font(-7), fill=FAINT,
        )

        # ---- 右侧控制标签 --------------------------------------------
        c.create_text(
            206, 787, anchor="nw", text=L["speed"],
            font=self._ui_font(-7, True), fill=MUTED,
        )
        c.create_text(
            206, 830, anchor="nw", text=L["timing"],
            font=self._ui_font(-7, True), fill=MUTED,
        )

        # ---- 按钮 ----------------------------------------------------
        self._draw_buttons()

        # ---- 状态行 --------------------------------------------------
        self._status_line1 = c.create_text(
            20, 944, anchor="nw", text="", font=self._ui_font(-9, True),
            fill=TEXT,
        )
        self._status_line2 = c.create_text(
            20, 960, anchor="nw", text="", font=self._ui_font(-8),
            fill=MUTED,
        )
        self._tool_default = L["sim_note"]
        self._footer_item = c.create_text(
            20, 976, anchor="nw", text=self._tool_default,
            font=self._ui_font(-7), fill=FAINT,
        )

        self._restore_panel()
        self._update_tilt_items()
        self._update_status()

    def _bind_canvas(self):
        c = self._canvas
        c.bind("<ButtonPress-1>", self._on_canvas_press)
        c.bind("<B1-Motion>", self._on_canvas_motion)
        c.bind("<ButtonRelease-1>", self._on_canvas_release)
        for name in BUTTON_GEOMETRY:
            tag = "btn:" + name
            c.tag_bind(
                tag, "<ButtonPress-1>",
                lambda _e, n=name: self._on_button_press(n),
            )
        self.root.bind("<KeyPress>", self._on_keypress)
        self.root.bind("<KeyRelease>", self._on_keyrelease)
        self.root.bind("<FocusOut>", self._on_focus_out)

    # ---- 按钮绘制 ----------------------------------------------------
    def _button_label(self, name: str) -> str:
        L = self.L
        st = self.lib.state() if self.startup_ok else STATE_TITLE
        if name == "primary":
            if not self.startup_ok:
                return L["start"]
            if st == STATE_TITLE:
                return L["start"]
            if st == STATE_PLAY:
                return L["pause"]
            if st == STATE_PAUSE:
                return L["resume"]
            if st == STATE_CALIBRATING:
                return L["calibrating_btn"]
            return L["restart"]
        if name == "calibrate":
            return L["calibrate"]
        if name == "lang":
            return "EN" if self.lang == "zh" else "中文"
        if name == "sound":
            return L["sound_on"] if self.sound_enabled else L["sound_off"]
        if name == "speed1":
            return "1x"
        if name == "speed2":
            return "2x"
        if name == "speed4":
            return "4x"
        if name == "timing_epd":
            return "EPD"
        if name == "timing_fast":
            return "FAST"
        if name == "timing_turbo":
            return "TURBO"
        if name == "shot":
            return L["shot"]
        if name == "rec":
            return L["stop_rec"] if self.recorder.active else L["record"]
        if name == "center":
            return L["center"]
        return name

    def _button_active(self, name: str) -> bool:
        if name == "speed1":
            return self.speed == 1
        if name == "speed2":
            return self.speed == 2
        if name == "speed4":
            return self.speed == 4
        if name == "timing_epd":
            return self.timing == "epd"
        if name == "timing_fast":
            return self.timing == "fast"
        if name == "timing_turbo":
            return self.timing == "turbo"
        if name == "sound":
            return self.sound_enabled
        if name == "rec":
            return self.recorder.active
        return False

    def _button_enabled(self, name: str) -> bool:
        if not self.startup_ok:
            return False
        st = self.lib.state()
        if name == "primary":
            return st != STATE_CALIBRATING
        if name == "calibrate":
            return st != STATE_CALIBRATING
        return True

    def _ui_signature(self):
        return (
            self.lang,
            self.speed,
            self.timing,
            self.sound_enabled,
            self.recorder.active,
            self.lib.state() if self.startup_ok else -1,
            self.lib.calibrated() if self.startup_ok else False,
        )

    def _draw_buttons(self):
        c = self._canvas
        for name in BUTTON_GEOMETRY:
            c.delete("btn:" + name)
            self._draw_button(name)
        self._ui_sig = self._ui_signature()

    def _draw_button(self, name: str):
        c = self._canvas
        x, y, w, h = BUTTON_GEOMETRY[name]
        label = self._button_label(name)
        active = self._button_active(name)
        enabled = self._button_enabled(name)
        tag = "btn:" + name
        primary = (name == "primary")

        if primary:
            face = GREEN if enabled else "#2a3a2e"
            outline = GREEN_BRIGHT if enabled else SHELL_LINE_SOFT
            fg = "#0f1b13" if enabled else MUTED
            font = self._ui_font(-13, True)
            shadow = "#0c120d"
            radius = 10
        else:
            face = SHELL_FACE_ACTIVE if active else SHELL_FACE
            if not enabled:
                face = "#221f1b"
            outline = GREEN if active else SHELL_LINE
            if not enabled:
                outline = SHELL_LINE_SOFT
            fg = GREEN_BRIGHT if active else (TEXT if enabled else FAINT)
            font = self._ui_font(-8, True)
            shadow = "#15130f"
            radius = 7

        _round_rect(
            c, x, y + 2, w, h, radius,
            fill=shadow, outline="", tags=(tag,),
        )
        _round_rect(
            c, x, y, w, h, radius,
            fill=face, outline=outline, width=1, tags=(tag,),
        )
        c.create_text(
            x + w / 2, y + h / 2, text=label,
            font=font, fill=fg, tags=(tag,),
        )

    def _refresh_buttons_if_needed(self):
        if self._ui_signature() != self._ui_sig:
            self._draw_buttons()

    # ---- 屏幕图像 ----------------------------------------------------
    def _restore_panel(self):
        if self.last_paper_image is None or self._canvas is None:
            return
        try:
            self._photo = self._ImageTk.PhotoImage(self.last_paper_image)
            if self.panel_item is None:
                self.panel_item = self._canvas.create_image(
                    PANEL_X, PANEL_Y, anchor="nw", image=self._photo,
                )
            else:
                self._canvas.itemconfigure(self.panel_item, image=self._photo)
        except Exception:
            pass

    def _show_panel(self, paper: Image.Image):
        self.last_paper_image = paper
        try:
            self._photo = self._ImageTk.PhotoImage(paper)
            if self.panel_item is None:
                self.panel_item = self._canvas.create_image(
                    PANEL_X, PANEL_Y, anchor="nw", image=self._photo,
                )
            else:
                self._canvas.itemconfigure(self.panel_item, image=self._photo)
        except Exception as exc:
            self._fatal(f"panel: {exc}")

    # ---- 倾斜盘 ------------------------------------------------------
    def _set_tilt_vec(self, dx: float, dy: float):
        self.tilt_vec = _clamp_vec(float(dx), float(dy))
        self.tilt_dir = _dir_from_vec(*self.tilt_vec)
        self._update_tilt_items()

    def _set_tilt_dir(self, direction: int):
        if direction not in DIR_NAMES:
            self._set_tilt_vec(0.0, 0.0)
            return
        unit = {
            DIR_UP: (0.0, -1.0),
            DIR_RIGHT: (1.0, 0.0),
            DIR_DOWN: (0.0, 1.0),
            DIR_LEFT: (-1.0, 0.0),
        }[direction]
        self._set_tilt_vec(unit[0] * TILT_TRAVEL, unit[1] * TILT_TRAVEL)

    def center_tilt(self):
        self._take_over_demo()
        self._set_tilt_vec(0.0, 0.0)

    def _take_over_demo(self):
        if not self.demo_mode or self.window_test:
            return
        self.demo_mode = False
        self.demo_step = 0
        self._set_tool_status("DEMO OFF · YOU HAVE CONTROL")

    def _tilt_readout_text(self) -> str:
        if self.tilt_dir not in DIR_NAMES:
            return self.L["tilt_none"]
        name = DIR_NAMES[self.tilt_dir]
        if self.startup_ok:
            core_dir = self.lib.direction()
            if core_dir in DIR_NAMES and _opposite(self.tilt_dir, core_dir):
                name += " · " + self.L["rev"]
        return "TILT " + name

    def _update_tilt_items(self):
        c = self._canvas
        if c is None or self._tilt_puck is None:
            return
        dx, dy = self.tilt_vec
        px = TILT_CX + dx
        py = TILT_CY + dy
        c.coords(
            self._tilt_shadow,
            px - PUCK_R + 1, py - PUCK_R + 2,
            px + PUCK_R + 1, py + PUCK_R + 2,
        )
        c.coords(
            self._tilt_puck,
            px - PUCK_R, py - PUCK_R,
            px + PUCK_R, py + PUCK_R,
        )
        c.coords(self._tilt_vector, TILT_CX, TILT_CY, px, py)
        for d, item in self._tilt_dir_items.items():
            if d == self.tilt_dir:
                c.itemconfigure(item, fill=GREEN_BRIGHT)
            else:
                c.itemconfigure(item, fill=FAINT)
        if self._tilt_readout is not None:
            text = self._tilt_readout_text()
            fill = AMBER if text.endswith(self.L["rev"]) else GREEN_BRIGHT
            c.itemconfigure(self._tilt_readout, text=text, fill=fill)

    def _in_disk(self, x: int, y: int) -> bool:
        return math.hypot(x - TILT_CX, y - TILT_CY) <= TILT_R + 6

    def _drag_to(self, x: int, y: int):
        self._set_tilt_vec(x - TILT_CX, y - TILT_CY)

    def _on_canvas_press(self, event):
        self.root.focus_set()
        if self._in_disk(event.x, event.y):
            self._take_over_demo()
            self.dragging = True
            self._drag_to(event.x, event.y)

    def _on_canvas_motion(self, event):
        if self.dragging:
            self._drag_to(event.x, event.y)

    def _on_canvas_release(self, _event):
        self.dragging = False

    # ---- 按钮动作 ----------------------------------------------------
    def _on_button_press(self, name: str):
        self.root.focus_set()
        if not self._button_enabled(name):
            return
        if name == "primary":
            self.primary_action()
        elif name == "calibrate":
            self.start_calibration()
        elif name == "lang":
            self.toggle_language()
        elif name == "sound":
            self.toggle_sound()
        elif name == "speed1":
            self.set_speed(1)
        elif name == "speed2":
            self.set_speed(2)
        elif name == "speed4":
            self.set_speed(4)
        elif name == "timing_epd":
            self.set_timing("epd")
        elif name == "timing_fast":
            self.set_timing("fast")
        elif name == "timing_turbo":
            self.set_timing("turbo")
        elif name == "shot":
            self.screenshot()
        elif name == "rec":
            self.toggle_record()
        elif name == "center":
            self.center_tilt()

    # ---- 状态行 ------------------------------------------------------
    def _state_label(self, st: dict) -> str:
        name = self.L["state_names"].get(int(st.get("state", -1)), "?")
        if int(st.get("state", -1)) == STATE_CALIBRATING:
            name += f" {int(st.get('calib', 0)):3d}%"
        return name

    def _update_status(self, st: dict | None = None):
        if self._canvas is None:
            return
        if st is None:
            st = self.lib.snapshot()
        state = self._state_label(st)
        tilt = "—"
        if self.startup_ok and self.tilt_dir in DIR_NAMES:
            tilt = DIR_NAMES[self.tilt_dir]
            core_dir = self.lib.direction()
            if core_dir in DIR_NAMES and _opposite(self.tilt_dir, core_dir):
                tilt += " · " + self.L["rev"]
        line1 = (
            f"SCORE {int(st['score']):04d}   HIGH {int(st['high']):04d}   "
            f"LEN {int(st['length']):03d}   STATE {state}   TILT {tilt}"
        )
        if self._status_line1 is not None:
            self._canvas.itemconfigure(self._status_line1, text=line1)

        timing = self.timing.upper()
        sound = (self.L["sound_on"] if self.sound_enabled else self.L["sound_off"]).split()[-1]
        calib = self.L["calib_yes"] if st.get("calibrated") else self.L["calib_no"]
        line2 = (
            f"TIMING {timing}   {self.speed}x   SOUND {sound}   {calib}"
            f"   {self.last_refresh_mode}"
        )
        if self._status_line2 is not None:
            self._canvas.itemconfigure(self._status_line2, text=line2)
        if self._caption_mode is not None:
            self._canvas.itemconfigure(self._caption_mode, text=self.last_refresh_mode)
        self._update_tilt_items()
        self._refresh_buttons_if_needed()

    def _set_tool_status(self, text: str):
        if self._footer_item is None:
            return
        self._canvas.itemconfigure(self._footer_item, text=text, fill=GREEN_BRIGHT)
        if self._tool_after_id is not None:
            try:
                self.root.after_cancel(self._tool_after_id)
            except Exception:
                pass
        self._tool_after_id = self.root.after(2800, self._restore_tool_status)

    def _restore_tool_status(self):
        self._tool_after_id = None
        try:
            if self._footer_item is not None:
                self._canvas.itemconfigure(
                    self._footer_item, text=self._tool_default, fill=FAINT
                )
        except Exception:
            pass

    # ---- 设置 --------------------------------------------------------
    def _save_settings(self):
        if not self.persist:
            return
        self.settings_store.save({
            "lang": self.lang,
            "sound": self.sound_enabled,
            "speed": self.speed,
            "timing": self.timing,
        })

    def set_language(self, lang: str):
        if lang not in LANG or lang == self.lang:
            return
        self.lang = lang
        self.L = LANG[lang]
        self._save_settings()
        self.sound_player.set_enabled(self.sound_enabled)
        self._build_ui()

    def toggle_language(self):
        self.set_language("en" if self.lang == "zh" else "zh")

    def set_speed(self, speed: int):
        if speed not in SPEED_LEVELS:
            return
        self.speed = speed
        self._save_settings()
        self._set_tool_status(f"SPEED {speed}x")
        self._update_status()

    def set_timing(self, name: str):
        if name not in TIMING_PROFILES or name == self.timing:
            return
        self.timing = name
        self._save_settings()
        self._set_tool_status(f"TIMING {name.upper()}")
        self._update_status()

    def cycle_timing(self):
        idx = TIMING_ORDER.index(self.timing) if self.timing in TIMING_ORDER else 0
        self.set_timing(TIMING_ORDER[(idx + 1) % len(TIMING_ORDER)])

    def toggle_sound(self):
        if not self.sound_player.available:
            self._set_tool_status("SOUND N/A (winsound)")
            return
        self.sound_enabled = not self.sound_enabled
        self.sound_player.set_enabled(self.sound_enabled)
        self._save_settings()
        self._set_tool_status(
            self.L["sound_on"] if self.sound_enabled else self.L["sound_off"]
        )
        self._refresh_buttons_if_needed()
        self._update_status()

    # ---- 启动 / 核心 -------------------------------------------------
    def start(self, autoplay: bool = False):
        try:
            if self.lib.init(self.seed) != 1:
                raise SnakeError("gs_init failed")
            self.lib.set_high(self.hiscore.load())
            self.startup_ok = True
            if autoplay:
                self._instant_play()
            self._update_status()
            self._start_loop()
        except Exception as exc:
            self.startup_ok = False
            self._fatal(f"startup: {exc}")

    def _instant_play(self):
        """窗口测试专用：跳过 3 秒校准动画，直接进入 PLAY。"""
        self.lib.begin_calibration()
        if self.lib.dirty():
            self.lib.mark_refreshed(True)
        self.lib.calib_progress(100)
        if self.lib.dirty():
            self.lib.mark_refreshed(True)
        if self.lib.calib_done(True) != 1:
            raise SnakeError("calibration_done did not enter PLAY")
        self.lib.drain_sounds()
        self.calib_active = False
        self.demo_mode = True
        self.demo_step = 0
        self._set_tilt_dir(DIR_UP)

    def restart_core(self) -> bool:
        self._cancel_timer()
        self.sound_player.clear()
        try:
            if self.lib.init(self.seed) != 1:
                raise SnakeError("gs_init failed during restart")
        except Exception as exc:
            self._fatal(f"restart: {exc}")
            return False
        self.lib.set_high(self.hiscore.load())
        self.last_paper_image = None
        self.last_core_image = None
        self.last_refresh_mode = "—"
        self.hiscore_saved = False
        self.pending_buttons = 0
        self.restart_pending = False
        self.calib_active = False
        self.demo_mode = False
        self.demo_step = 0
        self.held_keys.clear()
        self.center_tilt()
        self._update_status()
        self._start_loop()
        return True

    # ---- 定时器 ------------------------------------------------------
    def _cancel_timer(self):
        if self._after_id is not None:
            try:
                self.root.after_cancel(self._after_id)
            except Exception:
                pass
            self._after_id = None

    def _schedule(self, ms: int, callback):
        self._cancel_timer()

        def run():
            self._after_id = None
            if self._closed:
                return
            callback()

        self._after_id = self.root.after(max(1, int(ms)), run)

    def _start_loop(self):
        if self._closed:
            return
        self._schedule(1, self._step)

    def _request_tick(self):
        """按钮后的快速响应：不在面板刷新窗口里时立即推进一次。

        有待显示帧（例如刚 begin_calibration）时立即 present；面板正在
        “刷新”时让当前周期跑完，pending_buttons 会在下一个 tick 被消费。
        """
        if self._closed:
            return
        if not self.lib.dirty() and self.phase.startswith(("REFRESH", "OBSERVE")):
            return
        self._cancel_timer()
        self._step()

    # ---- 模拟时序 ----------------------------------------------------
    def _scaled(self, ms: int) -> int:
        return max(1, int(round(ms / float(self.speed))))

    def _timing(self) -> dict:
        return TIMING_PROFILES.get(self.timing, TIMING_PROFILES[DEFAULT_TIMING])

    def _poll_ms(self) -> int:
        if self.lib.state() == STATE_CALIBRATING:
            return CALIB_POLL_MS
        return self._scaled(self._timing()["think"])

    def _set_phase(self, text: str):
        self.phase = text

    # ---- 主循环 ------------------------------------------------------
    def _step(self):
        if self._closed or self.phase == "ERROR":
            return
        if self.lib.dirty():
            self._present()
            return
        if self.lib.state() == STATE_CALIBRATING:
            self._calib_step()
            return
        self._do_tick()

    def _present(self):
        if self._closed or self.phase == "ERROR":
            return
        if not self.lib.dirty():
            self._set_phase("IDLE")
            self._schedule(self._poll_ms(), self._step)
            return

        try:
            kind, buf = self.lib.frame_bytes()
            core_im = decode_mono(buf) if kind == FRAME_MONO else decode_gray4(buf)
            paper = panel_image(kind, core_im)
        except Exception as exc:
            self._fatal(f"frame: {exc}")
            return

        self.last_core_image = core_im
        self._show_panel(paper)

        full = (kind == FRAME_GRAY4) or self.lib.needs_full()
        if kind == FRAME_GRAY4:
            mode = "GRAY4"
            delay = self._timing()["gray4"]
        elif full:
            mode = "MONO FULL"
            delay = self._timing()["full"]
        else:
            mode = "MONO FAST"
            delay = self._timing()["fast"]
        sim_ms = self._scaled(delay)
        self.last_refresh_mode = f"{mode} {sim_ms}ms"

        if self.recorder.active:
            if not self.recorder.add(paper, self.lib.snapshot()):
                self._stop_recording(auto=True)

        self.lib.mark_refreshed(full)
        self._update_status()
        self._set_phase(f"REFRESH {sim_ms} ms (sim)")
        self._schedule(sim_ms, self._observe)

    def _observe(self):
        if self._closed or self.phase == "ERROR":
            return
        if self.lib.state() == STATE_CALIBRATING:
            wait = CALIB_POLL_MS
        else:
            wait = self._scaled(self._timing()["think"])
        self._set_phase(f"OBSERVE {wait} ms (sim)")
        self._schedule(max(1, wait), self._step)

    def _do_tick(self):
        if self._closed or self.phase == "ERROR":
            return
        st_before = self.lib.snapshot()
        buttons = self.pending_buttons
        self.pending_buttons = 0

        tilt = self.tilt_dir
        if self.demo_mode and st_before["state"] == STATE_PLAY:
            if self.demo_step >= DEMO_MAX_TICKS:
                self.demo_mode = False
                self._set_tool_status("DEMO stopped")
            else:
                tilt = demo_direction(self.demo_step)
                self._set_tilt_dir(tilt)
                self.demo_step += 1

        if self.lib.dirty():
            self._present()
            return
        if self.lib.tick(buttons, tilt) != 1:
            self._fatal("gs_tick refused (dirty frame or uninitialised core)")
            return
        self._after_core_tick()

        st = self.lib.snapshot()
        if st["state"] == STATE_CALIBRATING and not self.calib_active:
            self.calib_active = True
            self.calib_started = time.monotonic()
            self.pending_buttons = 0

        if self.restart_pending:
            if st["state"] == STATE_TITLE:
                self.restart_pending = False
                if st["calibrated"]:
                    self.lib.start_play()
                    self._after_core_tick()
                else:
                    self.calib_active = True
                    self.calib_started = time.monotonic()
                    self.lib.begin_calibration()
            elif st["state"] == STATE_GAME_OVER:
                self.pending_buttons |= BTN_OK

        self._update_status()
        if self.lib.dirty():
            self._present()
        else:
            self._set_phase("IDLE")
            self._schedule(self._poll_ms(), self._step)

    def _after_core_tick(self):
        sounds = self.lib.drain_sounds()
        if sounds and self.sound_player.enabled:
            self.sound_player.submit(sounds)
        st = self.lib.snapshot()
        self._maybe_persist_high(st)

    def _calib_step(self):
        if self._closed or self.phase == "ERROR":
            return
        if not self.calib_active:
            self.calib_active = True
            self.calib_started = time.monotonic()

        elapsed = time.monotonic() - self.calib_started
        pct = max(0, min(100, int(elapsed / CALIB_SECONDS * 100.0)))
        self.lib.calib_progress(pct)

        if elapsed >= CALIB_SECONDS:
            if self.lib.calib_done(True) != 1:
                self._fatal("calibration_done did not enter PLAY")
                return
            self.calib_active = False
            self._after_core_tick()
            self._update_status()
            if self.lib.dirty():
                self._present()
            else:
                self._set_phase("IDLE")
                self._schedule(self._poll_ms(), self._step)
            return

        if not self.lib.dirty():
            self.lib.tick(0, DIR_NONE)
        self._after_core_tick()
        self._update_status()
        if self.lib.dirty():
            self._present()
        else:
            self._set_phase("IDLE")
            self._schedule(CALIB_POLL_MS, self._step)

    # ---- 游戏动作 ----------------------------------------------------
    def primary_action(self):
        if not self.startup_ok:
            return
        self._take_over_demo()
        st = self.lib.state()
        if st == STATE_TITLE:
            if self.lib.calibrated():
                self.lib.start_play()
                self._after_core_tick()
                self._update_status()
                if self.lib.dirty():
                    self._present()
            else:
                self.start_calibration()
            return
        if st == STATE_CALIBRATING:
            self._set_tool_status(self.L["calibrating_status"])
            return
        if st in (STATE_PLAY, STATE_PAUSE):
            self.pending_buttons |= BTN_OK
            self._request_tick()
            return
        # DEAD / GAME_OVER: one press gets back to a fresh run.
        self.restart_pending = True
        self.pending_buttons |= BTN_OK
        self._request_tick()

    def start_calibration(self):
        if not self.startup_ok:
            return
        self._take_over_demo()
        if self.lib.state() == STATE_CALIBRATING and self.calib_active:
            return
        self.calib_active = True
        self.calib_started = time.monotonic()
        self.pending_buttons = 0
        self.restart_pending = False
        self.tilt_dir = DIR_NONE
        self.tilt_vec = (0.0, 0.0)
        self._update_tilt_items()
        self.lib.begin_calibration()
        self._update_status()
        self._request_tick()

    # ---- 最高分 ------------------------------------------------------
    def _maybe_persist_high(self, st: dict):
        if not self.persist:
            return
        if st.get("state") != STATE_GAME_OVER:
            self.hiscore_saved = False
            return
        if st.get("record") and not self.hiscore_saved:
            if self.hiscore.save(int(st.get("high", 0))):
                self.hiscore_saved = True
                self._set_tool_status(
                    f"{self.L['high_saved']} {int(st.get('high', 0)):04d}"
                )

    # ---- 截图 / 录制 -------------------------------------------------
    def _grab_window(self):
        from PIL import ImageGrab
        self.root.update_idletasks()
        x = int(self.root.winfo_rootx())
        y = int(self.root.winfo_rooty())
        w = int(self.root.winfo_width())
        h = int(self.root.winfo_height())
        return ImageGrab.grab(bbox=(x, y, x + w, y + h), all_screens=True)

    def screenshot(self):
        try:
            OUT_DIR.mkdir(parents=True, exist_ok=True)
            stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            path = _unique_path(OUT_DIR / f"screenshot_{stamp}.png")
            try:
                img = self._grab_window()
                note = ""
            except Exception as exc:
                if self.last_paper_image is None:
                    raise SnakeError(f"ImageGrab failed and no panel frame: {exc}")
                img = self.last_paper_image
                note = " (panel)"
            img.save(path, "PNG")
            self._set_tool_status(f"{self.L['shot_saved']}: {path.name}{note}")
            print(f"[GYRO SNAKE] screenshot {path}")
        except Exception as exc:
            self._set_tool_status(f"{self.L['shot_failed']}: {exc}")

    def toggle_record(self):
        if self.recorder.active:
            self._stop_recording(auto=False)
        else:
            if self.last_paper_image is None:
                self._set_tool_status(self.L["no_frame"])
                return
            try:
                d = self.recorder.start(OUT_DIR)
                self._set_tool_status(f"{self.L['rec_started']}: {d.name}")
                print(f"[GYRO SNAKE] record {d}")
            except Exception as exc:
                self._set_tool_status(f"{self.L['rec_failed']}: {exc}")
        self._refresh_buttons_if_needed()
        self._update_status()

    def _stop_recording(self, auto: bool):
        gif_path, _meta = self.recorder.stop(self.speed)
        if auto:
            self._set_tool_status("REC stopped (cap)")
        elif gif_path is not None:
            self._set_tool_status(f"{self.L['rec_saved']}: {gif_path.name}")
            print(f"[GYRO SNAKE] record saved {gif_path}")
        else:
            self._set_tool_status(self.L["rec_failed"])
        self._refresh_buttons_if_needed()

    # ---- 键盘 --------------------------------------------------------
    def _on_keypress(self, event):
        k = str(event.keysym).lower()
        if k in self.held_keys:
            return "break"
        self.held_keys.add(k)
        if k in ("space", "return", "kp_enter"):
            self.primary_action()
        elif k == "c":
            self.start_calibration()
        elif k in ("x", "0"):
            self.center_tilt()
        elif k in ("up", "w"):
            self._take_over_demo()
            self._set_tilt_dir(DIR_UP)
        elif k in ("right", "d"):
            self._take_over_demo()
            self._set_tilt_dir(DIR_RIGHT)
        elif k in ("down", "s"):
            self._take_over_demo()
            self._set_tilt_dir(DIR_DOWN)
        elif k in ("left", "a"):
            self._take_over_demo()
            self._set_tilt_dir(DIR_LEFT)
        elif k in ("1", "2", "4"):
            self.set_speed(int(k))
        elif k == "l":
            self.toggle_language()
        elif k == "m":
            self.toggle_sound()
        elif k == "t":
            self.cycle_timing()
        elif k == "p":
            self.screenshot()
        elif k == "r":
            self.toggle_record()
        else:
            self.held_keys.discard(k)
            return None
        return "break"

    def _on_keyrelease(self, event):
        self.held_keys.discard(str(event.keysym).lower())
        return "break"

    def _on_focus_out(self, event):
        if event.widget is not self.root:
            return
        self.held_keys.clear()

    # ---- 错误 / 关闭 --------------------------------------------------
    def _fatal(self, message: str):
        if self.fatal_reported:
            return
        self.fatal_reported = True
        self._cancel_timer()
        self.phase = "ERROR"
        print(f"GYRO SNAKE: {message}", file=sys.stderr)
        try:
            if self._error_item is not None:
                self._canvas.delete(self._error_item)
            self._error_item = self._canvas.create_text(
                WIN_W / 2, PANEL_Y + PANEL_H / 2,
                text="CONTRACT ERROR\n" + message,
                font=self._mono_font(-10, True), fill="#ffb3a8",
                width=PANEL_W - 40, justify="center",
            )
        except Exception:
            pass
        self._set_tool_status("ERROR: " + message)

    def on_tk_exception(self, exc_type, exc_value, exc_tb):
        traceback.print_exception(exc_type, exc_value, exc_tb, file=sys.stderr)
        self._fatal(f"{exc_type.__name__}: {exc_value}")

    def shutdown(self):
        if self._closed:
            return
        self._closed = True
        self._cancel_timer()
        if self.recorder.active:
            try:
                self.recorder.stop(self.speed)
            except Exception:
                pass
        try:
            self.sound_player.close()
        except Exception:
            pass
        try:
            self.root.destroy()
        except Exception:
            pass

    def on_close(self):
        self.shutdown()


# =========================================================================
# 11. 无窗口自测
# =========================================================================

class SelfTestFail(RuntimeError):
    pass


def run_selftest(lib: SnakeDLL, args) -> int:
    """同步、无窗口、无墙钟等待的自测。不导入 tkinter / ImageGrab。"""
    try:
        out = _resolve_out(args.out, OUT_DIR / "selftest")
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        print("SNAKE_SELFTEST fail reason=out")
        return 1

    def fail(reason: str, detail: str = ""):
        if detail:
            print(detail, file=sys.stderr)
        print(f"SNAKE_SELFTEST fail reason={reason}")
        return 1

    def check(cond, reason: str, detail: str = ""):
        if not cond:
            raise SelfTestFail(detail or reason)

    captured = []
    sounds_seen = []

    def decode_current():
        kind, buf = lib.frame_bytes()
        return kind, (decode_mono(buf) if kind == FRAME_MONO else decode_gray4(buf))

    def capture(tag: str) -> Image.Image:
        check(lib.dirty(), "capture", f"no pending frame for {tag}")
        kind, im = decode_current()
        name = f"frame_{len(captured):03d}_t{lib.tick_index():05d}_{tag}.png"
        im.save(out / name, "PNG")
        captured.append((im, name, tag, kind))
        return im

    def mark_pending():
        if not lib.dirty():
            return
        kind = lib.frame_kind()
        full = (kind == FRAME_GRAY4) or lib.needs_full()
        lib.mark_refreshed(full)
        check(lib.dirty() == False, "mark", "mark_refreshed did not clear dirty")

    def pump(tag: str | None = None):
        if not lib.dirty():
            return None
        im = capture(tag) if tag else None
        mark_pending()
        return im

    try:
        if int(args.ticks) < 1:
            raise SelfTestFail("--ticks must be >= 1")
        out.mkdir(parents=True, exist_ok=True)
        for old in out.glob("frame_*.png"):
            try:
                old.unlink()
            except OSError:
                pass
        for name in ("demo.gif", "selftest.json"):
            try:
                (out / name).unlink()
            except FileNotFoundError:
                pass
            except OSError:
                pass

        # --- ABI / init / high-score monotonic -------------------------
        check(lib.abi == 1, "abi", f"abi={lib.abi}")
        check(lib.init(args.seed) == 1, "init", "gs_init failed")
        st = lib.snapshot()
        check(st["dirty"], "init", "initial frame is not dirty")
        check(st["frame"] == FRAME_GRAY4, "init", f"initial frame={st['frame']}")
        check(st["state"] == STATE_TITLE, "init", f"state={st['state']}")
        check(st["card"] == CARD_TITLE, "init", f"card={st['card']}")
        check(st["dir"] == DIR_RIGHT, "init", f"dir={st['dir']}")
        check(len(lib.mono_bytes()) == MONO_FRAME_BYTES, "framebuffer", "mono len")
        check(len(lib.gray4_bytes()) == GRAY4_PAGE_BYTES, "framebuffer", "gray4 len")

        # dirty 时 tick 必须被拒绝
        before = lib.tick_index()
        check(lib.tick(0, DIR_NONE) == 0, "tick-guard",
              "gs_tick accepted a dirty frame")
        check(lib.tick_index() == before, "tick-guard",
              "tick index changed after a refused tick")

        lib.set_high(1234)
        check(lib.high() >= 1234, "hiscore", "high score did not adopt 1234")
        lib.set_high(1)
        check(lib.high() >= 1234, "hiscore", "high score went backwards")
        check(lib.init(args.seed) == 1, "reinit", "gs_init failed")
        check(lib.high() == 0, "reinit", f"high={lib.high()}")

        # --- title ----------------------------------------------------
        pump("title")
        check(len(captured) >= 1, "capture", "title frame missing")

        # --- calibration ---------------------------------------------
        lib.begin_calibration()
        pump("calib000")
        check(lib.state() == STATE_CALIBRATING, "calib", "state is not CALIBRATING")
        for pct in (20, 40, 60, 80, 100):
            check(lib.calib_progress(pct) == 1, "calib", f"progress {pct} refused")
            check(lib.calib_percent() == pct, "calib",
                  f"calib_percent={lib.calib_percent()} want {pct}")
            check(not lib.dirty(), "calib", "dirty before progress tick")
            check(lib.tick(0, DIR_NONE) == 1, "calib", "progress tick refused")
            pump(f"calib{pct:03d}")
            check(not lib.dirty(), "calib", "card left dirty")

        check(lib.calib_done(True) == 1, "calib", "calibration_done did not enter PLAY")
        st = lib.snapshot()
        check(st["state"] == STATE_PLAY, "play", f"state={st['state']}")
        check(st["card"] == CARD_NONE, "play", f"card={st['card']}")
        check(st["frame"] == FRAME_MONO, "play", f"frame={st['frame']}")
        check(st["calibrated"], "play", "calibrated flag clear")
        check(st["length"] == 3, "play", f"length={st['length']}")
        for snd in lib.drain_sounds():
            name = lib.sound_name(snd)
            if name not in sounds_seen:
                sounds_seen.append(name)
        pump("play_start")
        check(not lib.dirty(), "play", "start frame left dirty")

        # --- scripted play -------------------------------------------
        food_x, food_y, plan = plan_to_food(args.seed)
        if not args.quiet:
            print(f"[GYRO SNAKE] selftest food=({food_x},{food_y}) plan={len(plan)}")
        plan_idx = 0
        paused_once = False
        returned = False
        max_score = 0
        final = lib.snapshot()

        for step in range(int(args.ticks)):
            st = lib.snapshot()
            check(not st["dirty"], "pre-tick", f"dirty frame before tick {step}")

            if st["state"] == STATE_DEAD:
                check(lib.tick(0, DIR_NONE) == 1, "dead", "tick refused in DEAD")
                for snd in lib.drain_sounds():
                    name = lib.sound_name(snd)
                    if name not in sounds_seen:
                        sounds_seen.append(name)
                pump("game_over")
                final = lib.snapshot()
                continue

            if st["state"] == STATE_GAME_OVER:
                if not returned:
                    check(lib.tick(BTN_OK, DIR_NONE) == 1,
                          "gameover", "OK tick refused")
                    for snd in lib.drain_sounds():
                        name = lib.sound_name(snd)
                        if name not in sounds_seen:
                            sounds_seen.append(name)
                    pump("title_again")
                    returned = True
                else:
                    check(lib.tick(0, DIR_NONE) == 1, "gameover", "idle tick refused")
                final = lib.snapshot()
                continue

            if st["state"] == STATE_PAUSE:
                check(lib.tick(BTN_OK, DIR_NONE) == 1, "pause", "resume refused")
                for snd in lib.drain_sounds():
                    name = lib.sound_name(snd)
                    if name not in sounds_seen:
                        sounds_seen.append(name)
                pump("resume")
                continue

            if st["state"] != STATE_PLAY:
                check(lib.tick(0, DIR_NONE) == 1, "state", f"tick refused in {st['state']}")
                pump()
                continue

            tilt = plan[min(plan_idx, len(plan) - 1)]
            buttons = 0
            if not paused_once and step >= 2:
                buttons = BTN_OK
                paused_once = True

            check(lib.tick(buttons, tilt) == 1, "tick", f"tick refused at step {step}")
            for snd in lib.drain_sounds():
                name = lib.sound_name(snd)
                if name not in sounds_seen:
                    sounds_seen.append(name)

            after = lib.snapshot()
            final = after
            max_score = max(max_score, int(after["score"]))
            if not (buttons & BTN_OK):
                plan_idx += 1

            if lib.dirty():
                tag = None
                if buttons & BTN_OK:
                    tag = "paused"
                elif after["score"] > st["score"]:
                    tag = "eat"
                elif step % 25 == 0:
                    tag = "play"
                pump(tag)
            check(not lib.dirty(), "loop", f"dirty frame left at step {step}")

        check(max_score > 0, "loop", "score never advanced (sweep missed the apple)")
        check(len(captured) >= 8, "capture", f"only {len(captured)} frames")
        check(any(kind == FRAME_MONO for _im, _n, _t, kind in captured),
              "capture", "no mono frame captured")
        check(any(kind == FRAME_GRAY4 for _im, _n, _t, kind in captured),
              "capture", "no gray4 frame captured")

        # --- GIF ------------------------------------------------------
        gif_frames = [
            im.convert("L").resize((240, 400), Image.Resampling.NEAREST)
            for im, _n, _t, _k in captured
        ]
        gif_path = out / "demo.gif"
        if len(gif_frames) == 1:
            gif_frames[0].save(gif_path, "GIF", duration=250, loop=0)
        else:
            gif_frames[0].save(
                gif_path, "GIF", save_all=True,
                append_images=gif_frames[1:], duration=250, loop=0,
            )
        check(gif_path.exists(), "gif", "demo.gif missing")
        with Image.open(gif_path) as gif:
            check(gif.n_frames >= 2, "gif", f"gif frames={gif.n_frames}")

        meta = {
            "schema": 1,
            "ticks": int(args.ticks),
            "seed": int(args.seed),
            "frames": len(captured),
            "max_score": max_score,
            "final_score": int(final["score"]),
            "high_score": int(lib.high()),
            "length": int(final["length"]),
            "state": str(final["state_name"]),
            "card": str(final["card_name"]),
            "png": [name for _im, name, _t, _k in captured],
            "demo_gif": "demo.gif",
            "sounds": sounds_seen,
            "dll": str(lib.path),
            "abi": int(lib.abi),
            "timing": "scripted direction sequence; no wall-clock waits",
        }
        (out / "selftest.json").write_text(
            json.dumps(meta, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

        print(f"SNAKE_SELFTEST ok frames={len(captured)} score={max_score}")
        return 0
    except SelfTestFail as exc:
        return fail("check", str(exc))
    except Exception:
        traceback.print_exc(file=sys.stderr)
        return fail("internal")


# =========================================================================
# 12. 窗口模式 / 窗口截图测试
# =========================================================================

def enable_dpi_awareness():
    """在 tk.Tk() 之前设置 system DPI aware（Windows）。"""
    if sys.platform != "win32":
        return
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
        return
    except Exception:
        pass
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass


def _make_root(tk, title: str):
    root = tk.Tk()
    root.withdraw()
    root.tk.call("tk", "scaling", 1.0)
    sw = int(root.winfo_screenwidth())
    sh = int(root.winfo_screenheight())
    return root, sw, sh


def _prepare_window(root, title: str, sw: int, sh: int):
    root.title(title)
    root.configure(bg=SHELL)
    root.resizable(False, False)
    x = max(0, (sw - WIN_W) // 2)
    root.geometry(f"{WIN_W}x{WIN_H}+{x}+0")


def run_window(lib: SnakeDLL, args) -> int:
    """普通 GUI 模式。tkinter 只在函数内导入。"""
    enable_dpi_awareness()
    try:
        import tkinter as tk
    except Exception as exc:
        print(f"GYRO SNAKE: tkinter unavailable ({exc})", file=sys.stderr)
        print("SNAKE_START fail tkinter")
        return 1
    if float(tk.TkVersion) < 8.6:
        print(f"SNAKE_START fail tk={tk.TkVersion} need>=8.6")
        return 1

    title = "GYRO SNAKE — reTerminal Sticky Simulator"
    root, sw, sh = _make_root(tk, title)
    if sw < WIN_W or sh < WIN_H + 60:
        print(f"SNAKE_START fail screen={sw}x{sh} need>={WIN_W}x{WIN_H + 60}")
        root.destroy()
        return 1

    _prepare_window(root, title, sw, sh)
    try:
        app = GameUI(root, tk, lib, args)
    except Exception as exc:
        print(f"GYRO SNAKE: UI init failed ({exc})", file=sys.stderr)
        print("SNAKE_START fail ui")
        root.destroy()
        return 1

    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.report_callback_exception = app.on_tk_exception
    root.after(120, lambda: app.start(autoplay=bool(args.demo)))
    root.deiconify()
    root.lift()
    root.focus_force()
    print(f"[GYRO SNAKE] window {WIN_W}x{WIN_H} dll={lib.path}")
    try:
        root.mainloop()
    finally:
        app.shutdown()
    return 0


def run_window_test(lib: SnakeDLL, args, seconds: float) -> int:
    """打开窗口 -> 自动进入 PLAY demo -> 运行 N 秒 -> ImageGrab 截整窗。"""
    if seconds < 1.0:
        print("SNAKE_WINDOW fail reason=seconds must be >= 1")
        return 1

    enable_dpi_awareness()
    try:
        import tkinter as tk
    except Exception as exc:
        print(f"GYRO SNAKE: tkinter unavailable ({exc})", file=sys.stderr)
        print("SNAKE_WINDOW fail reason=tkinter")
        return 1
    if float(tk.TkVersion) < 8.6:
        print(f"SNAKE_WINDOW fail reason=tk={tk.TkVersion} need>=8.6")
        return 1
    try:
        from PIL import ImageGrab
    except Exception as exc:
        print(f"GYRO SNAKE: ImageGrab unavailable ({exc})", file=sys.stderr)
        print("SNAKE_WINDOW fail reason=imagegrab")
        return 1

    try:
        out = _resolve_out(args.out, OUT_DIR / "window.png")
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        print("SNAKE_WINDOW fail reason=out")
        return 1
    if out.is_dir():
        out = out / "window.png"

    title = "GYRO SNAKE — reTerminal Sticky Simulator"
    root, sw, sh = _make_root(tk, title)
    if sw < WIN_W or sh < WIN_H + 60:
        print(f"SNAKE_WINDOW fail screen={sw}x{sh} need>={WIN_W}x{WIN_H + 60}")
        root.destroy()
        return 1

    _prepare_window(root, title, sw, sh)
    args.demo = True
    try:
        app = GameUI(root, tk, lib, args)
    except Exception as exc:
        print(f"GYRO SNAKE: UI init failed ({exc})", file=sys.stderr)
        print("SNAKE_WINDOW fail reason=ui")
        root.destroy()
        return 1

    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.report_callback_exception = app.on_tk_exception
    result = {"code": 1, "done": False}

    def fail(reason: str, detail: str = ""):
        if result["done"]:
            return
        if detail:
            print(detail, file=sys.stderr)
        print(f"SNAKE_WINDOW fail reason={reason}")
        result["code"] = 1
        result["done"] = True
        app.shutdown()

    def do_grab():
        if result["done"]:
            return
        if not app.startup_ok or app.phase == "ERROR":
            fail("startup", "the simulator did not initialise")
            return
        state = app.lib.state()
        if state not in (STATE_PLAY, STATE_PAUSE):
            fail(f"state={app.lib.state_name(state)}", "demo is not in PLAY")
            return
        if app.last_paper_image is None:
            fail("panel", "no frame was presented")
            return
        try:
            root.update_idletasks()
            root.lift()
            root.focus_force()
            root.update_idletasks()
            x = int(root.winfo_rootx())
            y = int(root.winfo_rooty())
            w = int(root.winfo_width())
            h = int(root.winfo_height())
            if (w, h) != (WIN_W, WIN_H):
                fail(f"dpi size={w}x{h} expected={WIN_W}x{WIN_H}")
                return
            img = ImageGrab.grab(bbox=(x, y, x + w, y + h), all_screens=True)
        except Exception as exc:
            fail("grab", f"ImageGrab failed: {exc}")
            return

        if img.size != (WIN_W, WIN_H):
            fail(f"dpi grab={img.size[0]}x{img.size[1]} expected={WIN_W}x{WIN_H}")
            return

        panel = img.crop((PANEL_X, PANEL_Y, PANEL_X + PANEL_W, PANEL_Y + PANEL_H))
        lo, hi = panel.convert("L").getextrema()
        if lo == hi:
            fail("panel blank")
            return
        deck = img.crop((0, 684, WIN_W, WIN_H)).convert("RGB")
        dlo, dhi = deck.convert("L").getextrema()
        if dlo == dhi:
            fail("deck blank")
            return
        # 倾斜盘 / 主按钮的绿色强调必须在截图里真的出现。
        data = deck.resize((130, 79)).tobytes()
        if not any(_approx_green(data[i:i + 3]) for i in range(0, len(data), 3)):
            fail("deck accent", "no green accent pixels in the control deck")
            return

        try:
            out.parent.mkdir(parents=True, exist_ok=True)
            img.save(out, "PNG")
        except Exception as exc:
            fail("save", f"cannot save {out}: {exc}")
            return

        print(f"SNAKE_WINDOW ok path={out.resolve()} size={img.width}x{img.height}")
        result["code"] = 0
        result["done"] = True
        app.shutdown()

    def capture():
        if result["done"]:
            return
        root.attributes("-topmost", True)
        root.lift()
        root.focus_force()
        root.update_idletasks()
        root.after(320, do_grab)

    root.attributes("-topmost", True)
    root.after(140, lambda: app.start(autoplay=True))
    root.after(int(seconds * 1000), capture)
    root.deiconify()
    root.lift()
    root.focus_force()
    try:
        root.mainloop()
    finally:
        app.shutdown()
    if not result["done"]:
        print("SNAKE_WINDOW fail reason=window closed")
        return 1
    return int(result["code"])


# =========================================================================
# 13. CLI
# =========================================================================

def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        prog="play.py",
        description="GYRO SNAKE desktop simulator (Tkinter + ctypes).",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--selftest", action="store_true",
                      help="run the headless deterministic selftest")
    mode.add_argument("--window-test", nargs="?", const=3.0, type=float,
                      metavar="SECONDS",
                      help="open the window, run N seconds, grab a PNG")
    parser.add_argument("--ticks", type=int, default=240,
                        help="selftest ticks (default 240)")
    parser.add_argument("--out", default=None,
                        help="selftest output dir or window-test PNG path")
    parser.add_argument("--dll", default=None, help="explicit gyro_snake.dll path")
    parser.add_argument("--no-build", action="store_true",
                        help="accepted for compatibility; launchers build the DLL")
    parser.add_argument("--seed", type=int, default=1, help="gs_init seed")
    parser.add_argument("--speed", type=int, choices=SPEED_LEVELS, default=None,
                        help="initial speed override (1/2/4)")
    parser.add_argument("--timing", choices=TIMING_ORDER, default=None,
                        help="initial timing override")
    parser.add_argument("--lang", choices=("zh", "en"), default=None,
                        help="initial UI language override")
    parser.add_argument("--demo", action="store_true",
                        help="start the window in demo mode")
    parser.add_argument("--quiet", action="store_true", help="less console noise")
    return parser, parser.parse_args(argv)


def resolve_dll(cli_value) -> Path:
    """--dll / GYRO_SNAKE_DLL / desktop/build/gyro_snake.dll。"""
    if cli_value:
        p = Path(str(cli_value)).expanduser()
        return (p if p.is_absolute() else (Path.cwd() / p)).resolve()
    env = os.environ.get("GYRO_SNAKE_DLL", "").strip()
    if env:
        p = Path(env).expanduser()
        return (p if p.is_absolute() else (Path.cwd() / p)).resolve()
    return DLL_PATH.resolve()


def _version_ok() -> tuple:
    if sys.version_info < (3, 10):
        return False, f"python={sys.version.split()[0]} need>=3.10"
    if PIL is None:
        return False, "pillow missing"
    try:
        major = int(str(PIL.__version__).split(".")[0])
    except Exception:
        major = 0
    if major < 9:
        return False, f"pillow={PIL.__version__} need>=9"
    return True, ""


def main(argv=None) -> int:
    parser, args = parse_args(argv)

    ok, why = _version_ok()
    if not ok:
        print(f"SNAKE_START fail {why}")
        return 1

    # CLI overrides for the interactive window (never persisted by --selftest).
    if args.speed is not None:
        args._speed_override = args.speed
    if args.timing is not None:
        args._timing_override = args.timing
    if args.lang is not None:
        args._lang_override = args.lang

    mode = "selftest" if args.selftest else (
        "window-test" if args.window_test is not None else "window"
    )

    try:
        dll_path = resolve_dll(args.dll)
    except Exception as exc:
        print(f"GYRO SNAKE: {exc}", file=sys.stderr)
        if mode == "selftest":
            print("SNAKE_SELFTEST fail reason=dll path")
        elif mode == "window-test":
            print("SNAKE_WINDOW fail reason=dll path")
        else:
            print("SNAKE_START fail dll path")
        return 1

    if not dll_path.is_file() or dll_path.stat().st_size == 0:
        print(f"GYRO SNAKE: DLL not found: {dll_path}")
        print("GYRO SNAKE: build it first:  bash gyro-snake/desktop/build.sh")
        if mode == "selftest":
            print("SNAKE_SELFTEST fail reason=dll missing")
        elif mode == "window-test":
            print("SNAKE_WINDOW fail reason=dll missing")
        else:
            print("SNAKE_START fail dll missing")
        return 1

    try:
        lib = SnakeDLL(dll_path)
    except Exception as exc:
        print(f"GYRO SNAKE: {exc}", file=sys.stderr)
        if mode == "selftest":
            print("SNAKE_SELFTEST fail reason=dll load")
        elif mode == "window-test":
            print("SNAKE_WINDOW fail reason=dll load")
        else:
            print("SNAKE_START fail dll load")
        return 1

    if not args.quiet:
        print(f"[GYRO SNAKE] dll={dll_path} abi={lib.abi}")

    if args.selftest:
        return run_selftest(lib, args)
    if args.window_test is not None:
        return run_window_test(lib, args, float(args.window_test))
    return run_window(lib, args)


if __name__ == "__main__":
    sys.exit(main())