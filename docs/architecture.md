# GYRO SNAKE 架构

## 1. 分层

```text
┌──────────────────────────── 便携 C99（host 可单测） ────────────────────────────┐
│ game_core/snake.h   snake.c / render.c / rng.c / font.c                        │
│ imu_core/imu_core.h imu_calib.c / mahony.c / tilt.c                            │
└────────────────────────────────────────────────────────────────────────────────┘
                ▲                                   ▲
                │ snake_* / imu_*                   │
┌───────────────┴──────────────┐       ┌────────────┴───────────────────────────┐
│ firmware/components/imu_port │       │ firmware/components/board_port          │
│ LSM6DS3TR-C I2C (100 Hz)     │       │ display / input / audio / storage       │
└───────────────┬──────────────┘       └────────────┬───────────────────────────┘
                │ I2C1 (SDA1/SCL0, 0x6A)             │ EPD SPI / GPIO / LEDC / NVS
┌───────────────┴────────────────────────────────────┴───────────────────────────┐
│ firmware/main/main.c: 电源保持、NVS、任务装配                                  │
└────────────────────────────────────────────────────────────────────────────────┘

desktop/sim_api.c  = 同一份 game_core 编成 DLL（gs_* 导出）
desktop/play.py    = Tkinter 硬件模拟器（虚拟倾斜盘 + START/PAUSE/CALIBRATE）
```

`game_core` 与 `imu_core` 不依赖 ESP-IDF、FreeRTOS、I2C、malloc、时间或 I/O；
同一份源码在 ESP32-S3、host 测试和桌面 DLL 中逐字节一致。

## 2. 任务模型

| 任务 | 周期 | 职责 |
| --- | --- | --- |
| IMU | 100 Hz | 读 LSM6DS3 → 减陀螺零偏 → Mahony → 减中立角 → tilt_update → 发布方向；校准模式下采集 300 样本并发布进度 |
| game | 由显示刷新驱动 | 唯一调用 `snake_*` / `snake_mark_refreshed`；读按键与方向，推进一帧 |
| display | 队列驱动 | 唯一调用 `bsp_epd_*`；mono/gray4、shadow/prev、全刷策略 |
| audio | 队列驱动 | GPIO48 LEDC 短音；不阻塞游戏 |

game 任务与 display 任务通过 `board_port_refresh()` 同步：提交帧 → 显示任务
阻塞刷新 → 返回实测毫秒 → game 任务 `snake_mark_refreshed()`。没有双 framebuffer
ping-pong；SSD1677 全屏 BUSY/波形是瓶颈，ping-pong 只能重叠 CPU/SPI，不能缩短
面板可见刷新时间。

## 3. 刷新握手

```text
if (snake_dirty(g)) {
    kind  = snake_status(g)->frame;       // MONO / GRAY4
    data  = kind == MONO ? snake_mono(g) : snake_gray4(g);
    full  = snake_needs_full_refresh(g);
    ms    = display_port_refresh(data, kind, full);   // 阻塞
    snake_mark_refreshed(g, full);
}
snake_tick(g, buttons, tilt_dir);          // 仅在 !dirty 时
```

- 首帧、gray4 之后、暂停/死亡恢复、每 20 次快刷：全刷。
- gray4 双页：`snake_mark_refreshed()` 时翻页，下一帧 mono 必须全刷。
- game 任务的节奏 = 实测刷新时间 + `GS_THINK_MS`（默认 120 ms 观察窗）。

## 4. 状态机

```text
TITLE --OK--> CALIBRATING --done(true)--> PLAY
  ^                 |                       |
  |                 +--done(false)--> TITLE |
  |                                         v
  +---------------- GAME_OVER <-- DEAD <-- PLAY --OK--> PAUSE
  |                    ^                       |         |
  +--------------------+                       +--OK-----+
```

- TITLE：校准过则 OK 直接 PLAY，否则进入 CALIBRATING。
- PLAY：OK 暂停；tilt_dir 请求方向（180° 反向忽略）；每 tick 移动一格。
- DEAD：一帧死亡画面，下一 tick GAME_OVER；更新最高分。
- GAME_OVER：OK 回 TITLE；UP 重新校准。
- PAUSE：OK 继续；UP 重新校准。

## 5. IMU 校准与方向

详细调用链见 `docs/imu-calibration.md`。

```text
校准（3 s，静止）
  raw → imu_calib_feed(raw, raw) + mahony_update(raw)
  完成 → imu_calib_result() 取陀螺零偏
       → mahony_get_angles() 取中立 roll/pitch
       → NVS blob (bias, neutral, valid, version)

运行
  raw → corrected.g = raw.g - gyro_bias
      → mahony_update(corrected)         // accel 保留重力
      → roll/pitch - neutral
      → tilt_update() → -1/UP/RIGHT/DOWN/LEFT
```

**不要**把 `imu_calib_result()` 的 accel 均值减 1g 后喂给 Mahony；参考实现
`imu_calib_result` 会减 1g，而 `do_sample` 又把该 bias 用于 Mahony，导致校准后
Mahony 的 accel 接近 0 并提前返回。本项目只减陀螺零偏。

## 6. 文件所有权

| 目录 | 所有者 | 说明 |
| --- | --- | --- |
| `firmware/components/game_core/**` | game core | 冻结 `snake.h`；渲染/字体/RNG |
| `firmware/components/imu_core/**` | imu core | 冻结 `imu_core.h`；校准/Mahony/tilt |
| `firmware/components/imu_port/**` | firmware | LSM6DS3 I2C 驱动 |
| `firmware/components/board_port/**` | firmware | display/input/audio/storage |
| `firmware/main/**` | firmware | 电源保持、任务装配 |
| `desktop/**` | desktop | DLL + Tkinter 模拟器 |
| `tests/**` | core/imu | host 单测 |
| `docs/**` | all | 需求/架构/校准/硬件/测试 |