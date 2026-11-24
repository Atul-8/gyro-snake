# GYRO SNAKE

**把整块屏幕当方向盘倾斜着玩的贪吃蛇。**
跑在 Seeed **reTerminal Sticky** 开发板上：ESP32-S3 + 3.97" 480×800 电子墨水屏 +
六轴陀螺仪 + 电容触摸 + 蜂鸣器。没有硬件也可以先在 Windows 桌面模拟器里玩完整游戏。

> A gyro-steered Snake game for the Seeed reTerminal Sticky e-paper board,
> with a full Windows desktop simulator when you don't have the hardware.

![GYRO SNAKE](docs/screenshot.png)

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

## 仓库结构

```text
firmware/                  ESP-IDF 固件（esp32s3）
├─ components/game_core/   贪吃蛇核心与渲染（纯 C99，可脱离硬件复用）
├─ components/imu_core/    校准 / Mahony 姿态 / 倾角方向（纯 C99）
├─ components/bsp/         电子墨水与按键板级驱动（基于 Seeed 官方例程）
├─ components/imu_port/    LSM6DS3TR-C 驱动 + 100 Hz 姿态任务
├─ components/board_port/  显示 / 按键 / 蜂鸣器 / NVS / 串口控制台
└─ main/                   装配与游戏主循环
desktop/                   Windows 模拟器（Tkinter + 游戏 DLL）+ IMU 联调页
tests/                     纯 C 核心的主机单元测试（gcc 直接编译运行）
docs/                      需求、架构、IMU 算法分析、硬件笔记、测试记录
```

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

- 本项目代码：Apache-2.0（见 [LICENSE](LICENSE)）。
- `firmware/components/bsp/` 中的板级驱动基于 Seeed 官方例程改写，Apache-2.0，版权与出处见 [NOTICE](NOTICE)。
- 贪吃蛇为通用玩法；美术、音效、文档均为原创，不含任何第三方游戏素材。
