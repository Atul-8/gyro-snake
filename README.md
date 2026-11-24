# GYRO SNAKE

**把整块屏幕当方向盘倾斜着玩的贪吃蛇。**
跑在 Seeed **reTerminal Sticky** 开发板上：ESP32-S3 + 3.97" 480×800 电子墨水屏 +
六轴陀螺仪 + 电容触摸 + 蜂鸣器。没有硬件也可以先在 Windows 桌面模拟器里玩完整游戏。

**Tilt the whole screen like a steering wheel to steer a snake.** Runs on the
Seeed **reTerminal Sticky** board (ESP32-S3 + 3.97" 480×800 e-paper + 6-axis IMU +
capacitive touch + buzzer). No hardware? Play the full game in the Windows desktop
simulator first.

简体中文 ｜ [English](#english)

![GYRO SNAKE](docs/screenshot.png)

- [功能一览](#功能一览)
- [快速开始](#快速开始)
- [怎么玩](#怎么玩)
- [调参与联调](#调参与联调可选)
- [架构](#架构)
- [仓库结构](#仓库结构)
- [已支持与限制](#已支持与限制)
- [致谢](#致谢)
- [English](#english)

## 功能一览

- **倾斜即方向**：机身往哪边倾，蛇就往哪边走。方向按倾斜向量划分为四个 ±45° 扇区，
  斜着倾斜也算主方向；从"上"直接扭到"右"不用回中，**实时跟手**。
- **每次开局自动校准 4 秒**：校准时怎么握，游戏的零位就在哪。中途换握姿？
  点一下屏幕右下角的**倾斜球**（或按侧面音量下键），当前姿态立刻变成新零位。
- **画面右下角的实时倾斜球**：外圈 = 20° 满量程，内圈 = 8° 死区，圆点 = 当前倾斜，
  粗刻度 = 当前判定方向。倾斜是否被识别，一眼可见。
- **电子墨水原生玩法**：棋盘 15×23 格，每刷新一帧走一步；吃苹果 +10 分 +1 节；
  撞墙/撞自己结束，填满全盘获胜；最高分存板载 NVS 掉电不丢。
- 原创像素画风与蜂鸣器音效（吃、转向、开始、校准、结束），无任何第三方游戏素材。
- **桌面模拟器**：Windows 上 1:1 复刻游戏规则与画面，虚拟倾斜盘 + 中/EN 界面 +
  模拟电子墨水刷新时序，还带录制 GIF 功能。

## 硬件需求

| 项 | 说明 |
| --- | --- |
| 开发板 | Seeed reTerminal Sticky（ESP32-S3，16 MB Flash，8 MB PSRAM） |
| 连接 | USB-C 数据线（供电 + 烧录 + 串口日志） |
| 板上外设 | 480×800 四级灰度电子墨水（SSD1677）、六轴 IMU（LSM6DS3TR-C）、电容触摸（GT911）、蜂鸣器、AI/OK/UP/DOWN 四键 |

## 快速开始

### 方式一：没有硬件 —— 桌面模拟器（Windows）

需要 64 位 [MinGW-w64 gcc](https://winlibs.com/) 和 Python 3（`pip install pillow`）：

```bash
git clone https://github.com/Atul-8/gyro-snake.git
cd gyro-snake/desktop
bash build.sh      # 首次构建游戏核心 DLL（gcc）
python play.py     # 打开模拟器；Windows 也可直接双击 play.bat（缺 DLL 会自动构建）
```

- 上半部分是 480×800 屏幕（0.75 缩放），下半部分是虚拟倾斜盘和按键。
- 鼠标拖倾斜盘 / 方向键 = 倾斜；点屏幕 = 开始/暂停；**点画面右下角的倾斜球 = 归零**。
- `EPD / FAST / TURBO` 三个时序档模拟不同的墨水屏刷新速度（默认 FAST；
  EPD 最接近真机的节奏）。右上角 `中/EN` 切换界面语言。

### 方式二：烧录真机

需要 [ESP-IDF](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/) **v6.1.1**（其他 6.x 未验证）。
工程自带 `sdkconfig.defaults`（esp32s3 / 240 MHz / 16 MB Flash / 八线 PSRAM / 自定义分区表），
无需任何外部组件仓库：

```bash
git clone https://github.com/Atul-8/gyro-snake.git
cd gyro-snake/firmware
idf.py set-target esp32s3
idf.py build
idf.py -p <你的串口> flash monitor      # Windows 下通常是 COMx
```

烧完拔掉 USB 用电池也能玩（固件开机即拉高 GPIO45/46 保持电源；电池供电路径请以
实际设备为准）。串口波特率 115200。

## 怎么玩

1. **摆好握姿** —— 你打算怎么拿板子玩，就怎么拿。
2. 点屏幕任意位置或按 **AI/OK**，进入 4 秒校准：屏幕提示 KEEP YOUR PLAY POSE，
   **保持不动**。校准后还有 3 秒准备期，蛇先不动，可以先把方向倾好。
3. **倾斜机身**改变方向（±45° 扇区实时判定）；吃苹果成长，撞墙/撞自己结束。
4. 中途换了握姿：**点右下角倾斜球**或按 **DOWN**，当前姿态即刻成为新零位。
5. 暂停/继续：点屏幕或按 **AI/OK**；PAUSE / GAME OVER 下按 **UP** 重新校准。

| 按键/触摸 | 作用 |
| --- | --- |
| 点屏幕 / AI(OK) 键 | 开始 / 暂停 / 继续 / 结束后回标题 |
| 倾斜机身 | UP / RIGHT / DOWN / LEFT（±45° 扇区，实时） |
| 点右下角倾斜球 / DOWN 键 | 当前姿态设为零位 |
| UP 键（PAUSE / GAME OVER） | 重新校准 |

## 调参与联调（可选）

固件在串口（115200）上开放了一套 `$` 命令，**改参数不用重新烧录**：

```text
$?            查看当前配置        $zero   当前姿态设为零位
$map S R P    轴映射（0/1 三位）  $cal    重新校准
$dead N       死区（0.1°为单位）  $hz N   遥测频率 1..50
$rearm N      边界迟滞            $stable N  防抖样本数
```

配套图形化联调页（需 `pip install pyserial`，设备插在电脑上）：

```bash
python desktop/imu_debug.py --port COMx
```

倾斜四个方向各采一次样，它会对 8 种轴映射组合自动打分并给出建议——换新板子
或改装外壳后，用它两分钟就能重新标定方向。

## 架构

工程刻意分成两层：**纯 C99 核心**（不碰任何硬件：无 I2C、无时钟、无 malloc）
与**端口层**（唯一碰硬件的地方）。同一份核心被三个消费者复用，规则只需写一次：

```text
            ┌──────────────────────────────┐
            │  desktop/play.py （Tkinter） │  桌面模拟器 GUI / 虚拟倾斜盘
            └──────────────┬───────────────┘
                           │ ctypes
            ┌──────────────▼───────────────┐
            │  desktop/build/gyro_snake.dll│  sim_api.c：C ABI 薄封装
            └──────────────┬───────────────┘
                           │
        ┌──────────────────▼───────────────────┐
        │   纯 C99 核心（三处共享，零硬件依赖）│
        │   game_core 贪吃蛇规则+渲染+字体     │
        │   imu_core  校准+Mahony+45° 扇区     │
        └──────┬───────────────────┬───────────┘
               │                   │
   ┌───────────▼────────┐   ┌──────▼───────────────────────────┐
   │ tests/  gcc 主机单测│   │ firmware/ （ESP-IDF, esp32s3）   │
   └────────────────────┘   │ main/main.c   游戏主循环         │
                            │ main/console.c 串口 $ 命令调参   │
                            │ board_port  显示/按键/蜂鸣器/NVS │
                            │ touch_port  GT911 触摸           │
                            │ imu_port    LSM6DS3TR-C 100 Hz  │
                            │ bsp         SSD1677 电子墨水    │
                            └──────────────────────────────────┘
```

固件里一帧的生命周期：

1. **IMU 任务（100 Hz）**：原始六轴数据 → 只减陀螺零偏（加速度保留重力）→
   Mahony 姿态 → 减去零位姿态 → 轴映射到屏幕系 → **45° 扇区实时判定**；
2. **游戏任务**：取当前扇区 + 按键/触摸事件 → 核心走一步 → 渲染 MONO 帧 →
   叠画右下角倾斜球 → 交给显示任务；
3. **显示任务**（唯一有权碰墨水屏）：快刷约 0.85 s / 全刷约 1.4 s；标题与
   结算卡用 4 级灰度全刷；
4. 校准结果与最高分写入 NVS；串口命令随时改轴映射/死区/遥测率，不用重烧。

## 仓库结构

```text
gyro-snake/
├─ firmware/                    ESP-IDF 固件（esp32s3）
│  ├─ main/
│  │  ├─ main.c                 装配 + 游戏主循环（校准/归零/倾斜球点击）
│  │  └─ console.c              串口 $ 命令：运行时调参，无需重烧
│  ├─ components/
│  │  ├─ game_core/             贪吃蛇核心 + 渲染 + 字体（纯 C99）
│  │  ├─ imu_core/              校准 / Mahony / 45° 扇区方向（纯 C99）
│  │  ├─ imu_port/              LSM6DS3TR-C I2C 驱动 + 100 Hz 姿态任务
│  │  ├─ board_port/            显示 / 按键 / 蜂鸣器 / NVS / 倾斜球绘制
│  │  ├─ touch_port/            GT911 触摸驱动（坐标映射 + tap 日志）
│  │  └─ bsp/                   SSD1677 墨水屏与按键底层（上游驱动，见 NOTICE）
│  ├─ sdkconfig.defaults        esp32s3 / 240 MHz / 16 MB / 八线 PSRAM / 分区表
│  └─ partitions.csv            3 MB 应用 + NVS + 校准存储分区
├─ desktop/
│  ├─ sim_api.c                 游戏 DLL 的 C ABI 边界（无 GUI）
│  ├─ play.py  play.bat         桌面模拟器主程序（Tkinter，中/EN）
│  ├─ imu_debug.py              IMU 联调页：串口双向 + 四方向自动标定
│  ├─ imu_monitor.py  .bat      独立的实时姿态监视器（读串口遥测）
│  └─ build.sh                  一键编译游戏 DLL（gcc）
├─ tests/                       纯 C99 核心主机单测（701 项断言）
├─ host/preview/
│  ├─ esp_err.h                 esp_err_t 的主机替身（桌面 DLL 在 PC 上编译 tilt_ball.c 必需）
│  └─ play_preview.c            离线帧预览：真实核心渲染一帧 + 倾斜球 → PGM（不烧录即可检查版式）
├─ docs/                        需求 / 架构 / IMU 分析 / 硬件笔记 / 测试记录
├─ LICENSE（MIT） NOTICE（许可明细） AGENTS.md（AI 协作工程规则）
```

## 已支持与限制

| ✅ 已实现并验证 | ⚠️ 限制 / 未完成 |
| --- | --- |
| 四方向倾斜实时转向（±45° 扇区、边界迟滞、防抖，全部在线可调） | 电子墨水物理决定每步 ≈0.85 s（快刷），不可能 60 FPS |
| 每局 4 s 姿态校准；运行中点球 / DOWN 一键归零 | 电池供电续航未实测（USB 下电源保持正常） |
| 触摸开始/暂停；GT911 坐标读取 + 每次点击的坐标日志 | 触摸轴向翻转未做单点标定（角落归零判定不受翻转影响） |
| 最高分与校准结果 NVS 持久化；原创蜂鸣器音效 | 多点触控与滑动手势未实现 |
| 串口运行时调参 + 10~50 Hz 遥测；图形化四方向标定页 | 游戏内 HUD 仅英文（C 核心自带字体）；模拟器界面支持中/EN |
| 桌面模拟器全功能（虚拟倾斜盘 / 时序档 / 录制 GIF）；701 项主机单测 | ESP-IDF 6.1.1 验证，其他 6.x 未验证 |

## 测试状态

- 主机单元测试：核心 522 项、IMU 195 项，全部通过。任意装了 gcc 的机器即可复跑，
  无需 ESP-IDF：

```bash
gcc -std=c99 -I firmware/components/game_core/include \
    firmware/components/game_core/src/*.c tests/test_snake.c -o t && ./t
gcc -std=c99 -I firmware/components/imu_core/include -lm \
    firmware/components/imu_core/src/*.c tests/test_imu.c -o i && ./i
```

- 桌面自测：`python desktop/play.py --selftest` 与 `--window-test`。
- 真机：已在 reTerminal Sticky 上完成方向标定与全流程试玩；电池续航与触摸轴翻转
  标定仍在完善，详见 `docs/test-results.md`。

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/requirements.md](docs/requirements.md) | 需求与硬件能力对照 |
| [docs/architecture.md](docs/architecture.md) | 分层架构与数据流 |
| [docs/imu-calibration.md](docs/imu-calibration.md) | 姿态算法与校准流程分析 |
| [docs/hardware-notes.md](docs/hardware-notes.md) | 板级硬件笔记 |
| [docs/test-results.md](docs/test-results.md) | 构建 / 烧录 / 真机测试记录 |
| [desktop/README.md](desktop/README.md) | 模拟器与联调页详细说明 |

## 许可

- 本项目原创代码与文档：**MIT**（见 [LICENSE](LICENSE)）。
- `firmware/components/bsp/` 内的板级驱动派生自 CodeCraft AI Hardware 上游，
  按上游许可逐文件保留（MIT / Apache-2.0），明细见 [NOTICE](NOTICE)。
- 贪吃蛇为通用玩法；美术、音效、文档均为原创，不含任何第三方游戏素材。

## 致谢

本项目的电子墨水驱动与板级引脚定义站在别人的肩膀上，特别感谢：

- **[CodeCraft AI Hardware](https://github.com/Seeed-Solution/codecraft-ai-hardware)**
  —— `firmware/components/bsp/` 中的电子墨水/按键驱动与引脚表派生自该项目的
  reTerminal Sticky BSP。其中 `bsp_epd.c`（SSD1677 电子墨水驱动）为 **MIT** 许可，
  版权归 **JoJang2u** 与 **limengdu** 所有，原版权头逐字保留；其余引脚/按键文件为
  **Apache-2.0**（CodeCraft AI Hardware contributors）。
- **Seeed Studio** —— reTerminal Sticky 硬件本身及官方例程与硬件文档。
- **MagicRobot_V0.1/embedded** 的作者 —— 本项目的 IMU 校准与 Mahony 姿态算法在
  分析其真实执行路径后独立重写（未复刻其代码），详见
  [docs/imu-calibration.md](docs/imu-calibration.md)。

---

<a id="english"></a>

# English

**Snake you steer by tilting the whole screen like a steering wheel.** It runs
on the Seeed **reTerminal Sticky** board: ESP32-S3, a 3.97" 480×800 four-level
grayscale e-paper panel, a 6-axis IMU, capacitive touch and a buzzer. Without
hardware, play the complete game in the bundled Windows desktop simulator.

## Features

- **Tilt = direction.** The tilt vector is split into four ±45° sectors
  (UP / RIGHT / DOWN / LEFT); diagonals follow the dominant side, and swinging
  from "up" straight to "right" turns instantly — **no re-centering needed**.
- **Automatic 4-second calibration at every start.** Hold the board the way
  you are going to play; that pose becomes the zero point. Changed your grip
  mid-game? Tap the **tilt ball** in the bottom-right corner (or press the
  DOWN side key) and the current pose becomes the new zero immediately.
- **A live tilt ball in the bottom-right corner of the screen**: outer ring =
  20° full scale, inner ring = the 8° deadzone, the dot = current tilt, the
  heavy tick = the direction currently detected. You can always see whether a
  tilt registered.
- **Native e-paper gameplay**: a 15×23 board, one step per displayed frame;
  apples give +10 points and +1 segment; walls/yourself end the run; filling
  the whole board wins. The high score survives reboots in NVS.
- Original pixel art and buzzer jingles (eat, turn, start, calibrate, game
  over). No third-party game assets anywhere.
- **Desktop simulator**: a 1:1 Windows replica of the rules and artwork, with
  a virtual tilt disc, a zh/EN interface, simulated e-paper refresh timing and
  GIF recording.

## Hardware

| Item | Notes |
| --- | --- |
| Board | Seeed reTerminal Sticky (ESP32-S3, 16 MB flash, 8 MB PSRAM) |
| Connection | USB-C cable (power + flashing + serial log) |
| On-board peripherals | 480×800 grayscale e-paper (SSD1677), 6-axis IMU (LSM6DS3TR-C), capacitive touch (GT911), buzzer, AI/OK/UP/DOWN keys |

## Quick start

### Option A — no hardware: the Windows desktop simulator

Requires 64-bit [MinGW-w64 gcc](https://winlibs.com/) and Python 3
(`pip install pillow`):

```bash
git clone https://github.com/Atul-8/gyro-snake.git
cd gyro-snake/desktop
bash build.sh      # first build of the game-core DLL (gcc)
python play.py     # or double-click play.bat on Windows; it builds the DLL if missing
```

- The top half is the 480×800 screen (scaled to 0.75); the bottom half holds
  the virtual tilt disc and buttons.
- Drag the disc or use the arrow keys to tilt; click the screen to
  start/pause; **click the tilt ball in the bottom-right corner to re-zero**.
- `EPD / FAST / TURBO` timing profiles emulate different e-paper refresh
  speeds (default FAST; EPD matches the real panel best). `中/EN` switches the
  UI language.

### Option B — flash the real board

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
**v6.1.1** (other 6.x versions unverified). The project ships its own
`sdkconfig.defaults` (esp32s3 / 240 MHz / 16 MB flash / octal PSRAM / custom
partition table) and needs no external component repositories:

```bash
git clone https://github.com/Atul-8/gyro-snake.git
cd gyro-snake/firmware
idf.py set-target esp32s3
idf.py build
idf.py -p <your-port> flash monitor      # usually COMx on Windows
```

After flashing you can unplug USB and play on battery (the firmware holds
GPIO45/46 high at boot to keep the power rail alive; battery behaviour should
be verified on your actual device). Serial baud rate: 115200.

## How to play

1. **Pick your grip** — hold the board exactly the way you intend to play.
2. Tap the screen or press **AI/OK** to enter the 4-second calibration
   (KEEP YOUR PLAY POSE): hold still. Afterwards you get a 3-second ready
   window before the snake moves, so you can pre-tilt.
3. **Tilt the board** to steer (real-time ±45° sectors); eat apples to grow;
   walls and your own tail end the run.
4. Changed your grip mid-run? **Tap the bottom-right tilt ball** or press
   **DOWN** — the current pose becomes the new zero instantly.
5. Pause/resume: tap the screen or press **AI/OK**; press **UP** in
   PAUSE / GAME OVER to recalibrate.

| Key / touch | Action |
| --- | --- |
| Tap screen / AI(OK) key | Start / pause / resume / back to title |
| Tilt the board | UP / RIGHT / DOWN / LEFT (±45° sectors, real-time) |
| Tap the tilt ball / DOWN key | Set the current pose as the zero point |
| UP key (PAUSE / GAME OVER) | Recalibrate |

## Tuning (optional)

The firmware exposes a `$` command set on the serial port (115200) — **tune
without reflashing**:

```text
$?            show current config     $zero   set the current pose as zero
$map S R P    axis map (three 0/1)    $cal    recalibrate
$dead N       deadzone (0.1° units)   $hz N   telemetry rate 1..50
$rearm N      boundary hysteresis     $stable N  debounce samples
```

There is a graphical tuning page (needs `pip install pyserial`, board plugged
into the PC):

```bash
python desktop/imu_debug.py --port COMx
```

Tilt four ways, capture one sample run each, and it scores all 8 axis-mapping
combinations and sends the winner back to the firmware — re-calibrating a new
board or a new enclosure takes about two minutes.

## Architecture

The project is deliberately split into a **plain-C99 core** (touches no
hardware: no I2C, no clock, no malloc) and **port layers** (the only places
that do). The same core is reused by three consumers, so the rules are
written exactly once:

```text
            ┌──────────────────────────────┐
            │  desktop/play.py (Tkinter)   │  desktop GUI / virtual tilt disc
            └──────────────┬───────────────┘
                           │ ctypes
            ┌──────────────▼───────────────┐
            │  desktop/build/gyro_snake.dll│  sim_api.c: thin C ABI
            └──────────────┬───────────────┘
                           │
        ┌──────────────────▼───────────────────┐
        │  plain-C99 core (shared ×3, no HW)   │
        │  game_core  rules + render + font    │
        │  imu_core   calibration+Mahony+45°   │
        └──────┬───────────────────┬───────────┘
               │                   │
   ┌───────────▼────────┐   ┌──────▼───────────────────────────┐
   │ tests/  gcc unit   │   │ firmware/ (ESP-IDF, esp32s3)     │
   └────────────────────┘   │ main/main.c   game loop          │
                            │ main/console.c serial $ commands │
                            │ board_port  display/keys/buzzer  │
                            │ touch_port  GT911 touch          │
                            │ imu_port    LSM6DS3TR-C 100 Hz  │
                            │ bsp         SSD1677 e-paper     │
                            └──────────────────────────────────┘
```

Life of a single frame on the firmware:

1. **IMU task (100 Hz)**: raw six-axis sample → subtract gyro bias only
   (accelerometer keeps gravity) → Mahony attitude → subtract the neutral
   pose → axis-map into the screen frame → **real-time 45° sector decision**;
2. **Game task**: takes the current sector plus key/touch events → the core
   advances one cell → renders the MONO frame → stamps the bottom-right tilt
   ball → hands the frame over;
3. **Display task** (the only caller of the e-paper driver): fast refresh
   ≈0.85 s / full ≈1.4 s; title and game-over cards use 4-level grayscale
   full updates;
4. Calibration results and the high score are written to NVS; serial commands
   retune the axis map / deadzone / telemetry rate at runtime, no reflash.

## Repository layout

```text
gyro-snake/
├─ firmware/                    ESP-IDF firmware (esp32s3)
│  ├─ main/
│  │  ├─ main.c                 wiring + game loop (calibration/re-zero/ball tap)
│  │  └─ console.c              serial $ commands: runtime tuning, no reflash
│  ├─ components/
│  │  ├─ game_core/             snake core + render + font (plain C99)
│  │  ├─ imu_core/              calibration / Mahony / 45° sectors (plain C99)
│  │  ├─ imu_port/              LSM6DS3TR-C I2C driver + 100 Hz attitude task
│  │  ├─ board_port/            display / keys / buzzer / NVS / tilt-ball drawing
│  │  ├─ touch_port/            GT911 touch driver (coordinate map + tap log)
│  │  └─ bsp/                   SSD1677 e-paper and key low level (see NOTICE)
│  ├─ sdkconfig.defaults        esp32s3 / 240 MHz / 16 MB / octal PSRAM / partitions
│  └─ partitions.csv            3 MB app + NVS + calibration storage
├─ desktop/
│  ├─ sim_api.c                 C ABI boundary of the game DLL (no GUI)
│  ├─ play.py  play.bat         desktop simulator (Tkinter, zh/EN)
│  ├─ imu_debug.py              IMU tuning page: bidirectional serial + auto calibration
│  ├─ imu_monitor.py  .bat      standalone live attitude monitor (serial telemetry)
│  └─ build.sh                  one-shot game DLL build (gcc)
├─ tests/                       host unit tests for the C99 cores (701 assertions)
├─ host/preview/
│  ├─ esp_err.h                 host stand-in for esp_err_t (required to build tilt_ball.c into the desktop DLL)
│  └─ play_preview.c            offline frame preview: renders a real play frame + tilt ball to PGM, no flashing needed
├─ docs/                        requirements / architecture / IMU analysis / notes / test log
├─ LICENSE (MIT) NOTICE (license details) AGENTS.md (AI-collaboration engineering rules)
```

## Supported and limitations

| ✅ Implemented and verified | ⚠️ Limitations / open items |
| --- | --- |
| Real-time 4-direction tilt steering (±45° sectors, boundary hysteresis, debounce — all tunable at runtime) | E-paper physics: ≈0.85 s per step (fast refresh); 60 FPS is impossible on this panel |
| 4-second pose calibration every run; tap-the-ball / DOWN re-zero mid-run | Battery endurance untested (power-hold verified on USB only) |
| Touch start/pause; GT911 coordinates read + a log line per tap | Touch axis flips not yet calibrated from a known corner (the corner-zero gesture is flip-invariant) |
| High score and calibration persisted in NVS; original buzzer SFX | Multi-touch and swipe gestures not implemented |
| Serial runtime tuning + 10–50 Hz telemetry; graphical 4-direction calibration page | In-game HUD is English only (the C core ships its own font); the simulator UI is zh/EN |
| Full-featured desktop simulator (tilt disc / timing profiles / GIF recording); 701 host assertions | Verified on ESP-IDF 6.1.1; other 6.x versions unverified |

## Test status

- Host unit tests: 522 core + 195 IMU assertions, all passing. Any machine
  with gcc can re-run them — no ESP-IDF needed:

```bash
gcc -std=c99 -I firmware/components/game_core/include \
    firmware/components/game_core/src/*.c tests/test_snake.c -o t && ./t
gcc -std=c99 -I firmware/components/imu_core/include -lm \
    firmware/components/imu_core/src/*.c tests/test_imu.c -o i && ./i
```

- Desktop self-tests: `python desktop/play.py --selftest` and `--window-test`.
- On hardware: direction mapping calibrated and full gameplay verified on a
  reTerminal Sticky; battery endurance and touch-axis calibration are still
  being polished — see `docs/test-results.md` (Chinese).

## Documentation

| Doc | Contents |
| --- | --- |
| [docs/requirements.md](docs/requirements.md) | Requirements vs. hardware capability (Chinese) |
| [docs/architecture.md](docs/architecture.md) | Layered architecture and data flow (Chinese) |
| [docs/imu-calibration.md](docs/imu-calibration.md) | Attitude algorithm and calibration analysis (Chinese) |
| [docs/hardware-notes.md](docs/hardware-notes.md) | Board-level hardware notes (Chinese) |
| [docs/test-results.md](docs/test-results.md) | Build / flash / on-device test log (Chinese) |
| [desktop/README.md](desktop/README.md) | Simulator and tuning page in detail (Chinese) |

## License

- All original code and documentation in this repository: **MIT**
  (see [LICENSE](LICENSE)).
- The board drivers under `firmware/components/bsp/` are derived from the
  CodeCraft AI Hardware upstream and keep their per-file upstream licenses
  (MIT / Apache-2.0) — details in [NOTICE](NOTICE).
- Snake is a generic game genre; the art, sounds and documentation are
  original. No third-party game assets are used.

## Acknowledgements

The e-paper driver and the board pin definitions stand on other people's
shoulders. Special thanks to:

- **[CodeCraft AI Hardware](https://github.com/Seeed-Solution/codecraft-ai-hardware)**
  — the e-paper / key drivers and the pin table under
  `firmware/components/bsp/` are derived from their reTerminal Sticky BSP.
  `bsp_epd.c` (the SSD1677 e-paper driver) is **MIT**-licensed, copyright
  **JoJang2u** and **limengdu**, with the original headers preserved
  verbatim; the remaining pin/button files are **Apache-2.0**
  (CodeCraft AI Hardware contributors).
- **Seeed Studio** — the reTerminal Sticky hardware itself, its official
  examples and hardware documentation.
- The authors of **MagicRobot_V0.1/embedded** — the IMU calibration and
  Mahony attitude code in this project was independently rewritten after
  analysing their real execution paths (their code is not reproduced); see
  [docs/imu-calibration.md](docs/imu-calibration.md).
