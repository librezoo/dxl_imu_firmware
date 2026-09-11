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

Install the official [MounRiver Studio 2](https://www.mounriver.com/download)
with the WCH **RISC-V Embedded GCC** toolchain. The firmware sources, SDK
headers, peripheral drivers, startup code and system initialization files
are all included in this repository under `vendor/wch-ch32v20x/EVT/`.
There is no external SDK checkout, submodule, download or `WCH_EVT_ROOT`
configuration step. See the [SDK provenance](../vendor/wch-ch32v20x/README.md).

### Windows MCU build

From the repository root, run the supplied script using Windows PowerShell
5.1 or later. It uses the official IDE's compiler directly, so CMake, Make
and Python are not required for this entry point:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
# Override the IDE installation directory when necessary:
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -MounRiverRoot 'D:\Tools\MounRiver_Studio2'
```

With the default IDE installation, the compiler is:

```text
C:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC\bin\riscv-none-embed-gcc.exe
```

The same directory supplies `riscv-none-embed-objcopy.exe` and
`riscv-none-embed-size.exe`. The validated compiler is the IDE's xPack GNU
RISC-V Embedded GCC 8.2.0. The script verifies the bundled SDK hashes and
rebuilds all firmware sources on each invocation. `-BuildDirectory` can
select an output directory; relative paths are resolved from the script's
repository, independently of the caller's working directory.

### CMake MCU build (optional)

If CMake 3.16+ is already installed and on `PATH`, the same target can be
built with CMake and the official IDE's Make executable:

```powershell
$ideRoot = 'C:/MounRiver/MounRiver_Studio2'
$tools = "$ideRoot/resources/app/resources/win32"
$bin = "$tools/components/WCH/Toolchain/RISC-V Embedded GCC/bin"
$gcc = "$bin/riscv-none-embed-gcc.exe"
$objcopy = "$bin/riscv-none-embed-objcopy.exe"
$make = "$tools/others/Build_Tools/Make/bin/make.exe"
cmake -S . -B build-ch32-cmake -G 'MinGW Makefiles' -DIMU_POWER_BUILD_CH32=ON `
  -DCMAKE_SYSTEM_NAME=Generic -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY `
  -DCMAKE_C_COMPILER="$gcc" -DCMAKE_ASM_COMPILER="$gcc" `
  -DCMAKE_OBJCOPY="$objcopy" -DCMAKE_MAKE_PROGRAM="$make"
cmake --build build-ch32-cmake --target imu_power_ch32 --parallel 2
```

### Firmware layout and host tests

Both MCU build entry points use `CH32V20x_D6`, `IMU_POWER_CH32`, C99,
`-march=rv32imac`, `-mabi=ilp32` and size optimization (`-Os`). The target
uses `EXAM/SRC/Startup/startup_ch32v20x_D6.S`, the official
`EXAM/SRC/Peripheral` sources, and a generated copy of the official
`EXAM/SRC/Ld/Link.ld` with the application region capped at 56 KB. The final
two 4 KB pages of the 64 KB CH32V203C8 flash occupy `0x0800E000` and
`0x0800F000` for configuration slots.

The PowerShell build produces `imu_power_ch32.elf`, `imu_power_ch32.hex`,
`imu_power_ch32.bin` and `imu_power_ch32.map` in `build-ch32/`. The CMake
target produces the same image formats in `build-ch32-cmake/`, with the ELF
executable named `imu_power_ch32` without a suffix. Both keep the configuration
pages outside the linker's 56 KB application region. Build outputs are ignored
by Git. Current validation results are recorded in [validation.md](validation.md).

Host tests additionally require CMake 3.16+ and a host C99 compiler; their
commands are in the root README. They do not use the MCU toolchain.

The portable test target exercises the CRC parser, status packet generation,
the MicroDuck `SYNC_READ(124,12)` contract, binary16 validation, and the
default configuration path. The hardware port samples the LSM6DSV16X gyro and
consumes tagged SFLP quaternion FIFO records; an absent or invalid SFLP record
keeps the quaternion block at zero until the sensor is ready.
