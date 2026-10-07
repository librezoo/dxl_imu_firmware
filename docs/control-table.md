# Control table and maintenance interface

The board is a Dynamixel Protocol 2.0 device. This document describes the control table of firmware
`0x0210` and the text maintenance port. Multi-byte values are little-endian.

## Protocol

| Item | Value |
|---|---|
| Default ID / baud rate | 200 / 1,000,000 bps (8N1) |
| Valid IDs | 0–253 |
| Valid baud rates | 57,600, 115,200, 1,000,000, 2,000,000, 3,000,000 |
| Instructions | `PING`, `READ`, `WRITE`, `REG_WRITE`, `ACTION`, `RESET`, `SYNC_READ`, `SYNC_WRITE` |
| Maximum `READ` length | 64 bytes |
| Error codes | 1 result fail, 2 instruction, 3 CRC, 4 data range, 5 data length, 6 access |

`RESET` reboots the board. `REG_WRITE`, `ACTION` and `SYNC_WRITE` act on the configuration registers only.
Reads of unlisted addresses in 136–159 return zero; any other unlisted address returns a data-range error.

## Register map

| Address | Size | Access | Contents |
|---:|---:|---|---|
| 124 | 12 | R | IMU sample (see below) |
| 160 | 24 | R | Diagnostics |
| 184 | 8 | R | IMU register read-back |
| 192 | 1 | R/W | Device ID (applies after reboot) |
| 193 | 4 | R/W | Bus baud rate in bps (applies after reboot) |
| 197 | 1 | W | Maintenance command |
| 198 | 1 | R | Configuration status |
| 208 | 14 | R | Gyro diagnostics |
| 224 | 4 | R/W | IMU parameters: `gyro_fs`, `accel_fs`, `imu_odr`, `sflp_odr` (apply after reboot) |
| 232 | 4 | R | IMU parameters in use since the last boot |
| 768 (`0x0300`) | 32 | R | Firmware update status (slots, versions); see [ota.md](ota.md) |
| 800 (`0x0320`) | 1 | W | Update command; only the unlock and `ENTER_BOOT` are accepted by the application |

Writes require the maintenance unlock. Reads of 224–227 and 192–196 show the pending (staged) value.

### IMU sample, address 124

| Offset | Size | Contents |
|---:|---:|---|
| 0 | 6 | Gyro X, Y, Z as `int16`, in counts of the configured full scale (17.5 mdps/LSB at ±500 dps), SFLP bias removed when available |
| 6 | 6 | SFLP game-rotation quaternion x, y, z as IEEE 754 half precision; `w = sqrt(1 − x² − y² − z²)` |

An all-zero quaternion means the SFLP has not produced a valid record yet. The quaternion is the sensor-frame
attitude; no mounting rotation or levelling is applied.

### Diagnostics, address 160

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | Firmware version (`0x0210`) |
| 2 | 1 | Protocol version (2) |
| 3 | 1 | Device ID in use |
| 4 | 2 | Baud rate in use: 0 = 57,600, 1 = 115,200, 2 = 1 M, 3 = 2 M, 4 = 3 M |
| 6 | 2 | Status flags |
| 8 | 4 | Sample counter (increments per valid SFLP quaternion) |
| 12 | 4 | Sample age in µs (1 ms resolution; `0xFFFFFFFF` while no valid sample) |
| 16 | 2 | Last DXL error code |
| 18 | 2 | Reset reason: bit 0 watchdog, bit 1 software reset |
| 20 | 2 | Configuration generation |
| 22 | 2 | Configuration status: 0 stored, 1 no valid stored record (defaults in use), 2 defaults loaded, unsaved |

Status flags: bit 0 sample valid, 1 IMU ready, 2 IMU initialisation fault, 3 IMU FIFO fault, 4 soft-bus RX
overflow, 5 soft-bus TX queue overflow, 6 request echo mismatch. Bit 2 clears when a retry succeeds; bits 3–6
stay set until the next reset.

### IMU read-back, address 184

`WHO_AM_I`, `CTRL1_XL`, `CTRL2_G`, `CTRL3_C`, `CTRL6`, `CTRL8`, `EMB_FUNC_EN_A`, `SFLP_ODR`, read from the
LSM6DSV16X after initialisation.

### Gyro diagnostics, address 208

Raw gyro X, Y, Z (`int16`, configured full scale, offsets 0–5); SFLP gyro bias X, Y, Z (`int16`, always
4.375 mdps/LSB, offsets 6–11); `bias_valid` (offset 12); `sample_valid` (offset 13).

## Maintenance commands

Write one byte to address 197.

| Value | Command |
|---:|---|
| `0xA5`, then `0x5A` | Unlock. Both writes are required in this order; a reboot locks again. |
| `0x01` | Save the pending configuration to flash. Fails with error 1 if the configuration is invalid. |
| `0x02` | Reboot. |
| `0x03` | Load factory defaults into the pending configuration (not saved). |

Typical change: unlock, write the registers, `0x01`, `0x02`.

## IMU parameters

The values are LSM6DSV16X register codes. They are range-checked per byte on write. `sflp_odr` must not exceed
`imu_odr`; that pair is checked on save.

| Address | Name | Values (code: meaning) | Default |
|---:|---|---|---|
| 224 | `gyro_fs` | 0: ±125, 1: ±250, 2: ±500, 3: ±1000, 4: ±2000, 12: ±4000 dps | 2 |
| 225 | `accel_fs` | 0: ±2 g, 1: ±4 g, 2: ±8 g, 3: ±16 g | 0 |
| 226 | `imu_odr` | 6: 120 Hz, 7: 240 Hz, 8: 480 Hz (accelerometer and gyro) | 7 |
| 227 | `sflp_odr` | 2: 60 Hz, 3: 120 Hz, 4: 240 Hz, 5: 480 Hz | 4 |

The gyro bytes at address 124 follow `gyro_fs`: 17.5 mdps/LSB at ±500 dps, 70 at ±2000, 140 at ±4000. A host that
decodes 17.5 mdps/LSB (such as robotd) must keep `gyro_fs` at 2. The ±4000 dps range requires the gyro OIS
channel to be disabled; this firmware does not enable it.

## Stored configuration

Two 4 KiB flash pages at `0x0800E000` and `0x0800F000` hold CRC-32 protected records, written alternately
by generation. The newest valid record wins. Record version 2 stores ID, baud rate and the four IMU parameters;
version 1 records are upgraded on load and keep their ID and baud rate.

## Text maintenance port

USART1 (PA9 TX, PA10 RX), 115,200 bps 8N1, lines ending in `\r\n`. No unlock is needed.

| Command | Reply |
|---|---|
| `get` | `id=<n> baud=<n> generation=<n>` |
| `diag` | `status=<flags> samples=<n> age_us=<n> errors=<n>` |
| `set id <0–253>` | `ok` |
| `set baud <rate>` | `ok` |
| `save` | `ok` or `error save` |
| `load-defaults` | `ok` |
| `reboot` | `ok` |
| `dump-imu` | `gyro=<x>,<y>,<z> valid=<0/1>` |
| `dump-errors` | `last=<n> count=<n> reset=<n>` |

`set id`, `set baud` and `load-defaults` change the pending configuration; use `save` and `reboot` to keep and
apply it. The CLI does not expose the IMU parameters.
