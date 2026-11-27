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
- **M1 — Platform:** boot, GPIO, the SCB5 console, NVS preferences, watchdog,
  RNG, reset reason, unique ID, Wi-Fi, native API, native ESPHome OTA with
  MCUboot, I²C and UART. SPI arrives with radar; PWM, ADC, USB-CDC, deep sleep,
  BLE and RTC are deferred.
- **M2 — Onboard sensors:** DPS368, BMI270, BMM350, PDM microphone with sound
  level, then BGT60TR13C radar over SPI with open-source presence and distance
  processing.

## CI

The [fork workflow](../.github/workflows/ifx-ci.yml) runs on PRs into `ifx/dev`
and pushes to it. It checks Python lint and formatting, selected YAML, and
Zephyr/platform unit tests. Once the Infineon platform is present, CI requires
`tests/components/infineon/test.zephyr.yaml` and compiles it with the managed
Zephyr SDK and west workspace. CI does not flash devices. Disable inherited
upstream workflows in the fork's GitHub Actions settings; keep their files unchanged.
