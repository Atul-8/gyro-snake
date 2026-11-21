# IMU 校准与姿态算法：从 `app_main()` 追真实调用链

参考代码：`MagicRobot_V0.1/embedded`。**本文只以实际执行到的代码为准，
不采信注释**；下面每条都给出文件与函数。

## 1. 启动链

```text
app_main()                                  apps/espidf/main/main.c:178
├─ app_motor_early_safe()
├─ esp_idf_i2c_bus_init() / esp_idf_pstore_init() / esp_idf_uart_init() / esp_idf_clock_init()
├─ mod_register(...); serial_init(on_cmd)
├─ imu_setup()                              main.c:94
│  ├─ lsm6ds3_init(&imu, HAL_I2C(), &cfg)   lsm6ds3.c
│  └─ dev_fsm_transition: INIT -> SELF_TEST -> READY
│     ├─ INIT:      lsm6ds3_configure()     lsm6ds3.c:97
│     └─ SELF_TEST: lsm6ds3_who_am_i() == 0x6A
├─ app_config_init()                        app_config.c
│  ├─ lsm6ds3_configure(&imu, &cfg)         （按持久化配置重写寄存器）
│  └─ mahony_init(&mahony, kp, ki, 0.01f)   app_config.c:129
├─ imu_calib_reset(&calib, CALIB_SAMPLES)   main.c:233；CALIB_SAMPLES = 500
├─ mahony_init(&mahony, kp, ki, 0.01f)      main.c:234
└─ while (1) { serial_poll(); pstore_poll(); app_stream_poll(); ... }
```

`app_stream_poll()` 里每拍调用 `do_sample()`：

```text
do_sample()                                 app_stream.c
├─ lsm6ds3_read_data(&imu, &s)              // g / dps
├─ raw = s
├─ if (sys_flags & SYSF_APPLY_BIAS_EN) s -= bias
├─ mahony_update(&mahony, &s)               // ← 用的是减过 bias 的 s
└─ last_raw = (RAW_IN_STREAM ? raw : s)
```

默认 `DEF_SYS_FLAGS` 包含 `SYSF_APPLY_BIAS_EN`（`app_ctx.h:53-54`），所以这是默认路径。

## 2. 校准链

命令触发 `app_imu.c` → `imu_calib_start(&calib)`；随后 `poll_calib()` 每拍：

```text
poll_calib()                                app_stream.c
├─ lsm6ds3_read_data(&imu, &raw)
├─ s = raw - current_bias                  // 先减当前 bias
├─ imu_calib_feed(&calib, &s, &raw)        // s 用于均值；raw 只用于重力轴判定
├─ on complete: imu_calib_result(&calib, &delta)
└─ bias += delta                           // 残差累加，不是覆盖
```

`imu_calib_feed()` 的真实行为（`imu_calib.c:55`）：

- 累加 `s` 的三轴陀螺与加速度；
- 另累加 `raw` 的三轴加速度，**只用于重力轴判定**；
- 收满 `target_n` 后：`grav_axis = argmax(|mean(raw_accel)|)`，
  `grav_sign = sign(mean(raw_accel[grav_axis]))`。

`imu_calib_result()` 的真实行为（`imu_calib.c:106`）：

```text
bias.g* = mean(g*)                         // 陀螺零偏
bias.a* = mean(a*)                         // 加速度均值
bias.a[grav_axis] -= grav_sign * 1.0f      // 减掉重力
```

因此它输出的加速度 bias 是「线性加速度零偏」，不是「姿态用的重力参考」。

## 3. 参考实现里的关键不一致

`do_sample()` 在 `mahony_update()` 之前减掉了上面这个包含重力减法的 bias：

```c
s.ax -= c->bias.ax;  s.ay -= c->bias.ay;  s.az -= c->bias.az;
mahony_update(&c->mahony, &s);
```

`mahony_update()` 首先归一化加速度，并在 `n < 1e-6f` 时直接返回
（`mahony.c:46` 起）。校准后、静止时 `s.ax/ay/az` 接近 0，于是：

- Mahony 不再获得重力参考；
- 姿态只能靠陀螺积分，roll/pitch 会慢慢漂移；
- 注释说“解算始终用去偏后的值”，代码也确实如此——但这条路径对 Mahony 是错的，不能照抄。

## 4. GYRO SNAKE 采用的做法

```text
校准窗口（300 样本 @ 100 Hz ≈ 3 s，设备静止）
├─ imu_calib_feed(raw, raw)                  // 均值 + 重力轴
├─ mahony_update(raw)                        // 加速度保留重力，陀螺此时未减 bias
└─ 完成时：
   ├─ imu_calib_result() -> gyro bias（只用 gx/gy/gz）
   ├─ mahony_get_angles() -> neutral_roll / neutral_pitch
   └─ bias.valid = true；neutral 保存到 NVS
运行期（100 Hz）
├─ corrected = raw; corrected.g -= gyro_bias   // 只减陀螺
├─ mahony_update(&mahony, &corrected)          // 加速度保留重力
├─ roll/pitch = mahony_get_angles() - neutral
└─ tilt_update(roll, pitch) -> -1 / UP / RIGHT / DOWN / LEFT
```

理由：

- Mahony 需要**重力**作为长期参考；减掉 1g 的 accel bias 不能用在它身上；
- 陀螺零偏必须减，否则 yaw/roll/pitch 会持续漂移；
- 中立姿态的 roll/pitch 作为零位，解决“开机时手已经倾斜”的问题；
- 方向判定用死区 + 连续样本 + 回中解锁，避免抖动；180° 反向由 core 忽略。

## 5. 硬件与寄存器事实

- 器件：LSM6DS3TR-C，I2C 7 位地址 `0x6A`，`WHO_AM_I` 寄存器 `0x0F` 期望 `0x6A`。
- Sticky 总线：I2C1，SDA=GPIO1，SCL=GPIO0，BSP 配置 100 kHz（`bsp_pins.h:38-43`）。
- 配置（`lsm6ds3.c:97`）：`CTRL1_XL` = ODR<<4 | FS<<2；`CTRL2_G` 同理；
  `CTRL3_C` = `0x44`（BDU + IF_INC）。
- 默认：accel ODR 104 Hz / ±2 g；gyro ODR 104 Hz / ±500 dps。
- 灵敏度：accel `{0.000061, 0.000488, 0.000122, 0.000244} g/LSB`（FS 编码非顺序）；
  gyro `{0.00875, 0.0175, 0.035, 0.07} dps/LSB`。
- 读取：先 `OUTX_L_G (0x22)` 6 字节，再 `OUTX_L_XL (0x28)` 6 字节；小端 int16。

## 6. 仍需真机确认

- IMU 在 Sticky 板上的实际轴向（与屏幕的对应关系）未知；`tilt_set_axis()` 提供
  `swap_xy / invert_roll / invert_pitch`，需要真机做一次四方向检查。
- 校准窗口内 Mahony 的收敛速度与静止噪声未实测；3 s 是保守值。
- 面板刷新耗时、按键手感、蜂鸣器音量未实测。