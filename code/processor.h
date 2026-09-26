// The per-hop audio pipeline, independent of where the samples come from:
// conditioning → STFT → DOA → steering → beam → overlap-add. The firmware's
// processing task feeds it from the capture ring; test/host_tests.cpp feeds it
// synthetic plane waves. Holds no ESP-IDF state.
#pragma once

#include "beamformer.h"
#include "doa.h"

namespace proc {

using audio::NUM_MICS;
using audio::HOP;

enum class Mode : uint8_t {
  Raw,         // no spectral work at all; output = raw mic (geometry row 0)
  DoaOnly,     // DOA computed and reported; output = raw mic (geometry row 0)
  FixedBeam,   // beam steered to the user's angle
  AutoBeam,    // beam follows the tracked DOA, with hysteresis and slew
};
const char* modeName(Mode m);
bool        parseMode(const char* s, Mode* out);   // "raw", "doa", "fixed", "auto"

// AUTO_BEAM steering: move only after the smoothed DOA has been more than
// STEER_HYST_DEG away for STEER_HOLD_MS, then slew at STEER_SLEW_DEG_S until
// it catches up, so the beam neither chases noise nor jumps audibly.
class SteerTracker {
 public:
  void  init(bool linear, float startDeg);
  float update(float doaDeg);   // one hop; returns the steering angle
  void  set(float deg) { steer_ = deg; slewing_ = false; hold_ = 0; }
  float steer() const { return steer_; }
  bool  slewing() const { return slewing_; }
 private:
  float steer_ = 0, stepDeg_ = 0;
  int   hold_ = 0, holdHops_ = 0;
  bool  slewing_ = false, linear_ = true;
};

// Pairs whose GCC-PHAT lead is reported: each adjacent pair of geometry rows,
// then the two ends (the full aperture).
constexpr int NUM_LEADS = NUM_MICS;
void leadPair(int n, int* i, int* j);

// Make a calibration safe to apply: the channel map a permutation (else the
// identity), trims finite and within GAIN_TRIM_MAX_DB / ADVANCE_TRIM_MAX_US.
// Used by the Processor and by whoever stores the calibration, so what is
// shown and saved is what is applied.
audio::Calibration sanitizeCalibration(const audio::Calibration& c);

struct HopResult {
  float micDb[NUM_MICS];       // per capture channel, after high-pass and gain trim (dBFS)
  float micPreTrimDb[NUM_MICS];// per capture channel, before the trim (calibration input)
  float beamDb;                // output hop (dBFS)
  bool  spectral;              // STFT/DOA ran this hop (every mode but RAW)
  bool  speech;                // energy gate open
  bool  doaAccepted;           // gate open and confident: this hop moved the estimate
  doa::DoaResult doa;          // this hop's scan, accepted or not
  float smoothDeg;             // the tracked direction
  bool  smoothValid;           // at least one hop has been accepted
  float steerDeg;              // where the beam points (beam modes)
  bool  leadsUpdated;          // lead[] was measured this hop (gate open)
  float lead[NUM_LEADS];       // samples row j leads row i, see leadPair()
  float leadCoh[NUM_LEADS];    // normalised GCC peak, 1 = clean plane wave
};

class Processor {
 public:
  bool init(const audio::MicPos* table, const audio::Calibration& cal);
  void setCalibration(const audio::Calibration& cal);
  const audio::Calibration& calibration() const { return cal_; }
  void setMode(Mode m);
  Mode mode() const { return mode_; }
  void setFixedAngle(float deg);
  float fixedAngle() const { return fixedDeg_; }
  const dsp::Geometry& geometry() const { return geo_; }

  // in:      HOP frames × NUM_MICS, capture-channel order, full scale ±1.
  // beamOut: HOP samples: the beam in FIXED/AUTO, else geometry row 0's mic.
  // micOut:  NUM_MICS × HOP, geometry-row order, conditioned and delayed by
  //          audio::LATENCY so it lines up sample for sample with beamOut.
  //          May be null.
  void process(const float* in, float* beamOut, float* micOut, HopResult* r);

 private:
  bool beamMode() const { return mode_ == Mode::FixedBeam || mode_ == Mode::AutoBeam; }

  dsp::Geometry geo_;
  audio::Calibration cal_;
  int   chOfRow_[NUM_MICS];     // inverse of cal_.posOfChannel
  float gainLin_[NUM_MICS];     // per channel, from cal_.gainDb
  Mode  mode_ = Mode::DoaOnly;
  float fixedDeg_ = 0;

  dsp::Fft fft_;
  float*   window_ = nullptr;             // periodic sqrt-Hann, FFT_N
  dsp::Biquad hpf_[NUM_MICS];             // per channel
  dsp::StftAnalyzer ana_[NUM_MICS];       // per geometry row
  dsp::StftSynth    syn_;
  float*   cond_ = nullptr;               // [row][HOP] conditioned time signal this hop
  float*   spec_[NUM_MICS] = {};          // [row] 2·FFT_N, this hop's spectrum
  float*   out_ = nullptr;                // 2·FFT_N beam spectrum / IFFT scratch
  float*   delay_ = nullptr;              // [row][LATENCY + HOP] raw-mic delay line

  doa::SrpPhat srp_;
  doa::EnergyGate gate_;
  doa::DirectionSmoother smooth_;
  SteerTracker steer_;
  bf::DelayAndSum das_;
  bf::Beamformer* beam_ = &das_;
};

// The board's own mic (through its ES8311 codec), for the "inbuilt" source.
// It is one channel at an unknown position on a different ADC chain from the
// array, so nothing spatial can be done with it, and the Processor, with its
// mode, fixed angle and state, is left alone while it plays, ready for the
// array's return. It gets the array channels' high-pass (same HPF_HZ, same
// filter) and nothing else: the channel map and the trims describe the
// INMP441s, not this mic. No STFT either, so there is no LATENCY to line up.
class MonoConditioner {
 public:
  void  init();
  // in, out: HOP samples, full scale ±1; may be the same buffer. Returns the
  // hop's level after the high-pass, dBFS.
  float process(const float* in, float* out);
 private:
  dsp::Biquad hpf_;
};

}  // namespace proc
