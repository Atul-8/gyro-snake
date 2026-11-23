#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GYRO SNAKE — IMU 四方向联调页（串口双向）。

固件每秒打印 N 行（默认 10 Hz，可用 $hz 调到 20/50）：

    I (...) gs_imu: IMU r=<相对roll 0.1°> p=<相对pitch 0.1°> ar=<绝对roll> ap=<绝对pitch> \
        d=<方向> gx=<mdps> gy=<mdps> gz=<mdps> ax=<mg> ay=<mg> az=<mg> \
        m=<轴映射位> dz=<死区0.1°> ra=<回中0.1°> lk=<锁存方向> an=<|a|mg> pr=<纯加速计roll> pp=<纯加速计pitch>

并接受 $ 开头的命令（console.c）：$map S R P、$zero、$cal、$dead N、$rearm N、
$stable N、$hz N、$?。

联调流程（四个方向各一次）：
  1. CONNECT 后拿起设备，摆好“玩游戏的握姿”，按【置零】；
  2. 把设备往“上”倾斜并保持 1 秒，点【采 UP】；同样采 RIGHT / DOWN / LEFT；
  3. 点【分析】：窗口会给出每个方向的相对 roll/pitch 均值，并给出
     8 种轴映射组合的得分排行 —— 点【应用建议】直接把正确映射写进固件；
  4. 四个方向的大箭头都对上后，把映射写回 imu_port.h（IMU_PORT_AXIS_*）。

Usage:
    python desktop/imu_debug.py --port COM65
    python desktop/imu_debug.py --shot out.png   # 截图自检
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
    r"(?: m=(-?\d+) dz=(-?\d+) ra=(-?\d+) lk=(-?\d+) an=(-?\d+) pr=(-?\d+) pp=(-?\d+))?"
)
CFG_RE = re.compile(
    r"CFG map=(\d)(\d)(\d) deadzone=(-?\d+) rearm=(-?\d+) stable=(-?\d+) "
    r"telem=(-?\d+) neutral=(-?\d+)/(-?\d+) ready=(\d)"
)
CMD_RE = re.compile(r"CMD (\w+) (ok|err.*)")

SHELL = "#1b1a17"
PANEL = "#24221d"
PAPER = "#f3efe4"
GREEN = "#3f8f5a"
GREEN_BRIGHT = "#7fd6a0"
AMBER = "#d9a441"
MUTED = "#8c8577"
TEXT = "#ece6d8"
RED = "#c0503f"
BLUE = "#5a8fd6"

DIR_NAMES = {-1: "NONE", 0: "UP", 1: "RIGHT", 2: "DOWN", 3: "LEFT"}
DIR_ORDER = (0, 1, 2, 3)          # UP, RIGHT, DOWN, LEFT
DIR_KEYS = ("UP", "RIGHT", "DOWN", "LEFT")
FULL_SCALE_DEG = 20.0

CAPTURE_SECONDS = 1.2


def _dpi_aware() -> None:
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


class SerialLink(threading.Thread):
    """One reader thread; writes are serialised through send()."""

    def __init__(self, port: str, out: queue.Queue):
        super().__init__(daemon=True, name="imu-debug")
        self.port = port
        self.out = out
        self.stop_flag = threading.Event()
        self.ser = None
        self._tx_lock = threading.Lock()

    def run(self) -> None:
        try:
            self.ser = serial.Serial(self.port, 115200, timeout=0.15)
            try:
                self.ser.dtr = False
                self.ser.rts = False
            except Exception:
                pass
        except Exception as exc:
            self.out.put(("error", str(exc)))
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
                v = [int(g) if g is not None else 0 for g in m.groups()]
                self.out.put(("imu", {
                    "r": v[0] / 10.0, "p": v[1] / 10.0,
                    "ar": v[2] / 10.0, "ap": v[3] / 10.0,
                    "d": v[4],
                    "gx": v[5] / 1000.0, "gy": v[6] / 1000.0, "gz": v[7] / 1000.0,
                    "ax": v[8] / 1000.0, "ay": v[9] / 1000.0, "az": v[10] / 1000.0,
                    "m": v[11], "dz": v[12] / 10.0, "ra": v[13] / 10.0,
                    "lk": v[14] if m.group(15) else -1, "an": v[15] / 1000.0,
                    "pr": v[16] / 10.0, "pp": v[17] / 10.0,
                }))
                continue
            c = CFG_RE.search(line)
            if c:
                self.out.put(("cfg", {
                    "swap": int(c.group(1)), "invr": int(c.group(2)),
                    "invp": int(c.group(3)),
                    "dz": int(c.group(4)) / 10.0, "ra": int(c.group(5)) / 10.0,
                    "stable": int(c.group(6)), "hz": int(c.group(7)),
                    "nr": int(c.group(8)) / 100.0, "np": int(c.group(9)) / 100.0,
                    "ready": int(c.group(10)),
                }))
                continue
            k = CMD_RE.search(line)
            if k:
                self.out.put(("cmd", (k.group(1), k.group(2))))
                continue
            if "gs_" in line or "gyro_snake" in line:
                self.out.put(("log", line))

    def send(self, text: str) -> None:
        if not self.ser:
            return
        with self._tx_lock:
            try:
                self.ser.write(("$" + text + "\n").encode("ascii"))
            except Exception as exc:
                self.out.put(("error", f"send: {exc}"))

    def stop(self) -> None:
        self.stop_flag.set()
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass


class DebugApp:
    def __init__(self, root: tk.Tk, port: str):
        self.root = root
        self.port = port
        self.queue: queue.Queue = queue.Queue()
        self.link: SerialLink | None = None
        self.last = {
            "r": 0.0, "p": 0.0, "ar": 0.0, "ap": 0.0, "d": -1,
            "gx": 0.0, "gy": 0.0, "gz": 0.0,
            "ax": 0.0, "ay": 0.0, "az": 0.0,
            "m": 0, "dz": 8.0, "ra": 4.0, "lk": -1, "an": 1.0,
            "pr": 0.0, "pp": 0.0,
        }
        self.cfg = {"swap": 0, "invr": 0, "invp": 0, "dz": 8.0, "ra": 4.0,
                    "stable": 2, "hz": 10, "nr": 0.0, "np": 0.0, "ready": 0}
        self.suggestion: tuple[int, int, int] | None = None
        self.packets = 0
        self.trail: list[tuple[float, float]] = []
        # capture[dir_index] -> list of (rel_roll, rel_pitch) samples
        self.capture: dict[int, list[tuple[float, float]]] = {}
        self.capture_until = 0.0
        self.capture_dir = -1
        self.log_lines: list[str] = []

        self._build()
        self.root.after(40, self._pump)
        self.connect()

    # ---- UI ------------------------------------------------------------
    def _build(self) -> None:
        root = self.root
        root.title("GYRO SNAKE — IMU 四方向联调")
        root.geometry("680x880")
        root.configure(bg=SHELL)
        root.resizable(False, False)

        tk.Frame(root, bg=GREEN, height=3).place(x=0, y=0, width=680, height=3)
        tk.Label(root, text="IMU 联调 · 四方向标定", bg=SHELL, fg=PAPER,
                 font=("Microsoft YaHei UI", -14, "bold"), anchor="w").place(
            x=18, y=10, width=400, height=26)
        tk.Label(root, text="倾斜 4 次 → 采样 → 分析 → 应用建议 → 写回固件",
                 bg=SHELL, fg=MUTED, font=("Microsoft YaHei UI", -9), anchor="w").place(
            x=18, y=38, width=640, height=16)

        # compass
        self.canvas = tk.Canvas(root, width=330, height=330, bg=SHELL,
                                highlightthickness=0, bd=0)
        self.canvas.place(x=18, y=64)
        self.cx, self.cy, self.cr = 160, 160, 130

        # readout column
        self.readout: dict[str, tk.Label] = {}
        rows = [
            ("方向 DIR", "d"), ("锁存 LOCK", "lk"),
            ("相对 roll/pitch", "rp"), ("纯加速计 pr/pp", "acc"),
            ("|加速度| g", "an"), ("陀螺 dps", "gyro"),
            ("轴映射 m", "map"), ("死区/回中/稳定", "tune"),
            ("零位 neutral", "neutral"), ("遥测 Hz", "hz"),
        ]
        x, y = 368, 66
        for label, key in rows:
            tk.Label(root, text=label, bg=SHELL, fg=MUTED,
                     font=("Microsoft YaHei UI", -8, "bold"), anchor="w").place(
                x=x, y=y, width=290, height=14)
            val = tk.Label(root, text="—", bg=SHELL, fg=TEXT,
                           font=("Consolas", -11, "bold"), anchor="w")
            val.place(x=x, y=y + 15, width=290, height=18)
            self.readout[key] = val
            y += 34

        # capture buttons
        tk.Label(root, text="① 采样：往某方向倾斜并保持，点对应按钮（采 1.2 秒）",
                 bg=SHELL, fg=GREEN, font=("Microsoft YaHei UI", -9), anchor="w").place(
            x=18, y=412, width=640, height=16)
        for i, name in enumerate(DIR_KEYS):
            b = tk.Button(root, text=f"采 {name}", command=lambda i=i: self.start_capture(i),
                          bg="#35322a", fg=PAPER, activebackground=GREEN,
                          activeforeground=PAPER, relief="flat", bd=0,
                          font=("Microsoft YaHei UI", -10, "bold"), cursor="hand2")
            b.place(x=18 + i * 108, y=434, width=100, height=30)
        tk.Button(root, text="清除", command=self.clear_capture, bg="#2a2620",
                  fg=MUTED, relief="flat", bd=0,
                  font=("Microsoft YaHei UI", -9), cursor="hand2").place(
            x=18 + 4 * 108, y=434, width=84, height=30)

        tk.Label(root, text="② 分析（对 8 种轴映射组合打分，得分最高者即正确映射）",
                 bg=SHELL, fg=GREEN, font=("Microsoft YaHei UI", -9), anchor="w").place(
            x=18, y=472, width=640, height=16)
        self.analysis = tk.Text(root, height=9, bg=PANEL, fg=TEXT, relief="flat",
                                font=("Consolas", -10), state="disabled", wrap="none")
        self.analysis.place(x=18, y=492, width=644, height=150)

        # actions
        tk.Label(root, text="③ 固件动作 / 轴映射（立即生效，无需重新烧录）",
                 bg=SHELL, fg=GREEN, font=("Microsoft YaHei UI", -9), anchor="w").place(
            x=18, y=650, width=640, height=16)
        actions = [
            ("置零 (zero)", self.send_zero), ("校准 (cal)", self.send_cal),
            ("查询 (?)", self.send_query),
        ]
        for i, (label, fn) in enumerate(actions):
            tk.Button(root, text=label, command=fn, bg="#35322a", fg=PAPER,
                      activebackground=GREEN, activeforeground=PAPER, relief="flat",
                      bd=0, font=("Microsoft YaHei UI", -10, "bold"),
                      cursor="hand2").place(x=18 + i * 130, y=672, width=122, height=30)

        self.map_buttons = {}
        map_defs = [("SWAP XY", "swap"), ("INV ROLL", "invr"), ("INV PITCH", "invp")]
        for i, (label, key) in enumerate(map_defs):
            b = tk.Button(root, text=label, command=lambda k=key: self.toggle_map(k),
                          bg="#35322a", fg=PAPER, activebackground=GREEN,
                          activeforeground=PAPER, relief="flat", bd=0,
                          font=("Consolas", -10, "bold"), cursor="hand2")
            b.place(x=18 + i * 108, y=710, width=100, height=30)
            self.map_buttons[key] = b

        tuning = [
            ("死区 -1°", lambda: self.bump_tune("dz", -1.0)),
            ("死区 +1°", lambda: self.bump_tune("dz", +1.0)),
            ("回中 -1°", lambda: self.bump_tune("ra", -1.0)),
            ("回中 +1°", lambda: self.bump_tune("ra", +1.0)),
            ("HZ 20", lambda: self.link and self.link.send("hz 20")),
            ("HZ 50", lambda: self.link and self.link.send("hz 50")),
        ]
        for i, (label, fn) in enumerate(tuning):
            tk.Button(root, text=label, command=fn, bg="#2a2620", fg=TEXT,
                      activebackground=GREEN, activeforeground=PAPER, relief="flat",
                      bd=0, font=("Microsoft YaHei UI", -9), cursor="hand2").place(
                x=18 + i * 108, y=746, width=100, height=26)

        # port row
        tk.Label(root, text="PORT", bg=SHELL, fg=TEXT,
                 font=("Consolas", -9, "bold"), anchor="w").place(
            x=18, y=782, width=50, height=18)
        self.port_var = tk.StringVar(value=self.port)
        ttk.Entry(root, textvariable=self.port_var, width=12).place(
            x=70, y=780, width=110, height=24)
        tk.Button(root, text="CONNECT", command=self.connect, bg="#35322a", fg=PAPER,
                  activebackground=GREEN, activeforeground=PAPER, relief="flat",
                  bd=0, font=("Consolas", -9, "bold"), cursor="hand2").place(
            x=190, y=780, width=100, height=24)
        tk.Button(root, text="DISCONNECT", command=self.disconnect, bg="#35322a",
                  fg=PAPER, activebackground=RED, activeforeground=PAPER,
                  relief="flat", bd=0, font=("Consolas", -9, "bold"),
                  cursor="hand2").place(x=298, y=780, width=110, height=24)
        self.status = tk.Label(root, text="未连接", bg=SHELL, fg=MUTED,
                               font=("Microsoft YaHei UI", -9), anchor="w")
        self.status.place(x=420, y=780, width=240, height=24)

        self.log_box = tk.Label(root, text="", bg=SHELL, fg=MUTED,
                                font=("Consolas", -8), anchor="nw", justify="left")
        self.log_box.place(x=18, y=812, width=644, height=60)

    # ---- serial --------------------------------------------------------
    def connect(self) -> None:
        self.disconnect()
        port = self.port_var.get().strip() or "COM65"
        self.link = SerialLink(port, self.queue)
        self.link.start()
        self.status.configure(text=f"打开 {port} …", fg=MUTED)

    def disconnect(self) -> None:
        if self.link is not None:
            self.link.stop()
            self.link = None
        self.status.configure(text="未连接", fg=MUTED)

    def send_zero(self) -> None:
        if self.link:
            self.link.send("zero")

    def send_cal(self) -> None:
        if self.link:
            self.link.send("cal")

    def send_query(self) -> None:
        if self.link:
            self.link.send("?")

    def toggle_map(self, key: str) -> None:
        if not self.link:
            return
        swap, invr, invp = self.cfg["swap"], self.cfg["invr"], self.cfg["invp"]
        if key == "swap":
            swap = 0 if swap else 1
        elif key == "invr":
            invr = 0 if invr else 1
        else:
            invp = 0 if invp else 1
        self.link.send(f"map {swap} {invr} {invp}")

    def bump_tune(self, key: str, delta: float) -> None:
        if not self.link:
            return
        value = int(round(((self.cfg.get(key, 0.0) + delta) * 10.0)))
        self.link.send(f"{'dead' if key == 'dz' else 'rearm'} {value}")

    # ---- capture / analysis -------------------------------------------
    def start_capture(self, dir_index: int) -> None:
        self.capture_dir = dir_index
        self.capture[dir_index] = []
        self.capture_until = time.time() + CAPTURE_SECONDS
        self.status.configure(
            text=f"采集中：{DIR_KEYS[dir_index]} … 保持住姿态", fg=AMBER)

    def clear_capture(self) -> None:
        self.capture.clear()
        self.capture_dir = -1
        self._set_analysis("")

    def _set_analysis(self, text: str) -> None:
        self.analysis.configure(state="normal")
        self.analysis.delete("1.0", "end")
        self.analysis.insert("end", text)
        self.analysis.configure(state="disabled")

    @staticmethod
    def _dir_after_mapping(dr: float, dp: float, swap: int, invr: int, invp: int) -> int:
        if swap:
            dr, dp = dp, dr
        if invr:
            dr = -dr
        if invp:
            dp = -dp
        if abs(dr) >= abs(dp):
            return 1 if dr > 0 else 3
        return 0 if dp > 0 else 2

    def analyze(self) -> str:
        if len(self.capture) < 3:
            return "至少采 3 个方向（最好 4 个）再分析。\n"
        lines = ["== 各方向均值（相对零位，度） =="]
        for i in DIR_ORDER:
            samples = self.capture.get(i)
            if not samples:
                lines.append(f"{DIR_KEYS[i]:5s}  未采样")
                continue
            dr = sum(s[0] for s in samples) / len(samples)
            dp = sum(s[1] for s in samples) / len(samples)
            lines.append(
                f"{DIR_KEYS[i]:5s}  roll={dr:7.1f}  pitch={dp:7.1f}   n={len(samples)}")

        lines.append("")
        lines.append("== 轴映射得分（映射后方向 == 采样标签） ==")
        ranked = []
        for swap in (0, 1):
            for invr in (0, 1):
                for invp in (0, 1):
                    hits, total = 0, 0
                    for i in DIR_ORDER:
                        for dr, dp in self.capture.get(i, ()):
                            total += 1
                            if self._dir_after_mapping(dr, dp, swap, invr, invp) == i:
                                hits += 1
                    ranked.append((hits / max(1, total), swap, invr, invp, hits, total))
        ranked.sort(reverse=True)
        for score, swap, invr, invp, hits, total in ranked[:4]:
            mark = "  <-- 建议" if (score, swap, invr, invp) == ranked[0][0:4] else ""
            lines.append(
                f"map {swap} {invr} {invp}   命中 {hits}/{total} ({score * 100:.0f}%){mark}")
        best = ranked[0]
        if best[0] < 0.999:
            lines.append("")
            lines.append("注意：满分映射不存在 —— 四个方向里有混叠（如倾斜角度")
            lines.append("不够大、或有平移晃动），补采再试。")
        self._apply_suggestion_available(best)
        return "\n".join(lines) + "\n"

    def _apply_suggestion_available(self, best: tuple) -> None:
        self.suggestion = best[1:4]
        if not hasattr(self, "apply_btn"):
            self.apply_btn = tk.Button(
                self.root, text="应用建议", command=self.apply_suggestion,
                bg=GREEN, fg=PAPER, activebackground=GREEN_BRIGHT,
                activeforeground=PAPER, relief="flat", bd=0,
                font=("Microsoft YaHei UI", -10, "bold"), cursor="hand2")
            self.apply_btn.place(x=560, y=650, width=102, height=30)

    def apply_suggestion(self) -> None:
        if self.link and self.suggestion:
            swap, invr, invp = self.suggestion
            self.link.send(f"map {swap} {invr} {invp}")

    # ---- pump / draw ---------------------------------------------------
    def _pump(self) -> None:
        try:
            while True:
                kind, payload = self.queue.get_nowait()
                if kind == "imu":
                    self.last.update(payload)
                    self.packets += 1
                    mag = math.hypot(self.last["r"], self.last["p"])
                    scale = min(1.0, mag / FULL_SCALE_DEG)
                    angle = math.atan2(self.last["p"], self.last["r"]) if mag > 1e-6 else 0.0
                    self.trail.append(
                        (
                            self.cx + math.cos(angle) * scale * self.cr,
                            self.cy - math.sin(angle) * scale * self.cr,
                        )
                    )
                    if len(self.trail) > 120:
                        self.trail = self.trail[-120:]
                    if (self.capture_dir >= 0
                            and self.capture_dir in self.capture
                            and time.time() < self.capture_until):
                        self.capture[self.capture_dir].append(
                            (self.last["r"], self.last["p"]))
                    elif self.capture_dir >= 0 and time.time() >= self.capture_until:
                        done = self.capture_dir
                        n = len(self.capture[done])
                        self.capture_dir = -1
                        self.status.configure(
                            text=f"{DIR_KEYS[done]} 采完 {n} 个样本", fg=GREEN)
                        if len(self.capture) >= 3:
                            self._set_analysis(self.analyze())
                elif kind == "cfg":
                    self.cfg.update(payload)
                    self._refresh_map_buttons()
                elif kind == "cmd":
                    name, outcome = payload
                    self.log_lines.append(f"CMD {name} {outcome}")
                elif kind == "log":
                    self.log_lines.append(payload)
                elif kind == "open":
                    self.status.configure(text=f"已连接 {payload}", fg=GREEN)
                    if self.link:
                        self.link.send("?")
                elif kind == "error":
                    self.status.configure(text=f"串口错误: {payload}", fg=RED)
        except queue.Empty:
            pass
        if len(self.log_lines) > 6:
            self.log_lines = self.log_lines[-6:]
        self.log_box.configure(text="\n".join(self.log_lines))
        self._draw()
        self.root.after(40, self._pump)

    def _refresh_map_buttons(self) -> None:
        colors = {"swap": self.cfg["swap"], "invr": self.cfg["invr"],
                  "invp": self.cfg["invp"]}
        for key, on in colors.items():
            self.map_buttons[key].configure(
                bg=GREEN if on else "#35322a",
                fg=PAPER,
                text={"swap": "SWAP XY", "invr": "INV ROLL", "invp": "INV PITCH"}[key]
                + (" ●" if on else ""))

    def _draw(self) -> None:
        c = self.canvas
        c.delete("all")
        cx, cy, r = self.cx, self.cy, self.cr

        c.create_oval(cx - r, cy - r, cx + r, cy + r, fill=PANEL,
                      outline="#4a453a", width=1)
        c.create_line(cx - r, cy, cx + r, cy, fill="#33302a")
        c.create_line(cx, cy - r, cx, cy + r, fill="#33302a")
        c.create_text(cx, cy - r - 14, text="UP", fill=MUTED,
                      font=("Consolas", -10, "bold"))
        c.create_text(cx, cy + r + 14, text="DOWN", fill=MUTED,
                      font=("Consolas", -10, "bold"))
        c.create_text(cx - r - 26, cy, text="LEFT", fill=MUTED,
                      font=("Consolas", -10, "bold"))
        c.create_text(cx + r + 26, cy, text="RIGHT", fill=MUTED,
                      font=("Consolas", -10, "bold"))

        dz = r * (self.cfg["dz"] / FULL_SCALE_DEG)
        c.create_oval(cx - dz, cy - dz, cx + dz, cy + dz, outline=GREEN,
                      width=1, dash=(3, 3))

        # trail
        for i, (tx, ty) in enumerate(self.trail[-80:]):
            frac = (i + 1) / max(1, len(self.trail[-80:]))
            shade = int(70 + 120 * frac)
            c.create_oval(tx - 2, ty - 2, tx + 2, ty + 2,
                          fill=f"#{shade:02x}{shade + 20:02x}{shade:02x}", outline="")

        roll, pitch = self.last["r"], self.last["p"]
        mag = math.hypot(roll, pitch)
        scale = min(1.0, mag / FULL_SCALE_DEG)
        angle = math.atan2(pitch, roll) if mag > 1e-6 else 0.0
        px = cx + math.cos(angle) * scale * r
        py = cy - math.sin(angle) * scale * r
        c.create_line(cx, cy, px, py, fill=GREEN, width=2)
        c.create_oval(px - 9, py - 9, px + 9, py + 9, fill=GREEN,
                      outline=PAPER, width=2)
        c.create_oval(cx - 3, cy - 3, cx + 3, cy + 3, fill=PAPER, outline="")

        # accel-only dot (small, hollow): compares Mahony with raw gravity
        amag = math.hypot(self.last["pr"], self.last["pp"])
        ascale = min(1.0, amag / FULL_SCALE_DEG)
        aangle = math.atan2(self.last["pp"], self.last["pr"]) if amag > 1e-6 else 0.0
        ax = cx + math.cos(aangle) * ascale * r
        ay = cy - math.sin(aangle) * ascale * r
        c.create_oval(ax - 7, ay - 7, ax + 7, ay + 7, outline=BLUE, width=2)

        # readouts
        d = self.last["d"]
        self.readout["d"].configure(
            text=DIR_NAMES.get(d, "?"), fg=GREEN if d >= 0 else MUTED)
        lk = self.last["lk"]
        self.readout["lk"].configure(
            text=DIR_NAMES.get(lk, "?"), fg=AMBER if lk >= 0 else MUTED)
        self.readout["rp"].configure(text=f"{roll:7.1f}° / {pitch:7.1f}°")
        self.readout["acc"].configure(
            text=f"{self.last['pr']:7.1f}° / {self.last['pp']:7.1f}°")
        self.readout["an"].configure(text=f"{self.last['an']:6.3f} g")
        self.readout["gyro"].configure(
            text=f"{self.last['gx']:6.2f} {self.last['gy']:6.2f} "
                 f"{self.last['gz']:6.2f}")
        self.readout["map"].configure(
            text=f"swap={self.cfg['swap']} inv_roll={self.cfg['invr']} "
                 f"inv_pitch={self.cfg['invp']}")
        self.readout["tune"].configure(
            text=f"死区 {self.cfg['dz']:.1f}° / 回中 {self.cfg['ra']:.1f}° / "
                 f"稳定 {self.cfg['stable']} 样本")
        self.readout["neutral"].configure(
            text=f"roll {self.cfg.get('nr', 0.0):6.1f}° / "
                 f"pitch {self.cfg.get('np', 0.0):6.1f}°")
        self.readout["hz"].configure(text=f"{self.cfg['hz']} Hz · {self.packets} 包")


def main() -> int:
    _dpi_aware()
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM65")
    parser.add_argument("--shot", default=None,
                        help="grab the debug window to this PNG and exit")
    parser.add_argument("--seconds", type=float, default=4.0)
    args = parser.parse_args()

    root = tk.Tk()
    app = DebugApp(root, args.port)

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
                root.attributes("-topmost", True)
                root.update_idletasks()
                x = int(root.winfo_rootx())
                y = int(root.winfo_rooty())
                w = int(root.winfo_width())
                h = int(root.winfo_height())
                img = ImageGrab.grab(bbox=(x, y, x + w, y + h), all_screens=True)
                img.save(args.shot, "PNG")
                print(f"DEBUG_SHOT ok path={args.shot} size={img.width}x{img.height}")
            except Exception as exc:
                print(f"DEBUG_SHOT fail {exc}")
            on_close()

        root.after(int(args.seconds * 1000), grab)

    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
