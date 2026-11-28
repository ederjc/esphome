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
verifies the flash, and checks the first boot after programming before
resetting the board again. Both boots must emit sustained heartbeats and
consecutive boot counters. Run it from the repository root:

```powershell
.\.venv\Scripts\python.exe -m esphome compile tests\components\infineon\test.zephyr.yaml
.\.venv\Scripts\python.exe ifx\hardware_smoke.py --openocd <path-to-openocd.exe> --image <path-to-zephyr.hex> --port COM4
```

The runner does not actuate the user button or observe the LED; verify those
separately before marking the GPIO input test as passed.

## DPS368 measurements

The existing `dps310` component supports the onboard DPS368; do not duplicate
the driver. Compile `tests/components/infineon/test-dps368.zephyr.yaml` and
flash its credential-free factory image with the same reservation and recovery
procedure. SCB0 uses P0.2/P0.3 and the sensor address is `0x77`; no jumper is
required. Capture at least five temperature and five pressure publications
from the SCB5 logger. Require finite values within the sensor's rated ranges
(-40 to 85 degrees Celsius and 300 to 1200 hPa), with no setup or communication
failure. These checks demonstrate live measurements, not calibrated accuracy.
Record the observed ranges and restore and verify the known-good core image
before releasing the kit.

## BMI270 measurements

Reuse the existing `bmi270` motion component with `aux_device: NONE` for the
onboard IMU; BMM150 auxiliary support is not BMM350 support. Compile
`tests/components/infineon/test-bmi270.zephyr.yaml`, reserve the kit and flash
the credential-free factory image. Leave the board stationary. After its new
boot marker, capture at least five coherent accelerometer/gyroscope vectors
from the verbose motion log and five temperature publications. Require finite
data, gravity magnitude between 0.85 and 1.15 g, each angular-rate axis within
5 degrees per second of rest, and temperature between -40 and 85 degrees
Celsius. These are static sanity checks, not calibrated accuracy, dynamic
response or axis-orientation validation. Restore and verify the core image
and overwrite any stale bench rows before releasing the kit.

## BMM350 fresh-capture smoke test

Use the credential-free `ifx/fixtures/bmm350.yaml` and the corresponding freshly
built `zephyr.hex`. Reserve the kit as above; the parent bench harness/kit owner
performs these steps, not the component author:

1. Halt the target, program the fresh image, and verify flash while it remains
   halted. Save the image digest and programming verification result.
2. Open UART0/SCB5 at the fixture's logger baud rate and clear the serial receive
   buffer while halted. Discard all earlier serial data.
3. Reset and run the target. Require a new `BMM350 smoke boot` marker after this
   reset before accepting any samples. Ignore pre-marker values; a missing
   fresh marker fails the capture. The fixture emits this marker at boot
   priority -100.
4. Capture at least **five live publications per channel** after the fresh
   marker: `BMM350 field_x=... uT`, `field_y`, `field_z` and
   `BMM350 temperature=... C`. These lines come only from `on_value` callbacks;
   cached-state polling and replayed logs do not count.
5. Every accepted sample must be finite. Require temperature in **[-40, 85] °C**
   and each native magnetic axis in **[-2000, 2000] µT**, inclusively. Reject
   a fully-zero XYZ window (all three axes zero throughout the accepted
   window), not individual zero triples; an individual axis may legitimately
   approach zero.
   Retain the sample counts, values and boot marker in sanitized evidence.

This verifies fresh firmware, initialization, data delivery and basic
plausibility, **not calibrated magnetic accuracy**. Do not claim heading,
orientation calibration or hard/soft iron compensation. Peer review approval
and fork CI must still be green before merging; actual kit evidence remains
mandatory. Do not infer hardware success from compilation, vendor-oracle
vectors or the Windows C++ runner's intentional skip.

Link evidence from the PR: commit tested, board and revision, config or build
command, relevant tool versions, test steps, expected and actual results, and
sanitized logs or captures. List what was not tested. If no kit behavior could
change, explain why hardware testing is not applicable instead of marking it
passed.

## Onboard radar SPI identification

Compile `tests/components/infineon/test-radar-spi.zephyr.yaml`. The fixture uses
the existing Zephyr SPI backend and `spi_device`, with a board-local SCB6
overlay: MOSI P12.0, MISO P12.1, clock P12.2 and GPIO chip select P12.3.
P11.1 pulses the active-low radar reset before the fresh-boot marker.
Before reading the ID, the fixture writes SFCTL = 0 to select normal MISO
timing, matching `xensiv_bgt60trxx_init(high_speed=false)` in the vendor driver.
These connections are from schematic 630-60699-01 rev03, sheets 8/11; the
CY8C624A package pin table assigns HSIOM 20 to all three SPI signals.

Reserve the kit and program/verify while the target remains halted. Clear the
serial input before a separate reset/run so earlier firmware cannot satisfy the
capture. Require `RADAR SPI fixture boot` and five subsequent
`RADAR SPI PASS: CHIP_ID 0x000303` publications, with no SPI setup/transfer
failure. Record GSR0 as well. The 24-bit CHIP_ID contains digital ID 3 and RF ID
3, as specified by the open-source Infineon BGT60TRxx driver. A communication
failure or a missing boot marker is a failed test, not a successful zero reading.
Finally overwrite any stale application tail and restore/verify the known-good
core, including three fresh heartbeats, without erasing settings.

This fixture only proves a live hardware SPI path and radar identity. It does
not start chirps, read the FIFO/IRQ, or provide presence/distance entities.
Those require separate radar acquisition and open-source processing work.
