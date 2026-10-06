# Command-line tool: `tools/dxl_softbus_tool.py`

A single-file Python tool for the soft bus: inspect the internal IMU, scan the DXL bus and inspect or reconfigure
Dynamixel servos (XL330-M288 class devices, model 1200) behind the board. For live viewing and IMU settings use
the `dxl_imu_host` web UI instead.

## Requirements

Python 3 (developed and tested with 3.14) and [pyserial](https://pypi.org/project/pyserial/) (`python -m pip install pyserial`).
`self-test` needs no hardware or pyserial.

The adapter must support 1 Mbps (an FTDI adapter does; the serial port of the WCH-LinkE stops at 921,600). Connect
it to the soft bus (PA2/PA3 and GND) of this firmware, or to PA9/PA10 of `dxl_debugger_firmware`, and close
every other program that has the port open.

## Commands

All commands take `--port` (for example `COM5` or `/dev/cu.usbserial-XXXX`), `--softbus-baud` (default
1,000,000), `--internal-id` (default 200; set it to the ID of the bridge board when it is not 200),
`--timeout` and `--color auto|always|never`.

| Command | Purpose |
|---|---|
| `check` | Read the internal IMU (`--samples N`, `--show-samples`), scan the bus and inspect the servos found: voltage, temperature, torque, hardware error. `--bauds` lists the real-bus baud rates to scan (default: the current one), `--ids` the ID range (default `0-253`). An empty scan is `FAIL`; `--allow-no-external` downgrades it to `WARN`. `--json-out FILE` writes the findings. |
| `scan` | Scan the bus for devices (`--bauds`, `--ids`, `--retries`, `--scan-timeout`). |
| `configure` | Change a servo's ID and/or baud rate: `--current-id`, `--current-baud`, `--new-id`, `--new-baud`. Without `--apply` it only prints the plan. It checks the model (`--expected-model`), ID conflicts, torque and hardware errors, reads the result back and restores the board's real-bus baud rate. It never re-enables torque or writes a goal position. |
| `self-test` | Run the protocol self-tests. |

Examples:

```bash
python tools/dxl_softbus_tool.py check --port /dev/cu.usbserial-XXXX --samples 20
python tools/dxl_softbus_tool.py scan --port COM5 --bauds 57600 1000000 --ids 1-20,30
python tools/dxl_softbus_tool.py configure --port COM5 --current-id 1 --current-baud 57600 \
    --new-id 2 --new-baud 1000000 --apply
```

## Results

Every command ends with a status and an exit code, and a plain last line for scripts:

| Status | Exit code | Meaning |
|---|---:|---|
| `PASS` | 0 | All checks passed |
| `WARN` | 1 | Finished with warnings: a device alert (status bit `0x80`), a sticky diagnostic bit, or an allowed empty scan |
| `FAIL` | 2 | A check failed, a device is in a dangerous state, or the command raised an error |

```text
RESULT command=check status=PASS pass=6 warn=0 fail=0 external=1
```

Diagnostic status bits 3–6 are sticky: if one is reported, reset the board and run `check` again to see whether
it recurs. Run serial tests with the WCH-Link detached and the board reset; an attached debugger changes the
target clock and shifts the UART rate by about 25 %.

## Other tools

[DYNAMIXEL Wizard 2.0](https://emanual.robotis.com/docs/en/software/dynamixel/dynamixel_wizard2/) works for
servos on the same port (Protocol 2.0, 1,000,000 baud). The internal IMU is not a servo: never run Firmware
Update or Factory Reset on it, and read its control table with a manual `READ` packet (address 124, length 12).
