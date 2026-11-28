"""Run pinned Bosch/portable compensation C++20 in the existing Arm SDK's QEMU."""

import hashlib
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess

VENDOR_COMMIT = "3daf377ccaf589319c0d41af192105e9275987a2"
VENDOR_HASHES = {
    "bmm350.c": "7e9794658daf6bdd4cb781e38bc1dd99d7d7f32a80fc221ee52e011b8f92fb2a",
    "bmm350.h": "630011123d5842050ad75e350e9d18ae2f831113cd1907631ccbea40a8eec036",
    "bmm350_defs.h": "f4f936e559d24ed5576aea465b371eedb5ad4f9f02ca457c50ff53a3761245d2",
}


def main() -> None:
    root = Path(__file__).resolve().parents[3]
    source = Path(__file__).resolve().parent
    work = root / ".bmm350-oracle"
    work.mkdir(exist_ok=True)
    sdk = (
        Path(os.environ["LOCALAPPDATA"])
        / "ifx-z"
        / "toolchains"
        / "zephyr-sdk-1.0.1-arm-zephyr-eabi"
    )
    gcc = sdk / "gnu" / "arm-zephyr-eabi" / "bin" / "arm-zephyr-eabi-gcc.exe"
    gxx = gcc.with_name("arm-zephyr-eabi-g++.exe")
    qemu = sdk / "hosttools" / "qemu" / "qemu-system-arm.exe"
    for name, expected_hash in VENDOR_HASHES.items():
        path = work / name
        if not path.exists():
            subprocess.run(
                [
                    "curl.exe",
                    "--fail",
                    "--silent",
                    "--show-error",
                    "-L",
                    f"https://raw.githubusercontent.com/boschsensortec/BMM350_SensorAPI/{VENDOR_COMMIT}/{name}",
                    "-o",
                    str(path),
                ],
                check=True,
            )
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected_hash:
            raise RuntimeError(f"Pinned vendor digest mismatch: {name}")
    shutil.copyfile(source / "vendor_oracle.cpp.template", work / "oracle.cpp")
    flags = [
        "-mcpu=cortex-m3",
        "-mthumb",
        "-mfloat-abi=soft",
        "-O1",
        "-ffunction-sections",
        "-fdata-sections",
        "-fno-builtin",
        "-I",
        str(root),
        "-I",
        str(work),
    ]
    subprocess.run(
        [gcc, *flags, "-c", work / "bmm350.c", "-o", work / "vendor.o"], check=True
    )
    subprocess.run(
        [
            gxx,
            *flags,
            "-std=c++20",
            "-fno-exceptions",
            "-fno-rtti",
            "-c",
            work / "oracle.cpp",
            "-o",
            work / "oracle.o",
        ],
        check=True,
    )
    subprocess.run(
        [
            gxx,
            *flags,
            "-nostdlib",
            "-nostartfiles",
            "-Wl,--gc-sections",
            "-T",
            source / "vendor_oracle.ld",
            work / "vendor.o",
            work / "oracle.o",
            "-lgcc",
            "-o",
            work / "oracle.elf",
        ],
        check=True,
    )
    ran = subprocess.run(
        [
            qemu,
            "-M",
            "mps2-an385",
            "-nographic",
            "-monitor",
            "none",
            "-serial",
            "none",
            "-semihosting-config",
            "enable=on,target=native",
            "-kernel",
            work / "oracle.elf",
        ],
        capture_output=True,
        text=True,
        timeout=30,
        check=True,
    )
    vectors = []
    for line in (ran.stdout + ran.stderr).splitlines():
        if not line.startswith("VECTOR "):
            continue
        values = [
            struct.unpack("!f", bytes.fromhex(word))[0] for word in line.split()[1:]
        ]
        if len(values) != 8:
            raise RuntimeError(f"Malformed oracle output: {line}")
        expected, actual = values[:4], values[4:]
        if not all(
            math.isclose(a, b, rel_tol=2e-6, abs_tol=1e-5)
            for a, b in zip(expected, actual, strict=True)
        ):
            raise RuntimeError(f"Vendor differential failed: {line}")
        vectors.append(expected)
        print(f"{len(vectors):02d} vendor={expected} component={actual}")
    if len(vectors) != 12:
        raise RuntimeError(f"Expected 12 oracle vectors, got {len(vectors)}")
    print(f"PASS: 12 vendor/portable C++20 differential vectors, {VENDOR_COMMIT}")


if __name__ == "__main__":
    main()
