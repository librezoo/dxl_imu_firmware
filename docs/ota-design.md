# Proposal: firmware update over the DXL bus

**Status: design only; not implemented.** Goal: update the firmware through the existing 1 Mbps DXL bus (or the
soft bus) without opening the robot or attaching a WCH-Link, and without a failed update ever leaving the board
unrecoverable.

## Constraints

- CH32V203C8U6: 64 KiB flash, 20 KiB RAM. The application is currently about 16.6 KiB (limit 56 KiB).
- The last two 4 KiB pages hold the configuration and must keep their address and format.
- There is no bootloader today; the only way to program the chip is SWD.

## Layout (recommended: A/B slots)

| Address | Size | Content |
|---:|---:|---|
| `0x08000000` | 8 KiB | Bootloader, write-protected with the option-byte WRP; never changed by an update |
| `0x08002000` | 24 KiB | Slot A application |
| `0x08008000` | 24 KiB | Slot B application |
| `0x0800E000` | 8 KiB | Configuration (unchanged) |

Each slot is linked for its own address, so a release contains `app-A.bin` and `app-B.bin` built from the same
sources. A 64-byte header starts every image: magic, header version, slot, image size, image CRC-32, firmware
version, hardware ID, minimum bootloader version, header CRC-32. An invalid header means an empty slot.

If the application outgrows 24 KiB, use one 48 KiB slot with a resident bootloader instead: an interrupted update
then leaves the bootloader waiting for a new image, but there is no older version to fall back to.

## Boot flow

1. The bootloader reads the backup-register flags, validates both slots (header and CRC-32) and picks the
   confirmed slot, otherwise the newer valid one.
2. If an update was requested, or no slot is valid, it stays in update mode on the bus (same ID and baud rate).
3. Otherwise it stops its peripherals and jumps to the slot; the application sets up `mtvec` and the hardware
   interrupt stack itself, as it does today.

A new image is first started as *unconfirmed*. The application confirms it after the IMU initialises and the board
has answered one bus request. After three unconfirmed starts the bootloader returns to the other slot.

## Protocol

Dynamixel 2.0 frames with the existing CRC; no new instruction. A separate register window is used by both the
application (status only) and the bootloader.

| Address | Size | Access | Contents |
|---:|---:|---|---|
| `0x0300` | 32 | R | Mode (application/bootloader), versions, running slot, slot validity, next expected offset, last error, maximum chunk |
| `0x0320` | 1 | W | Command: unlock (`0xA5`, `0x5A`), `ENTER_BOOT`, `BEGIN`, `END`, `COMMIT`, `ABORT` |
| `0x0328` | 16 | W | `BEGIN` arguments: target slot, size, CRC-32, version |
| `0x0400` | 4 + N | W | Data chunk: offset, then at most 128 bytes |

Sequence: read the status; unlock and `ENTER_BOOT` (the application resets into the bootloader); `BEGIN` erases the
target slot page by page; write chunks (an out-of-order offset is refused with the expected offset, a repeated chunk
is harmless); `END` checks the CRC-32 and header; `COMMIT` marks the slot for a trial start and resets.

A 24 KiB image is about 200 frames, well under a second on the wire; flash erase and programming dominate, so an
update should take a few seconds.

## Safety

| Risk | Measure |
|---|---|
| Power loss during an update | The bootloader and the other slot are untouched; a half-written slot has no valid header |
| Corrupting the bootloader | Write protection on the first 8 KiB; the update code only accepts slot addresses |
| Transfer errors | DXL CRC-16 per frame, CRC-32 over the whole image |
| Wrong image for the board | Hardware ID and minimum bootloader version in the header |
| Accidental trigger | Two-step unlock, exact ID match, no broadcast |
| New image does not start | Unconfirmed-start counter with automatic fall-back |

Signatures are not planned: Ed25519 verification would take about 10 KiB of flash. The header reserves room for a
signature or MAC.

## Implementation and acceptance

1. A hardware-independent state machine (`src/ota.c`) with host tests: header validation, begin/write/end/commit,
   retransmission rules, trial-start counting, power-loss injection at every step.
2. The bootloader (USART3 and USART2, DXL parser, flash driver, CRC-32), target 7 KiB or less.
3. Linker scripts and build entries for both slots.
4. Application status registers and `ENTER_BOOT`; first deployment by SWD (bootloader plus slot A).
5. An update command in the host tool.
6. Bench acceptance: 20 updates from each starting state; 20 power cuts at each of: during transfer, before and after
   `END`, before and after `COMMIT` (each must still allow a new update); rejected images (bad CRC, wrong board, too
   large) must leave the old version running; an image that never confirms must fall back after three starts.
