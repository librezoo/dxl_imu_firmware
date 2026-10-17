#!/usr/bin/env bash
# Build the bootloader, both application slots and the update files with the xPack riscv-none-elf-gcc.
# Usage: scripts/build-ota.sh [output-dir]      (default: build-ota/)
# Environment: FW_VERSION (for example 0x0211, default: include/imu_power/config.h), EXTRA_CFLAGS.
# Outputs: bootloader.bin, app-A.bin, app-B.bin, dxl_imu_firmware-<version>.dxlfw (update bundle for
# `mdbus ota update`), and initial.bin (bootloader + slot A, flashed once over SWD at 0x08000000).
set -euo pipefail
R="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$R/build-ota}"
mkdir -p "$OUT"; cd "$OUT"; rm -f ./*.o
FWV="${FW_VERSION:-$(grep -o 'IMU_POWER_FW_VERSION 0x[0-9A-Fa-f]*' "$R/include/imu_power/config.h" | cut -d' ' -f2)}"
SRC=$R/vendor/wch-ch32v20x/EVT/EXAM/SRC
SYS=$R/vendor/wch-ch32v20x/EVT/EXAM/USART/USART_DMA/User
P=$SRC/Peripheral/src
A="-march=rv32imac_zicsr_zifencei -mabi=ilp32"
F="-Os $A -msmall-data-limit=8 -mno-save-restore -ffunction-sections -fdata-sections \
-include $R/scripts/wchcompat.h -DCH32V20x_D6 -DIMU_POWER_FW_VERSION=${FWV}u ${EXTRA_CFLAGS:-} \
-I$R/include -I$R/platform -I$SRC/Core -I$SRC/Debug -I$SRC/Peripheral/inc -I$SYS"
L="$A -nostartfiles --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections"

# Bootloader (first 8 KiB).
mkdir -p boot; rm -f boot/*.o
for f in "$SRC/Core/core_riscv.c" "$P/ch32v20x_flash.c" "$SYS/system_ch32v20x.c" \
         "$SRC/Startup/startup_ch32v20x_D6.S" "$R/bootloader/boot_reset.S" "$R/bootloader/boot_main.c" "$R"/src/{config,dxl,ota}.c; do
  riscv-none-elf-gcc $F -c "$f" -o "boot/$(basename "$f").o"
done
riscv-none-elf-gcc $L -T "$R/bootloader/boot.ld" -Wl,-Map=bootloader.map boot/*.o -o bootloader.elf
riscv-none-elf-objcopy -O binary bootloader.elf bootloader.bin
riscv-none-elf-size bootloader.elf

# Application: compiled once, linked for each slot (the header block takes the first 1 KiB of a slot).
mkdir -p app; rm -f app/*.o
for f in "$SRC/Core/core_riscv.c" "$P"/ch32v20x_{gpio,rcc,spi,usart,dma,exti,flash,iwdg,misc}.c \
         "$SYS/system_ch32v20x.c" "$R/platform/ch32_main.c" "$R/platform/ch32v203_port.c" \
         "$SRC/Startup/startup_ch32v20x_D6.S" "$R"/src/{config,device,dxl,imu_wire,maintenance,ota,softbus}.c; do
  riscv-none-elf-gcc $F -DIMU_POWER_CH32 -DIMU_POWER_OTA -c "$f" -o "app/$(basename "$f").o"
done
for slot in A B; do
  if [ "$slot" = A ]; then origin=0x08002400; else origin=0x08008400; fi
  python3 - "$SRC/Ld/Link.ld" "$origin" > "link-$slot.ld" <<'PY'
import sys
text, origin = open(sys.argv[1]).read(), sys.argv[2]
for old, new in (("ORIGIN = 0x00000000, LENGTH = 64K", "ORIGIN = %s, LENGTH = 23K" % origin),
                 ("ORIGIN = 0x20000000, LENGTH = 20K", "ORIGIN = 0x20000000, LENGTH = 20K - 16"),
                 ("SECTIONS\n{\n", "SECTIONS\n{\n\tPROVIDE( __slot_base = ORIGIN(FLASH) - 0x400 );\n")):
    assert text.count(old) == 1, old
    text = text.replace(old, new)
sys.stdout.write(text)
PY
  riscv-none-elf-gcc $L -T "link-$slot.ld" -Wl,-Map="app-$slot.map" app/*.o -o "app-$slot.elf"
  riscv-none-elf-objcopy -O binary "app-$slot.elf" "app-$slot.bin"
  riscv-none-elf-size "app-$slot.elf"
done

python3 "$R/tools/ota_pack.py" --boot bootloader.bin --slot-a app-A.bin --slot-b app-B.bin \
  --version "$FWV" --out-dir "$OUT"
