# Hardware testing

The shared CY8CKIT-062S2-AI uses KitProg3 for flashing and exposes the SCB5
console on `COM4` on the native Windows build host. Check the port assignment
before opening it. Use the Arm Zephyr SDK and the west workspace managed by
ESPHome, not a separate WSL environment.

## Build and reserve the kit

Set up the Python environment once from the repository root in PowerShell:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -e ".[dev,test]"
```

Install CMake, Ninja, Git and Infineon OpenOCD on the Windows host. ESPHome
installs the selected Zephyr workspace and Arm SDK during compilation. Use a
local board configuration and build it with
`.\.venv\Scripts\python.exe -m esphome compile <config.yaml>`. Keep any Wi-Fi
credentials and ESPHome API or OTA secrets in a local `secrets.yaml` next to the
config, referenced with `!secret`. Never force-add it or include it in evidence.

Reserve the hardware by posting a lock on the active GitHub Project issue
before flashing, opening serial or running OTA. Record the holder, PR and
expected end time. Check with the current holder if there is an overlap. Close
the serial client and explicitly release the lock on the issue when finished,
including on failure. Compiling without the device does not require the lock.

## Verify the change

1. Flash the factory image using KitProg3 and Infineon OpenOCD; retain the
   sanitized command and result. Keep a known-good factory image available for
   recovery.
2. Capture the `COM4` boot log at the YAML logger baud rate and prove the changed
   behavior. For example, confirm LED GPIO operation or that a preference
   survives a reboot. Observe actual values for bus and sensor changes.
3. For network features, verify Wi-Fi and the native API locally with
   `aioesphomeapi` and `esphome logs`. Do not add the test device to, or change,
   the user's Home Assistant.
4. For OTA changes, start from a known-good factory image and use a signed
   update with native ESPHome OTA (not mcumgr). Confirm the new image boots and
   reconnects. For rollback changes, test an unconfirmed update and verify that
   the previously confirmed image boots again. The planned MCUboot layout is
   internal primary and external QSPI secondary; record the actual layout.

For the core platform fixture, the Windows bench runner reserves the kit locally,
verifies the flash, and checks that two resets produce consecutive boot
counters and sustained heartbeats without unexpected reboots. Run it from the
repository root:

```powershell
.\.venv\Scripts\python.exe -m esphome compile tests\components\infineon\test.zephyr.yaml
.\.venv\Scripts\python.exe ifx\hardware_smoke.py --openocd <path-to-openocd.exe> --image <path-to-zephyr.hex> --port COM4
```

The runner does not actuate the user button or observe the LED; verify those
separately before marking the GPIO input test as passed.

Link evidence from the PR: commit tested, board and revision, config or build
command, relevant tool versions, test steps, expected and actual results, and
sanitized logs or captures. List what was not tested. If no kit behavior could
change, explain why hardware testing is not applicable instead of marking it
passed.
