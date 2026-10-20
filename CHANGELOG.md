# Changelog

Firmware versions are reported in the diagnostics block (address 160). Test results are in
[docs/validation.md](docs/validation.md).

## 0x0211 — 2026-10-21

- Fast Sync Read (instruction 0x8A). The board starts the combined status packet when it is the first ID of the
  request, as the robot's control loop lists it: header from the broadcast ID, then its block with the running CRC.
  The servos behind it append theirs; they have to implement 0x8A too (XL330 firmware 46 or newer; the KPower
  RD05T does not). Not first in the list, or addressed directly, the board stays silent. Plain `SYNC_READ` is
  unchanged. See [docs/control-table.md](docs/control-table.md).
- Host tests: the first block is checked byte for byte against the ROBOTIS e-manual's status packet. Not yet run on a
  board.

## 0x0210 — 2026-10-07

- Firmware update over the bus: a resident bootloader (8 KiB) with two 24 KiB application slots, trial start and
  automatic fall-back, update window at 0x0300–0x04FF, `ENTER_BOOT` in the application. See
  [docs/ota.md](docs/ota.md). `hdbus ota update` (0.2.0) performs the update.
- New files: `bootloader/`, `src/ota.c`, `scripts/build-ota.sh`, `tools/ota_pack.py`; `docs/ota-design.md` became
  `docs/ota.md`.
- The standalone image (`build.ps1`, `build-xpack.sh`) is unchanged apart from the update status registers; the
  first deployment of the bootloader is over SWD (`initial.bin`).
- `IMU_POWER_FW_VERSION` sets the reported version at compile time.

## 0x020B — 2026-10-06

- Gyro range, accelerometer range, sensor rate and SFLP rate are configurable over the bus (addresses 224–227,
  running values at 232–235) and stored in flash. Defaults are unchanged: ±500 dps, ±2 g, 240 Hz.
- Configuration record version 2; version 1 records are upgraded on load and keep their ID and baud rate.
- Wire gyro counts follow the configured full scale.
- Fix: `LOAD_DEFAULTS` now discards earlier staged writes.
- Build option `IMU_POWER_DEFAULT_ID` sets the factory ID at compile time (default 200).
- `scripts/build-xpack.sh` builds with the xPack toolchain on macOS/Linux.
- Built with xPack `riscv-none-elf-gcc` 12.5.0 (earlier releases: MounRiver WCH GCC 8.2.0). BIN 16,644 bytes,
  SHA-256 `69e4eb55aea68198e5ce21b4e178d5b719da7bfb5e1e05748423d3ea2f0c06d7`.

## 0x020A — 2026-09-24

- The raw 1 Mbps soft bus moved from USART1 (PA9/PA10) to USART2 (PA2/PA3) with DMA1 channels 6 and 7.
- USART1 is now always the 115,200 bps text CLI; the separate CLI build is no longer needed.
- Idle pull-ups on PA3 and PA10.
- `dxl_softbus_tool.py check` combines IMU sampling, bus scan and servo status; PASS/WARN/FAIL with exit codes 0/1/2;
  DYNAMIXEL status bit `0x80` is reported as an alert, not a protocol error.
- BIN 15,956 bytes, SHA-256 `3d132032302cfaab52e658bcc59df0d08435d2abacf9f8401c15d8cce023a51e`.

## 0x0209 — 2026-09-18

- Fix: USART1 RX is pulled up while idle. A floating input produced spurious bytes that the bridge echoed onto the
  DXL bus.

## 0x0208 — 2026-09-17

- USART1 became a raw 1 Mbps 8N1 bridge to the DXL bus (DMA with idle-line framing, bursts of up to 512 bytes,
  4,096-byte mirror queue); the internal device answers requests without relying on the bus echo.
- Status flags for bridge overflow and echo mismatch; larger physical-bus RX ring (1,024 bytes).
- `tools/dxl_softbus_tool.py`: multi-baud scan, servo ID/baud change with dry run and safety checks.

## 0x0207 — 2026-09-17

- Accelerometer, gyro and SFLP run at the 240 Hz setting (previously 120 Hz).

## 0x0206 — 2026-09-16

- Fixes for lost combined reads (DMA ring index, first-packet truncation, byte stuffing, CRC order, bad-frame
  recovery).
- Gyro set explicitly to ±500 dps (17.5 mdps/LSB); sample counter advances only on new SFLP records; sample age is
  reported; SFLP gyro bias is applied; SFLP_ODR reserved bits are preserved; quaternion validity and binary16
  encoding hardened.
