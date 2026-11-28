# Zephyr relative sound-level verification

This separate, credential-free fixture computes **uncalibrated, unweighted
relative dBFS** from two selected PCM slots. It does not measure calibrated SPL,
Pascal pressure, acoustic independence or individual physical microphone
attribution. No A-weighting, DC removal or sensitivity calibration is applied.

## Implementation and dependencies

- `sound_level` accepts one selected signed little-endian 16-bit
  `MicrophoneSource` channel on Zephyr. Explicit pooled stereo is rejected;
  ESP32's existing pooled-channel/ring-buffer path remains unchanged.
- Zephyr sensors default to `dBFS` without a `sound_pressure` device class.
  ESP32 retains its `dB`/`sound_pressure` defaults. Explicit overrides remain
  unchanged; this fixture uses the Zephyr defaults.
- Channel selection and optional integer gain use the existing portable PCM
  `MicrophoneSource` implementation. The fixture fixes gain to 1.
- Zephyr kernel spinlocks protect scalar accumulators, lifecycle generation and
  a four-element nonblocking queue. The queue stores squared peak, squared sum
  and sample count, **not PCM**. Sensor publication happens in the main loop.
- Statistics use integer accumulation and existing C/C++ `log10`/`log10f`,
  normalized to `32768²`. Peak is `10 log10(max(x²)/32768²)`; RMS is
  `10 log10(mean(x²)/32768²)`. Silence is mathematically `-inf` dBFS; absent or
  invalid input is explicitly `NaN`, not a fabricated sound level.
- No ESP-IDF audio codec/DSP or ESP32 ring-buffer dependency is loaded on Zephyr.
  Transient microphone/source buffers remain necessary for capture/conversion,
  but this fixture records no PCM, exposes no audio transport and has no
  Wi-Fi/API/OTA/MQTT/web-server configuration.
- The real board build needs the separately reviewed PDMv1 backend/Zephyr patch,
  Zephyr `d42a8e9e40735695cc3319c9c6b35cbe18f00108`, existing CMSIS 6 and
  Infineon HAL modules, Arm Zephyr SDK 1.0.1/GCC 14.3/C++20, and the preinstalled
  ESPHome and Zephyr Python/CMake/Ninja environments.

## Lifetime and failure behavior

Both sources are passive and select `[0]` and `[1]` independently. They share
one physical backend lifetime; `MicrophoneSource` is not reference-counted.
Passive `sound_level.start/stop` do not control that shared microphone.
The single-mode fixture script alone starts/stops capture and includes one-second
quiet gaps, allowing consumer loops to observe stopped input and discard partial
windows. Immediate external STOP/START without a consumer loop observing STOP
cannot be detected through the current microphone API and is not supported by
this fixture.

Active consumer STOP/START discards partial/queued statistics. A peak sensor
callback stopping the component suppresses old RMS/queued publications.
STOP and fault publication recheck lifecycle generation after peak `NaN` to
avoid publishing stale RMS `NaN` after a restart. The STOP path also checks
whether a passive source was restarted directly by the callback without
changing the generation. No sensor callback runs under the capture spinlock.
Malformed PCM or aggregate-queue overflow reports warning/NaN and latches until
an explicit restart or an observed microphone stop. There is no silent dropping
of measurement windows.

## Offline validation

Run the schema tests with ESPHome's test environment. Build and run the QEMU
fixture from a Zephyr workspace with its Arm SDK and QEMU installed:

```text
python -m pytest tests/unit_tests/components/zephyr/test_sound_level.py --no-cov -q
west build -b qemu_cortex_m3 -d <qemu-build> <esphome>/tests/components/sound_level/zephyr
west build -d <qemu-build> -t run
```

The fork's native-board CI compiles `ifx/sound-level-ci.yaml` using the pinned
PDMv1 Zephyr tag and verifies its commit before compiling.

The QEMU suite compiles the actual component in its Zephyr branch and separately
its preserved ESP32 branch with compatibility fakes. It covers callback/window
boundaries, independent mono selection, full-scale negative samples, silence,
active/passive stop/restart, malformed input, queue overflow, sensor-callback
STOP and peak-NaN callback restart in stopped and fault paths. This is not an
ESP32 hardware build or an acoustic test.

Verified offline results:

- Nine focused pytest schema/metadata/privacy/sequencing cases passed.
- Thirteen QEMU cases passed: twelve Zephyr statistics/lifecycle cases and one
  ESP32 pooled-stereo compatibility case.
- The real board fixture linked with actual `MicrophoneSource` channel
  conversion, `SoundLevelComponent` and `log10`/`log10f`. Its configuration has
  `CONFIG_PICOLIBC_IO_FLOAT=y`, `CONFIG_CBPRINTF_FP_SUPPORT=y`,
  `CONFIG_STD_CPP20=y` and the reviewed PDMv1 driver.
- Generated metadata uses a `dBFS` unit table without a device-class table.
- Ruff check/format, clang-format and language-schema extraction check passed.
- Footprint: **96,748 bytes flash; 32,268 bytes RAM**.

## Hardware verification

Only `slot0/slot1 peak_dBFS` and `RMS_dBFS` numeric aggregates are logged at INFO.
The one-shot script has three ten-second capture runs with stopped gaps and
then stops permanently. The exact ASCII INFO lifecycle messages use tag
`sl_bench`, in this order:

```text
BOOT sound_level aggregate-only bench
START run=1
STOP run=1
START run=2
STOP run=2
START run=3
STOP run=3
COMPLETE runs=3
```

BOOT precedes the initial STOP and one-second quiet gap; initial output may
contain NaNs only. Each START marker immediately precedes capture, followed by
ten seconds of measurement. Each STOP marker immediately precedes STOP,
followed by at least one second quiet, including after STOP run=3 before
COMPLETE. Metric messages remain `slot0 peak_dBFS=%.2f`,
`slot0 RMS_dBFS=%.2f`, `slot1 peak_dBFS=%.2f` and `slot1 RMS_dBFS=%.2f`.

The transcript parser, **not the completion banner**, must verify
at least five coherent peak/RMS pairs per slot in every run, including at least
one finite pair per slot. Digital silence can legitimately report `-inf` during
capture. After each STOP, both metrics of both slots must become `NaN`, with
no subsequent finite or silent pairs throughout that quiet gap. Expect resumed
delivery for both logical slots, no capture errors and no window pooling across
observed gaps.

On A1 silicon, [the reviewed image passed all three runs](https://github.com/ederjc/esphome/issues/6#issuecomment-7230810260)
with ten finite pairs per slot per run, unavailable states and quiet STOP gaps,
and verified flash-tail cleanup and core recovery. Its first window after each
start was about -1 dBFS peak and -13 dBFS RMS on both slots. Keep this window in
the evidence, but do not use it as a steady-state baseline: the cause and
settling time were not established. The scheduled capture sequence does not
exercise reentrant callbacks; those are covered by the QEMU tests.

Do not record/transmit PCM or infer calibrated SPL, exact sample frequency,
hardware FIFO freshness or physical microphone attribution from these values.
