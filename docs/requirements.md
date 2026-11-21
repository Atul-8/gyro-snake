# GYRO SNAKE 需求与设计

## 1. 目标

在 reTerminal Sticky 上做一个**陀螺仪控制方向的贪吃蛇**：

- 手持设备，倾斜机身改变蛇的移动方向（上/右/下/左）；
- 屏幕 480×800 电子墨水，蛇每显示一帧移动一格；
- 按键负责开始/暂停/重新校准；
- 桌面模拟器可先用鼠标/键盘虚拟倾斜，体验完整游戏；
- 固件使用真实 LSM6DS3TR-C，包含静止校准、Mahony 姿态解算、倾角→方向判定。

## 2. 硬件事实

| 项 | 值 |
| --- | --- |
| 屏幕 | 3.97" 4 级灰度 EPD，480×800，SSD1677；整屏 mono 快刷/全刷、gray4 全刷，无局部窗口 |
| IMU | LSM6DS3TR-C，I2C1，SDA=GPIO1，SCL=GPIO0，地址 0x6A，100 kHz |
| 按键 | AI/OK=GPIO4，UP=GPIO5，DOWN=GPIO6，低有效 |
| 蜂鸣器 | GPIO48，LEDC PWM |
| 存储 | NVS（最高分、IMU 零偏/中立姿态） |
| 内存 | ESP32-S3，8 MB octal PSRAM；mono 48 KB + gray4 192 KB |
| 电源 | 启动必须把 GPIO45(HOLD)、GPIO46(LOCK) 拉高 |

## 3. 游戏规则

- 棋盘 15 列 × 23 行，每格 32 px；顶部 64 px HUD。
- 蛇初始长度 3，向右；每 tick 移动一格。
- 吃到食物：长度 +1，分数 +10，重新生成食物；食物只出现在空格。
- 撞墙或撞自身：死亡 → 一帧死亡画面 → GAME OVER。
- 填满棋盘：WIN。
- 分数上限 9999；最高分存 NVS；破纪录标记。
- 状态：TITLE / CALIBRATING / PLAY / PAUSE / DEAD / GAME_OVER。
- 卡片：标题、校准进度、暂停、GAME OVER、WIN；标题/校准/结束用 4 级灰度全刷，游戏/暂停用 mono。
- 按键：OK = 开始/暂停/继续/结束后回标题；UP = 重新校准（暂停/结束时）；DOWN 预留。
- 音效事件：吃、转向、死亡、开始、校准完成/失败、GAME OVER、WIN（原创短音）。

## 4. IMU 控制

- 校准：静止 300 样本 @ 100 Hz（≈3 s）；陀螺零偏 = 均值；重力轴 = |均值| 最大轴；
  中立姿态 = 校准窗口结束时 Mahony 的 roll/pitch。
- 运行：陀螺减零偏，加速度**保留重力**送入 Mahony；roll/pitch 减中立姿态；
  倾角超过死区（12°）并连续 3 样本稳定 → 方向；回中（6°）解锁下一次转向。
- 方向映射：默认屏幕自然坐标（roll=X，pitch=Y）；`tilt_set_axis(swap_xy,
  invert_roll, invert_pitch)` 供真机轴向修正。
- 180° 反向由 core 忽略，tilt 只提出请求。
- 校准结果（陀螺零偏、中立 roll/pitch、轴向配置）存 NVS；开机有有效记录则跳过校准。
- 详细算法与参考代码的真实调用链见 `docs/imu-calibration.md`。

## 5. 桌面模拟器

- 顶部 480×800 屏幕（0.75 → 360×600），底部控制区：
  - 虚拟倾斜盘（圆形，鼠标拖动/方向键），显示当前倾斜向量和选中方向；
  - START/PAUSE、CALIBRATE、CENTER、速度、语言、音效、截图、录制。
- 首次 START 走 3 s 校准进度（模拟），之后进入 PLAY；CALIBRATE 可重来。
- 时序档位：EPD（550/1800+1500 ms）、FAST（120/450+120 ms，默认）、TURBO（40/160+40 ms）。
- 中英文 UI 切换；游戏 HUD 由 core 渲染，保持英文。
- `--selftest`：无窗口脚本输入，保存 PNG/GIF，打印 `SNAKE_SELFTEST ok`。
- `--window-test`：开窗 3 s，`ImageGrab` 截图，打印 `SNAKE_WINDOW ok size=...`。

## 6. 固件结构

```text
firmware/
├─ components/bsp/         上游 BSP（epd/button/pins，Apache-2.0）
├─ components/game_core/   snake.h + snake.c/render.c/rng.c
├─ components/imu_core/    imu_core.h + imu_calib.c/mahony.c/tilt.c
├─ components/imu_port/    LSM6DS3TR-C I2C 驱动（ESP-IDF）
├─ components/board_port/  display/input/audio/storage
└─ main/main.c             电源保持、NVS、任务装配
```

任务：

- IMU 任务：100 Hz 采样 → 减陀螺零偏 → Mahony → tilt_update → 发布方向；
  校准模式下采集 300 样本并发布进度。
- 游戏任务：唯一调用 `snake_*` 与 `snake_mark_refreshed`；按刷新节奏 tick。
- 显示任务：唯一调用 `bsp_epd_*`；队列 + 单缓冲 shadow（不做 ping-pong；
  面板全屏波形是瓶颈，见 `docs/imu-calibration.md` 的说明）。
- 音频任务：蜂鸣器短音队列。

## 7. 验收标准

1. 主机端 `tests/test_snake.c` 与 `tests/test_imu.c` 全绿（gcc）。
2. `desktop/sim_api.c` 编出 DLL；`--selftest` 输出 `SNAKE_SELFTEST ok`。
3. `--window-test` 输出 `SNAKE_WINDOW ok`，截图能看出 480×800 屏幕、蛇、食物、HUD、虚拟倾斜盘。
4. 固件在 `idf-vm`（ESP-IDF v6.1.1）上 `idf.py build` 成功，产出 `gyro_snake.bin`。
5. Git：feature → develop → release → main，tag `v0.1.0`，工作区干净。
6. IP：无第三方游戏名称/角色/音乐；贪吃蛇是通用玩法，美术与音效原创。
7. 文档：README、requirements、imu-calibration、architecture、test-results、hardware-notes。

## 8. 不覆盖

- 真机烧录、IMU 实际轴向/噪声、EPD 实际刷新耗时/残影、按键手感、蜂鸣器音量、续航。
- GT911 触摸不参与本游戏。
- ESP-IDF 6.0.2 对照构建（实际 6.1.1）。