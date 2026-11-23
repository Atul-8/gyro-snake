# 测试记录 — GYRO SNAKE

日期：2026-11-22
环境：Windows 11 / gcc 16.2.0 / Python 3.12.10 / Pillow 13 / Tk 8.6；
固件在 `idf-vm`（Ubuntu，ESP-IDF v6.1.1）上构建。

## 1. 核心单测

```bash
gcc -std=c99 -O2 -Wall -Wextra \
    -I gyro-snake/firmware/components/game_core/include \
    gyro-snake/firmware/components/game_core/src/*.c \
    gyro-snake/tests/test_snake.c -o /tmp/test_snake && /tmp/test_snake
```

```text
gyro-snake core tests
TESTS passed=522 failed=0
```

覆盖：初始化、移动、转向、180° 反向忽略、吃苹果/增长/分数、撞墙、撞自身、
暂停/继续、死亡→GAME OVER、WIN、最高分、校准进度卡、刷新握手、确定性。

## 2. IMU 单测

```bash
gcc -std=c99 -O2 -Wall -Wextra \
    -I gyro-snake/firmware/components/imu_core/include \
    gyro-snake/firmware/components/imu_core/src/*.c \
    gyro-snake/tests/test_imu.c -lm -o /tmp/test_imu && /tmp/test_imu
```

```text
TESTS passed=179 failed=0
```

覆盖：陀螺零偏 (1.5,-0.5,0.25) 收敛、重力轴 +Z/-X/-Y 检测、加速度均值保留重力
（不减 1g）、Mahony 静止/倾斜/零加速度早退/yaw 积分、tilt 死区/稳定样本/回中
解锁/轴向映射/方向名。

## 3. 桌面模拟器

```bash
bash gyro-snake/desktop/build.sh
python gyro-snake/desktop/test_dll.py
python gyro-snake/desktop/play.py --selftest --ticks 240 --out gyro-snake/desktop/out/selftest
python gyro-snake/desktop/play.py --window-test 3 --out gyro-snake/desktop/out/window.png
```

实际输出：

```text
build ok  .../desktop/build/gyro_snake.dll
DLL_SMOKE ok
[GYRO SNAKE] selftest food=(6,20) plan=42
SNAKE_SELFTEST ok frames=14 score=10
SNAKE_WINDOW ok path=...\desktop\out\window.png size=520x1000
```

截图：`docs/screenshot.png`（520×1000，上半部分 480×800 屏幕，下半部分虚拟倾斜盘 +
START/PAUSE/CALIBRATE；HUD 的 DIR 箭头已修正，棋盘有点阵网格）。

## 4. 固件

```bash
bash <ai_global>/meta/scripts/espidf-vm/vm-stage-build.sh \
  <项目根>/firmware --set-target esp32s3 \
  --fetch <项目根>/firmware/build-vm --log-tail 50
```

```text
[vms] 退出码 0 / 耗时 139s / 错误行 0
gyro_snake.bin binary size 0x48870 bytes.
Smallest app partition is 0x300000 bytes. 0x2b7790 bytes (91%) free.
Project build complete.
297072  ~/work/firmware/build/gyro_snake.bin
```

本机回拷：`firmware/build/gyro_snake.bin`，297,072 B，
SHA-256 `2142664f33b372e3c679da26f05992f15f0acbf70718a6d30debe2168da4c168`。

构建日志中 `error:` 与 `warning:` 计数为 0；IDF v6.1.1。

## 5. 未验证 / 不在本轮范围

- 真机烧录、IMU 实际轴向/噪声/校准收敛（`IMU_PORT_AXIS_*` 需真机四方向检查）。
- EPD 实际快刷/全刷耗时、残影。
- 按键手感、蜂鸣器音量、电池续航。
- I2C1 多设备并发访问。
- GT911 触摸。
- ESP-IDF 6.0.2 对照构建（实际 6.1.1）。

桌面模拟器的 550/1800/1500 ms 与 FAST/TURBO 都是模拟值，不是面板实测；
桌面倾斜盘是虚拟 IMU，不读取真实 LSM6DS3TR-C。

## 6. 2026-11-23 更新：触摸、归零、倾斜球

### 6.1 固件构建与烧录

```text
[vms] 退出码 0 / 耗时 37s / 错误行 0
gyro_snake.bin binary size 0x49f30 bytes (302,896 B)
Smallest app partition is 0x300000 bytes. 0x2b60d0 bytes (90%) free.
```

```text
esptool --chip esp32s3 -p COM65 -b 460800 write-flash \
  0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 gyro_snake.bin
Wrote 302896 bytes ... Hash of data verified.
```

### 6.2 启动日志（真机，COM65，115200）

```text
I (761) gyro_snake: power hold high: GPIO45(HOLD)=1 GPIO46(LOCK)=1
I (1180) gs_touch: GT911 ready: address=0x14 id=911 res=480x800
I (1180) gs_touch: touch polling every 30 ms
I (1213) gs_imu: LSM6DS3TR-C on I2C1 SDA=1 SCL=0 @0x6A, 104 Hz, +-2 g, +-500 dps
I (1225) gyro_snake: tilt: deadzone 8 deg, rearm 4 deg, 2 stable samples; axis map 0/0/0
I (2661) bsp_epd: e-paper gray4 refresh in 1219 ms
I (2661) gyro_snake: gray4 full 1404 ms | state=TITLE score=0 high=0
```

**关键发现**：GT911 的 0x8146 分辨率寄存器返回 **480x800**（不是 800x480），
因此坐标映射按同轴缩放处理（raw X → 屏幕 X，raw Y → 屏幕 Y）；轴向翻转尚未
用“点击已知角落”实测标定。驱动在每次按下时打印：

```text
gs_touch: tap raw=<rx>,<ry> -> screen=<sx>,<sy>
```

游戏侧归零判定接受**任意角落**的点击（x ≤ 100 或 ≥ 380，且 y ≤ 100 或 ≥ 700），
所以在四种翻转组合下，点击右下角倾斜球都能触发归零；日志用于事后收紧到单一角落。

### 6.3 倾斜球与归零

- `board_port/src/tilt_ball.c`：纯 1 bpp 绘制，外圈 26 px / 20° 满量程，
  内圈 8° 死区，圆点 = 相对 roll/pitch，粗刻度 = 当前方向。
- 固件在 MONO 帧交给面板前调用 `board_draw_tilt_ball()`；
  PLAY 每帧由核心整帧重绘，不会留下拖影。
- 点击屏幕角落（PLAY/PAUSE）或按 DOWN(6) → `imu_port_request_rezero()`。
- 桌面 DLL 链接同一份 `tilt_ball.c`，`gs_draw_ball()` 在取帧前盖章；
  GUI 点击画面右下角的球 → `center_tilt()`。

### 6.4 桌面回归

```text
SNAKE_SELFTEST ok frames=14 score=10
SNAKE_WINDOW ok path=desktop\out\window.png size=520x1000
```

### 6.5 仍未验证

- GT911 轴向翻转（需用户点击一次已知角落，读 `tap raw=...` 日志）。
- 倾斜球在真机电子纸上的实际可读性（1 bpp、160 dpi 下的细线）。
- 电池供电下的功率保持（GPIO45/46 已确认读回 1，但只在 USB 下观察）。
- IMU 四方向物理映射（`IMU_PORT_AXIS_SWAP_XY / INVERT_ROLL / INVERT_PITCH`）。