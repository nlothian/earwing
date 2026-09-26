// Portable DSP building blocks: FFT wrapper, periodic sqrt-Hann STFT,
// high-pass biquad and the geometry derived from the mic table. Everything
// here builds on the host (test/) and on the ESP32-S3; only the FFT backend
// and the allocator differ.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "audio_config.h"

namespace dsp {

// 16-byte aligned, internal-RAM allocation for hot buffers. The Arduino build
// sends every plain malloc >= 4 KB to PSRAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
// = 4096), and a 512-point complex float buffer is exactly 4 KB, so DSP code
// must not use malloc/new for its working buffers. 16 bytes is what the S3's
// 128-bit SIMD loads in esp-dsp's aes3 FFT need.
void* alloc(size_t bytes);
void  release(void* p);

// ---------- FFT ----------
// Complex, in place, interleaved {re, im} (esp-dsp's layout), natural order in
// and out. forward: X[k] = Σ x[n]·e^{−j2πkn/N}, unscaled. inverse: includes
// the 1/N, so inverse(forward(x)) == x. esp-dsp has no inverse, so both
// backends use the conjugate trick: ifft(X) = conj(fft(conj(X))) / N.
enum class FftBackend : uint8_t { Portable, EspDsp };

#if defined(ESP_PLATFORM)
constexpr FftBackend DEFAULT_FFT_BACKEND = FftBackend::EspDsp;
#else
constexpr FftBackend DEFAULT_FFT_BACKEND = FftBackend::Portable;   // esp-dsp is Xtensa-only
#endif

class Fft {
 public:
  bool init(int n, FftBackend backend = DEFAULT_FFT_BACKEND);
  void forward(float* data) const;
  void inverse(float* data) const;
  int  size() const { return n_; }

 private:
  void portableForward(float* data) const;
  int        n_ = 0;
  FftBackend backend_ = FftBackend::Portable;
  float*     tw_ = nullptr;    // portable: cos/sin pairs for k < N/2
  uint16_t*  rev_ = nullptr;   // portable: bit-reversal permutation
};

// Periodic sqrt-Hann, w[n] = sin(πn/N). Its square is the periodic Hann, which
// sums to exactly 1 at 50% overlap, so analysis × synthesis reconstructs.
// (esp-dsp's dsps_wind_hann_f32 is the symmetric Hann, /(N−1): it ripples by
// ~0.03 dB on overlap-add, so it isn't used.)
void sqrtHannPeriodic(float* w, int n);

// ---------- STFT ----------
// One channel's analysis: keeps the last FFT_N samples; each push of HOP new
// samples yields the windowed spectrum of that frame (full N complex bins).
class StftAnalyzer {
 public:
  bool init(const Fft* fft, const float* window);
  // spectrum: 2·N floats, or null to only keep the history current (RAW mode
  // skips the FFT but must not come back to a stale frame)
  void push(const float* hop, float* spectrum);
 private:
  const Fft*   fft_ = nullptr;
  const float* win_ = nullptr;
  float*       hist_ = nullptr;   // last N input samples, oldest first
};

// Synthesis: inverse FFT, window again, overlap-add. Output lags the input by
// audio::LATENCY (= N − HOP) samples.
class StftSynth {
 public:
  bool init(const Fft* fft, const float* window);
  // spectrum: 2·N floats, conjugate-symmetric, consumed (used as scratch)
  void pull(float* spectrum, float* hopOut);
  void reset();   // drop the overlap tail, e.g. when a beam mode starts
 private:
  const Fft*   fft_ = nullptr;
  const float* win_ = nullptr;
  float*       ola_ = nullptr;    // overlap-add accumulator, N samples
};

// Fill bins N/2+1..N−1 from 1..N/2−1 so the inverse FFT of a real signal's
// half spectrum comes out real. Forces DC and Nyquist real.
void makeHermitian(float* spectrum, int n);

// ---------- conditioning ----------
// 2nd-order Butterworth high-pass, direct form I (better behaved in float than
// DF-II when the corner is this close to DC).
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  void  highpass(float fs, float fc);
  float step(float x) {
    float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x; y2 = y1; y1 = y;
    return y;
  }
};

// ---------- geometry ----------
// Everything the DSP needs to know about the layout, derived from the table
// so that swapping ACTIVE_GEOMETRY changes delays, scan range and band limit
// together.
struct Geometry {
  audio::MicPos pos[audio::NUM_MICS];
  bool  linear = true;        // all mics on the x axis: DOA is −90..+90 (a cone)
  float minSpacing = 0;       // m, closest pair: sets the aliasing limit
  float aperture = 0;         // m, farthest pair
  float aliasHz = 0;          // c / (2 · minSpacing)
  float bandHiHz = 0;         // aliasHz · ALIAS_MARGIN, top of the DOA band
  int   binLo = 0, binHi = 0; // DOA band as FFT bins, inclusive; binHi is rounded
                              // down so the band is a whole number of blocks
  int   blockBins = 1;        // bins per DOA block (see DOA_BLOCK_SPAN_RAD)
  int   numBlocks = 0;
  float scanMinDeg = 0, scanMaxDeg = 0;   // DOA scan range (planar: max excluded)

  bool  init(const audio::MicPos* table);
  // τ_m(θ): how much earlier mic m (geometry row) hears a plane wave from θ
  // than the array centre, in seconds.
  float delay(int m, float thetaDeg) const;
};

// Wrap an angle difference into [−180, 180).
float wrapDeg(float d);

constexpr float PI_F = 3.14159265358979f;
inline float deg2rad(float d) { return d * (PI_F / 180.0f); }
inline float rad2deg(float r) { return r * (180.0f / PI_F); }

}  // namespace dsp
