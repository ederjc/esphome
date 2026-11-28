# BGT60TR13C: diagnostic and user-supplied-profile acquisition

**The positive A1 register diagnostic passed; the physical register-transfer
fault test is pending.** Without a register list the component remains the
diagnostic fixture below. With a user-supplied list it configures the chip,
starts frames and reads the FIFO; **frame acquisition is compiled but not yet
bench-verified**. It does not publish presence or distance.

## Acquisition from a user-supplied register list

Every published Infineon register table found (ModusToolbox radar examples,
including `mtb-example-ml-imagimob-data-collection`
`b4decd0d4154394bc8504d0c0a2a58d45127fd03` `source/radar_settings.h`, which
targets this kit) is under the Cypress/Infineon EULA, which allows binary-only
firmware distribution and requires source confidentiality. **No vendor table is
in this repository.** Users supply configurator output themselves:

```yaml
infineon_bgt60tr13c:
  # ... spi_id, cs_pin, reset_pin, irq_pin as for the diagnostic
  registers_file: radar_settings.h   # or `registers:` with a list of words
  num_samples_per_chirp: 128         # from the configurator output
  num_chirps_per_frame: 16
  num_rx_antennas: 1
  fifo_threshold: 2048               # samples; default = one frame
  acquisition_mode: triggered        # or continuous
  use_interrupt: true                # false: IRQ pin level + FSTAT polling
  timeout: 500ms
  frame_log_interval: 1              # 0 disables per-frame lines
sensor:
  - platform: infineon_bgt60tr13c
    frame_rate: {name: Radar frame rate}
    signal_rms: {name: Radar signal RMS}
    motion_rms: {name: Radar motion RMS}
```

`registers_file` is read at codegen time. It accepts a C initializer (only the
first `{...}` is used; comments and `#define` lines are ignored) or a plain hex
list. Each word must be a write command (`addr << 25 | 1 << 24 | data`) to an
address below the FIFO (`0x60`). The geometry must match the list; frames must
be an even sample count of at most 16384.

Sequence, mirroring the Apache driver's `xensiv_bgt60trxx_config()`/
`start_frame()`: hardware reset, CHIP_ID check, MAIN software reset, writes in
list order (SFCTL FIFO limit `CREF = fifo_threshold/2 - 1` and MISO high-speed
off are owned by the component), full readback (mismatches are warnings),
FSM+FIFO reset, then `MAIN.FRAME_START`. Data-ready is the rising IRQ edge on
the configured pin, with a 50 ms FSTAT watchdog. A frame is read only once FSTAT
holds a whole frame, in ≤256-sample bursts with a 20 ms per-loop budget. Any
error or timeout discards the whole frame and resets FSM+FIFO.

- `triggered` (default) forces CCR2 `MAX_FRAME_CNT` (bits 11:0, datasheet
  decode) to 1 and starts one frame per `update_interval`. This suits 1 MHz SPI:
  a 2048-sample frame is 3072 payload bytes, about 25 ms on the wire.
- `continuous` keeps the list's CCR2 and re-polls after each frame. It only
  works when SPI throughput exceeds the profile's data rate; otherwise FIFO
  overflow causes discards.

Statistics (portable `frame_stats.h`) assume FIFO order chirp → sample → RX
interleaved per sample: per-RX mean/min/max/peak (`max(max-mean, mean-min)`),
`signal_rms` (RMS of each chirp's AC part, averaged over chirps and RX) and
`motion_rms` (RMS change of the DC-free mean chirp versus the previous frame;
`nan` on the first frame). These are raw-ADC LSB values, not calibrated
presence or range.

UART lines (tag `infineon_bgt60tr13c`) for bench parsing:

```text
BGT60TR13C acquisition boot v1
Configured: registers=%u readback_mismatches=%u mode=%s samples_per_chirp=%u chirps=%u rx=%u frame_samples=%u frame_words=%u fifo_limit_words=%u irq=%s
Register 0x%02X readback 0x%06X expected 0x%06X            (warning)
Frame #%u src=%s samples=%u fill=%u read_ms=%u rx1_mean=%.1f rx1_min=%u rx1_max=%u rx1_peak=%.1f[ rx2_... rx3_...] signal_rms=%.2f motion_rms=%.2f
Frame discarded: reason=%s result=%u fill=%u discarded=%u   (warning)
Acquisition summary: frames=%u rate_hz=%.2f discarded=%u irq_events=%u
Radar protocol failed: %u (...)                             (error)
```

`src` is `irq`, `pin` or `poll`; `fill` is FSTAT words at detection. Discard
reasons are `fifo_status`, `timeout`, `fifo_read` and `sample_count`.

`tests/components/infineon_bgt60tr13c/test-acquisition.zephyr.yaml` uses a
**non-functional placeholder** list that only exercises validation and codegen.
For bench builds, keep the licensed configurator output in a gitignored path
(for example `tests/components/infineon_bgt60tr13c/.esphome/bench/`) and never
commit or publish it.

Open bench gates: CCR2 field decode and FSM behaviour after `MAX_FRAME_CNT=1`,
IRQ edge/threshold on P11.0, measured burst throughput at 1 MHz, register
readback of status-like fields, multi-RX ordering, and 80 MHz reference
provenance.

## Confirmed protocol and licensing

Protocol and LFSR are adapted from the public Apache-2.0 driver
[Infineon/sensor-xensiv-bgt60trxx](https://github.com/Infineon/sensor-xensiv-bgt60trxx/tree/78099697159a2a736504ef5bd1d33820b21f4e67).
Files verified: `xensiv_bgt60trxx.c`, `.h`, `_regs.h`, `_mtb.c`, and `LICENSE`.
Copyright retained in `protocol.h` and the extracted test oracle; the complete
upstream license is included as `esphome/components/infineon_bgt60tr13c/LICENSE.txt`,
matching the source/wheel packaging rule. No proprietary DSP or SDK
binary is incorporated.

- Register read: `address << 25`; write additionally sets bit 24 and 24-bit data.
- Bootstrap writes `0x0D000000` (SFCTL=0) before CHIP_ID. Accept **only** `0x000303`.
  The base commit's A1 bench response is ID `0x000303`, GSR `0x70`, mode 0/1 MHz.
  `0x000606` is not accepted as another identity.
- Hardware reset holds RESET low for 10 ms, then high for 10 ms, with CS high;
  this retains the verified board fixture's timing.
- FIFO address `0x60`: burst header `FF C0 00 00`, CS held throughout header/data.
  Each 24-bit FIFO word holds two unsigned 12-bit samples, high sample first.
- FSTAT `0x5F`: fill is words, bits 13:0; capacity 8192 words/16384 samples.
  Reject clock/burst/underflow/overflow/full status. GSR error mask is `0x0D`;
  upper GSR bits are logged, not invented as a universal fixed signature.
- LFSR seed is 1; the recurrence is extracted from the pinned Apache source.
  Vendor documentation requires **RX1 enabled** for the LFSR test.

SPI's old void transfer API cannot distinguish a legitimate zero ADC sample
from RX zeroed on failure. Readback, CHIP_ID and FIFO status cannot prove a
payload transfer succeeded: clocks may consume FIFO data even when RX fails.
The generic SPI API therefore gains optional, sticky per-transaction status.
Zephyr reports controller setup/header/data errors and unsupported variable-width
writes; a later successful transfer in the
same CS window cannot clear an earlier error. Existing void clients retain
their behavior. The component fails visibly on backends lacking this optional
capability instead of silently claiming successful acquisition.

`protocol.h` is platform independent. Its FIFO API checks availability before
consuming, caps each call at 256 samples (384 payload bytes; 388 wire bytes),
checks header GSR and post-read FSTAT, and only then unpacks. Incomplete/error
calls never deliver valid samples. A failed burst may have advanced the FIFO;
the future frame owner must discard its whole partially assembled frame, issue
the bounded FIFO/FSM reset (MAIN bit 3), verify reset completion, and restart
from a known frame boundary. It must not concatenate across such a loss.

## Blocking profile evidence

The suggested public
[mtb-example-psoc6-radar-presence release-v2.1.0](https://github.com/Infineon/mtb-example-psoc6-radar-presence/tree/e1b031bd650d714875c102d48199829092c63d08)
is **not Apache/BSD**: its root `LICENSE` is the Cypress EULA, sections 2/4
restrict source distribution/use. The register headers have no separate
permissive license. The release-v1.0.0 and release-v0.5.1 LICENSE files also
use that EULA. **None of those register tables has been copied.**
The Apache driver does not supply a generated register table and directs users
to the separate Radar Development Kit configurator.

An appropriately licensed generated profile, with explicit external **80 MHz**
reference-clock provenance, is required before configuring RF or starting
frames. The current board-local overlay is copied unchanged from the verified
SPI fixture; no oscillator/clock binding has been guessed or added.

The additional public
[Rust profile lead](https://github.com/thedevleon/bgt60trxx-rs/tree/1042a4c123734ecd32288294c56b7a196f61f6c9)
has a root MIT license, but that alone does not establish clean upstream
configuration provenance. Its initial configuration commit
`4a518e3b2d75b3d3e1c4cfe492a9cb46efce7bf9` embeds a generated RDK header
marked `v3.3.0+207.a6ebda979`; its high/low JSON files have identical git blobs
to the EULA-covered example (`fd5e6273e9a691d2f6cfe9bc73dcba5442868718` and
`9886ec50147936b6b54e16c1b8f5433c275c1375`). The test preset was added in
`d69245279c08d7da40fbc5f2baf692c2b40e9f02`: its JSON requests RX antenna `[1]`,
128 samples, one chirp and 100 ms; the Rust metadata reports one RX and
99.8265 ms. This is not an independently licensed configurator-output grant.
The advertised kit
support is an experimental passthrough, not evidence of native SCB6 acquisition.
No Rust register table has been imported.

An independent, conditional decode using the
[Infineon datasheet V2.4.9](https://www.infineon.com/assets/row/public/documents/30/49/infineon-bgt60tr13c-datasheet-en.pdf)
(pages 30, 37, 43, 47–48 and 55–56) establishes that the test preset enables
RX1 mixer/LO and baseband mask `001`; the high preset instead enables only RX3,
**not three RX channels** despite its Rust count metadata. At an assumed 80 MHz,
the test preset's ADC divider 34 and PLL DIVSET 20 give 2352941.176 samples/s.
Its signed start-frequency word gives 61.005084229 GHz, ramp increment 164 gives
8.0078125 MHz/us, ramp-time count 594 gives 59.4 us, and 128 ADC samples span
54.4 us. The full RF ramp spans 475.664063 MHz, while the sampled sweep spans
435.625 MHz: a nominal unpadded range FFT bin is about **0.344095 m**.
The metadata's endpoint difference, 459.804 MHz, is not interchangeable with
that sampled bandwidth. These are datasheet arithmetic, **not physical range,
frame timing or acquisition verification**. A prospective 128-sample/RX1 burst
would use 192 payload bytes plus its 4-byte header, taking 1.568 ms at 1 MHz
before status reads and overhead; frame cadence remains a separate verification.
Independent complete configuration generation is still blocked by unestablished
analog/init timing and reserved-bit defaults, not just PLL arithmetic.

The chip has 1 TX/3 physical RX. The suggested example's metadata enables only
one receiver (RX3), which also cannot satisfy the RX1-only vendor self-test.
Do not infer three-channel sample ordering from that profile. Obtain/document
active RX mask, ADC sample/chirp/channel ordering, samples/chirp, chirps/frame,
actual sample rate, frequency sweep and frame cadence with the licensed table.

Until then **bandwidth, range-bin spacing, frame sample layout and frame budget
are unknown**, not guessed from a differently licensed table. For a confirmed
sweep, sampled bandwidth is `f_end - f_start` over the ADC acquisition interval,
not RF ramp margins; an unpadded range FFT's nominal bin spacing
is `c/(2*bandwidth)`. Any zero padding changes bin spacing, not resolution.
At 1 MHz, the theoretical ceiling is 125000 wire bytes/s; a capped burst takes
3.104 ms on wire, excluding status reads, CS gaps and software overhead.
Actual throughput must be measured. Choose frame cadence/burst scheduling only
after measuring it, with a bounded per-loop budget and overflow headroom.
This step found no FFT implementation in existing portable ESPHome components.
FFT/clutter/motion processing is deferred with the profile, rather than creating
a physically uncalibrated distance placeholder.

## Validation and parent bench gates

The standalone native tests are under `ifx/tests/bgt60tr13c`, outside the
standard GoogleTest component discovery tree. Fork Linux CI builds/runs both.
`protocol_test.cpp`
It uses a mock transport, base-board identity bytes and the separately extracted
vendor LFSR/packet oracle, never room data or simulated hardware evidence.
`spi_delegate_test.cpp` compiles the **actual** `spi_zephyr.cpp` and `spi.h`,
with test-only core/Zephyr interface stubs and scripted controller return codes.
It checks setup/header/data failures, error followed by success under the same
CS, next-transaction clearing, and unsupported/invalid writes. These are host
regressions, not real-controller or FIFO hardware evidence.
Build with a C++20 compiler using CMake/CTest:

```powershell
cmake -S ifx\tests\bgt60tr13c -B build\radar-protocol
cmake --build build\radar-protocol
ctest --test-dir build\radar-protocol --output-on-failure
```

Compile the credential-free diagnostic fixture from this worktree, using the
existing approved Zephyr installation and an isolated `ESPHOME_BUILD_PATH`:

```powershell
..\esphome-psoc6\.venv\Scripts\python.exe -m esphome compile tests\components\infineon_bgt60tr13c\test-diagnostic.zephyr.yaml
```

Corrected increment's validation:

- Both C++20 CTests passed with Clang 20.1.2: portable protocol plus the actual
  Zephyr delegate. Cases include checked SFCTL writes/readbacks and overflow
  reset followed by a successful checked vendor-oracle sample read.
- The 29 selected existing Zephyr Python regressions, diagnostic generation,
  Ruff, clang-format, CI YAML parsing and standalone discovery layout passed.
- Source distribution and wheel built with the declared setuptools/wheel
  backend, without build isolation. Both contain the **byte-identical** component
  `LICENSE.txt` (SHA256
  `9fb6f1f4809e149758b5f725c44fc37d4de6662aef4b7b5df6c45906a575f615`).
  Missing compiler/package tools were restored only under the owned build
  directory; no shared Python installation was changed.
- Arm GNU 14.3/Zephyr SDK 1.0.1 firmware linked: **flash 92412 bytes,
  RAM 27276 bytes**. No hardware was accessed. Generation used
  `--only-generate`, then direct west/CMake with four build jobs and explicit
  `USER_CACHE_DIR` under `build\radar-arm-review`; SDK installers, blob fetches
  and ccache were not used. The corrected bench HEX is
  `build\radar-arm-review\psoc6-radar-diagnostic\.west_build\zephyr\zephyr.hex`.
  SHA256: `5c54400f8711d1f6d8e10be6ca1a997f239dae75132336c70836d9692f3f605b`.
- The earlier reviewed HEX under `ifx-rd\psoc6-radar-diagnostic` was left
  untouched; its SHA256 remains
  `975669f7a5bd48b49f11f06be786d3d88125b4b8c5725e425924bc526b79feec`.
  It does **not** contain the corrected reset timing.

The parent/bench owner must reserve the kit according to
`HARDWARE_TESTING.md`; this driver author must not open COM4/SWD or flash:

1. Record exact staged tree/build, kit A1 revision and fixture. Program/verify
   while halted, clear serial input, then separately reset/run.
2. Require fresh `BGT60TR13C diagnostic fixture boot v1`, followed by five
   consecutive diagnostic summaries: exact ID `0x000303`, SFCTL 0, GSR/status
   and IRQ **pin level sampled**, no SPI/protocol errors. This does not establish
   IRQ polarity, threshold behavior or FIFO acquisition.
3. Separately inject a confirmed benign **register-transfer** fault using the
   bench-approved method. Require visible failure and component failed state.
   Do not infer FIFO payload fault handling from this diagnostic: it never reads
   FIFO. Header/data sticky handling is presently a host regression only.
4. **Blocked pending licensed 80 MHz/RX1 profile:** configure with readbacks,
   establish IRQ polarity and FIFO threshold in words, start bounded frames,
   compare every RX1 sample with seed-1 vendor LFSR across consecutive bursts/
   frame boundaries. Log only count, mismatch index, frame, duration and status.
5. Then disable LFSR, verify ADC sample/chirp/RX layout and measured throughput.
   Deliberately cause incomplete FIFO and overflow; require discard/reset and
   correctly aligned recovery. No huge raw-frame logging.
6. Only after those gates, add separate open-source processing with synthetic
   range/motion/clutter tests, and physical calibrated target/presence tests.
   Neither physical presence nor distance is verified by this increment.
7. Restore/verify the known-good core with three fresh heartbeats without
   erasing settings; release the reservation. Production device is unchanged
   by the driver author.

The positive register-only bench passed on A1 using the corrected HEX above.
All five fresh summaries reported ID `0x000303`, SFCTL `0x000000`,
GSR `0x70`, FSTAT `0x100000`, zero FIFO words, and IRQ level 0. OpenOCD
verified the 7,168-byte tail scrub and the restored core; the core emitted
three fresh heartbeats.
[Bench evidence](https://github.com/ederjc/esphome/issues/6#issuecomment-7230083885).

The physical register-transfer fault test remains pending: no safe electrical
injection method is available on the attached PCB. Scripted host regressions
do not satisfy that integration gate. The evidence supports a diagnostic
draft only, not FIFO, IRQ edge, presence, or distance claims.
