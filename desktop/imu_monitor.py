#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GYRO SNAKE — real-time IMU position ball over USB serial.

The firmware prints one line at 10 Hz:

    I (...) gs_imu: IMU r=<rel roll ddeg> p=<rel pitch ddeg> ar=<abs roll> ap=<abs pitch> \
        d=<dir> gx=<mdps> gy=<mdps> gz=<mdps> ax=<mg> ay=<mg> az=<mg>

This window reads COM65 (or --port), draws the tilt as a ball with a trail,
and shows the raw gyro/accel values so a shake can be distinguished from a
tilt. It is read-only; press DOWN on the device to set the current pose as
zero.

Usage:
    python desktop/imu_monitor.py --port COM65
"""

from __future__ import annotations

import argparse
import math
import queue
import re
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk

try:
    import serial
except Exception as exc:  # pragma: no cover
    print(f"pyserial is required: {exc}", file=sys.stderr)
    raise SystemExit(2)

LINE_RE = re.compile(
    r"IMU r=(-?\d+) p=(-?\d+) ar=(-?\d+) ap=(-?\d+) d=(-?\d+) "
    r"gx=(-?\d+) gy=(-?\d+) gz=(-?\d+) ax=(-?\d+) ay=(-?\d+) az=(-?\d+)"
)

SHELL = "#1b1a17"
PANEL = "#24221d"
PAPER = "#f3efe4"
GREEN = "#3f8f5a"
MUTED = "#8c8577"
TEXT = "#ece6d8"
RED = "#c0503f"

DIR_NAMES = {-1: "NONE", 0: "UP", 1: "RIGHT", 2: "DOWN", 3: "LEFT"}


class SerialReader(threading.Thread):
    def __init__(self, port: str, out: queue.Queue):
        super().__init__(daemon=True, name="imu-serial")
        self.port = port
        self.out = out
        self.stop_flag = threading.Event()
        self.ser = None
        self.error = None

    def run(self) -> None:
        try:
            self.ser = serial.Serial(self.port, 115200, timeout=0.2)
            # Do not hold the board in reset.
            try:
                self.ser.dtr = False
                self.ser.rts = False
            except Exception:
                pass
        except Exception as exc:
            self.error = str(exc)
            self.out.put(("error", self.error))
            return

        self.out.put(("open", self.port))
        while not self.stop_flag.is_set():
            try:
                raw = self.ser.readline()
            except Exception as exc:
                self.out.put(("error", str(exc)))
                break
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").strip()
            m = LINE_RE.search(line)
            if m:
                values = [int(g) for g in m.groups()]
                self.out.put(
                    (
                        "imu",
                        {
                            "r": values[0] / 10.0,
                            "p": values[1] / 10.0,
                            "ar": values[2] / 10.0,
                            "ap": values[3] / 10.0,
                            "d": values[4],
                            "gx": values[5] / 1000.0,
                            "gy": values[6] / 1000.0,
                            "gz": values[7] / 1000.0,
                            "ax": values[8] / 1000.0,
                            "ay": values[9] / 1000.0,
                            "az": values[10] / 1000.0,
                        },
                    )
                )
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass
        self.out.put(("closed", None))

    def stop(self) -> None:
        self.stop_flag.set()
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass


class MonitorApp:
    def __init__(self, root: tk.Tk, port: str):
        self.root = root
        self.port = port
        self.queue: queue.Queue = queue.Queue()
        self.reader: SerialReader | None = None
        self.trail: list[tuple[float, float]] = []
        self.last = {
            "r": 0.0, "p": 0.0, "ar": 0.0, "ap": 0.0, "d": -1,
            "gx": 0.0, "gy": 0.0, "gz": 0.0, "ax": 0.0, "ay": 0.0, "az": 0.0,
        }
        self.packets = 0
        self.ball_cx = 210
        self.ball_cy = 260
        self.ball_r = 150
        self._build()
        self.root.after(50, self._pump)
        self.connect()

    def _build(self) -> None:
        root = self.root
        root.title("GYRO SNAKE — IMU monitor")
        root.geometry("600x760")
        root.configure(bg=SHELL)
        root.resizable(False, False)

        tk.Frame(root, bg=GREEN, height=3).place(x=0, y=0, width=600, height=3)
        tk.Label(
            root, text="GYRO SNAKE", bg=SHELL, fg=PAPER,
            font=("Consolas", -18, "bold"), anchor="w",
        ).place(x=18, y=10, width=300, height=24)
        tk.Label(
            root, text="real-time IMU position ball · read-only serial monitor",
            bg=SHELL, fg=MUTED, font=("Consolas", -9), anchor="w",
        ).place(x=18, y=36, width=560, height=14)

        self.canvas = tk.Canvas(
            root, width=420, height=520, bg=SHELL, highlightthickness=0, bd=0
        )
        self.canvas.place(x=18, y=60)

        self.status = tk.Label(
            root, text="connecting…", bg=SHELL, fg=MUTED,
            font=("Consolas", -9), anchor="w",
        )
        self.status.place(x=18, y=592, width=560, height=16)

        tk.Label(
            root, text="PORT", bg=SHELL, fg=TEXT,
            font=("Consolas", -9, "bold"), anchor="w",
        ).place(x=18, y=620, width=60, height=16)
        self.port_var = tk.StringVar(value=self.port)
        ttk.Entry(root, textvariable=self.port_var, width=12).place(x=78, y=618, width=110, height=24)
        tk.Button(
            root, text="CONNECT", command=self.connect, bg="#35322a", fg=PAPER,
            activebackground=GREEN, activeforeground=PAPER, relief="flat", bd=0,
            font=("Consolas", -9, "bold"), cursor="hand2",
        ).place(x=198, y=618, width=100, height=24)
        tk.Button(
            root, text="DISCONNECT", command=self.disconnect, bg="#35322a", fg=PAPER,
            activebackground=RED, activeforeground=PAPER, relief="flat", bd=0,
            font=("Consolas", -9, "bold"), cursor="hand2",
        ).place(x=306, y=618, width=120, height=24)

        tk.Label(
            root, text="Hold the device the way you play, press DOWN on the device to set zero.",
            bg=SHELL, fg=GREEN, font=("Consolas", -9), anchor="w",
        ).place(x=18, y=658, width=560, height=16)
        tk.Label(
            root, text="Tilt = rotate the board. A pure up/down shake without rotation is translation, "
                      "not tilt: the gyro/accel traces show it, but the tilt ball will not move.",
            bg=SHELL, fg=MUTED, font=("Consolas", -8), anchor="w", justify="left",
            wraplength=560,
        ).place(x=18, y=680, width=560, height=40)

        # readout column
        self.readout = {}
        rows = [
            ("DIR", "d"), ("REL ROLL", "r"), ("REL PITCH", "p"),
            ("ABS ROLL", "ar"), ("ABS PITCH", "ap"),
            ("GX dps", "gx"), ("GY dps", "gy"), ("GZ dps", "gz"),
            ("AX g", "ax"), ("AY g", "ay"), ("AZ g", "az"),
            ("PACKETS", "n"),
        ]
        x = 450
        y = 70
        for label, key in rows:
            tk.Label(
                root, text=label, bg=SHELL, fg=MUTED,
                font=("Consolas", -8, "bold"), anchor="w",
            ).place(x=x, y=y, width=130, height=12)
            val = tk.Label(
                root, text="—", bg=SHELL, fg=TEXT,
                font=("Consolas", -10, "bold"), anchor="w",
            )
            val.place(x=x, y=y + 13, width=130, height=16)
            self.readout[key] = val
            y += 38

    # ---- drawing ------------------------------------------------------
    def _draw_ball(self) -> None:
        c = self.canvas
        c.delete("all")
        cx, cy, r = self.ball_cx, self.ball_cy, self.ball_r

        c.create_oval(cx - r - 4, cy - r - 4, cx + r + 4, cy + r + 4,
                      outline="#3a362d", width=2)
        c.create_oval(cx - r, cy - r, cx + r, cy + r,
                      fill=PANEL, outline="#4a453a", width=1)

        # sphere grid: latitudes / longitudes
        for k in (0.35, 0.7):
            rr = int(r * k)
            c.create_oval(cx - rr, cy - r, cx + rr, cy + r,
                          outline="#33302a", width=1)
            c.create_oval(cx - r, cy - rr, cx + r, cy + rr,
                          outline="#33302a", width=1)
        c.create_line(cx - r, cy, cx + r, cy, fill="#33302a")
        c.create_line(cx, cy - r, cx, cy + r, fill="#33302a")

        # direction labels
        c.create_text(cx, cy - r - 18, text="UP", fill=MUTED, font=("Consolas", -9, "bold"))
        c.create_text(cx, cy + r + 18, text="DOWN", fill=MUTED, font=("Consolas", -9, "bold"))
        c.create_text(cx - r - 24, cy, text="LEFT", fill=MUTED, font=("Consolas", -9, "bold"))
        c.create_text(cx + r + 24, cy, text="RIGHT", fill=MUTED, font=("Consolas", -9, "bold"))

        # deadzone ring (8 deg of the 60 deg full scale)
        dz = r * (8.0 / 60.0)
        c.create_oval(cx - dz, cy - dz, cx + dz, cy + dz,
                      outline=GREEN, width=1, dash=(3, 3))

        # trail
        for i, (tx, ty) in enumerate(self.trail[-60:]):
            frac = (i + 1) / max(1, len(self.trail[-60:]))
            shade = int(70 + 120 * frac)
            c.create_oval(tx - 2, ty - 2, tx + 2, ty + 2,
                          fill=f"#{shade:02x}{shade + 20:02x}{shade:02x}", outline="")

        roll = self.last["r"]
        pitch = self.last["p"]
        mag = math.hypot(roll, pitch)
        scale = min(1.0, mag / 60.0)
        angle = math.atan2(pitch, roll) if mag > 1e-6 else 0.0
        px = cx + math.cos(angle) * scale * r
        py = cy - math.sin(angle) * scale * r

        c.create_line(cx, cy, px, py, fill=GREEN, width=2)
        c.create_oval(px - 9, py - 9, px + 9, py + 9, fill=GREEN, outline=PAPER, width=2)
        c.create_oval(cx - 3, cy - 3, cx + 3, cy + 3, fill=PAPER, outline="")

        # readouts
        d = self.last["d"]
        self.readout["d"].configure(text=DIR_NAMES.get(d, "?"), fg=GREEN if d >= 0 else MUTED)
        for key in ("r", "p", "ar", "ap"):
            self.readout[key].configure(text=f"{self.last[key]:7.1f}°")
        for key in ("gx", "gy", "gz"):
            self.readout[key].configure(text=f"{self.last[key]:7.2f}")
        for key in ("ax", "ay", "az"):
            self.readout[key].configure(text=f"{self.last[key]:7.3f}")
        self.readout["n"].configure(text=str(self.packets))

    # ---- serial pump --------------------------------------------------
    def connect(self) -> None:
        self.disconnect()
        port = self.port_var.get().strip() or self.port
        self.reader = SerialReader(port, self.queue)
        self.reader.start()
        self.status.configure(text=f"opening {port}…", fg=MUTED)

    def disconnect(self) -> None:
        if self.reader is not None:
            self.reader.stop()
            self.reader = None
        self.status.configure(text="disconnected", fg=MUTED)

    def _pump(self) -> None:
        try:
            while True:
                kind, payload = self.queue.get_nowait()
                if kind == "imu":
                    self.last.update(payload)
                    self.packets += 1
                    mag = math.hypot(self.last["r"], self.last["p"])
                    scale = min(1.0, mag / 60.0)
                    angle = math.atan2(self.last["p"], self.last["r"]) if mag > 1e-6 else 0.0
                    self.trail.append(
                        (
                            self.ball_cx + math.cos(angle) * scale * self.ball_r,
                            self.ball_cy - math.sin(angle) * scale * self.ball_r,
                        )
                    )
                    if len(self.trail) > 120:
                        self.trail = self.trail[-120:]
                elif kind == "open":
                    self.status.configure(text=f"connected {payload} · 10 Hz telemetry", fg=GREEN)
                elif kind == "closed":
                    self.status.configure(text="serial closed", fg=MUTED)
                elif kind == "error":
                    self.status.configure(text=f"serial error: {payload}", fg=RED)
        except queue.Empty:
            pass
        self._draw_ball()
        self.root.after(50, self._pump)


def _dpi_aware() -> None:
    """Make Tk and ImageGrab use the same physical pixels on Windows."""
    if sys.platform != "win32":
        return
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
        return
    except Exception:
        pass
    try:
        import ctypes
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass


def main() -> int:
    _dpi_aware()
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM65")
    parser.add_argument("--shot", default=None,
                        help="grab the monitor window to this PNG and exit")
    parser.add_argument("--seconds", type=float, default=3.0)
    args = parser.parse_args()
    root = tk.Tk()
    app = MonitorApp(root, args.port)

    def on_close() -> None:
        app.disconnect()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)

    if args.shot:
        def grab() -> None:
            try:
                from PIL import ImageGrab
                root.update_idletasks()
                root.lift()
                root.focus_force()
                root.update_idletasks()
                x = int(root.winfo_rootx())
                y = int(root.winfo_rooty())
                w = int(root.winfo_width())
                h = int(root.winfo_height())
                img = ImageGrab.grab(bbox=(x, y, x + w, y + h), all_screens=True)
                img.save(args.shot, "PNG")
                print(f"MONITOR_SHOT ok path={args.shot} size={img.width}x{img.height}")
            except Exception as exc:
                print(f"MONITOR_SHOT fail {exc}")
            on_close()

        root.after(int(args.seconds * 1000), grab)

    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())