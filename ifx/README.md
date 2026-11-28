# PSOC 6 for ESPHome

This fork targets the Infineon CY8CKIT-062S2-AI using the ESPHome
Zephyr backend and mainline Zephyr (4.5.0 or later). The YAML platform is
`infineon:`, the variant is `CY8C624A`, and the Zephyr board is
`cy8ckit_062s2_ai`.

- Track milestones and work in the [PSOC 6 GitHub Project](https://github.com/users/ederjc/projects/1).
- Read [fork conventions](CONVENTIONS.md) before starting a topic branch or PR.
- Follow [hardware testing](HARDWARE_TESTING.md) to use the shared kit and collect evidence.
- Follow [upstreaming](UPSTREAMING.md) to keep changes portable without opening upstream PRs.

## Milestones

- **M0 — Infrastructure:** fork conventions, focused CI, and a native Windows build and test setup.
- **M1 — Platform:** boot, GPIO, the SCB5 console, flash preferences, watchdog,
  RNG, reset reason, unique ID, Wi-Fi, native API, native ESPHome OTA with
  MCUboot, I²C and UART. SPI arrives with radar; PWM, ADC, USB-CDC, deep sleep,
  BLE and RTC are deferred.
- **M2 — Onboard sensors:** DPS368, BMI270, BMM350, PDM microphone with sound
  level, then BGT60TR13C radar over SPI with open-source presence and distance
  processing.

## Wi-Fi build smoke test

On `cy8ckit_062s2_ai`, adding `wifi:` selects Zephyr's AIROC driver for
the onboard CYW43439 Murata Type 1YN. The SDIO overlay and the module's
[firmware and regulatory blob](https://github.com/zephyrproject-rtos/hal_infineon/blob/eae914ed1ad0b54af28191d13da9b8e98fb5cb2b/zephyr/module.yml#L343-L387)
are requested only for Wi-Fi builds. The
[Infineon BSP](https://github.com/Infineon/TARGET_CY8CKIT-062S2-AI/blob/master/config/design.modus)
defines the kit wiring; the
[Zephyr PSoC 6 reference board](https://github.com/zephyrproject-rtos/zephyr/tree/v4.5.0/boards/infineon/cy8cproto_062_4343w)
provides the SDHC0 pinmux and clock setup.

Compile the [smoke fixture](../tests/components/infineon/test-wifi.zephyr.yaml)
with `python -m esphome compile tests/components/infineon/test-wifi.zephyr.yaml`.
It has no credentials and does not ask ESPHome to connect at boot. The
[native API fixture](../tests/components/infineon/test-api.zephyr.yaml)
extends it with `api:` and uses the existing Zephyr BSD sockets backend.
Compile it with `python -m esphome compile tests/components/infineon/test-api.zephyr.yaml`.
These fixtures are for build checks, not hardware tests. The API fixture
does not enable encryption; do not deploy it.

## CI

The [fork workflow](../.github/workflows/ifx-ci.yml) runs on PRs into `ifx/dev`
and pushes to it. It checks Python lint and formatting, selected YAML, and
Zephyr/platform unit tests. Once the Infineon platform is present, CI requires
`tests/components/infineon/test.zephyr.yaml` and compiles it with the managed
Zephyr SDK and west workspace. CI does not flash devices. Disable inherited
upstream workflows in the fork's GitHub Actions settings; keep their files unchanged.
