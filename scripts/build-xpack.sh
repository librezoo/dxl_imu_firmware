#!/usr/bin/env bash
# Build the firmware with the xPack riscv-none-elf-gcc toolchain (macOS/Linux).
# Usage: scripts/build-xpack.sh [output-dir]      (default: build-xpack/)
# The toolchain is https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases (put its bin/ on PATH).
# This is not the reference toolchain (MounRiver's WCH GCC 8.2, see build.ps1); keep the source list and flags
# in step with build.ps1 and CMakeLists.txt.
set -euo pipefail
R="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$R/build-xpack}"
mkdir -p "$OUT"; cd "$OUT"; rm -f ./*.o
SRC=$R/vendor/wch-ch32v20x/EVT/EXAM/SRC
SYS=$R/vendor/wch-ch32v20x/EVT/EXAM/USART/USART_DMA/User
P=$SRC/Peripheral/src
# Same 56 KiB application region as the other build entries.
sed 's/LENGTH = 64K/LENGTH = 56K/' "$SRC/Ld/Link.ld" > imu_power_link.ld
A="-march=rv32imac_zicsr_zifencei -mabi=ilp32"
F="-Os $A -msmall-data-limit=8 -mno-save-restore -ffunction-sections -fdata-sections \
-include $R/scripts/wchcompat.h -DIMU_POWER_CH32 -DCH32V20x_D6 ${EXTRA_CFLAGS:-} \
-I$R/include -I$R/platform -I$SRC/Core -I$SRC/Debug -I$SRC/Peripheral/inc -I$SYS"
for f in "$SRC/Core/core_riscv.c" "$P"/ch32v20x_{gpio,rcc,spi,usart,dma,exti,flash,iwdg,misc}.c \
         "$SYS/system_ch32v20x.c" "$R/platform/ch32_main.c" "$R/platform/ch32v203_port.c" \
         "$SRC/Startup/startup_ch32v20x_D6.S" "$R"/src/{config,device,dxl,imu_wire,maintenance,softbus}.c; do
  riscv-none-elf-gcc $F -c "$f" -o "$(basename "$f").o"
done
riscv-none-elf-gcc $A -nostartfiles --specs=nano.specs --specs=nosys.specs -T imu_power_link.ld \
  -Wl,--gc-sections -Wl,-Map=imu_power_ch32.map ./*.o -o imu_power_ch32.elf
riscv-none-elf-objcopy -O binary imu_power_ch32.elf imu_power_ch32.bin
riscv-none-elf-objcopy -O ihex imu_power_ch32.elf imu_power_ch32.hex
riscv-none-elf-size imu_power_ch32.elf
