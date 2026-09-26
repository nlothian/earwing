// Audio constants shared by the capture, DSP and UI code, plus the mic
// geometry table and the per-mic calibration struct. Portable: the host tests
// (test/) build against this header too, so nothing here may pull in ESP-IDF.
//
// Conventions (used in every comment, printout and web page):
//   * Angle θ is measured from broadside, positive toward Mic 4 (+x). A linear
//     array reports −90°..+90°; a planar layout reports azimuth −180°..+180°
//     with the same zero (+y) and the same positive direction (toward +x).
//   * τ_m(θ) = (x_m·sinθ + y_m·cosθ) / c is how much EARLIER mic m hears a
//     far-field plane wave than the array centre does. With the forward DFT
//     (e^{−j2πkn/N}) that makes X_m(f) = S(f)·e^{+j2πfτ_m}, so aligning a
//     channel means multiplying it by e^{−j2πfτ_m}. test/host_tests.cpp checks
//     this sign against synthetic plane waves; don't change one without the other.
//   * A linear array only sees the angle to its own axis: every source on a
//     cone around the axis gives the same delays, so front/back and up/down
//     are indistinguishable. Never present its output as 360° localisation.
#pragma once

#include <stdint.h>

namespace audio {

// ---------- stream format ----------
// 16 kHz: BCLK = 64 × fs = 1.024 MHz, inside the INMP441's 0.5–3.2 MHz. 48 kHz
// (3.072 MHz BCLK) also works if a test needs finer time resolution; every
// derived quantity below follows this constant.
constexpr int   SAMPLE_RATE = 16000;
constexpr int   NUM_MICS    = 4;

// STFT: 512-point FFT (32 ms at 16 kHz), hop 256, periodic sqrt-Hann on both
// analysis and synthesis. The hop is also the DMA buffer length, so one DMA
// buffer per controller is exactly one hop of audio.
constexpr int   FFT_N       = 512;
constexpr int   HOP         = 256;
constexpr int   NUM_BINS    = FFT_N / 2 + 1;       // 0..Nyquist, real-signal half spectrum
constexpr float BIN_HZ      = (float)SAMPLE_RATE / FFT_N;
constexpr int   LATENCY     = FFT_N - HOP;         // STFT in → out delay, samples
constexpr float HOP_US      = 1e6f * HOP / SAMPLE_RATE;   // the DSP time budget per hop

constexpr float SPEED_OF_SOUND = 343.0f;           // m/s, ~20 °C

// ---------- band limits ----------
// DOA and any adaptive weighting only use BAND_LO_HZ .. the spatial-aliasing
// limit. The upper limit is NOT a literal: Geometry derives it from the
// smallest spacing in the table below (c / 2d, times ALIAS_MARGIN), which is
// ~5.5 kHz for the 30 mm linear array and ~4.1 kHz for the 40 mm square.
// Below 300 Hz the 90 mm aperture sees almost no phase difference, and room
// rumble dominates. The beam OUTPUT is never band-limited to this range:
// outside it the beamformer falls back to plain delay-and-sum.
constexpr float BAND_LO_HZ   = 300.0f;
constexpr float ALIAS_MARGIN = 0.96f;

// ---------- mic geometry ----------
// Positions in metres, in the array's own frame: x along the row (Mic 4 end
// positive), y toward broadside. Every delay, the DOA scan range and the band
// limit are computed from this table; nothing in the DSP assumes a spacing.
// Switching enclosure = switching the table.
struct MicPos { float x, y; };

// 3dprint/micarray.scad `linear_array_large`: one row, 30 mm centre-to-centre.
// Mic 1 is assumed leftmost when facing the mic side; the tap test (milestone
// 1) confirms this, and Calibration::posOfChannel corrects it if not.
constexpr MicPos GEOMETRY_LINEAR[NUM_MICS] = {
  {-0.045f, 0.0f}, {-0.015f, 0.0f}, {+0.015f, 0.0f}, {+0.045f, 0.0f},
};

// `circular_array`: a square of 40 mm sides, rotated 45° so the mics sit on the
// axes at ±40/√2 mm. The position order here is a placeholder until it is
// tap-tested. It resolves azimuth in the plane (still not above/below it) and
// aliases from c / (2 × 40 mm) ≈ 4.3 kHz.
constexpr float SQ_R = 0.040f / 1.41421356f;
constexpr MicPos GEOMETRY_SQUARE[NUM_MICS] = {
  {-SQ_R, 0.0f}, {0.0f, +SQ_R}, {0.0f, -SQ_R}, {+SQ_R, 0.0f},
};

// The geometry the firmware runs with.
constexpr const MicPos* ACTIVE_GEOMETRY = GEOMETRY_LINEAR;

// ---------- DOA / tracking ----------
constexpr float DOA_STEP_DEG        = 2.0f;    // SRP-PHAT scan resolution (peak is interpolated)
// SRP-PHAT and GCC-PHAT sum the PHAT cross-spectra over blocks of adjacent
// bins before scanning. Across a block, the steering phase can't turn by
// more than this even at the largest possible lag (the full aperture), so a
// block behaves like one bin at its centre: ~1% amplitude loss, and no bias
// because the blocks are symmetric. It cuts the scan cost by the block size
// (9× for the 90 mm array); Geometry derives the block size from the aperture.
constexpr float DOA_BLOCK_SPAN_RAD  = 0.5f;
constexpr float DOA_MIN_CONFIDENCE  = 0.15f;   // peak − mean of the normalised scan; tune on hardware
constexpr float DOA_SMOOTH_ALPHA    = 0.25f;   // per accepted hop, on the direction unit vector
constexpr float GATE_OPEN_DB        = 8.0f;    // speech gate: band energy over the noise floor
constexpr float GATE_FLOOR_RISE_DB_S = 3.0f;   // noise floor creep-up rate when not at a minimum
constexpr float GATE_HANGOVER_MS    = 200.0f;  // keep the gate open across short pauses
constexpr float GATE_HI_HZ          = 4000.0f; // speech-energy band top (low edge = BAND_LO_HZ)

// AUTO_BEAM: re-steer only after the smoothed DOA has sat more than
// STEER_HYST_DEG from the beam for STEER_HOLD_MS, then slew instead of jumping.
constexpr float STEER_HYST_DEG       = 10.0f;
constexpr float STEER_HOLD_MS        = 300.0f;
constexpr float STEER_SLEW_DEG_S     = 90.0f;

// ---------- conditioning ----------
constexpr float HPF_HZ          = 90.0f;   // DC block + rumble; 2nd-order Butterworth
constexpr float GAIN_TRIM_MAX_DB = 6.0f;   // INMP441 sensitivity spread is ±3 dB; allow margin
constexpr float ADVANCE_TRIM_MAX_US = 500.0f;   // per-mic time trim; ~2× the aperture's 262 µs

// Fixed gain applied only when converting to 16-bit for /beam.wav, so speech
// at a metre (~−60 dBFS at the INMP441's −26 dBFS/94 dB SPL) is audible. It is
// the same for beam and raw-mic streams so A/B listening stays fair. Not AGC:
// every level the firmware reports is measured before it.
constexpr float STREAM_GAIN_DB = 24.0f;

// ---------- calibration ----------
// Everything that depends on the particular mics and how they were wired, in
// one place. "Channel" is the capture slot: 0 = I2S0 left (Mic 1, GPIO14,
// L/R→GND), 1 = I2S0 right (Mic 2), 2 = I2S1 left (Mic 3, GPIO21), 3 = I2S1
// right (Mic 4). Trims belong to the physical mic, so they are per channel.
struct Calibration {
  uint8_t posOfChannel[NUM_MICS];   // geometry-table row each channel's mic sits at
  float   gainDb[NUM_MICS];         // added after the high-pass; matches sensitivities
  float   advanceUs[NUM_MICS];      // fractional time trim: + means the channel is
                                    // recorded late and gets moved earlier
};

constexpr Calibration DEFAULT_CALIBRATION = {
  {0, 1, 2, 3},
  {0.0f, 0.0f, 0.0f, 0.0f},
  {0.0f, 0.0f, 0.0f, 0.0f},
};

}  // namespace audio
