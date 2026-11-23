# GYRO SNAKE — project rules

- **Host first.** Before touching firmware, run:
  ```bash
  gcc -std=c99 -O2 -Wall -Wextra -I firmware/components/game_core/include \
      firmware/components/game_core/src/*.c tests/test_snake.c -o /tmp/t && /tmp/t
  gcc -std=c99 -O2 -Wall -Wextra -I firmware/components/imu_core/include \
      firmware/components/imu_core/src/*.c tests/test_imu.c -lm -o /tmp/i && /tmp/i
  ```
- **Desktop gate.** `bash desktop/build.sh`, `python desktop/play.py --selftest ...`,
  `python desktop/play.py --window-test 3 ...` must pass before a release.
- **Firmware via the VM.** Do not install ESP-IDF on Windows. Use
  `<ai_global>/meta/scripts/espidf-vm/vm-stage-build.sh` as in README.
- **Do not trust comments in the MagicRobot reference.** The real call chain and the
  accel-bias/Mahony inconsistency are documented in `docs/imu-calibration.md`.
  Do not feed a gravity-subtracted accel bias to Mahony.
- **IMU axis is compile-time.** `IMU_PORT_AXIS_SWAP_XY / INVERT_ROLL / INVERT_PITCH`
  default to screen-natural. A real unit must be checked in all four directions
  before claiming the tilt mapping works.
- **No fake hardware claims.** A build is not a flash; a desktop screenshot is not a
  device observation. Only `idf.py -p <PORT> flash monitor` counts as on-device.
- **Touch coordinates come from the device.** Read `GT911_REG_COORD_RES (0x8146)` at
  boot and use the range it reports (this unit says `480x800`); the vendor
  `sticky_touch.cpp` hardcodes `800x480` for a different canvas. Log every press as
  `tap raw=... -> screen=...` so one tap in a known corner can falsify the transform.
  See `.ai/errors/raw/ERR-001.md` / `META-001`.
- **No third-party game IP.** Snake is a generic game; keep art, names and sounds
  original. No Nintendo/Mario/etc. names, sprites or melodies.
- **Keep `snake.h` and `imu_core.h` frozen.** Additions go through the architect; the
  host and firmware ports depend on the ABI.