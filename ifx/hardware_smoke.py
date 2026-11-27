"""Flash and smoke-test the PSoC 6 core fixture on a KitProg3 bench."""

import argparse
from pathlib import Path
import re
import subprocess
from tempfile import gettempdir
from time import monotonic

from filelock import FileLock, Timeout
import serial
from serial.tools import list_ports

_BOOT = re.compile(r"PSoC 6 smoke boot (\d+)")
_HEARTBEAT = "PSoC 6 smoke heartbeat"
_BOOT_TIMEOUT = 15
_STABLE_SECONDS = 8


def _read_boot(port: serial.Serial) -> int:
    deadline = monotonic() + _BOOT_TIMEOUT
    boot = None
    heartbeat = None
    while monotonic() < deadline:
        line = port.readline().decode("utf-8", errors="replace").strip()
        if line:
            print(line, flush=True)
        if match := _BOOT.search(line):
            if boot is not None:
                raise RuntimeError("Unexpected second boot during the stability window")
            boot = int(match[1])
            deadline = monotonic() + _STABLE_SECONDS
        elif boot is not None and _HEARTBEAT in line:
            heartbeat = monotonic()
    if boot is None:
        raise RuntimeError("No ESPHome boot marker received from KitProg3")
    if heartbeat is None or heartbeat < deadline - 3:
        raise RuntimeError("No ESPHome heartbeat near the end of the stability window")
    return boot


def _test(args: argparse.Namespace) -> None:
    probe = next((p for p in list_ports.comports() if p.device == args.port), None)
    if probe is None or probe.vid != 0x04B4 or not probe.serial_number:
        raise RuntimeError(f"No identifiable Cypress KitProg3 on {args.port}")
    if not probe.serial_number.isalnum():
        raise RuntimeError("Unexpected characters in KitProg3 serial number")

    image = args.image.resolve(strict=True)
    if any(char in str(image) for char in "{}"):
        raise RuntimeError("Image path must not contain braces")
    openocd = args.openocd.resolve(strict=True)
    command = [
        str(openocd),
        "-s",
        str(openocd.parent.parent / "scripts"),
        "-f",
        "interface/cmsis-dap.cfg",
        "-c",
        "transport select swd",
        "-c",
        f"adapter serial {probe.serial_number}",
        "-f",
        "target/psoc6_2m.cfg",
        "-c",
        "gdb_port disabled; tcl_port disabled; telnet_port disabled",
    ]
    with serial.Serial(args.port, 115200, timeout=0.25) as console:
        subprocess.run(
            [*command, "-c", f"program {{{image.as_posix()}}} verify reset exit"],
            check=True,
            timeout=120,
        )
        boots = [_read_boot(console)]
        console.reset_input_buffer()
        subprocess.run(
            [*command, "-c", "init; reset run; shutdown"],
            check=True,
            timeout=30,
        )
        boots.append(_read_boot(console))
    if boots[1] != boots[0] + 1:
        raise RuntimeError(f"Boot counter did not persist across reset: {boots}")
    print(f"PASS: stable boot and preferences persistence ({boots[0]} -> {boots[1]})")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--openocd", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--port", required=True, help="KitProg3 serial port")
    args = parser.parse_args()
    lock = Path(gettempdir()) / "ifxhome-cy8ckit-062s2-ai.lock"
    try:
        with FileLock(lock, timeout=0, fallback_to_soft=False):
            _test(args)
    except (OSError, RuntimeError, subprocess.SubprocessError, Timeout) as err:
        parser.exit(1, f"Hardware smoke test failed: {err}\n")


if __name__ == "__main__":
    main()
