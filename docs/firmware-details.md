# IMU Power firmware

Firmware core for the EasyEDA `imu_and_power / PCB1` board. The live netlist
identifies U4 as WCH `CH32V203C8U6` (LCSC `C6407762`) with an 8 MHz crystal.

The portable core implements the MicroDuck `imu_to_dxl v2` wire contract:

- Dynamixel Protocol 2.0, configurable ID and baud rate (defaults `200` and
  `1000000`)
- `PING`, `READ`, `WRITE`, `REG_WRITE`, `ACTION`, `RESET`, `SYNC_READ`, and
  `SYNC_WRITE`
- IMU data at address 124, 12 bytes
- two-slot CRC-protected configuration records
- diagnostic/control table and a DEBUG UART maintenance command parser

The `platform/` adapter is intentionally separate from the portable core. It
is the only layer that should include the WCH CH32V20x headers and provide SPI,
UART, timer, flash, GPIO and reset hooks.

## Build

The host test target uses CMake and a C99 compiler. The MCU target is built in
MounRiver Studio with the WCH CH32V20x standard peripheral library; copy the
same `src/` and `include/` files into that project and compile
`platform/ch32v203_port.c` with `IMU_POWER_CH32` defined.

The CH32 target references the official WCH EVT in place. It does not fetch or
vendor a copy of the SDK. With the package from this task:

```powershell
$env:WCH_EVT_ROOT = 'C:\Users\biao\Downloads\CH32V20xEVT\EVT'
$wch = 'C:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH'
$gcc = "$wch/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc.exe"
$make = 'C:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/others/Build_Tools/Make/bin/make.exe'
cmake -S . -B build-ch32 -G 'MinGW Makefiles' -DIMU_POWER_BUILD_CH32=ON `
  -DCMAKE_SYSTEM_NAME=Generic -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY `
  -DCMAKE_C_COMPILER="$gcc" -DCMAKE_ASM_COMPILER="$gcc" `
  -DCMAKE_MAKE_PROGRAM="$make"
cmake --build build-ch32 --target imu_power_ch32 -- -j2
```

The target uses `EXAM/SRC/Startup/startup_ch32v20x_D6.S`, the official
`EXAM/SRC/Peripheral` sources, and a generated copy of the official
`EXAM/SRC/Ld/Link.ld` with the application region capped at 56 KB. The final
two 4 KB pages of the 64 KB CH32V203C8 flash occupy `0x0800E000` and
`0x0800F000` for configuration slots.

The build produces `imu_power_ch32`, `imu_power_ch32.hex`, and
`imu_power_ch32.bin` in the build directory. The verified image uses 11,040
bytes of text and 4,264 bytes of BSS, leaving the configuration pages outside
the linker's 56 KB application region.

The portable test target exercises the CRC parser, status packet generation,
the MicroDuck `SYNC_READ(124,12)` contract, binary16 validation, and the
default configuration path. The hardware port samples the LSM6DSV16X gyro and
consumes tagged SFLP quaternion FIFO records; an absent or invalid SFLP record
keeps the quaternion block at zero until the sensor is ready.
