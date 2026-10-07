#!/usr/bin/env python3
"""Pack the images of build-ota into the update bundle and the first-flash image.

    ota_pack.py --boot bootloader.bin --slot-a app-A.bin --slot-b app-B.bin --version 0x0210 --out-dir build-ota

Writes <prefix>-<version>.dxlfw (used by `mdimu ota update`) and initial.bin (bootloader, then slot A
with a confirmed header; flash it once over SWD at 0x08000000).

Bundle format (little endian): 16-byte header: magic "DXLU", format 1, hardware ID, firmware version,
minimum bootloader version, entry count, reserved; one 16-byte entry per slot: slot, 3 reserved bytes,
payload size, payload CRC-32, offset in the file; the payloads; a CRC-32 of everything before it.
Slot images are linked for their own address, so a bundle carries both.
"""
import argparse
import os
import struct
import sys
import zlib

BOOT_SIZE = 0x2000
SLOT_SIZE = 0x6000
HEADER_BLOCK = 0x400
PAYLOAD_MAX = SLOT_SIZE - HEADER_BLOCK
HWID = 0x4401  # default; --hwid for products that share the hardware
BOOT_VERSION = 0x0100
HEADER_MAGIC = 0x494C5844  # "DXLI"
BOOT_INFO_MAGIC = 0x424C5844  # "DXLB"


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def slot_header(slot, payload, version, seq, confirmed, hwid=HWID):
    """The 64-byte on-flash header; keep in step with include/imu_power/ota.h."""
    h = bytearray(b"\xff" * 64)
    struct.pack_into("<IHHIIHHIHH", h, 0, HEADER_MAGIC, 1, slot, len(payload), crc32(payload), version, hwid,
                     seq, BOOT_VERSION, 0)
    struct.pack_into("<I", h, 28, crc32(bytes(h[:28])))
    if confirmed:
        struct.pack_into("<H", h, 32, 0xA55A)  # committed
        struct.pack_into("<H", h, 40, 0x600D)  # confirmed
    return bytes(h)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--boot", required=True)
    ap.add_argument("--slot-a", required=True)
    ap.add_argument("--slot-b", required=True)
    ap.add_argument("--version", required=True, help="firmware version, for example 0x0210")
    ap.add_argument("--hwid", default=hex(HWID), help="hardware/product ID (default 0x4401)")
    ap.add_argument("--prefix", default="dxl_imu_firmware", help="file name prefix of the bundle")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--name", help="base name of the files (default dxl_imu_firmware-<version>)")
    args = ap.parse_args()
    version = int(args.version, 0)
    hwid = int(args.hwid, 0)
    boot = open(args.boot, "rb").read()
    if len(boot) > BOOT_SIZE:
        sys.exit("bootloader is %d bytes, the region holds %d" % (len(boot), BOOT_SIZE))
    info = struct.unpack_from("<IHH", boot, BOOT_SIZE - 16) if len(boot) == BOOT_SIZE else None
    if info != (BOOT_INFO_MAGIC, BOOT_VERSION, hwid):
        sys.exit("bootloader image lacks the version record at the end of its region: %r" % (info,))
    payloads = []
    for slot, path in ((0, args.slot_a), (1, args.slot_b)):
        data = open(path, "rb").read()
        if not 0 < len(data) <= PAYLOAD_MAX:
            sys.exit("%s: %d bytes, a slot takes 1..%d" % (path, len(data), PAYLOAD_MAX))
        if len(data) % 2:
            data += b"\xff"
        payloads.append(data)
    name = args.name or "%s-%04x" % (args.prefix, version)
    # Update bundle.
    body = bytearray(struct.pack("<4sHHHHHH", b"DXLU", 1, hwid, version, BOOT_VERSION, 2, 0))
    offset = 16 + 16 * len(payloads)
    for slot, data in enumerate(payloads):
        body += struct.pack("<B3xIII", slot, len(data), crc32(data), offset)
        offset += len(data)
    for data in payloads:
        body += data
    body += struct.pack("<I", crc32(bytes(body)))
    os.makedirs(args.out_dir, exist_ok=True)
    bundle = os.path.join(args.out_dir, name + ".dxlfw")
    open(bundle, "wb").write(body)
    # First flash: bootloader and a confirmed slot A.
    image = bytearray(b"\xff" * (BOOT_SIZE + HEADER_BLOCK))
    image[:len(boot)] = boot
    image[BOOT_SIZE:BOOT_SIZE + 64] = slot_header(0, payloads[0], version, 1, True, hwid)
    image += payloads[0]
    initial = os.path.join(args.out_dir, "initial.bin")
    open(initial, "wb").write(image)
    print("%s: %d bytes; %s: %d bytes; slot A %d B, slot B %d B (limit %d)" % (
        bundle, len(body), initial, len(image), len(payloads[0]), len(payloads[1]), PAYLOAD_MAX))


if __name__ == "__main__":
    main()
