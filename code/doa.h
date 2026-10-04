// Direction of arrival: SRP-PHAT over the mic table, a GCC-PHAT pair lag for
// the sync diagnostics, and the gate/smoothing that decide when a scan is
// allowed to move the estimate. Knows nothing about beamforming; the
// beamformer just takes an angle.
//
// Spectra are passed as one pointer per geometry row (not capture channel),
// each 2·FFT_N interleaved floats, of which bins 0..FFT_N/2 are read.
#pragma once

#include "dsp.h"

namespace doa {

using audio::NUM_MICS;

struct DoaResult {
  float thetaDeg = 0;     // scan peak, parabolically interpolated
  float confidence = 0;   // peak − mean of the normalised scan, 0..2
  float peak = 0;         // normalised peak: 1 = every pair and bin agree
};

// Steered response power with PHAT weighting. For each pair (i, j) the
// cross-spectrum X_i·X_j* is normalised to unit magnitude, then for every scan
// angle θ, Σ Re{G_ij(f)·e^{−j2πf(τ_i(θ) − τ_j(θ))}} over pairs and DOA-band
// bins is the score; the peak is the DOA. Two reductions keep it cheap:
// pairs with the same baseline vector (three 30 mm pairs, two 60 mm pairs on
// the linear array) always want the same phase ramp, so their cross-spectra
// are summed and scanned once; and bins are summed in blocks of
// Geometry::blockBins, across which the ramp barely turns.
class SrpPhat {
 public:
  bool init(const dsp::Geometry& g, float stepDeg);
  DoaResult estimate(const float* const X[NUM_MICS]);

 private:
  static constexpr int MAX_PAIRS = NUM_MICS * (NUM_MICS - 1) / 2;
  struct Group {
    float bx, by;                  // baseline pos_i − pos_j, metres
    int   n;                       // pairs sharing it
    int8_t i[MAX_PAIRS], j[MAX_PAIRS], sign[MAX_PAIRS];   // −1: pair is the reverse baseline
  };
  dsp::Geometry g_;
  Group  groups_[MAX_PAIRS];
  int    nGroups_ = 0, nPairs_ = 0, nAngles_ = 0, nBins_ = 0;
  float* angles_ = nullptr;   // [a] degrees
  float* ph0_ = nullptr;      // [a][g] e^{−j2π f_0 Δτ}, f_0 = first block's centre
  float* phStep_ = nullptr;   // [a][g] e^{−j2π B·Δf Δτ}: steps one block
  float* xn_ = nullptr;       // [m][bin] unit-magnitude spectra over the band
  float* gsum_ = nullptr;     // [g][block] PHAT cross-spectra summed over pairs and bins
  float* scores_ = nullptr;   // [a]
};

// How many samples mic j (geometry row) leads mic i, τ_j − τ_i · fs, from the
// PHAT cross-spectrum over the DOA band. Positive = j hears the sound first.
// Evaluated directly in the frequency domain (coarse grid, then a fine one)
// on the same bin blocks as SRP-PHAT, so it is sub-sample exact rather than a
// parabola through integer lags.
// `coherence` gets the normalised peak (1 = perfect plane wave).
float gccPhatLead(const float* Xi, const float* Xj, const dsp::Geometry& g,
                  int i, int j, float* coherence);

// Energy gate standing in for a VAD: open when the speech-band energy is
// GATE_OPEN_DB above a tracked noise floor (drops to any new minimum at once,
// creeps up slowly), held open for a short hangover.
class EnergyGate {
 public:
  void init();
  bool update(float energy);
  bool  open() const { return hang_ > 0; }
 private:
  float floor_ = 0, rise_ = 1, openRatio_ = 1;
  int   hang_ = 0, hangHops_ = 0;
  bool  primed_ = false;
};

// A linear array on a table measures θ, the cone angle to its axis. A talker
// at table azimuth az, raised elevDeg above the table plane, reads
// sinθ = sin(az)·cos(el), so height pulls every reading toward broadside, most
// near the axis. This inverts that for an assumed elevation:
// az = asin(sinθ / cos(el)), clamped to ±90° where |sinθ| > cos(el). A planar
// layout's azimuth isn't biased by elevation (every delay shrinks by the same
// cos(el)), so it is returned as is.
float tableAzimuthDeg(float thetaDeg, bool linear, float elevDeg);

// Onset detector: true while the energy is DOA_ONSET_DB over an exponential
// average of the hops before it, i.e. on the rising edge of a syllable.
class OnsetDetector {
 public:
  void init();
  bool update(float energy);
 private:
  float ref_ = 0, alpha_ = 1, ratio_ = 1;
  bool  primed_ = false;
};

// Exponential smoothing of a direction, done on its unit vector so it also
// works across ±180° for a planar array.
class DirectionSmoother {
 public:
  void  reset(float deg);
  void  update(float deg, float alpha);
  float deg() const;
  bool  valid() const { return valid_; }
 private:
  float vx_ = 0, vy_ = 1;
  bool  valid_ = false;
};

}  // namespace doa
