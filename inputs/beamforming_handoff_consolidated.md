# Handoff: 4-mic beamforming firmware for heimdallr

You are picking up work on **heimdallr**, a 4-microphone linear beamforming array built on an ESP32-S3 board. Your job is to write the firmware that captures all four mics in sync, estimates where speech is coming from, and steers a beam at it. The goal is **measurably better speech pickup**, not only direction finding. The WiFi layer is done; the audio side does not exist yet.

Correct synchronised capture and measured beam performance matter more than a complete-looking feature list. Don't invent peripheral capabilities. If something below turns out not to work on this chip, say so and propose the smallest hardware or firmware change that fixes it.

Repo: `/Users/nlothian/dev/fifthvertex/mic-array`. Firmware lives in `code/` (PlatformIO, `src_dir = .`).

## Read these first

| File | What it gives you |
|---|---|
| `code/main.cpp` | Existing firmware: non-blocking WiFi (saved creds → captive portal fallback), synchronous `WebServer` on port 80 + mDNS at `http://heimdallr.local/`, status on the TFT, BOOT-button hold to reopen the portal. Keep all of it working. |
| `code/platformio.ini` | Env `fnk0104`: pioarduino platform (arduino-esp32 3.3.9 / ESP-IDF 5.5), octal PSRAM, USB-CDC serial, TFT_eSPI config. |
| `inputs/4x_INMP441_ESP32_wiring_handoff.md` | Mic wiring, pin by pin. |
| `docs/FNK0104-pinout.md` | Every GPIO on the board and what it's wired to; the three sockets. |
| `docs/fnk0104-2.8inch-schematic.pdf` | Freenove's schematic (source of truth for the pinout). |
| `docs/esp32-inmp441-wiring.svg` | Wiring diagram. |
| `3dprint/micarray.scad` | Enclosure; defines the mic geometry below. |
| `/Users/nlothian/dev/fifthvertex/amy-fnk-keyboard/FNK0104-BOARD.md` | Hardware-verified board gotchas from a sibling project (PSRAM build keys, USB-serial re-enumeration). |

## Hardware

- **Board:** Freenove FNK0104, **2.8" variant** (ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM, ILI9341 320×240, native USB). The 3.5"/4.0" variants are wired differently, so don't take pinouts from their docs.
- **Mics:** 4 × INMP441 (I2S MEMS, 24-bit, SNR 61 dB, SCK 0.5–3.2 MHz), arranged as two stereo pairs on one shared clock.
- **Serial / flashing:** `/dev/cu.usbmodem101`. Build and flash with `pio run -e fnk0104 -t upload --upload-port /dev/cu.usbmodem101` from `code/`. USB-CDC re-enumerates on every reset, so reopen the port after flashing, and expect to miss the first ~1 s of boot output.

### Pins (fixed; these are the board's only four free GPIOs)

| Signal | GPIO | Board socket |
|---|---|---|
| SCK / BCLK → all 4 mics | **GPIO2** | P3 (Expanded IO) pin 1 |
| WS / LRCLK → all 4 mics | **GPIO3** | P3 pin 2 |
| SD from Mic 1 (L) + Mic 2 (R) | **GPIO14** | P3 pin 3 |
| SD from Mic 3 (L) + Mic 4 (R) | **GPIO21** | P3 pin 4 |
| Mic VDD | 3V3 | P4 (I2C) pin 1 |
| Mic GND | GND | P4 pin 2 |

`L/R` straps: Mics 1 and 3 → GND (left slot), Mics 2 and 4 → 3V3 (right slot).

Don't touch these: GPIO4–8 (ES8311 codec I2S), GPIO1 (speaker amp enable; it's pulled to "off" by default, so leave it alone), GPIO15/16 (I2C shared by touch and codec), GPIO10–13/45/46 (display), GPIO38–41/47/48 (SD card), GPIO19/20 (USB). There are no spare GPIOs. A fifth mic is not possible: both I2S controllers are in use and each carries one stereo pair.

**Before any hardware test, ask the user whether the mics are wired.** They may not be yet.

### Geometry

The printed enclosure is `linear_array_large`: **4 mics in one row, 30 mm centre-to-centre**, 90 mm total aperture. Positions along the array axis:

| Mic | x (mm) |
|---|---|
| 1 | −45 |
| 2 | −15 |
| 3 | +15 |
| 4 | +45 |

Which physical mic sits at which position is **not yet verified**. Assume Mic 1 is leftmost when facing the mic side, then confirm with the tap test (milestone 1) and record the result in the code and the wiring doc.

Keep mic positions in one `(x, y)` table and compute every delay from it. Don't write `index × 30 mm` anywhere in the DSP. The enclosure also has a `circular_array` option: four mics on a square with 40 mm sides, at (±28.3, 0) and (0, ±28.3) mm, so the square is rotated 45° to the axes. Switching to it should only mean changing that table. So derive the DOA scan range and the spatial-aliasing band limit from the table too, not just the delays: the square aliases from c / (2 × 40 mm) ≈ 4.3 kHz, not 5.7 kHz.

## Conventions and what this geometry can and can't do

Put these in code comments and use them when you report results to the user:

- **Angle:** θ is measured from broadside, −90° to +90°, **positive toward Mic 4 (+x)**. c ≈ 343 m/s.
- **Delay:** τ_m(θ) = x_m · sin θ / c is how much *earlier* mic m hears a far-field plane wave than the array centre does. With the usual forward DFT (e^{−j2πkn/N}, which esp-dsp uses), that means X_m(f) = S(f) · e^{+j2πfτ_m}. **To align the channels, multiply by e^{−j2πfτ_m}.** Don't trust this sign on paper alone; the synthetic test below has to confirm it.
- **Delays are tiny:** 90 mm / c ≈ 262 µs end to end, which is **4.2 samples at 16 kHz**; adjacent mics are 1.4 samples apart. Integer-sample shifts are useless here. Do fractional delays as frequency-domain phase shifts, which are exact at any sample rate.
- **Spatial aliasing:** above c / (2 × 30 mm) ≈ **5.7 kHz**, DOA becomes ambiguous and grating lobes can appear. That figure is the worst case (steering to endfire); at broadside, grating lobes don't start until ~11.4 kHz. Restrict DOA and any adaptive weighting to **300 Hz–5.5 kHz**. Compute the upper limit from the smallest spacing in the geometry table, with a little margin, rather than writing 5.5 kHz as a literal. Don't cut the beam *output* to that band: below 300 Hz and above 5.5 kHz, pass plain delay-and-sum, which is still unity gain toward the look direction.
- **Low-frequency directivity is weak.** Below ~2 kHz the wavelength is large compared with the 90 mm aperture, so delay-and-sum is nearly omnidirectional there. Expect a modest overall gain: at most 6 dB against uncorrelated noise, and less in a reverberant room. Real low-frequency rejection needs MVDR or superdirective weights, and the mics' self-noise limits those.
- **A linear array only measures the angle to its axis.** Every source on a cone around that axis gives the same delays, so the array can't tell front from back or up from down. Report DOA as −90° to +90° and don't claim 360° localisation. The square `circular_array` geometry would resolve azimuth in the horizontal plane with the same four mics and pins, though it still couldn't tell above the plane from below it.

## Architecture

### 1. Synchronous 4-channel capture (the hard part)

The ESP32-S3 has two I2S controllers, and each reads **one** data line in standard mode. The INMP441 doesn't do TDM. Two data lines (GPIO14, GPIO21) therefore need both controllers running from the same clock:

- **I2S0 = master, RX:** drives BCLK on GPIO2 and WS on GPIO3, and reads GPIO14 (Mics 1/2).
- **I2S1 = slave, RX:** takes BCLK/WS as *inputs* from the same GPIO2/GPIO3, and reads GPIO21 (Mics 3/4).

Use the ESP-IDF 5 standard-mode driver directly (`driver/i2s_std.h`). Don't use the legacy `driver/i2s.h` or the Arduino `ESP_I2S` wrapper, because you need control over roles and pin routing.

- Channels: `I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER)` and `(I2S_NUM_1, I2S_ROLE_SLAVE)`, RX only.
- Clock: `I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE)` on both controllers, including the slave. Start at **16 kHz**; BCLK = 64 × fs = 1.024 MHz, inside the INMP441's range. Keep the sample rate a named constant. 48 kHz (3.072 MHz BCLK) also works if a test needs finer time resolution.
- **Slave sampling clock:** the slave samples the external BCLK with its own internal clock, set by `clk_cfg.bclk_div`. The IDF S3 docs say `bclk_div` must be at least 8 and that a larger value reduces lag; at 8, data can arrive more than one BCLK late and land in the wrong bit. `I2S_STD_CLK_DEFAULT_CONFIG` uses 8, the minimum, so set the slave's `bclk_div` explicitly to 16–32.
- Slots: `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)`. The INMP441 sends 24-bit, MSB-aligned two's-complement data in 32-bit slots, so arithmetic-shift each sample right by 8. It is not 16-bit PCM.
- GPIO: `mclk = I2S_GPIO_UNUSED`, `bclk = 2`, `ws = 3`, `dout = I2S_GPIO_UNUSED`, `din = 14` (I2S0) / `21` (I2S1).
- **Pad routing:** sharing GPIO2/3 between the master's outputs and the slave's inputs works with the stock driver in IDF 5.5.4. For inputs, the slave's pin setup (`i2s_gpio_check_and_set` in `esp_driver_i2s/i2s_common.c`) only selects the GPIO function, enables input and connects the input signal. It never turns off the master's output, so both routes survive in either init order, and the driver doesn't warn about the shared pins. Don't add a manual re-routing step. If you ever do need to touch the routing: an RX-only master drives BCLK/WS from `I2S0I_BCK_OUT_IDX` / `I2S0I_WS_OUT_IDX`. `I2S0O_BCK_OUT_IDX` / `I2S0O_WS_OUT_IDX` are the *TX* clocks and are idle without a TX channel, so routing those to the pins would stop the clock. The slave's inputs are `I2S1I_BCK_IN_IDX` / `I2S1I_WS_IN_IDX`. Check with the serial RMS output that both controllers deliver live data.
- **Data-line pull-downs:** the INMP441 tri-states SD after the LSB, and its datasheet asks for a pull-down on SD (100 kΩ is enough). The wiring has none, so call `gpio_pulldown_en(14)` and `gpio_pulldown_en(21)` after both channels are initialised, or fit external 100 kΩ resistors. With the pull-down in place, the low byte of every raw 32-bit slot should read 0 on both controllers. Check this on the raw words: a non-zero low byte means the data is shifted by a bit.
- **Start order:** enable I2S1 (slave) before I2S0 (master), so the slave is already running when BCLK/WS start (the master's enable is what starts them).
- **Alignment self-test:** the start order is sensible, but nothing in the driver guarantees that the slave captures the same first frame, or even the same slot, as the master. The master starts its first frame without a preceding WS edge, and the slave may need one, which could leave a fixed one-frame offset or a half-frame offset (left and right swapped). Measure it electrically instead of assuming. After every start and every resync, and after the start-up discard below, briefly route GPIO14 into I2S1 as well, with `esp_rom_gpio_connect_in_signal(14, I2S1I_SD_IN_IDX, false)`. Both controllers then receive Mics 1/2, so comparing the two streams sample by sample gives the exact frame/slot offset and any bit shift. Then route GPIO21 back with `esp_rom_gpio_connect_in_signal(21, I2S1I_SD_IN_IDX, false)`.
  - A constant offset that is the same on every start can be removed by dropping frames on the leading controller. Record the offset and the correction in the instrumentation.
  - An offset that changes from one start to the next is a real failure; report it to the user. The hardware fallback is PDM MEMS mics: I2S0 accepts up to four PDM data lines on one controller (`SOC_I2S_PDM_MAX_RX_LINES`), which would be synchronous by construction. Whether the S3's hardware PDM-to-PCM filter handles four lines at once is unverified.
- **Staying aligned:** the two controllers share a clock, so they can't drift, but a DMA overflow on only one of them shifts the pairing by a buffer from then on. Register `on_recv_q_ovf` on both channels, and keep a running frame count for each. The callback runs in the GDMA interrupt, and the Arduino build sets `CONFIG_I2S_ISR_IRAM_SAFE`:
  - the callback must be `IRAM_ATTR`, and its `user_ctx` must be in internal RAM, or registration fails with `ESP_ERR_INVALID_ARG`;
  - it must only set a flag or bump a counter: no floats, no blocking.

  Do the resync in the capture task, because the enable/disable calls take mutexes. On any overflow or count mismatch: disable master, disable slave, enable slave, enable master, drop the partial data, and count the resync. Stopping BCLK puts the INMP441 into standby, and it needs 2¹⁴ SCK cycles to wake up (16 ms at 1.024 MHz), so discard at least that much after each resync, then rerun the alignment self-test. Don't paper over resyncs in software.
- **Start-up:** the INMP441 outputs zeros for 2¹⁸ SCK cycles after the clock starts (datasheet): 256 ms at 16 kHz, 85 ms at 48 kHz. Compute the discard from the sample rate, with some margin (about 300 ms at 16 kHz).
- Verify the channel order in the DMA buffer (left slot first?) with the tap test rather than assuming it.
- DMA buffers go in internal RAM (e.g. 6 descriptors × 256 frames per controller). Large, rarely touched buffers (captures, lookup tables) can go in PSRAM.
- **Allocate the hot DSP buffers explicitly.** The Arduino build sends any plain `malloc` of 4 KB or more to PSRAM (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`), and a 512-point complex float buffer is exactly 4 KB. Allocate FFT and per-hop working buffers with `heap_caps_aligned_alloc(16, size, MALLOC_CAP_INTERNAL)`. 16-byte alignment is a safe choice for the SIMD FFTs; check esp-dsp's exact requirement.
- **Tasks:** run capture in a FreeRTOS task **pinned to core 1** at a priority above Arduino's `loopTask` (which also runs on core 1); WiFi runs on core 0. The capture task pairs frames from the two controllers into 4-channel frames and passes them through a ring buffer to a processing task, also on core 1, below capture and above `loopTask`. `loop()` must stay non-blocking.
- **Per channel conditioning:** a DC-blocking high-pass filter at ~80–100 Hz, then a per-mic gain trim, calibrated from RMS levels with a source at broadside. INMP441 sensitivity is −26 dBFS typical but can be anywhere from −29 to −23 dBFS, so the trim range must cover **±3 dB**. The datasheet gives only a typical low-frequency corner (60 Hz), so phase may also differ between units at low frequencies, which is a reason to keep the optional per-mic delay trim.
- Keep calibration (channel → position map, gain trims, optional per-mic delay trim) in one config struct, not scattered through the DSP. Persisting it in NVS is a nice-to-have.

### 2. Processing (frequency domain only)

Skip a separate time-domain delay-and-sum. SRP-PHAT and MVDR need the STFT anyway, and interpolated sample shifts are less accurate than phase shifts.

- **STFT:** 512-point FFT (32 ms at 16 kHz), hop 256, **periodic sqrt-Hann** window (w[n] = sin(πn/N)) on both analysis and synthesis, overlap-add. That window pair reconstructs exactly and tapers any artifacts once weights vary per bin. Generate the window in code, and don't use `dsps_wind_hann_f32`: it is the *symmetric* Hann (it divides by N−1), which ripples by ~0.03 dB on overlap-add and fails the reconstruction test below. Per 16 ms hop that's 4 forward FFTs + 1 inverse, which the S3 handles easily.
- **FFT:** use **esp-dsp** (`dsps_fft2r_fc32` / `dsps_fft4r_fc32`). arduino-esp32 3.3.9 already bundles esp-dsp 1.8.0 and links it by default, so just `#include "esp_dsp.h"`; no `idf_component.yml` needed. The S3 SIMD variants are picked automatically (`CONFIG_DSP_OPTIMIZED=1`). esp-dsp exists only as a prebuilt Xtensa library, so put the FFT behind a thin wrapper with an esp-dsp backend and a portable backend for the host tests. The wrapper hides esp-dsp's details: interleaved re/im data, a separate bit-reversal call (`dsps_bit_rev*`), its scaling, and the lack of a direct inverse FFT (use the conjugate trick).
- **DOA: SRP-PHAT.** For each frame, compute the PHAT-weighted cross-spectra G_ij = X_i X_j* / |X_i X_j*| for the 6 mic pairs. Then scan θ from −90° to +90° in 2° steps and sum Re{G_ij(f) · e^{−j2πf(τ_i(θ) − τ_j(θ))}} over pairs and over the 300 Hz–5.5 kHz bins; the peak is the DOA. Report a confidence value (e.g. peak-to-mean ratio of the scan). Update the estimate only when an energy/VAD gate says speech is present and the confidence clears a threshold, then smooth it over time.
- **GCC-PHAT** between individual pairs, with sub-sample peak interpolation, is the diagnostic for the sync test. It also makes a reasonable first cut at DOA before SRP-PHAT.
- **Beamformer, stage 1: delay-and-sum.** Y(f) = (1/M) Σ_m X_m(f) · e^{−j2πfτ_m(θ)} (sign as confirmed by the synthetic test), steered to the tracked DOA or to a fixed angle.
- **Stage 2 (optional, after stage 1 is verified): MVDR.** Estimate the noise covariance per bin from frames the VAD marks as non-speech, add diagonal loading so it doesn't amplify self-noise, and use delay-and-sum outside 300 Hz–5.5 kHz. Superdirective weights (MVDR against a diffuse-noise model) are a variant worth comparing. Keep the beamformer behind an interface, so weighted delay-and-sum, MVDR and superdirective can be swapped in.
- Keep DOA and beamforming in separate modules. The beamformer takes an angle; it doesn't care where the angle came from.
- **Post-processing** (noise suppression, AGC) is out of scope for now. If you add either, it must be switchable off, and it must be off for every measurement.

### 3. Modes

One mode variable, settable from the web UI:

| Mode | Behaviour |
|---|---|
| `RAW` | No beamforming; raw channels available for capture and streaming. |
| `DOA_ONLY` | DOA and confidence are computed and displayed; output is a single raw mic. |
| `FIXED_BEAM` | Beam steered to a user-set angle. |
| `AUTO_BEAM` | Beam follows the tracked DOA, with hysteresis: re-steer only after the smoothed DOA has moved more than ~10° for more than ~300 ms, then slew rather than jump. |

### 4. Outputs

- **TFT:** keep the WiFi status, and add the current DOA (a needle over −90° to +90°), confidence, mode and per-mic levels. Repaint only what changes; direct drawing is fine.
- **Port 80, existing synchronous `WebServer`** (quick handlers only):
  - `/` status page: add mode, DOA, confidence, per-mic RMS, overrun/resync counts, processing time, and links to the audio endpoints.
  - A control endpoint to set the mode and the fixed angle.
- **Port 81, a separate audio server task on core 0.** `WebServer` is synchronous, so a live-stream handler would block `loop()`; audio gets its own `WiFiServer`.
  - `/beam.wav`: live 16 kHz, 16-bit mono stream of the beamformed output (256 kbps). The same endpoint with a parameter streams a single raw mic, for A/B listening. If the client falls behind, drop audio and count it; never block the DSP.
  - `/raw.wav?s=N`: capture N seconds of all four channels into PSRAM (24-bit, 4-channel WAV; 10 s ≈ 1.9 MB), then serve the file. Capturing to RAM first means WiFi hiccups can't corrupt the timing analysis.
- **Serial:** a compact 1 Hz line with mode, DOA, confidence, levels, overruns/resyncs and DSP load. Print only from `loop()`, never from the capture or DSP tasks, and call `Serial.setTxTimeoutMs(0)`. USB-CDC writes otherwise block for up to 100 ms per chunk, with retries, when the Mac has the port open but isn't reading, which is the normal state with the board plugged in and no monitor running.

### 5. Instrumentation

Track and expose (status page + serial): DMA overflows and resyncs per controller, frame-count mismatch, ring-buffer depth (current and high-water), processing time per hop (mean and max) as a percentage of the 16 ms hop budget, stream drops, per-mic input RMS and beam output RMS. The processing time must stay well under the hop time, including while WiFi is streaming.

## Tests

### On the host (before hardware)

Add a PlatformIO `native` env, or plain C++ built on the Mac, that runs the DSP code on synthetic input. It uses the portable FFT backend, so also run a quick on-device check that the esp-dsp backend gives the same output for a fixed input.

- Generate a plane wave (broadband noise, fractional delays from the geometry table) at known angles. SRP-PHAT must return each angle within a couple of degrees, and delay-and-sum steered at the source must give ~unity gain, while steering away must attenuate high frequencies. This test is what settles the phase sign.
- STFT with identical inputs and θ = 0 must reconstruct the input (after the fixed latency) to within float precision.

### Offline analysis

Put a small Python script in `tools/` that reads a `/raw.wav` capture and prints per-pair GCC-PHAT lags (upsampled for sub-sample resolution) and per-channel RMS. It can also compute the beam response offline for the polar plots below.

## Milestones (verify each on hardware before moving on)

1. **Raw capture.** All four channels stream, with per-channel RMS on serial and no overflows. The low byte of each raw slot reads 0 on both controllers. The alignment self-test reports the same offset (ideally 0) across ~10 resets and forced resyncs. **Tap test:** tap each mic in turn; exactly one channel should spike. Record which physical position each channel maps to, and fix the geometry table if needed.
2. **Sync test.** Make a sharp click (clap, snap) at broadside, the same distance from all mics, and grab it with `/raw.wav`. Lags between all pairs should be about 0 samples, including across the controller boundary (Mic 2 ↔ Mic 3). Then click at the Mic 4 end (endfire): the Mic 1 ↔ Mic 4 lag should be about 4.2 samples at 16 kHz, and Mic 4 should lead. The alignment self-test should already have removed any whole-frame offset between the I2S0 pair and the I2S1 pair. An offset that remains here means the self-test or its correction is wrong. Repeat after a forced resync.
3. **Calibration.** With a broadband source at broadside, set the gain trims so the channels match to within ~0.5 dB.
4. **DOA.** Play speech from a phone at known angles (0°, ±30°, ±60°) about 1 m away. Report mean error, jitter, and how the estimate behaves in silence.
5. **Beam.** Measure beam output power against steering angle for a fixed source, and plot polar responses at 500 Hz, 1, 2, 3, 4 and 5 kHz (on-device, or offline from `/raw.wav`). Then listen via `/beam.wav` against a single raw mic in these cases: desired talker at broadside with an interferer off-axis; both talking at once; a stationary fan; a moving talker (checks `AUTO_BEAM` tracking); a reverberant room. Report measured numbers and don't overstate the gain.
6. **Optional:** MVDR and/or superdirective, compared against delay-and-sum on the same recordings.

At each milestone, also report RAM (internal and PSRAM) and DSP load.

## Constraints

- Don't break the WiFi/portal/mDNS behaviour, the BOOT-hold portal, or the non-blocking `loop()`.
- Don't initialise the ES8311 codec or drive GPIO1; the mic array uses both I2S controllers.
- Match the style of `main.cpp`: explain the *why* in comments, and give each state or buffer a comment explaining its purpose. Split audio into a few files in `code/` (e.g. `audio_capture.*`, `doa.*`, `beamformer.*`, `audio_server.*`), not one giant loop and not a dozen tiny modules.
- Keep geometry, sample rate, FFT size, hop and band limits as named constants in one place.
- If you change wiring assumptions or the mic order, update `inputs/4x_INMP441_ESP32_wiring_handoff.md` and `docs/esp32-inmp441-wiring.svg` to match.
- Commit only when the user asks; this repo has no remote and works on `main`.
