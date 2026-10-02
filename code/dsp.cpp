#include "dsp.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#include "esp_dsp.h"
#endif

namespace dsp {

using namespace audio;

// ---------- allocation ----------
void* alloc(size_t bytes) {
  bytes = (bytes + 15) & ~(size_t)15;   // aligned_alloc wants a multiple of the alignment
#if defined(ESP_PLATFORM)
  void* p = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
  void* p = aligned_alloc(16, bytes);
#endif
  if (p) memset(p, 0, bytes);
  return p;
}

void release(void* p) {
#if defined(ESP_PLATFORM)
  heap_caps_free(p);
#else
  free(p);
#endif
}

// ---------- FFT ----------
#if defined(ESP_PLATFORM)
// esp-dsp keeps one global twiddle table; it can only be initialised once, so
// remember the size it was built for.
static int espTableSize = 0;
#endif

bool Fft::init(int n, FftBackend backend) {
  if (n < 2 || (n & (n - 1))) return false;   // radix-2 only
  n_ = n;
  backend_ = backend;
  if (backend == FftBackend::EspDsp) {
#if defined(ESP_PLATFORM)
    if (espTableSize >= n) return true;
    if (espTableSize) return false;   // built smaller; esp-dsp can't re-init
    // Our own buffer, so the table lands in internal RAM rather than wherever
    // esp-dsp's internal malloc would put it (PSRAM, for >= 4 KB).
    float* table = (float*)alloc(sizeof(float) * n);
    if (!table || dsps_fft2r_init_fc32(table, n) != ESP_OK) return false;
    espTableSize = n;
    return true;
#else
    return false;
#endif
  }
  tw_ = (float*)alloc(sizeof(float) * n);          // n/2 complex twiddles
  rev_ = (uint16_t*)alloc(sizeof(uint16_t) * n);
  if (!tw_ || !rev_) return false;
  for (int k = 0; k < n / 2; k++) {
    double a = -2.0 * M_PI * k / n;                // e^{−j2πk/N}
    tw_[2 * k] = (float)cos(a);
    tw_[2 * k + 1] = (float)sin(a);
  }
  int bits = 0;
  while ((1 << bits) < n) bits++;
  for (int i = 0; i < n; i++) {
    int r = 0;
    for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
    rev_[i] = (uint16_t)r;
  }
  return true;
}

// Iterative radix-2 decimation-in-time: permute, then log2(N) butterfly passes.
void Fft::portableForward(float* d) const {
  const int n = n_;
  for (int i = 0; i < n; i++) {
    int r = rev_[i];
    if (r > i) {
      float t0 = d[2 * i], t1 = d[2 * i + 1];
      d[2 * i] = d[2 * r]; d[2 * i + 1] = d[2 * r + 1];
      d[2 * r] = t0; d[2 * r + 1] = t1;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const int half = len / 2, step = n / len;
    for (int i = 0; i < n; i += len) {
      for (int j = 0; j < half; j++) {
        const float wr = tw_[2 * j * step], wi = tw_[2 * j * step + 1];
        float* a = d + 2 * (i + j);
        float* b = d + 2 * (i + j + half);
        const float vr = b[0] * wr - b[1] * wi;
        const float vi = b[0] * wi + b[1] * wr;
        b[0] = a[0] - vr; b[1] = a[1] - vi;
        a[0] += vr;       a[1] += vi;
      }
    }
  }
}

void Fft::forward(float* d) const {
#if defined(ESP_PLATFORM)
  if (backend_ == FftBackend::EspDsp) {
    dsps_fft2r_fc32(d, n_);       // output in bit-reversed order...
    dsps_bit_rev2r_fc32(d, n_);   // ...so put it back: table-driven, 6× faster
                                  // than dsps_bit_rev_fc32 (the boot check compares)
    return;
  }
#endif
  portableForward(d);
}

void Fft::inverse(float* d) const {
  for (int i = 0; i < n_; i++) d[2 * i + 1] = -d[2 * i + 1];
  forward(d);
  const float s = 1.0f / n_;
  for (int i = 0; i < n_; i++) {
    d[2 * i] *= s;
    d[2 * i + 1] *= -s;
  }
}

void sqrtHannPeriodic(float* w, int n) {
  for (int i = 0; i < n; i++) w[i] = (float)sin(M_PI * i / n);
}

// ---------- STFT ----------
bool StftAnalyzer::init(const Fft* fft, const float* window) {
  fft_ = fft;
  win_ = window;
  hist_ = (float*)alloc(sizeof(float) * fft->size());
  return hist_ != nullptr;
}

void StftAnalyzer::push(const float* hop, float* spec) {
  const int n = fft_->size();
  memmove(hist_, hist_ + HOP, sizeof(float) * (n - HOP));
  memcpy(hist_ + n - HOP, hop, sizeof(float) * HOP);
  if (!spec) return;
  for (int i = 0; i < n; i++) {
    spec[2 * i] = hist_[i] * win_[i];
    spec[2 * i + 1] = 0.0f;
  }
  fft_->forward(spec);
}

bool StftSynth::init(const Fft* fft, const float* window) {
  fft_ = fft;
  win_ = window;
  ola_ = (float*)alloc(sizeof(float) * fft->size());
  return ola_ != nullptr;
}

void StftSynth::pull(float* spec, float* out) {
  const int n = fft_->size();
  fft_->inverse(spec);
  for (int i = 0; i < n; i++) ola_[i] += spec[2 * i] * win_[i];
  // The oldest HOP samples now have both overlapping frames summed in.
  memcpy(out, ola_, sizeof(float) * HOP);
  memmove(ola_, ola_ + HOP, sizeof(float) * (n - HOP));
  memset(ola_ + n - HOP, 0, sizeof(float) * HOP);
}

void StftSynth::reset() { memset(ola_, 0, sizeof(float) * fft_->size()); }

void makeHermitian(float* s, int n) {
  s[1] = 0.0f;
  s[n + 1] = 0.0f;   // Nyquist bin n/2: imag part at index 2·(n/2)+1
  for (int k = 1; k < n / 2; k++) {
    s[2 * (n - k)] = s[2 * k];
    s[2 * (n - k) + 1] = -s[2 * k + 1];
  }
}

// ---------- conditioning ----------
// RBJ cookbook high-pass with Q = 1/√2 (Butterworth).
void Biquad::highpass(float fs, float fc) {
  const double w0 = 2.0 * M_PI * fc / fs;
  const double alpha = sin(w0) / (2.0 * M_SQRT1_2);
  const double c = cos(w0);
  const double a0 = 1.0 + alpha;
  b0 = (float)((1.0 + c) / 2.0 / a0);
  b1 = (float)(-(1.0 + c) / a0);
  b2 = b0;
  a1 = (float)(-2.0 * c / a0);
  a2 = (float)((1.0 - alpha) / a0);
  x1 = x2 = y1 = y2 = 0.0f;
}

// ---------- geometry ----------
bool Geometry::init(const MicPos* table) {
  for (int m = 0; m < NUM_MICS; m++) pos[m] = table[m];
  linear = true;
  for (int m = 0; m < NUM_MICS; m++)
    if (fabsf(pos[m].y - pos[0].y) > 1e-6f) linear = false;
  minSpacing = 1e9f;
  aperture = 0.0f;
  for (int i = 0; i < NUM_MICS; i++)
    for (int j = i + 1; j < NUM_MICS; j++) {
      float d = hypotf(pos[i].x - pos[j].x, pos[i].y - pos[j].y);
      if (d < minSpacing) minSpacing = d;
      if (d > aperture) aperture = d;
    }
  if (minSpacing < 1e-4f) return false;   // two mics on top of each other
  aliasHz = SPEED_OF_SOUND / (2.0f * minSpacing);
  bandHiHz = aliasHz * ALIAS_MARGIN;
  if (bandHiHz > SAMPLE_RATE / 2.0f) bandHiHz = SAMPLE_RATE / 2.0f;
  binLo = (int)ceilf(BAND_LO_HZ / BIN_HZ);
  binHi = (int)floorf(bandHiHz / BIN_HZ);
  if (binHi >= NUM_BINS) binHi = NUM_BINS - 1;
  // Steering phase turns by 2π·Δf·τ per bin; τ is at most aperture / c.
  const float radPerBin = 2.0f * PI_F * BIN_HZ * aperture / SPEED_OF_SOUND;
  const int nBins = binHi - binLo + 1;
  if (nBins < 2) return false;   // no band left below the alias limit
  blockBins = (int)floorf(DOA_BLOCK_SPAN_RAD / radPerBin);
  if (blockBins < 1) blockBins = 1;
  if (blockBins > nBins) blockBins = nBins;
  numBlocks = nBins / blockBins;
  binHi = binLo + numBlocks * blockBins - 1;
  // A line only sees the angle to its own axis; a plane sees full azimuth.
  scanMinDeg = linear ? -90.0f : -180.0f;
  scanMaxDeg = linear ? +90.0f : +180.0f;
  return binHi > binLo;
}

float Geometry::delay(int m, float thetaDeg) const {
  const float t = deg2rad(thetaDeg);
  return (pos[m].x * sinf(t) + pos[m].y * cosf(t)) / SPEED_OF_SOUND;
}

float wrapDeg(float d) {
  if (!isfinite(d)) return 0.0f;
  d = fmodf(d + 180.0f, 360.0f);   // fmod, not a loop: huge inputs must not spin
  if (d < 0.0f) d += 360.0f;
  return d - 180.0f;
}

}  // namespace dsp
