# CY8CKIT-062S2-AI stereo PDM feasibility

**The A1 stereo capture and restart smoke test passed.**
This is a deliberately single-board downstream Zephyr driver plus a separate,
API-based ESPHome `microphone: platform: zephyr_dmic` backend. Its supported
contract is deliberately restricted to this synchronous PDMv1 driver and board,
not arbitrary Zephyr DMIC drivers. It is not an
implementation of Zephyr's existing MXPDM v2 driver.

## Hardware and clock contract

The verified public schematic **630-60699-01 revision 03, sheet 10** has two
IM72D128V01XTMA1 microphones, **U4 and U5**. They share P10.4 (PDMCLK) and
P10.5 (PDMDATA), with opposite clock-edge selects. Both channels must work.
Channel 0/1 are converter left/right; physical U4/U5 attribution and edge order
still require bench verification. Do not describe this as a single microphone.

CY8C624ABZI-S2D44 has `CY_IP_MXAUDIOSS_VERSION == 1`, PDM0 at `0x40a00000`,
and interrupt 157. Its exact 124-BGA pin map selects HSIOM 21 for both signals.
**21 means ACT_9, not ACT_5.** Packed pinmux must be `0x15040a` for P10.4 and
`0x15050a` for P10.5. Generated C++ asserts the actual PDM0 pinctrl phandles,
both packed values, the device compatible and register address.
The added binding is `infineon,pdm-v1`; `infineon,pdm` is incompatible v2.

The board enables **HF1 = path_mux1 = IMO = 8 MHz**. HF0, FLL, CM4, CM0+,
PERI and their existing 100/50 MHz configuration remain unchanged. No PLL is
retuned. PDM dividers 1, 1, 4 yield **2 MHz PDM clock**, 50% duty.
Decimation 128 yields **15625 stereo frames/s**, not 16000. The driver checks
the actual HF1 frequency and rejects another clock or format. The board's
existing clock-control initialization owns clock routing.

Only signed, little-endian **16-bit interleaved L/R stereo** is implemented.
The driver uses PDL `cy_pdm_pcm.c`, interrupt-driven FIFO draining, standard
Zephyr PCM slabs and a completed-block queue. There is no DMA/HAL resource
allocator mixed with Zephyr IRQ ownership. Startup discards 64 stereo frames.
STOP/RESET release queued and partial buffers, never a slab borrowed by the
caller. RESET requires configuration again. FIFO/slab/queue errors stop capture
and are reported by `dmic_read`; STOP followed by START recovers. PAUSE/RELEASE
are explicitly unsupported.
Read supports **timeout 0 only**; positive and forever timeouts return
`-ENOTSUP`. State validation, nonblocking dequeue and returned block-size
snapshot share one IRQ-locked transaction on this single-core Zephyr target.
STOP/RESET/IRQ errors cannot free the caller-borrowed block.

The ESPHome backend owns eight 640-byte slab blocks, drains at most eight blocks
per main-loop iteration, calls `publish_audio_` with `AudioCapture` metadata,
and frees each borrowed slab after callbacks. `capture_time_us = 0` explicitly
means unknown: the standard DMIC API provides no capture timestamp. Main-loop
callbacks must not block; excessive latency stops capture rather than silently
losing channel alignment.
Only one YAML backend can own resolved PDM0 (`/soc/pdm@40a00000`); alternate
labels/aliases and other boards/controllers are explicitly rejected. Runtime
ownership also prevents a second backend from reconfiguring the driver's slab.
Other drivers' unspecified `act_num_streams` or asynchronous STOP behavior are
not treated as compatible.

`loop_interval` must be at most **20 ms**. Eight blocks provide 81.92 ms of PCM;
validation reserves half that capacity (40.96 ms) as headroom and another
20 ms for processing/scheduling overhead. Runtime changes to the loop interval
are checked too. These are bounded budget assumptions, not real-time guarantees:
callbacks/other components exceeding the reserved overhead can still cause an
explicit capture error.

Plain PCM audio and MicrophoneSource selection/gain conversion now work without
ESP-IDF libraries on Zephyr. Codec requests fail explicitly; the ESP32 codec and
conversion paths remain unchanged. `sound_level` portability is a separate,
unstaged, unverified work item; it is not enabled by this fixture.

## Deliverables and evidence

- `psoc6-pdm-v1.patch`: apply to an **isolated** Zephyr checkout at
  `d42a8e9e40735695cc3319c9c6b35cbe18f00108`. Contains driver, binding, Kconfig,
  PDL source wiring and board clock/pinctrl/node changes. It changes no HAL
  module source. The source copies beside this document support simulation.
- `esphome/components/zephyr_dmic/`: standard Zephyr microphone backend.
- `ifx/pdm-stereo-bench.yaml` and `pdm_stereo_metrics.h`: credential-free fixture,
  no Wi-Fi/API/OTA, no audio transport, file, waveform, energy or recording.
  Only timing, frame counts and per-channel nonzero sample counts are logged.
  One `mode: single` script owns all transitions: initial STOP, >=1 s quiet,
  three serial 10 s capture runs separated by >=1 s quiet, final STOP/quiet,
  then explicit completion. There is no independently scheduled interval.
  Concurrent START, incomplete/invalid runs and callback delivery while stopped
  latch failure; `COMPLETE` requires a full validated >=5 s window in each run.
- Fifteen focused pytest cases passed, including deterministic fixture sequencing,
  duplicate ownership, alias
  refusal, polling-budget boundaries, unsupported formats, plain
  PCM refusing codecs/ESP-IDF resources, and preserved ESP32 codec setup.
- Eleven QEMU ztests passed using the actual driver and ESPHome backend/AudioCapture
  publication path with fake registers and real
  Zephyr queues/slabs: bad clock/shape, allocation exhaustion, borrowed-slab
  ownership, fresh stereo read after exhaustion/restart, nonblocking read/reset
  metadata, callback STOP of a caller-borrowed block, duplicate runtime ownership,
  polling interval changes, completion gating/concurrent START rejection,
  full metrics windows/quiet gaps, channel selection
  and signed gain.
  Fake registers cannot prove physical FIFO ordering, pin routing or clocks.
  Tests use a 4096-byte checked stack and static PCM slabs/synthetic input; no
  hardware or captured room audio is involved.
- Arm SDK 1.0.1 / GCC 14.3 / C++20 compiled and linked the real board fixture.
  `.config` enables `CONFIG_AUDIO_DMIC_INFINEON_PDM_V1`, not the v2 driver.
  Outputs are in `.pdm-workspace/board-build/zephyr/zephyr.{elf,hex,bin}`.
  Deterministic-sequencer build footprint: 89,080 bytes flash, 31,756 bytes RAM.
  Both generated pinmux values and their actual controller phandles passed
  compile-time assertions. Replacement HEX SHA-256:
  `C866E758B0EB00CA99151B2CF5119E8F5B659BB2AE5648D77645242002398EC8`.
  The earlier `3D877457...` HEX is held: its independently scheduled interval
  could race boot/startup. Do not flash it.
  The earlier `0A990278...` HEX used wrong ACT_5 pinmux: do not flash saved copies.
- Ruff checks/format and language-schema extraction `--check` passed.
- Native Python sequencing/schema tests passed. Native host C++ tests were not
  run: `clang++`, `g++` and `cl` are unavailable. The actual ARM board cross-build
  and QEMU C++/driver tests above are distinct evidence, not a native-host claim.
- Reproducible logs: `.pdm-workspace/qemu.log`,
  `qemu_cortex_m3-{configure,build}.log` and
  `cy8ckit_062s2_ai-{configure,build}.log` in the same owned directory.

The simulated suite is `ifx/zephyr-pdm-v1/tests` for `qemu_cortex_m3`. It needs
the existing Zephyr SDK, Python environment and CMSIS module. Configure/build
with CMake and explicit read-only `ZEPHYR_MODULES`; execute the ELF using the
SDK's `qemu-system-arm` (`lm3s6965evb`, Cortex-M3, semihosting). No hardware is
involved.

**Installation caution:** even ESPHome `compile --only-generate` invokes the
local framework installer, `west update`, and managed Python setup. An initial
attempt inadvertently ran this through a module junction; it was stopped and
the junction removed. Read-only status checks of the touched shared module
repositories found no tracked source changes, but Git indexes/timestamps and
the managed requirements file were touched. Subsequent generation disabled
installers in the isolated process and builds used explicit read-only module
paths. Never use a shared-module junction with the ESPHome local installer.

## Hardware smoke test

After peer review, flash only the credential-free HEX to the shared kit using
the parent's existing authorized procedure. The deterministic sequencer takes
approximately 34 seconds after boot; wait for explicit completion. Require:

1. Correct stereo shape (640 bytes = 160 frames), no DMIC errors or resets.
2. Two active PCM slots: both `ch0_nonzero` and `ch1_nonzero` repeatedly nonzero,
   each no larger than `frames`. The broad delivery-rate gate is a sanity check
   only: its existing tolerance **cannot distinguish 15625 Hz from 16000 Hz**.
3. Observe `BOOT`, `STOP run=0`, a >=1000 ms `QUIET blocks=0`, then `START`
   and `FIRST_BLOCK`. Observe two further STOP/QUIET/START/FIRST_BLOCK sequences
   (`run=2`, `run=3`), with at least one full >=5000 ms `WINDOW` per run.
   Callback delivery must resume in both slots after each restart.
4. No callbacks during initial, intermediate and final stopped intervals.
   Require `COMPLETE runs=3 validated_runs=3 stopped_callbacks=0` after the last
   STOP/quiet. Any invalid shape, quiet gap,
   incomplete run, timing gap or nonzero/rate gate error fails this bench.

Reproduce with the preinstalled ESPHome Python and read-only cache:
`ifx\zephyr-pdm-v1\tests\offline_build.py --cache <ifx-z> --tests` or `--bench`.
The helper disables installers, confines scratch/user caches to this worktree,
uses explicit read-only HAL modules, and checks both generated pinmux values.

Only the parent may access the hardware. Do not save, transmit or publish room
audio. Acceptance is limited to PCM shape, two active slots, stopped-callback
silence and resumed delivery. Synthetic driver tests prove software behavior
with injected data, not physical FIFO order or absence of stale hardware data.

On A1, the exact `C866E758...` HEX delivered a valid window for each of the
three fresh runs, with 78,240 to 78,400 stereo frames per window over 5,006
to 5,008 ms. Both slots had nonzero counts in each window and capture resumed
twice. The fixture reported zero callbacks during STOP. OpenOCD verified the
3,584-byte tail scrub and restoration of the known-good core; the core emitted
three fresh heartbeats. [Bench evidence](https://github.com/ederjc/esphome/issues/6#issuecomment-7230211516).

This does not prove uninterrupted 10-second delivery, an exact clock
frequency, acoustic independence, or physical U4/U5 attribution.

## Public source references (inspected from the pinned local public cache)

- [Zephyr v2 DMIC driver](https://github.com/ederjc/zephyr/blob/d42a8e9e40735695cc3319c9c6b35cbe18f00108/drivers/audio/dmic_infineon.c):
  `cy_pdm_pcm_v2.h`, six channels; not PSOC 6 MXAUDIOSS v1.
- [Board and original clock setup](https://github.com/ederjc/zephyr/blob/d42a8e9e40735695cc3319c9c6b35cbe18f00108/boards/infineon/cy8ckit_062s2_ai/cy8ckit_062s2_ai.dts)
  and [CAT1 clock initialization](https://github.com/ederjc/zephyr/blob/d42a8e9e40735695cc3319c9c6b35cbe18f00108/drivers/clock_control/clock_control_infineon.c).
- [PDMv1 PDL API](https://github.com/zephyrproject-rtos/hal_infineon/blob/eae914ed1ad0b54af28191d13da9b8e98fb5cb2b/mtb-pdl-cat1/drivers/include/cy_pdm_pcm.h)
  and [HAL rate/FIFO implementation](https://github.com/zephyrproject-rtos/hal_infineon/blob/eae914ed1ad0b54af28191d13da9b8e98fb5cb2b/mtb-hal-cat1/source/cyhal_pdmpcm.c):
  divider formula, stereo FIFO words, 35–45 startup cycles.
- [Exact device header](https://github.com/zephyrproject-rtos/hal_infineon/blob/eae914ed1ad0b54af28191d13da9b8e98fb5cb2b/mtb-pdl-cat1/devices/COMPONENT_CAT1A/include/cy8c624abzi_s2d44.h)
  and [124-BGA GPIO functions](https://github.com/zephyrproject-rtos/hal_infineon/blob/eae914ed1ad0b54af28191d13da9b8e98fb5cb2b/mtb-pdl-cat1/devices/COMPONENT_CAT1A/include/gpio_psoc6_02_124_bga.h).
- [Kit resources](https://www.infineon.com/evaluation-board/CY8CKIT-062S2-AI):
  schematic identity above was supplied as already verified by the coordinator;
  no new schematic download or hardware access occurred here.
