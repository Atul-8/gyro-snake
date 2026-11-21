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