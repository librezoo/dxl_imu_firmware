# Firmware update over the bus

Firmware `0x0210` and later can be updated through the DXL bus (or the soft bus) without a debugger. A resident
bootloader keeps two application slots; an update is written to the slot that is not running, checked, started as a
trial and confirmed by the new application itself. A failed update leaves the old firmware running.

## Flash layout

| Address | Size | Content |
|---:|---:|---|
| `0x08000000` | 8 KiB | Bootloader (about 6 KiB used); version record in the last 16 bytes |
| `0x08002000` | 24 KiB | Slot A: 1 KiB header block, application from `0x08002400` |
| `0x08008000` | 24 KiB | Slot B: 1 KiB header block, application from `0x08008400` |
| `0x0800E000`, `0x0800F000` | 2 × 4 KiB | Configuration (unchanged; ID, baud rate and IMU settings survive updates) |

Both slots hold the same firmware linked for their own address; a release carries both. The application is about
18 KiB, the limit per slot is 23 KiB. The top 16 bytes of RAM (`0x20004FF0`) are reserved for the request that makes
the bootloader stay in update mode.

The bootloader is linked at the chip's alias address `0x00000000` (the CPU starts there and addresses RAM relative
to the program counter); applications are linked and started at `0x080xxxxx`. It runs in machine mode and hands over
with `mstatus` cleared so that the application's own reset code runs as after a reset.

## Slot header

64 bytes at the start of a slot, written by the bootloader after the image has been verified:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Magic `DXLI` |
| 4 / 6 | 2 / 2 | Header version (1) / slot (0 = A, 1 = B) |
| 8 / 12 | 4 / 4 | Payload size / payload CRC-32 |
| 16 / 18 | 2 / 2 | Firmware version / hardware ID (`0x4401`) |
| 20 | 4 | Sequence number: one more than the other slot's |
| 24 | 2 | Minimum bootloader version |
| 28 | 4 | CRC-32 of bytes 0–27 |
| 32 | 2 | Committed, `0xA55A` |
| 34, 36, 38 | 2 each | Trial starts, each programmed to `0x0000` when a trial start is used |
| 40 | 2 | Confirmed, `0x600D` |
| 42 | 2 | Bad, `0x0BAD` |

The state half-words are programmed one by one without erasing. Erased flash of this chip reads `0xE339`, not
`0xFFFF`, so a half-word counts only when it holds exactly its marker.

## Boot flow

1. If the application asked for an update (RAM request), stay in the bootloader.
2. Otherwise validate both slots (header and payload CRC-32) and choose one:
   - a committed slot that is not confirmed is a trial; it is started if it has used fewer than three trial starts
     (a start is recorded first), and marked bad otherwise;
   - else the confirmed slot with the higher sequence number.
3. Start the slot. If there is none, stay in the bootloader.

The application confirms itself once the IMU is initialised, a bus request has been answered and it has run for two
seconds. A new image that crashes or hangs is reset by the watchdog (about 4 s) and is marked bad after its third
start; the previous firmware starts again.

An update that is never finished does not strand the board: in update mode the bootloader restarts the boot flow
after 60 s without a request, as long as a slot can be started. A power cycle also clears the update request.

## Protocol

Dynamixel 2.0 frames on the board's ID and baud rate, USART3 (the DXL bus) and USART2 (the soft bus); a reply goes
out on the port the request came in on, after 1 ms. The bootloader implements `PING`, `READ`, `WRITE` and `RESET`;
broadcast and other IDs are ignored.

| Address | Size | Access | Contents |
|---:|---:|---|---|
| `0x0300` | 32 | R | Status block (application and bootloader) |
| `0x0320` | 1 | W | Command |
| `0x0328` | 16 | W | `BEGIN` arguments (bootloader) |
| `0x0400` | 4 + n | W | Data chunk: byte offset, then 1–128 bytes (bootloader) |

Status block: mode (1 application, 2 bootloader) 0–1; firmware version 2–3 (application); bootloader version 4–5;
running slot 6 (`0xFF` in the bootloader); state of slot A 7 and B 8 (0 empty, 1 verified, 2 trial, 3 confirmed,
4 bad); last error 9; hardware ID 10–11; maximum chunk 12–13; session 14 (0 idle, 1 receiving, 2 verified); next
expected offset 16–19; version of A 20–21 and B 22–23; sequence of A 24–27 and B 28–31.

Commands: `0xA5` then `0x5A` unlock (like the configuration unlock; all others need it), `0x10` enter bootloader
(application), `0x12` end of data, `0x13` commit, `0x14` abort, `0x15` leave update mode.

`BEGIN` arguments: slot (byte 0), reserved, firmware version (2–3), payload size (4–7), payload CRC-32 (8–11),
hardware ID (12–13), minimum bootloader version (14–15).

Sequence: read the status; unlock and `ENTER_BOOT` (the application answers, sets the RAM request and resets);
wait for mode 2; unlock; `BEGIN` (erases the first page of the target slot, which invalidates it); chunks in order
(a chunk that was already accepted is acknowledged again, a gap is refused with data-range error and the next
expected offset can be read from the status); `END` checks the whole image against the CRC-32 and writes the
header; `COMMIT` marks the slot as a trial and resets.

Image data is collected into 256-byte blocks and written with the chip's fast page programming; a 4 KiB page is
erased when its first block is written. A 18 KiB image takes about 2.5 s on the wire and the whole update, including
the three restarts and the confirmation, about 5 s.

Refusals set the error in the status block: 1 not unlocked, 2 bad slot, 3 size, 4 wrong hardware, 5 would leave no
slot that can start, 6 out of order, 7 flash, 8 CRC-32 mismatch, 9 wrong state, 10 arguments, 11 bootloader too old.

## Update file

`tools/ota_pack.py` writes `*.dxlfw`: a 16-byte header (`DXLU`, format 1, hardware ID, firmware version, minimum
bootloader version, entry count), one 16-byte entry per slot (slot, size, CRC-32, offset), the two payloads and a
CRC-32 of the file. `mdbus ota update` (see `mdbus`) picks the payload for the slot to write.

## Building and installing

```bash
scripts/build-ota.sh                    # build-ota/: bootloader.bin, app-A.bin, app-B.bin, *.dxlfw, initial.bin
FW_VERSION=0x0211 scripts/build-ota.sh out   # another version, for tests
wlink flash --address 0x08000000 build-ota/initial.bin   # once, over SWD: bootloader + slot A
wlink reset
mdbus ota update -p /dev/cu.usbserial-XXXX build-ota/dxl_imu_firmware-0210.dxlfw
```

`initial.bin` stops at the end of slot A and leaves the configuration pages alone. The script needs the xPack
toolchain (as `build-xpack.sh`) and Python 3. `build.ps1` and `build-xpack.sh` still build the standalone image
(no bootloader, 56 KiB) which reports slot `0xFF` and cannot be updated over the bus. `-DIMU_POWER_TEST_HANG`
builds an image that never feeds the watchdog (for testing the fall-back).

## Safety

| Risk | Measure |
|---|---|
| Power loss or reset during an update | The other slot and the bootloader are untouched; a half-written slot has no valid header and is never started. Host test: power cut after every flash operation of two complete updates |
| Corrupted transfer | DXL CRC-16 per frame, CRC-32 over the whole image before the header is written |
| Wrong image | Hardware ID and minimum bootloader version are checked on the board; slot, size and CRC-32 are in the file |
| Overwriting the only working slot | `BEGIN` refuses a target that is startable while the other slot is not |
| Accidental trigger | Two-step unlock, exact ID, no broadcast |
| New image does not work | Trial starts with automatic fall-back after three starts |
| Bootloader damaged by an update | The update code only erases and programs addresses inside a slot |

Not covered: images are not signed (a signature check would cost about 10 KiB, more than the bootloader's spare
space); the bootloader region is not write-protected with the option bytes;
`dxl_debugger_firmware` has its own build of the same bootloader (`-DIMU_POWER_BOOT_SOFTBUS_USART1`, serving USART1
instead of USART2) and the product ID `0x4402` (`-DIMU_POWER_OTA_HWID`), so update files of the two products refuse each other.
