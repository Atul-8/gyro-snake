# 硬件笔记（reTerminal Sticky）

来源：CodeCraft AI Hardware `devices/reterminal-sticky/AGENTS.md` 与 BSP 头文件；
以下是本项目实际使用的部分。

## 1. 屏幕

- 3.97" 4 级灰度 EPD，SSD1677，逻辑 480×800（竖屏），235 ppi。
- mono 1bpp：60 B/行，MSB 最左，1=白，0=黑；48,000 B。
- gray4 2bpp：120 B/行，0=黑..3=白；每页 96,000 B，双页 192,000 B。
- 只有整屏 mono 全刷 / mono 快刷 / gray4 全刷；没有局部窗口刷新。
- 快刷依赖 `prev` 帧；连续快刷会累积残影，每 20 次快刷强制全刷。
- gray4 之后下一次 mono 必须全刷。
- 刷新调用同步阻塞到 BUSY 释放；只能在任务上下文调用。

## 2. IMU

- LSM6DS3TR-C，I2C1：SDA=GPIO1，SCL=GPIO0，地址 **0x6A**，100 kHz。
- WHO_AM_I 0x0F 期望 0x6A。
- 配置：`CTRL1_XL = 0x40`（104 Hz，±2 g），`CTRL2_G = 0x44`（104 Hz，
  ±500 dps，注意 bit2 为 FS_125 相关位，本项目按 0x44 写入），
  `CTRL3_C = 0x44`（BDU + IF_INC）。
- 读取：`OUTX_L_G (0x22)` 6 字节、`OUTX_L_XL (0x28)` 6 字节；小端 int16。
- 灵敏度：accel 0.000061 g/LSB，gyro 0.0175 dps/LSB。
- 与 SHT40（0x44）、BQ27220（0x55）、PCF8563（0x51）共用 I2C1；
  本项目只访问 0x6A。
- **轴向未在真机验证**：`imu_port.h` 的 `IMU_PORT_AXIS_SWAP_XY /
  INVERT_ROLL / INVERT_PITCH` 默认 0（屏幕自然映射）。真机需要做四方向检查后
  调整并重编。

## 3. 按键 / 蜂鸣器 / 电源

| 功能 | 引脚 | 说明 |
| --- | --- | --- |
| AI / OK | GPIO4 | 低有效，内部上拉；也是深睡唤醒源 |
| UP | GPIO5 | 低有效 |
| DOWN | GPIO6 | 低有效 |
| 蜂鸣器 | GPIO48 | LEDC PWM，单音 |
| HOLD | GPIO45 | 启动必须拉高，否则电池供电掉电 |
| LOCK | GPIO46 | 启动必须拉高 |
| EPD | SPI2 20 MHz | SCK13/MOSI14/MISO12/CS15/DC16/RST17/BUSY18/EN47 |
| USB-UART | CH343P | TX43/RX44 |

## 4. 存储 / 内存

- ESP32-S3，240 MHz，8 MB octal PSRAM @80 MHz。
- Flash：厂方 32 MB，`sdkconfig.defaults` 保守配置 16 MB。
- 分区：factory app 3 MB；NVS 24 KB；phy_init 4 KB。
- NVS 保存：最高分、IMU 校准 blob（gyro bias、neutral roll/pitch、valid、version）。
- 帧缓冲：mono 48,000 B + gray4 192,000 B + mono shadow 48,000 B，全部 PSRAM。

## 5. 未验证

- 真机 IMU 实际轴向、噪声、零偏随温度漂移、校准收敛。
- EPD 实际快刷/全刷耗时与残影。
- 按键手感、蜂鸣器音量、电池续航。
- I2C1 多设备并发访问（本项目只读 0x6A，未做总线争用测试）。
- ESP-IDF 6.0.2（实际使用 6.1.1）。