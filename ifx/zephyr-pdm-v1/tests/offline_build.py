"""Reproduce offline PDM tests/fixture without installers or shared-cache writes."""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
sys.dont_write_bytecode = True


def run(command: list[str], env: dict[str, str], log: Path) -> None:
    result = subprocess.run(
        command,
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
    )
    output = result.stdout + result.stderr
    log.write_text(output, encoding="utf-8")
    print("\n".join(output.splitlines()[-20:]))
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); see {log}")


def stop_qemu(process: subprocess.Popen, env: dict[str, str]) -> None:
    if process.poll() is None:
        subprocess.run(
            [
                "powershell.exe",
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                f"Stop-Process -Id {process.pid} -ErrorAction Stop",
            ],
            cwd=ROOT,
            env=env,
            capture_output=True,
            check=True,
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache", type=Path, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--tests", action="store_true")
    mode.add_argument("--bench", action="store_true")
    args = parser.parse_args()
    cache = args.cache.resolve()
    sdk = cache / "toolchains" / "zephyr-sdk-1.0.1-arm-zephyr-eabi"
    python = cache / "penvs" / "v4.5.0" / "Scripts" / "python.exe"
    modules = (
        cache
        / "frameworks"
        / "zephyr-v4.5.0-ifx-psoc6-v4.5.0-core.1-59410a9a"
        / "modules"
        / "hal"
    )
    zephyr = ROOT / ".pdm-workspace" / "zephyr"
    for path in (sdk, python, modules / "cmsis_6", modules / "infineon", zephyr):
        if not path.exists():
            raise RuntimeError(
                f"Missing preinstalled input: {path}; installers are forbidden"
            )
    output = ROOT / ".pdm-workspace"
    scratch = output / "scratch"
    scratch.mkdir(exist_ok=True)
    tempfile.tempdir = str(scratch)
    env = {
        **os.environ,
        "PYTHONUTF8": "1",
        "PYTHONDONTWRITEBYTECODE": "1",
        "GIT_OPTIONAL_LOCKS": "0",
        "TMP": str(scratch),
        "TEMP": str(scratch),
        "ZEPHYR_BASE": str(zephyr),
        "ZEPHYR_SDK_INSTALL_DIR": str(sdk),
        "ZEPHYR_TOOLCHAIN_VARIANT": "zephyr",
        "PATH": str(python.parent) + os.pathsep + os.environ["PATH"],
    }
    source = ROOT / "ifx" / "zephyr-pdm-v1" / "tests"
    module_paths = [modules / "cmsis_6"]
    board = "qemu_cortex_m3"
    build = output / "qemu-tests"
    if args.bench:
        import esphome.__main__ as cli  # noqa: PLC0415
        from esphome.components.zephyr import framework  # noqa: PLC0415
        from esphome.components.zephyr.build_zephyr import generate_cmake_lists  # noqa: PLC0415
        from esphome.core import CORE  # noqa: PLC0415

        os.environ.update(env)
        os.environ["ESPHOME_ZEPHYR_PREFIX"] = str(cache)
        os.environ["ESPHOME_BUILD_PATH"] = str(output / "firmware")
        framework.check_and_install = lambda *a, **k: (python, zephyr.parent, env)
        framework.install_toolchain = lambda *a, **k: sdk
        sys.argv = [
            "esphome",
            "compile",
            str(ROOT / "ifx" / "pdm-stereo-bench.yaml"),
            "--only-generate",
        ]
        gitignore = ROOT / "ifx" / ".gitignore"
        existed = gitignore.exists()
        try:
            if cli.main() != 0:
                raise RuntimeError("Fixture generation failed")
            if CORE.platformio_libraries:
                raise RuntimeError("External library downloads are forbidden")
            generate_cmake_lists("Zephyr", False)
            cmake = CORE.relative_build_path("zephyr", "CMakeLists.txt")
            text = cmake.read_text(encoding="utf-8").replace(
                "find_package(Zephyr REQUIRED)",
                "foreach(path Python3_EXECUTABLE ZEPHYR_MODULES USER_CACHE_DIR)\n"
                '  file(TO_CMAKE_PATH "${${path}}" ${path})\n'
                "endforeach()\nfind_package(Zephyr REQUIRED)",
            )
            cmake.write_text(text, encoding="utf-8")
            source = CORE.relative_build_path("zephyr")
        finally:
            if not existed and gitignore.exists():
                gitignore.unlink()
        board = "cy8ckit_062s2_ai"
        module_paths.append(modules / "infineon")
        build = output / "board-build"
    run(
        [
            "cmake",
            "-G",
            "Ninja",
            "-B",
            str(build),
            "-S",
            str(source),
            f"-DBOARD={board}",
            f"-DPython3_EXECUTABLE={python}",
            "-DZEPHYR_MODULES=" + ";".join(map(str, module_paths)),
            f"-DUSER_CACHE_DIR={output / 'user-cache'}",
        ],
        env,
        output / f"{board}-configure.log",
    )
    run(["cmake", "--build", str(build), "-j", "8"], env, output / f"{board}-build.log")
    if args.bench:
        dts = (build / "zephyr" / "zephyr.dts").read_text(encoding="utf-8")
        for label, pinmux in (
            ("p10_4_audioss0_pdm_clk", "0x15040a"),
            ("p10_5_audioss0_pdm_data", "0x15050a"),
        ):
            node = dts.split(f"{label}:", 1)[1].split("};", 1)[0]
            if pinmux not in node:
                raise RuntimeError(f"Wrong pinmux for {label}")
        firmware = build / "zephyr" / "zephyr.hex"
        print("PINMUX verified: 0x15040a / 0x15050a")
        print("HEX SHA256:", hashlib.sha256(firmware.read_bytes()).hexdigest().upper())
        return
    process = subprocess.Popen(
        [
            str(sdk / "hosttools" / "qemu" / "qemu-system-arm.exe"),
            "-machine",
            "lm3s6965evb",
            "-cpu",
            "cortex-m3",
            "-nographic",
            "-monitor",
            "none",
            "-kernel",
            str(build / "zephyr" / "zephyr.elf"),
            "-semihosting-config",
            "enable=on,target=native",
            "-no-reboot",
        ],
        cwd=ROOT,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        try:
            captured, _ = process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            stop_qemu(process, env)
            captured, _ = process.communicate(timeout=10)
    finally:
        if process.poll() is None:
            stop_qemu(process, env)
            process.wait()
    (output / "qemu.log").write_text(captured, encoding="utf-8")
    print(captured)
    if "PROJECT EXECUTION SUCCESSFUL" not in captured:
        raise RuntimeError("QEMU PDM tests failed")


if __name__ == "__main__":
    main()
