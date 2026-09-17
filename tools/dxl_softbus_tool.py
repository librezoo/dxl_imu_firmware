#!/usr/bin/env python3
"""Scan and safely reconfigure DYNAMIXEL devices through the DEBUG soft bus.

The DEBUG UART is always opened at 1 Mbps by default.  The script uses the
internal IMU node's maintenance registers to switch the real half-duplex bus,
then restores the board bus in a finally block.

Requires pyserial for hardware commands.  The self-test has no dependencies.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence

try:
    import serial  # type: ignore
except ImportError:  # self-test remains usable without pyserial
    serial = None


HEADER = b"\xff\xff\xfd\x00"
BROADCAST_ID = 0xFE
STATUS_INSTRUCTION = 0x55
PING = 0x01
READ = 0x02
WRITE = 0x03
RESET = 0x06  # Internal IMU firmware reset command.
MAX_PACKET = 512

INTERNAL_ID_DEFAULT = 200
INTERNAL_DIAG_ADDRESS = 160
INTERNAL_CONFIG_ADDRESS = 193
INTERNAL_COMMAND_ADDRESS = 197
CMD_SAVE = 0x01
CMD_UNLOCK = 0xA5
CMD_UNLOCK_CONFIRM = 0x5A

BOARD_BAUDS = (57_600, 115_200, 1_000_000, 2_000_000, 3_000_000)
BOARD_BAUD_ENUM = {baud: index for index, baud in enumerate(BOARD_BAUDS)}
SERVO_BAUD_REGISTER = {
    9_600: 0,
    57_600: 1,
    115_200: 2,
    1_000_000: 3,
    2_000_000: 4,
    3_000_000: 5,
    4_000_000: 6,
}


class ToolError(RuntimeError):
    pass


def crc16(data: bytes) -> int:
    crc = 0
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x8005 if crc & 0x8000 else 0)) & 0xFFFF
    return crc


def stuff(data: bytes) -> bytes:
    out = bytearray()
    for value in data:
        out.append(value)
        if out[-3:] == b"\xff\xff\xfd":
            out.append(0xFD)
    return bytes(out)


def unstuff(data: bytes) -> bytes:
    out = bytearray()
    index = 0
    while index < len(data):
        out.append(data[index])
        index += 1
        if out[-3:] == b"\xff\xff\xfd":
            if index >= len(data) or data[index] != 0xFD:
                raise ToolError("invalid DXL byte stuffing")
            index += 1
    return bytes(out)


def build_packet(device_id: int, instruction: int, parameters: bytes = b"") -> bytes:
    if not 0 <= device_id <= 0xFE:
        raise ValueError("device ID out of range")
    body = stuff(bytes((instruction,)) + parameters)
    length = len(body) + 2
    if length + 7 > MAX_PACKET:
        raise ValueError("packet too large")
    packet = HEADER + bytes((device_id,)) + struct.pack("<H", length) + body
    return packet + struct.pack("<H", crc16(packet))


@dataclass(frozen=True)
class StatusFrame:
    device_id: int
    error: int
    payload: bytes
    wire: bytes


def parse_status(packet: bytes) -> StatusFrame:
    if len(packet) < 11 or not packet.startswith(HEADER):
        raise ToolError("invalid status header or length")
    declared = struct.unpack_from("<H", packet, 5)[0]
    if declared + 7 != len(packet):
        raise ToolError("status length mismatch")
    received = struct.unpack_from("<H", packet, len(packet) - 2)[0]
    if received != crc16(packet[:-2]):
        raise ToolError("status CRC mismatch")
    body = unstuff(packet[7:-2])
    if len(body) < 2 or body[0] != STATUS_INSTRUCTION:
        raise ToolError("not a DXL status packet")
    return StatusFrame(packet[4], body[1], body[2:], packet)


class SoftBus:
    def __init__(self, port: str, debug_baud: int, timeout: float) -> None:
        if serial is None:
            raise ToolError("pyserial is required: python -m pip install pyserial")
        self._serial = serial.Serial(
            port=port,
            baudrate=debug_baud,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.002,
            write_timeout=max(timeout, 0.3),
            dsrdtr=False,
            rtscts=False,
            xonxoff=False,
        )
        self._serial.dtr = False
        self._serial.rts = False
        self._timeout = timeout
        self._buffer = bytearray()

    def close(self) -> None:
        self._serial.close()

    def __enter__(self) -> "SoftBus":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def clear(self) -> None:
        self._buffer.clear()
        self._serial.reset_input_buffer()

    def send(self, packet: bytes) -> None:
        self._serial.write(packet)
        self._serial.flush()

    def read_frame(self, timeout: float | None = None) -> StatusFrame | None:
        deadline = time.monotonic() + (self._timeout if timeout is None else timeout)
        while time.monotonic() < deadline:
            start = self._buffer.find(HEADER)
            if start > 0:
                del self._buffer[:start]
            elif start < 0 and len(self._buffer) > 3:
                del self._buffer[:-3]

            if len(self._buffer) >= 7 and self._buffer.startswith(HEADER):
                declared = struct.unpack_from("<H", self._buffer, 5)[0]
                total = declared + 7
                if declared < 3 or total > MAX_PACKET:
                    del self._buffer[0]
                    continue
                if len(self._buffer) >= total:
                    wire = bytes(self._buffer[:total])
                    del self._buffer[:total]
                    try:
                        return parse_status(wire)
                    except ToolError:
                        continue

            waiting = self._serial.in_waiting
            chunk = self._serial.read(waiting if waiting else 1)
            if chunk:
                self._buffer.extend(chunk)
        return None

    def transact(
        self,
        packet: bytes,
        expected_id: int,
        *,
        retries: int = 8,
        timeout: float | None = None,
        allow_error: bool = False,
    ) -> StatusFrame | None:
        for _ in range(retries):
            self.clear()
            self.send(packet)
            frame = self.read_frame(timeout)
            if frame is None or frame.device_id != expected_id:
                time.sleep(0.005)
                continue
            if frame.error and not allow_error:
                raise ToolError(
                    f"device {expected_id} returned error 0x{frame.error:02x}"
                )
            return frame
        return None

    def ping(self, device_id: int, *, retries: int = 3, timeout: float = 0.03) -> StatusFrame | None:
        return self.transact(
            build_packet(device_id, PING),
            device_id,
            retries=retries,
            timeout=timeout,
        )

    def read_register(
        self, device_id: int, address: int, length: int, *, retries: int = 15
    ) -> bytes:
        parameters = struct.pack("<HH", address, length)
        frame = self.transact(
            build_packet(device_id, READ, parameters),
            device_id,
            retries=retries,
            timeout=max(self._timeout, 0.07),
        )
        if frame is None or len(frame.payload) != length:
            raise ToolError(
                f"read failed: id={device_id} address={address} length={length}"
            )
        return frame.payload

    def write_register(
        self,
        device_id: int,
        address: int,
        data: bytes,
        *,
        retries: int = 15,
        require_ack: bool = True,
    ) -> StatusFrame | None:
        packet = build_packet(device_id, WRITE, struct.pack("<H", address) + data)
        if not require_ack:
            self.clear()
            self.send(packet)
            return self.read_frame(max(self._timeout, 0.1))
        frame = self.transact(
            packet,
            device_id,
            retries=retries,
            timeout=max(self._timeout, 0.1),
        )
        if frame is None:
            raise ToolError(
                f"write failed: id={device_id} address={address} length={len(data)}"
            )
        return frame


@dataclass(frozen=True)
class BoardDiagnostics:
    firmware: int
    protocol: int
    device_id: int
    baud: int
    status_flags: int
    sample_counter: int
    sample_age_us: int
    last_error: int


def read_board_diagnostics(bus: SoftBus, internal_id: int) -> BoardDiagnostics:
    data = bus.read_register(internal_id, INTERNAL_DIAG_ADDRESS, 24, retries=20)
    baud_enum = struct.unpack_from("<H", data, 4)[0]
    inverse = {value: baud for baud, value in BOARD_BAUD_ENUM.items()}
    if baud_enum not in inverse:
        raise ToolError(f"unknown board baud enum {baud_enum}")
    return BoardDiagnostics(
        firmware=struct.unpack_from("<H", data, 0)[0],
        protocol=data[2],
        device_id=data[3],
        baud=inverse[baud_enum],
        status_flags=struct.unpack_from("<H", data, 6)[0],
        sample_counter=struct.unpack_from("<I", data, 8)[0],
        sample_age_us=struct.unpack_from("<I", data, 12)[0],
        last_error=struct.unpack_from("<H", data, 16)[0],
    )


def unlock_board_maintenance(bus: SoftBus, internal_id: int) -> None:
    for _ in range(8):
        try:
            bus.write_register(
                internal_id,
                INTERNAL_COMMAND_ADDRESS,
                bytes((CMD_UNLOCK,)),
                retries=2,
            )
        except ToolError:
            pass
        try:
            bus.write_register(
                internal_id,
                INTERNAL_COMMAND_ADDRESS,
                bytes((CMD_UNLOCK_CONFIRM,)),
                retries=2,
            )
            return
        except ToolError:
            continue
    raise ToolError("could not unlock internal IMU maintenance")


def set_board_bus_baud(bus: SoftBus, internal_id: int, baud: int) -> None:
    if baud not in BOARD_BAUD_ENUM:
        raise ToolError(f"board firmware does not allow baud {baud}")
    current = read_board_diagnostics(bus, internal_id)
    if current.baud == baud:
        return
    unlock_board_maintenance(bus, internal_id)
    bus.write_register(
        internal_id,
        INTERNAL_CONFIG_ADDRESS,
        struct.pack("<I", baud),
        retries=20,
    )
    # Flash save may finish after the ordinary status window.  Retrying the
    # same value is safe; diagnostics after reboot are authoritative.
    bus.write_register(
        internal_id,
        INTERNAL_COMMAND_ADDRESS,
        bytes((CMD_SAVE,)),
        retries=20,
    )
    bus.clear()
    bus.send(build_packet(internal_id, RESET))
    bus.read_frame(0.1)
    time.sleep(0.5)
    for _ in range(20):
        try:
            if read_board_diagnostics(bus, internal_id).baud == baud:
                return
        except ToolError:
            pass
        time.sleep(0.05)
    raise ToolError(f"board real-bus baud did not change to {baud}")


@dataclass(frozen=True)
class FoundDevice:
    baud: int
    device_id: int
    model: int | None
    firmware: int | None


def scan_at_baud(
    bus: SoftBus,
    baud: int,
    ids: Iterable[int],
    internal_id: int,
    retries: int,
    timeout: float,
) -> list[FoundDevice]:
    found: list[FoundDevice] = []
    for device_id in ids:
        if device_id == internal_id:
            continue
        frame = bus.ping(device_id, retries=retries, timeout=timeout)
        if frame is None:
            continue
        model = struct.unpack_from("<H", frame.payload, 0)[0] if len(frame.payload) >= 2 else None
        firmware = frame.payload[2] if len(frame.payload) >= 3 else None
        found.append(FoundDevice(baud, device_id, model, firmware))
    return found


def parse_int(value: str) -> int:
    return int(value, 0)


def parse_id_spec(value: str) -> list[int]:
    result: set[int] = set()
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        if "-" in item:
            start_text, end_text = item.split("-", 1)
            start, end = parse_int(start_text), parse_int(end_text)
            if end < start:
                raise argparse.ArgumentTypeError(f"invalid ID range {item}")
            result.update(range(start, end + 1))
        else:
            result.add(parse_int(item))
    if not result or min(result) < 0 or max(result) > 253:
        raise argparse.ArgumentTypeError("IDs must be in 0..253")
    return sorted(result)


def servo_baud_register(baud: int) -> int:
    try:
        return SERVO_BAUD_REGISTER[baud]
    except KeyError as exc:
        raise ToolError(f"unsupported servo baud {baud}") from exc


def read_servo_state(bus: SoftBus, device_id: int) -> dict[str, int]:
    return {
        "model": struct.unpack("<H", bus.read_register(device_id, 0, 2))[0],
        "firmware": bus.read_register(device_id, 6, 1)[0],
        "id": bus.read_register(device_id, 7, 1)[0],
        "baud_register": bus.read_register(device_id, 8, 1)[0],
        "torque": bus.read_register(device_id, 64, 1)[0],
        "hardware_error": bus.read_register(device_id, 70, 1)[0],
    }


def configure_servo(
    bus: SoftBus,
    *,
    internal_id: int,
    current_id: int,
    current_baud: int,
    new_id: int | None,
    new_baud: int | None,
    expected_model: int,
) -> tuple[dict[str, int], dict[str, int]]:
    target_id = current_id if new_id is None else new_id
    target_baud = current_baud if new_baud is None else new_baud
    if not 0 <= target_id <= 252 or target_id == internal_id:
        raise ToolError(f"invalid or conflicting target ID {target_id}")
    target_baud_register = servo_baud_register(target_baud)

    set_board_bus_baud(bus, internal_id, current_baud)
    before = read_servo_state(bus, current_id)
    if (
        before["model"] != expected_model
        or before["id"] != current_id
        or before["baud_register"] != servo_baud_register(current_baud)
        or before["hardware_error"] != 0
    ):
        raise ToolError(f"servo preflight failed: {before}")

    if target_id != current_id and bus.ping(target_id, retries=3, timeout=0.03):
        raise ToolError(f"target ID {target_id} already responds at {current_baud}")

    if before["torque"] != 0:
        bus.write_register(current_id, 64, b"\x00")
        if bus.read_register(current_id, 64, 1) != b"\x00":
            raise ToolError("could not disable torque before EEPROM write")

    active_id = current_id
    if target_id != current_id:
        # ID may change before its write ACK is returned.
        bus.write_register(current_id, 7, bytes((target_id,)), require_ack=False)
        time.sleep(0.05)
        if bus.read_register(target_id, 7, 1) != bytes((target_id,)):
            raise ToolError(f"new ID {target_id} did not verify at {current_baud}")
        active_id = target_id

    if target_baud != current_baud:
        # Baud can switch before the old-baud ACK is returned.
        bus.write_register(
            active_id,
            8,
            bytes((target_baud_register,)),
            require_ack=False,
        )
        time.sleep(0.1)
        set_board_bus_baud(bus, internal_id, target_baud)

    after = read_servo_state(bus, active_id)
    if (
        after["model"] != expected_model
        or after["id"] != target_id
        or after["baud_register"] != target_baud_register
        or after["torque"] != 0
        or after["hardware_error"] != 0
    ):
        raise ToolError(f"servo post-write verification failed: {after}")
    return before, after


def command_scan(args: argparse.Namespace) -> int:
    bauds = args.bauds or list(BOARD_BAUDS)
    original_baud: int | None = None
    found: list[FoundDevice] = []
    with SoftBus(args.port, args.debug_baud, args.timeout) as bus:
        original_baud = read_board_diagnostics(bus, args.internal_id).baud
        try:
            for baud in bauds:
                set_board_bus_baud(bus, args.internal_id, baud)
                print(f"BUS_BAUD={baud} VERIFIED", flush=True)
                devices = scan_at_baud(
                    bus,
                    baud,
                    args.ids,
                    args.internal_id,
                    args.retries,
                    args.scan_timeout,
                )
                found.extend(devices)
                if devices:
                    for device in devices:
                        print(
                            f"FOUND baud={device.baud} id={device.device_id} "
                            f"model={device.model} firmware={device.firmware}",
                            flush=True,
                        )
                else:
                    print(f"BUS_BAUD={baud} NO_EXTERNAL_RESPONDERS", flush=True)
        finally:
            if original_baud is not None:
                set_board_bus_baud(bus, args.internal_id, original_baud)
                print(f"FINAL_BUS_BAUD={original_baud} RESTORED", flush=True)
    if args.json_out:
        Path(args.json_out).write_text(
            json.dumps([asdict(item) for item in found], indent=2),
            encoding="utf-8",
        )
    print(f"TOTAL_EXTERNAL_RESPONDERS={len(found)}")
    return 0


def command_configure(args: argparse.Namespace) -> int:
    if args.new_id is None and args.new_baud is None:
        raise ToolError("configure requires --new-id and/or --new-baud")
    plan = {
        "port": args.port,
        "debug_baud": args.debug_baud,
        "current_id": args.current_id,
        "current_baud": args.current_baud,
        "new_id": args.current_id if args.new_id is None else args.new_id,
        "new_baud": args.current_baud if args.new_baud is None else args.new_baud,
        "expected_model": args.expected_model,
    }
    if not args.apply:
        print(json.dumps({"dry_run": True, "plan": plan}, indent=2))
        print("Re-run with --apply to write the servo EEPROM.")
        return 0

    original_board_baud: int | None = None
    success = False
    with SoftBus(args.port, args.debug_baud, args.timeout) as bus:
        original_board_baud = read_board_diagnostics(bus, args.internal_id).baud
        try:
            before, after = configure_servo(
                bus,
                internal_id=args.internal_id,
                current_id=args.current_id,
                current_baud=args.current_baud,
                new_id=args.new_id,
                new_baud=args.new_baud,
                expected_model=args.expected_model,
            )
            success = True
            print("BEFORE " + json.dumps(before, sort_keys=True))
            print("AFTER  " + json.dumps(after, sort_keys=True))
            print("CONFIGURATION_OK")
        finally:
            if original_board_baud is not None:
                final_baud = (
                    plan["new_baud"]
                    if success and args.final_board_baud == "target"
                    else original_board_baud
                )
                set_board_bus_baud(bus, args.internal_id, int(final_baud))
                print(f"FINAL_BUS_BAUD={final_baud} VERIFIED")
    return 0


def command_self_test(_: argparse.Namespace) -> int:
    ping = bytes.fromhex("ff ff fd 00 01 03 00 01 19 4e")
    assert crc16(ping[:-2]) == 0x4E19
    assert build_packet(1, PING) == ping
    status = bytes.fromhex("ff ff fd 00 01 07 00 55 00 06 04 26 65 5d")
    parsed = parse_status(status)
    assert parsed.device_id == 1 and parsed.error == 0
    assert parsed.payload == bytes((6, 4, 0x26))
    payload = b"\xff\xff\xfd\x00\xff\xff\xfd"
    assert unstuff(stuff(payload)) == payload
    assert servo_baud_register(57_600) == 1
    assert servo_baud_register(1_000_000) == 3
    assert parse_id_spec("1,2,10-12") == [1, 2, 10, 11, 12]
    print("SELF_TEST_OK")
    return 0


def add_common_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--port",
        required=True,
        help="DEBUG UART port, for example COM5 or /dev/ttyUSB0",
    )
    parser.add_argument("--debug-baud", type=parse_int, default=1_000_000)
    parser.add_argument("--internal-id", type=parse_int, default=INTERNAL_ID_DEFAULT)
    parser.add_argument("--timeout", type=float, default=0.08)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    scan = subparsers.add_parser("scan", help="scan external DXL devices")
    add_common_arguments(scan)
    scan.add_argument("--bauds", type=parse_int, nargs="*", choices=BOARD_BAUDS)
    scan.add_argument("--ids", type=parse_id_spec, default=parse_id_spec("0-253"))
    scan.add_argument("--retries", type=int, default=3)
    scan.add_argument("--scan-timeout", type=float, default=0.025)
    scan.add_argument("--json-out")
    scan.set_defaults(handler=command_scan)

    configure = subparsers.add_parser("configure", help="change servo ID/baud")
    add_common_arguments(configure)
    configure.add_argument("--current-id", type=parse_int, required=True)
    configure.add_argument(
        "--current-baud", type=parse_int, required=True, choices=BOARD_BAUDS
    )
    configure.add_argument("--new-id", type=parse_int)
    configure.add_argument("--new-baud", type=parse_int, choices=BOARD_BAUDS)
    configure.add_argument("--expected-model", type=parse_int, default=1200)
    configure.add_argument(
        "--final-board-baud",
        choices=("original", "target"),
        default="original",
        help="board real-bus baud after a successful change",
    )
    configure.add_argument(
        "--apply",
        action="store_true",
        help="perform EEPROM writes; omission is a dry run",
    )
    configure.set_defaults(handler=command_configure)

    self_test = subparsers.add_parser("self-test", help="run protocol self-tests")
    self_test.set_defaults(handler=command_self_test)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.handler(args))
    except (ToolError, OSError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
