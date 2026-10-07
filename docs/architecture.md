# Architecture

## Hardware

| Item | Value |
|---|---|
| MCU | WCH CH32V203C8U6 (RV32IMAC, 64 KiB flash, 20 KiB RAM), 8 MHz crystal, 96 MHz core clock |
| IMU | ST LSM6DSV16X on SPI1 (PA5 SCK, PA6 MISO, PA7 MOSI, PB0 chip select), data-ready on PB14 |
| Watchdog | IWDG, prescaler 64, reload 2500 (about 4 s with the nominal 40 kHz LSI) |

## Source layout

| Path | Role |
|---|---|
| `include/imu_power/`, `src/` | Portable core: Dynamixel framing and CRC, control table, configuration storage, IMU sample packing, soft-bus bridge logic, text CLI, update protocol and slot selection (`ota.c`). No hardware headers. |
| `platform/` | CH32V203 port: clocks, GPIO, USART/DMA, SPI, flash, watchdog, main loop. With `bootloader/`, the only code that includes WCH headers. |
| `bootloader/` | Bootloader for the A/B slots: linker script, reset handler, serial/flash glue around `src/ota.c` (see [ota.md](ota.md)) |
| `vendor/wch-ch32v20x/` | Pinned WCH EVT SDK (see `manifest.json` for per-file SHA-256). The build reads nothing outside the repository. |
| `tests/` | Host tests of the portable core (C99, CMake/CTest). |

## Serial interfaces

| Port | Pins | Role |
|---|---|---|
| USART1 | PA9 TX, PA10 RX | Text maintenance CLI, 115,200 bps 8N1, polled |
| USART2 | PA2 TX, PA3 RX | Soft bus: raw 1 Mbps 8N1 bridge to the DXL bus; RX DMA1 channel 6, TX DMA1 channel 7 |
| USART3 | PB10 TX (open drain), PB11 RX, PA15 direction | Physical half-duplex DXL bus at the configured baud rate; RX DMA1 channel 3 (circular, 1,024 bytes), TX DMA1 channel 2 |

PA3 and PA10 use internal pull-ups so that an unconnected adapter cannot inject noise onto the bus.

## Soft-bus bridge

USART2 RX bytes are framed by the UART idle condition into bursts of at most 512 bytes and sent unchanged
to the DXL bus through USART3. Everything received from the DXL bus is queued (4,096 bytes) to USART2 TX, so
the soft-bus host sees the bus traffic, including replies from other devices.

The bridge neither validates nor rewrites frames; a frame with a wrong CRC is forwarded as it is. There is no
arbitration, collision recovery or retry: the host must wait for a transaction to finish before sending the next
burst. Byte order is preserved but timing is not (use a logic analyser for timing evidence).

A soft-bus burst is also given to the local DXL parser after it has been transmitted, so the internal device (ID
200 by default) answers without relying on the bus echoing the request. The status bytes it sends are queued to
USART2 explicitly. If a hardware revision echoes the request on RX, exact echo matching removes the echo from the
mirror and from the local parser.

The soft bus and the CLI run together in the same image. The build option `IMU_POWER_DEBUG_CLI` and the script
switch `-DebugCli` are accepted for compatibility and have no effect.

## IMU data path

1. At start-up the firmware reads `WHO_AM_I`, resets the IMU and writes the configuration (full scales, 240 Hz
   by default, FIFO and SFLP), then reads the registers back and compares them with the request.
2. The main loop updates the sample once per millisecond or when the data-ready interrupt fires. If the gyro
   data-ready bit is set it reads the gyro output; it then drains the FIFO, which carries the SFLP game-rotation
   quaternion and the SFLP gyro bias.
3. The sample counter increments for each valid quaternion. The sample age is measured against the older of the
   latest gyro read and latest quaternion.
4. Wire gyro value = raw gyro − SFLP bias, in counts of the configured full scale (the bias is always 4.375 mdps/LSB
   and is converted).
5. If initialisation fails the firmware retries every second and reports the fault in the status flags.

## Flash layout

| Range | Standalone image | With bootloader (`scripts/build-ota.sh`) |
|---|---|---|
| `0x08000000`–`0x08001FFF` | Application | Bootloader |
| `0x08002000`–`0x0800DFFF` | Application (linker region capped at 56 KiB) | Slots A and B, 24 KiB each |
| `0x0800E000`, `0x0800F000` | Two configuration pages | Two configuration pages |

Updating the application must not erase the configuration pages. Clearing read protection erases the whole
chip, including both pages. Details of the slots: [ota.md](ota.md).

## Build entries

| Entry | Toolchain | Notes |
|---|---|---|
| `build.ps1` | MounRiver Studio 2 WCH RISC-V GCC 8.2.0 | Windows. Verifies the SDK hashes, rebuilds everything. Reference build. |
| `scripts/build-xpack.sh` | xPack `riscv-none-elf-gcc` | macOS/Linux. Maps the WCH-specific interrupt attribute to the standard one. Standalone image. |
| `scripts/build-ota.sh` | xPack `riscv-none-elf-gcc` | Bootloader, both slots, update file and first-flash image. |
| CMake, `IMU_POWER_BUILD_CH32=ON` | RISC-V GCC with a C library | Optional MCU build; see the README. |
| CMake (default) | Host C99 compiler | Builds and runs the host tests. |

All entries use `-Os -march=rv32imac -mabi=ilp32` and the same source list; keep `build.ps1` and `CMakeLists.txt`
in step.

## Host tests

`ctest` runs eight executables: control table and maintenance sequence (`core`); IMU parameters, configuration
persistence and migration (`param`); DXL CRC, byte stuffing and parsing (`dxl_wire`, `dxl_stream`); circular-buffer
indexing (`dma_ring`); firmware update — slot selection, trial starts, protocol refusals and a power cut after every flash
operation (`ota`); IMU sample packing and scaling (`imu_sample`); and the soft-bus bridge — echo suppression,
mismatch recovery, 512-byte bursts and queue overflow (`softbus`).
