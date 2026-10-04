#include "processor.h"

#include <math.h>
#include <string.h>
#include <strings.h>

namespace proc {

using namespace audio;

const char* modeName(Mode m) {
  switch (m) {
    case Mode::Raw:       return "RAW";
    case Mode::DoaOnly:   return "DOA_ONLY";
    case Mode::FixedBeam: return "FIXED_BEAM";
    case Mode::AutoBeam:  return "AUTO_BEAM";
  }
  return "?";
}

bool parseMode(const char* s, Mode* out) {
  if (!strcasecmp(s, "raw"))                                   *out = Mode::Raw;
  else if (!strcasecmp(s, "doa") || !strcasecmp(s, "doa_only")) *out = Mode::DoaOnly;
  else if (!strcasecmp(s, "fixed") || !strcasecmp(s, "fixed_beam")) *out = Mode::FixedBeam;
  else if (!strcasecmp(s, "auto") || !strcasecmp(s, "auto_beam"))   *out = Mode::AutoBeam;
  else return false;
  return true;
}

void leadPair(int n, int* i, int* j) {
  if (n < NUM_MICS - 1) { *i = n; *j = n + 1; }
  else                  { *i = 0; *j = NUM_MICS - 1; }
}

static float dbfs(float sumSq, int n) { return 10.0f * log10f(sumSq / n + 1e-20f); }

// ---------- steering ----------
void SteerTracker::init(bool linear, float startDeg) {
  linear_ = linear;
  const float hopSec = (float)HOP / SAMPLE_RATE;
  stepDeg_ = STEER_SLEW_DEG_S * hopSec;
  holdHops_ = (int)ceilf(STEER_HOLD_MS / 1000.0f / hopSec);
  set(startDeg);
}

float SteerTracker::update(float doaDeg) {
  const float diff = dsp::wrapDeg(doaDeg - steer_);
  if (!slewing_) {
    // The excursion has to last: any hop back inside the band restarts the clock.
    if (fabsf(diff) > STEER_HYST_DEG) { if (++hold_ >= holdHops_) slewing_ = true; }
    else hold_ = 0;
  }
  if (slewing_) {
    // While slewing, chase the live estimate rather than the one that started it.
    if (fabsf(diff) <= stepDeg_) { steer_ = doaDeg; slewing_ = false; hold_ = 0; }
    else steer_ += diff > 0 ? stepDeg_ : -stepDeg_;
    steer_ = linear_ ? fminf(90.0f, fmaxf(-90.0f, steer_)) : dsp::wrapDeg(steer_);
  }
  return steer_;
}

// ---------- processor ----------
bool Processor::init(const MicPos* table, const Calibration& cal) {
  if (!geo_.init(table)) return false;
  if (!fft_.init(FFT_N)) return false;
  window_ = (float*)dsp::alloc(sizeof(float) * FFT_N);
  cond_ = (float*)dsp::alloc(sizeof(float) * NUM_MICS * HOP);
  out_ = (float*)dsp::alloc(sizeof(float) * 2 * FFT_N);
  delay_ = (float*)dsp::alloc(sizeof(float) * NUM_MICS * (LATENCY + HOP));
  if (!window_ || !cond_ || !out_ || !delay_) return false;
  dsp::sqrtHannPeriodic(window_, FFT_N);
  for (int m = 0; m < NUM_MICS; m++) {
    spec_[m] = (float*)dsp::alloc(sizeof(float) * 2 * FFT_N);
    if (!spec_[m] || !ana_[m].init(&fft_, window_)) return false;
    hpf_[m].highpass(SAMPLE_RATE, HPF_HZ);
  }
  if (!syn_.init(&fft_, window_)) return false;
  if (!srp_.init(geo_, DOA_STEP_DEG)) return false;
  if (!das_.init(geo_)) return false;
  gate_.init();
  onset_.init();
  steer_.init(geo_.linear, 0.0f);
  setCalibration(cal);
  return true;
}

static float clampFinite(float v, float lim) {
  return isfinite(v) ? fminf(lim, fmaxf(-lim, v)) : 0.0f;
}

Calibration sanitizeCalibration(const Calibration& in) {
  Calibration c = in;
  // Reject a map that isn't a permutation: two channels on one row would
  // silently drop a mic.
  bool seen[NUM_MICS] = {};
  bool ok = true;
  for (int ch = 0; ch < NUM_MICS; ch++) {
    const int p = c.posOfChannel[ch];
    if (p >= NUM_MICS || seen[p]) ok = false;
    else seen[p] = true;
  }
  if (!ok) for (int ch = 0; ch < NUM_MICS; ch++) c.posOfChannel[ch] = (uint8_t)ch;
  for (int ch = 0; ch < NUM_MICS; ch++) {
    c.gainDb[ch] = clampFinite(c.gainDb[ch], GAIN_TRIM_MAX_DB);
    c.advanceUs[ch] = clampFinite(c.advanceUs[ch], ADVANCE_TRIM_MAX_US);
  }
  return c;
}

void Processor::setCalibration(const Calibration& cal) {
  cal_ = sanitizeCalibration(cal);
  for (int c = 0; c < NUM_MICS; c++) {
    chOfRow_[cal_.posOfChannel[c]] = c;
    gainLin_[c] = powf(10.0f, cal_.gainDb[c] / 20.0f);
  }
  for (int p = 0; p < NUM_MICS; p++) {
    const double ps = 2.0 * M_PI * BIN_HZ * (cal_.advanceUs[chOfRow_[p]] * 1e-6f);   // e^{+j2πf·adv}: move earlier
    trimStep_[p][0] = (float)cos(ps);
    trimStep_[p][1] = (float)sin(ps);
  }
}

void Processor::setMode(Mode m) {
  if (m == mode_) return;
  const bool wasBeam = beamMode();
  mode_ = m;
  if (beamMode() && !wasBeam) syn_.reset();   // no stale overlap tail from last time
  if (m == Mode::FixedBeam) { steer_.set(fixedDeg_); beam_->steer(fixedDeg_); }
}

void Processor::setFixedAngle(float deg) {
  if (!isfinite(deg)) return;
  fixedDeg_ = geo_.linear ? fminf(90.0f, fmaxf(-90.0f, deg)) : dsp::wrapDeg(deg);
  if (mode_ == Mode::FixedBeam) { steer_.set(fixedDeg_); beam_->steer(fixedDeg_); }
}

void Processor::process(const float* in, float* beamOut, float* micOut, HopResult* r) {
  memset(r, 0, sizeof(*r));

  // Conditioning, per capture channel: high-pass, then the gain trim. Written
  // straight into the channel's geometry row.
  for (int c = 0; c < NUM_MICS; c++) {
    float* dst = cond_ + cal_.posOfChannel[c] * HOP;
    const float g = gainLin_[c];
    float pre = 0.0f;
    for (int n = 0; n < HOP; n++) {
      const float y = hpf_[c].step(in[n * NUM_MICS + c]);
      pre += y * y;
      dst[n] = y * g;
    }
    r->micPreTrimDb[c] = dbfs(pre, HOP);
    r->micDb[c] = r->micPreTrimDb[c] + cal_.gainDb[c];
  }

  // Raw-mic delay line: LATENCY samples, so raw and beam outputs line up.
  for (int p = 0; p < NUM_MICS; p++) {
    float* d = delay_ + p * (LATENCY + HOP);
    memcpy(d + LATENCY, cond_ + p * HOP, sizeof(float) * HOP);
    if (micOut) memcpy(micOut + p * HOP, d, sizeof(float) * HOP);
    if (p == 0 && !beamMode()) memcpy(beamOut, d, sizeof(float) * HOP);   // RAW / DOA_ONLY output
    memmove(d, d + HOP, sizeof(float) * LATENCY);
  }

  r->spectral = mode_ != Mode::Raw;
  for (int p = 0; p < NUM_MICS; p++) ana_[p].push(cond_ + p * HOP, r->spectral ? spec_[p] : nullptr);

  if (r->spectral) {
    // Per-mic time trim as a phase ramp, bins 0..N/2 (all anything reads).
    for (int p = 0; p < NUM_MICS; p++) {
      if (cal_.advanceUs[chOfRow_[p]] == 0.0f) continue;
      const float sr = trimStep_[p][0], si = trimStep_[p][1];
      float pr = 1.0f, pi = 0.0f;
      float* s = spec_[p];
      for (int k = 0; k < NUM_BINS; k++) {
        const float xr = s[2 * k], xi = s[2 * k + 1];
        s[2 * k] = xr * pr - xi * pi;
        s[2 * k + 1] = xr * pi + xi * pr;
        const float t = pr * sr - pi * si;
        pi = pr * si + pi * sr;
        pr = t;
      }
    }

    // Speech gate on mean band energy across the mics.
    const int gHi = (int)fminf(GATE_HI_HZ / BIN_HZ, NUM_BINS - 1);
    float e = 0.0f;
    for (int p = 0; p < NUM_MICS; p++)
      for (int k = geo_.binLo; k <= gHi; k++)
        e += spec_[p][2 * k] * spec_[p][2 * k] + spec_[p][2 * k + 1] * spec_[p][2 * k + 1];
    r->speech = gate_.update(e / NUM_MICS);
    r->onset = onset_.update(e / NUM_MICS);

    const float* rows[NUM_MICS] = {spec_[0], spec_[1], spec_[2], spec_[3]};
    r->doa = srp_.estimate(rows);
    r->doaAccepted = r->speech && r->onset && r->doa.confidence >= DOA_MIN_CONFIDENCE;
    if (r->doaAccepted) smooth_.update(r->doa.thetaDeg, DOA_SMOOTH_ALPHA);

    if (r->speech) {
      r->leadsUpdated = true;
      for (int n = 0; n < NUM_LEADS; n++) {
        int i, j;
        leadPair(n, &i, &j);
        r->lead[n] = doa::gccPhatLead(spec_[i], spec_[j], geo_, i, j, &r->leadCoh[n]);
      }
    }
  }
  r->smoothValid = smooth_.valid();
  r->smoothDeg = smooth_.valid() ? smooth_.deg() : 0.0f;

  if (beamMode()) {
    if (mode_ == Mode::AutoBeam && smooth_.valid()) {
      const float before = steer_.steer();
      if (steer_.update(r->smoothDeg) != before) beam_->steer(steer_.steer());
    }
    beam_->apply(spec_, out_);
    dsp::makeHermitian(out_, FFT_N);
    syn_.pull(out_, beamOut);
  }
  r->steerDeg = beam_->steering();
  r->smoothAzDeg = doa::tableAzimuthDeg(r->smoothDeg, geo_.linear, DOA_ASSUMED_ELEV_DEG);
  r->steerAzDeg = doa::tableAzimuthDeg(r->steerDeg, geo_.linear, DOA_ASSUMED_ELEV_DEG);

  float sq = 0.0f;
  for (int n = 0; n < HOP; n++) sq += beamOut[n] * beamOut[n];
  r->beamDb = dbfs(sq, HOP);
}

// ---------- the board mic ----------
void MonoConditioner::init() { hpf_ = dsp::Biquad(); hpf_.highpass(SAMPLE_RATE, HPF_HZ); }

float MonoConditioner::process(const float* in, float* out) {
  float sq = 0.0f;
  for (int n = 0; n < HOP; n++) {
    const float y = hpf_.step(in[n]);
    sq += y * y;
    out[n] = y;
  }
  return dbfs(sq, HOP);
}

}  // namespace proc
