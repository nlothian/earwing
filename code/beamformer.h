// Frequency-domain beamformers behind one interface, so weighted
// delay-and-sum, MVDR and superdirective weights can be swapped in and
// compared on the same recordings. A beamformer takes a look direction; it
// doesn't know or care whether that came from the DOA tracker or the web UI.
//
// Spectra are one pointer per geometry row, 2·FFT_N interleaved floats; only
// bins 0..FFT_N/2 are read or written (the caller mirrors the output with
// dsp::makeHermitian before the inverse FFT).
#pragma once

#include "dsp.h"

namespace bf {

using audio::NUM_MICS;

class Beamformer {
 public:
  virtual ~Beamformer() {}
  virtual void steer(float thetaDeg) = 0;
  virtual void apply(const float* const X[NUM_MICS], float* Y) = 0;
  virtual float steering() const = 0;
  virtual const char* name() const = 0;
};

// Y(f) = (1/M) Σ_m X_m(f)·e^{−j2πfτ_m(θ)}: undoes each mic's lead so the look
// direction adds in phase. Unity gain toward θ at every frequency, which is
// also why it's the fallback outside the DOA band for adaptive designs.
// Directivity is weak below ~2 kHz, where the 90 mm aperture is small next to
// the wavelength; expect at most 6 dB against uncorrelated noise overall.
class DelayAndSum : public Beamformer {
 public:
  bool init(const dsp::Geometry& g);
  void steer(float thetaDeg) override;
  void apply(const float* const X[NUM_MICS], float* Y) override;
  float steering() const override { return theta_; }
  const char* name() const override { return "delay-and-sum"; }

 private:
  dsp::Geometry g_;
  float* w_ = nullptr;   // [m][bin] weights e^{−j2πfτ_m}/M, bins 0..N/2
  float  theta_ = 0;
};

}  // namespace bf
