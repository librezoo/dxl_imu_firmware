# Validation record

What has been verified for each firmware version, and what has not. Details of the individual test sessions are
in the Git history. Test hosts: Windows with an FTDI adapter at 1 Mbps (0x0201–0x020A) and macOS with an FTDI
adapter (0x020B). The WCH-LinkE serial port (921,600 baud at most) is not suitable for 1 Mbps acceptance.

## Measurement conditions

- Serial timing needs a USB-UART with a 1 ms latency timer. With the default 16 ms VCP latency, 193 of 30,000
  50 Hz cycles timed out at the cycle boundary; this was traced to USB host buffering, not to the firmware or the bus.
- Attaching the WCH-Link debugger changes the target PLL and raises the UART rate by about 25 %. Detach it and
  reset the board before serial measurements.
- The nominal 240 Hz sensor rate measured about 252 Hz on the board used for 0x020A and about 233 Hz on the board
  used for 0x020B. `sample_counter` lets a host detect repeated samples.

## Results

| Version | Date | Change | Verified |
|---|---|---|---|
| 0x0202 | 2026-09-16 | DMA ring index, DXL byte stuffing and CRC order, bad-frame recovery, first-packet UART truncation | 5,000 combined reads without failure; flash read-back identical, configuration page unchanged |
| 0x0206 | 2026-09-16 | Gyro fixed at ±500 dps, sample counter and age, SFLP bias, SFLP_ODR reserved bits | 6,000 static and combined reads in total, none failed; flash read-back identical |
| 0x0207 | 2026-09-17 | Sensor rate raised to 240 Hz | IMU 200 Hz plus servos 50 Hz, 60 s: 12,000/12,000, bus load about 25.6 %. Reading every device at 200 Hz did not meet the target. |
| 0x0208 | 2026-09-17 | Raw 1 Mbps soft bus on USART1 | Host tests 6/6. IMU 200/200 at 1 Mbps. Two servos and the IMU at 50 Hz, 10 min: 30,000/30,000, mean 2.89 ms, max 6.70 ms. Safe control-table access on two servos: 142/142. Reversible servo ID/baud change by the tool. |
| 0x0209 | 2026-09-18 | USART1 RX idle pull-up (spurious bytes were echoed onto the bus) | IMU plus 15 servos at 50 Hz: 30,001/30,001. Single IMU at 200 Hz and mixed reads: 12,000/12,000 each, no errors. |
| 0x020A | 2026-09-24 | Soft bus moved to USART2, CLI always on USART1 | BIN 15,956 bytes, SHA-256 `3d132032302cfaab52e658bcc59df0d08435d2abacf9f8401c15d8cce023a51e`, identical from all three build entries; flash read-back identical. `READ` 300/300, `SYNC_READ` 100/100, CLI commands, baud switching 57.6 k–3 M. A servo (ID 3, model 1200) found through the soft bus; IMU 20/20 samples valid at about 251.7 Hz. |
| 0x020B | 2026-10-06 | Bus-writable IMU parameters | See below. |

## 0x020B

Build: xPack `riscv-none-elf-gcc` 12.5.0 (not the official WCH GCC 8.2), 16,644 bytes. Host tests 7/7 with
assertions enabled and `-Wall -Wextra` free of warnings. One CH32V203C8U6 board, reached through the DXL bus
by a `dxl_debugger_firmware` bridge.

- Parameter registers 224–227 and 232–235: a write without unlock returns the access error; an out-of-range
  value returns the data-range error; a valid write changes only the pending value; the running value changes
  after a reboot; a saved value reads back identically.
- A pair with `sflp_odr` above `imu_odr` makes `SAVE` return the result-fail error and nothing is stored.
- Each gyro range, each accelerometer range and each valid sensor/SFLP rate pair (nine) was applied and read back
  from the chip (`CTRL1`, `CTRL2`, `CTRL6`, `CTRL8`, `SFLP_ODR`). Sample counter rate: 116.5 Hz (120), 233.3 Hz (240), 468.8 Hz (480); SFLP 60/120/240/480 Hz gave
  about 58, 116, 234, 469 Hz.
- Gyro full scales 125–4000 dps: after about 12 s the wire value equals raw − SFLP bias to within 0.04 dps and the
  static mean is within ±0.07 dps. In the first 1–2 s after a reboot the bias estimate has not converged and the
  static mean can reach 0.5–1.4 dps.
- Accelerometer ±2/4/8/16 g: the SFLP gravity direction of a board at rest stays within 0.3–0.83° of the ±2 g
  value. Behaviour under motion has not been measured.
- Fixed in this version: `LOAD_DEFAULTS` did not discard earlier staged writes, so a later single-field write
  restarted from stale data (regression test added).

## Not verified

- 0x020A: long runs with several external devices, power-loss persistence, IMU unplug recovery, forced
  watchdog reset, fault-frame injection.
- 0x020B: servos on the bus, persistence across a real power cycle (only software reboots were done), upgrade of a
  stored version-1 record on a board (covered by host tests only), a build with the official WCH toolchain.
