# GYRO SNAKE 桌面硬件模拟器

在 PC 上体验 reTerminal Sticky 上的陀螺仪贪吃蛇。桌面程序通过 `ctypes`
加载 `desktop/build/gyro_snake.dll`，DLL 里链接的是固件同源的
`game_core`（`snake.c / render.c / rng.c / font.c`），所以屏幕像素、
HUD、卡片、蛇/苹果的绘制与真机核心完全一致。

**它不是真机，也不是真实 IMU。** 下方的倾斜盘是虚拟 IMU，产生
`snake_tick()` 的 `tilt_dir`；界面标注 `SIMULATED TILT / 模拟倾斜`。
所有刷新时间（550 / 1800 / 1500 / 120 / 450 / 40 / 160 ms）都是模拟值，
不是 EPD 实测数据。

---

## 1. 视觉与布局

```text
+--------------------------------------------------+  520 x 1000
|  GYRO SNAKE                                      |
|  reTerminal Sticky hardware simulator ...        |
|  +--------------------------------------------+  |
|  |  480x800 core framebuffer, 0.75x -> 360x600 | |
|  |  HUD (English) + snake + apple              | |
|  +--------------------------------------------+  |
|  SIMULATED E-PAPER 480x800 0.75x      MONO FAST  |
|  +--------------------------------------------+  |
|  | SIMULATED TILT        [  START / PAUSE  ]  |  |
|  |   (o) green ring      [ CALIBRATE ][中][S] |  |
|  |   crosshair + puck    [ 1x ][ 2x ][ 4x ]   |  |
|  |   UP/RIGHT/DOWN/LEFT  [EPD][FAST][TURBO]   |  |
|  |   [ CENTER ]          [ SHOT ][  REC   ]   |  |
|  |   SCORE / HIGH / LEN / STATE / TILT        |  |
|  +--------------------------------------------+  |
+--------------------------------------------------+
```

- 机身深炭 `#1b1a17`，屏幕纸色 `#f3efe4`，绿色强调 `#3f8f5a`，米色文字。
- 上半部分：核心的 1 bpp / gray4 画面按 **0.75** 缩放到 360x600，
  居中，细墨色边框；像素用最近邻缩放，保持电子墨水质感。
- 下半部分：直径约 170 px（外环）的虚拟倾斜盘 + 右侧按钮 + 底部状态行。
- 游戏画面里的 HUD 由 C 核心渲染，保持英文；窗口外壳中英文可切换。

---

## 2. 依赖

| 组件 | 要求 |
| --- | --- |
| OS | Windows 10/11（音效依赖 `winsound`；其余部分可移植） |
| 编译器 | MinGW-w64 `gcc`（验证过 GCC 16.2.0），支持 `-shared -static-libgcc` |
| Python | 3.10+（验证过 3.12.10），带 Tk 8.6 |
| Pillow | 9+（验证过 13.0.0） |
| 显示 | 分辨率至少 520x1060；窗口测试需要可见桌面 |

`build.sh` 需要 Git Bash（Windows）或任意 POSIX shell。

---

## 3. 构建 DLL

在仓库根目录（`codecraft/`）或 `gyro-snake/` 下都可以执行：

```bash
bash gyro-snake/desktop/build.sh
```

脚本会：

1. 找到 `gyro-snake/firmware/components/game_core/src/*.c`；
2. 用 `gcc -std=c99 -O2 -Wall -Wextra -shared -static-libgcc` 编译；
3. 链接 `sim_api.c`，输出 `gyro-snake/desktop/build/gyro_snake.dll`。

实际输出示例：

```text
[cc] core  4 file(s) + sim_api.c -> .../desktop/build/gyro_snake.dll
build ok
  .../desktop/build/gyro_snake.dll
```

`play.bat` / `play.sh` 在 DLL 缺失时会自动调用 `build.sh`。

---

## 4. 运行

```bash
python gyro-snake/desktop/play.py
# 或双击 gyro-snake/desktop/play.bat
# 或 bash gyro-snake/desktop/play.sh
```

启动后是 TITLE 卡。**每次**按 START（或点击屏幕）都会进入约 3 秒的
模拟校准归零，即使之前的运行已经 calibrated；窗口逐帧调用
`snake_calibration_progress()`，进度条由核心绘制，3 秒后调用
`snake_calibration_done(true)`，进入 PLAY。GUI 的 START 路径不会直接
调用 `snake_start_play()` 跳过校准。

核心标题卡现在是 `TAP TO START` / `OR PRESS OK` /
`AUTO-CALIBRATES ON START`；窗口底部提示同步显示
“点击屏幕或按 OK 开始 · 自动校准 3 秒”（英文为
`TAP SCREEN OR PRESS OK · AUTO-CALIBRATES 3S`）。

可选启动参数：

```bash
python gyro-snake/desktop/play.py --demo        # 直接进入方形 demo
python gyro-snake/desktop/play.py --speed 4 --timing turbo --lang en
```

`--demo` 下拖动倾斜盘或按方向键即可接管（接管后 demo 自动停止）。

---

## 5. 操作

### 5.1 按钮

| 按钮 | 行为 |
| --- | --- |
| **点击屏幕** | 等同 OK/START：TITLE 开始并进入 3 秒模拟校准；PLAY 暂停；PAUSE 继续；GAME_OVER 回标题/重开。屏幕区域与下方按钮/倾斜盘不重叠 |
| **START / PAUSE / RESUME / RESTART** | TITLE：每次开始都进入 3 秒模拟校准（即使已校准）；PLAY：暂停；PAUSE：继续；结束/死亡：回 TITLE 并重新校准开局 |
| **CALIBRATE** | 随时重新校准（标题/暂停/游戏中/结束均可）；完成后从 PLAY 重新开始 |
| **CENTER** | 倾斜盘回中，`tilt_dir = NONE` |
| **1x / 2x / 4x** | 只会缩放模拟等待时间，不改变核心规则 |
| **EPD / FAST / TURBO** | 时序档位，见 §6 |
| **中 / EN** | 界面语言；游戏画面 HUD 始终英文 |
| **SOUND ON/OFF** | 静音开关（`winsound.Beep`） |
| **SHOT** | 截取整个窗口到 `desktop/out/screenshot_*.png` |
| **REC / STOP REC** | 录制面板帧（360x600 PNG）并在停止时合成 `record.gif` + `record.json` |

### 5.1.1 IMU 联调页（`desktop/imu_debug.py`）

四方向标定工具，串口**双向**：读固件遥测，同时发 `$` 命令（`console.c`，
UART0），改轴映射/死区/遥测率**不用重新烧录**。

```bash
python desktop/imu_debug.py --port COM65
```

流程：拿起设备 → 【置零】→ 每个方向倾斜保持 1 秒点【采 UP/RIGHT/DOWN/LEFT】
→ 【② 分析】自动对 8 种轴映射打分 → 【应用建议】→ 四方向箭头都对上后，
把映射写回 `imu_port.h` 的 `IMU_PORT_AXIS_*`。

- 左侧罗盘：绿点 = 固件报告的相对倾斜（外圈 20°），蓝圈 = 纯加速计角度；
  两者长期不重合说明 Mahony 滤波有问题。
- 右侧读数含锁存方向（LOCK）与 |加速度|：倾斜时 |a|≈1 g，纯平移晃动会让它波动。
- 底部日志区显示固件的 `CMD ... ok/err` 与关键启动行。

### 5.2 虚拟倾斜盘

- 鼠标在圆盘内按下/拖动：puck 跟随，角度决定 UP/RIGHT/DOWN/LEFT。
- 方向键 / WASD：直接把 puck 推到对应方向；`X` 或 `0` 回中。
- 盘上高亮当前方向；底部读数为 `TILT <DIR>`。
- 若请求方向与 core 当前方向相反（180°），core 会忽略；界面显示
  `· REV` 提醒，蛇继续原方向。
- 倾斜向量在松开鼠标后保持，便于“设好方向让蛇走”；按 CENTER 回中。

### 5.2.1 画面里的倾斜球

- 游戏画面（纸色屏幕）右下角有和真机相同的倾斜球：外圈 20° 满量程，内圈 8° 死区，
  圆点 = 当前倾斜，粗刻度 = 当前方向。
- **PLAY / PAUSE 时点击这个球** = 把当前倾斜设为零点（等同固件 DOWN 键 / CENTER）；
  点屏幕其他位置仍然是 OK（开始/暂停/继续）。
- 球由固件的 `tilt_ball.c` 直接画进 MONO 帧，桌面和真机共用同一份绘制代码。

### 5.3 键盘

| 键 | 行为 |
| --- | --- |
| Space / Enter | 主按钮（开始/暂停/继续/重开） |
| C | 校准 |
| ↑ / W、→ / D、↓ / S、← / A | 倾斜方向 |
| X / 0 | 倾斜盘回中 |
| 1 / 2 / 4 | 速度 |
| L | 中英文切换 |
| M | 音效开关 |
| T | 循环 EPD → FAST → TURBO |
| P | 截图 |
| R | 开始/停止录制 |

---

## 6. 时序档位

| 档位 | mono 快刷 | mono 全刷 | gray4 卡片 | 观察窗口 | 说明 |
| --- | --- | --- | --- | --- | --- |
| EPD | 550 ms | 1800 ms | 1800 ms | 1500 ms | 模拟真机电子墨水节奏 |
| FAST | 120 ms | 450 ms | 450 ms | 120 ms | 默认，适合试玩 |
| TURBO | 40 ms | 160 ms | 160 ms | 40 ms | 桌面快速验证 |

速度 1x/2x/4x 会把这些等待时间再除以倍率；校准的 3 秒是墙钟时间，
不受速度影响。

刷新握手严格按 `snake.h`：

```text
if dirty: decode/present -> snake_mark_refreshed(was_full)
else:     snake_tick(g, buttons, tilt_dir)
```

`dirty` 未清除时绝不 tick；`gs_tick()` 在 DLL 层也会拒绝并返回 0。

---

## 7. 存档

| 文件 | 内容 |
| --- | --- |
| `desktop/highscore.json` | `schema=1`, `high_score`, `updated`, `source`；只在破纪录且进入 GAME_OVER 时写 |
| `desktop/settings.json` | `schema=1`, `lang`, `sound`, `speed`, `timing`, `updated` |

两者都是本地运行状态，已被 `gyro-snake/.gitignore` 忽略。最高分单调递增；
删除文件即可重置。校准结果（陀螺零偏/中立姿态）只存在于本次进程的
core 状态里，不写桌面文件——桌面倾斜盘本来就是虚拟 IMU。

---

## 8. 无窗口自测

```bash
python gyro-snake/desktop/play.py --selftest --ticks 240 \
    --out gyro-snake/desktop/out/selftest
```

它会：

- 复算 seed 对应的开局苹果格（core 的 xorshift32 + 空格取样），
  用**不含 180° 反向**的脚本方向序列吃到苹果，再直行撞墙；
- 覆盖 TITLE、CALIBRATING 0/20/40/60/80/100%、PLAY、PAUSE、RESUME、
  EAT、GAME_OVER、再回 TITLE；
- 保存至少 8 张 480x800 PNG（验证过 14 张）+ `demo.gif` + `selftest.json`；
- 打印：

```text
SNAKE_SELFTEST ok frames=14 score=10
```

`--out` 的相对路径：以 `desktop/` 或 `gyro-snake/desktop/` 开头时都钉到
`gyro-snake/desktop/` 下，所以从仓库根或 `gyro-snake/` 执行都会写到
`gyro-snake/desktop/out/...`。

---

## 9. 窗口截图自测

```bash
python gyro-snake/desktop/play.py --window-test 3 \
    --out gyro-snake/desktop/out/window.png
```

流程：

1. `SetProcessDpiAwareness(1)`，再创建 520x1000 窗口；
2. 测试夹具跳过 3 秒校准动画（直接校准并进入 PLAY），以 FAST/2x 跑方形
   demo；这是自动化路径，不是用户按 START 的路径；
3. 3 秒后用 `ImageGrab` 截取窗口客户区；
4. 校验窗口尺寸 520x1000、屏幕面板非空白、控制区有绿色强调；
5. 打印：

```text
SNAKE_WINDOW ok path=...\desktop\out\window.png size=520x1000
```

截图里应能看到：360x600 的模拟屏幕、HUD（SCORE/HIGH/LEN/DIR）、
蛇、苹果，以及下方的倾斜盘和 START/PAUSE、CALIBRATE、CENTER 等按钮。

窗口测试会强制英文界面、2x、FAST、静音，且不写 `settings.json` /
`highscore.json`，以便得到可复现的截图。`out/window.png` 可复制为
`gyro-snake/docs/screenshot.png` 作为文档截图。

---

## 10. DLL ABI（`sim_api.c`）

静态存储：一个 `snake_t`、`mono[48000]`、`gray4[192000]`（两页）。
所有 `gs_*` 调用必须由同一线程串行执行；getter 只复制字段，不返回
指向 core 内部的指针。布尔值以 `int` 0/1 跨界。

| 导出 | 说明 |
| --- | --- |
| `gs_abi_version` | 返回 1 |
| `gs_init(seed)` | 绑定缓冲、初始化、生成 TITLE 卡 |
| `gs_tick(buttons, tilt_dir)` | dirty 时返回 0；否则推进一帧 |
| `gs_begin_calibration` | 进入 CALIBRATING 并生成 0% 卡 |
| `gs_calib_progress(pct)` | 设置 0..100；下一次 tick 重绘卡片 |
| `gs_calib_done(ok)` | 成功则 `start_run()` 进入 PLAY |
| `gs_start_play` | 低层 API：已校准则直接开局，否则回落到校准。GUI 的 START/屏幕点击**不调用**它，总是先走 3 秒校准 |
| `gs_set_high(v)` | 单调最高分 |
| `gs_mono` / `gs_gray4` | 借出的帧缓冲指针（调用方立即复制） |
| `gs_frame` / `gs_state` / `gs_card` | 帧类型 / 状态 / 卡片 |
| `gs_dirty` / `gs_needs_full` / `gs_mark` | 刷新握手 |
| `gs_score` / `gs_high` / `gs_length` / `gs_tick_index` | 状态字段 |
| `gs_calib_percent` / `gs_dir` / `gs_record_beaten` | 校准/方向/破纪录 |
| `gs_calibrated` / `gs_food_eaten` / `gs_cells_total` | 扩展只读字段 |
| `gs_take_sound` | 弹出音效事件（0 = NONE） |
| `gs_state_name` / `gs_card_name` / `gs_sound_name` | 名字（ASCII） |

`gs_gray4` 在 dirty 时返回 staging page（96,000 B），调用方只需拷这一页。

---

## 11. 文件

```text
gyro-snake/desktop/
├─ sim_api.c        ctypes DLL 边界（唯一 gs_* 实现）
├─ build.sh         编译 game_core + sim_api -> build/gyro_snake.dll
├─ test_dll.py      DLL 冒烟测试（DLL_SMOKE ok）
├─ play.py          窗口 / 自测 / 截图测试
├─ play.bat         Windows 双击启动器
├─ play.sh          Git Bash / POSIX 启动器
├─ README.md        本文件
├─ build/           生成物（gitignore）
└─ out/             截图、录制、自测输出（gitignore）
```

运行时会生成 `highscore.json` 和 `settings.json`。文档截图是
`gyro-snake/docs/screenshot.png`（由 `out/window.png` 复制）。

---

## 12. 验证记录（本机实测）

```text
$ bash gyro-snake/desktop/build.sh
[cc] core  4 file(s) + sim_api.c -> .../desktop/build/gyro_snake.dll
build ok

$ python gyro-snake/desktop/test_dll.py
DLL_SMOKE ok

$ python gyro-snake/desktop/play.py --selftest --ticks 240 --out gyro-snake/desktop/out/selftest
SNAKE_SELFTEST ok frames=14 score=10

$ python gyro-snake/desktop/play.py --window-test 3 --out gyro-snake/desktop/out/window.png
SNAKE_WINDOW ok path=.../desktop/out/window.png size=520x1000
```

截图 `gyro-snake/docs/screenshot.png`（源文件 `desktop/out/window.png`）
已人工检查：480x800 屏幕（0.75 缩放）、HUD、蛇、苹果、倾斜盘、
START/PAUSE、CALIBRATE 均可见。

---

## 13. 已知限制 / 排查

- **虚拟 IMU**：鼠标/键盘产生方向，不读取真实 LSM6DS3TR-C；不要用它
  判断真机轴向、噪声或校准算法。
- **模拟时序**：所有 ms 为模拟值，EPD 档也不是真机实测；真机残影、
  刷新耗时、按键手感、蜂鸣器音量不在覆盖范围。
- **每次 START 都重新校准**：即使上一次运行已经 calibrated，按 START/
  点击屏幕仍会走约 3 秒模拟归零；校准偏置只存在于本次进程的 core 状态里，
  不写桌面文件。最高分和界面设置会持久化。
- **EPD 档校准卡更新少**：gray4 校准卡走 1800 ms 全刷，3 秒校准期间只能
  显示少数几帧进度；默认 FAST 档会显示约 6 帧进度。这是模拟电子墨水
  刷新节奏的结果，不是卡死。
- **音效**：依赖 `winsound.Beep`；远程桌面/无声卡环境会静默失败，
  界面显示 `SOUND N/A` 或仍可开关，不影响游戏。
- **窗口测试失败**：确认桌面可见、分辨率 ≥ 520x1060、没有把窗口最小化；
  `ImageGrab` 需要真实桌面会话。DPI 已在创建 Tk 前设为 system aware。
- **DLL 加载失败**：确认 Python 与 DLL 同为 64 位；`build.sh` 已使用
  `-static-libgcc`，避免 MinGW 运行库缺失。
- **路径**：`--out desktop/...` 或 `--out gyro-snake/desktop/...` 都固定
  落到 `gyro-snake/desktop/` 下；绝对路径原样使用。
- 核心 SNAKE 状态/卡片文本（`TITLE`、`PAUSE`、`GAME OVER` 等）由 C 核心
  绘制，保持英文；窗口外壳的中英文切换不影响它们。