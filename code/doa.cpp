#include "doa.h"

#include <math.h>
#include <string.h>

namespace doa {

using namespace audio;

// Unit-magnitude copy of a spectrum over [lo, hi]. Exact zeros (no mic wired,
// digital silence) stay zero so they contribute nothing instead of NaN.
static void phatNormalize(const float* X, int lo, int hi, float* out) {
  for (int k = lo; k <= hi; k++) {
    const float re = X[2 * k], im = X[2 * k + 1];
    const float m2 = re * re + im * im;
    const float s = m2 > 1e-30f ? 1.0f / sqrtf(m2) : 0.0f;
    out[2 * (k - lo)] = re * s;
    out[2 * (k - lo) + 1] = im * s;
  }
}

// ---------- SRP-PHAT ----------
bool SrpPhat::init(const dsp::Geometry& g, float stepDeg) {
  g_ = g;
  nBins_ = g.binHi - g.binLo + 1;

  // Group pairs by baseline vector, treating b and −b as one group.
  nGroups_ = nPairs_ = 0;
  for (int i = 0; i < NUM_MICS; i++)
    for (int j = i + 1; j < NUM_MICS; j++) {
      const float bx = g.pos[i].x - g.pos[j].x, by = g.pos[i].y - g.pos[j].y;
      int gi = -1, sign = 1;
      for (int k = 0; k < nGroups_ && gi < 0; k++) {
        if (fabsf(groups_[k].bx - bx) < 1e-5f && fabsf(groups_[k].by - by) < 1e-5f) gi = k;
        else if (fabsf(groups_[k].bx + bx) < 1e-5f && fabsf(groups_[k].by + by) < 1e-5f) { gi = k; sign = -1; }
      }
      if (gi < 0) {
        gi = nGroups_++;
        groups_[gi].bx = bx; groups_[gi].by = by; groups_[gi].n = 0;
      }
      Group& gr = groups_[gi];
      gr.i[gr.n] = (int8_t)i; gr.j[gr.n] = (int8_t)j; gr.sign[gr.n] = (int8_t)sign;
      gr.n++;
      nPairs_++;
    }

  // Scan grid from the geometry: −90..+90 inclusive for a line, a full turn
  // (end excluded, it's the start again) for a plane.
  const float span = g.scanMaxDeg - g.scanMinDeg;
  nAngles_ = (int)lroundf(span / stepDeg) + (g.linear ? 1 : 0);

  angles_ = (float*)dsp::alloc(sizeof(float) * nAngles_);
  ph0_ = (float*)dsp::alloc(sizeof(float) * 2 * nAngles_ * nGroups_);
  phStep_ = (float*)dsp::alloc(sizeof(float) * 2 * nAngles_ * nGroups_);
  xn_ = (float*)dsp::alloc(sizeof(float) * 2 * NUM_MICS * nBins_);
  gsum_ = (float*)dsp::alloc(sizeof(float) * 2 * nGroups_ * g.numBlocks);
  scores_ = (float*)dsp::alloc(sizeof(float) * nAngles_);
  if (!angles_ || !ph0_ || !phStep_ || !xn_ || !gsum_ || !scores_) return false;

  for (int a = 0; a < nAngles_; a++) {
    const float th = g.scanMinDeg + a * stepDeg;
    angles_[a] = th;
    const double ux = sin(dsp::deg2rad(th)), uy = cos(dsp::deg2rad(th));
    for (int k = 0; k < nGroups_; k++) {
      // Δτ = τ_i − τ_j = (b · u) / c for the group's baseline b = pos_i − pos_j.
      const double dtau = (groups_[k].bx * ux + groups_[k].by * uy) / SPEED_OF_SOUND;
      const double f0 = (g.binLo + (g.blockBins - 1) * 0.5) * BIN_HZ;
      const double p0 = -2.0 * M_PI * f0 * dtau;
      const double ps = -2.0 * M_PI * g.blockBins * BIN_HZ * dtau;
      float* q0 = ph0_ + 2 * (a * nGroups_ + k);
      float* qs = phStep_ + 2 * (a * nGroups_ + k);
      q0[0] = (float)cos(p0); q0[1] = (float)sin(p0);
      qs[0] = (float)cos(ps); qs[1] = (float)sin(ps);
    }
  }
  return true;
}

DoaResult SrpPhat::estimate(const float* const X[NUM_MICS]) {
  const int lo = g_.binLo, hi = g_.binHi, nb = nBins_, B = g_.blockBins, nk = g_.numBlocks;
  for (int m = 0; m < NUM_MICS; m++) phatNormalize(X[m], lo, hi, xn_ + 2 * m * nb);

  // G_ij = x̂_i · conj(x̂_j), conjugated for pairs stored as the reverse
  // baseline, summed over the group's pairs and over each block's bins.
  memset(gsum_, 0, sizeof(float) * 2 * nGroups_ * nk);
  for (int k = 0; k < nGroups_; k++) {
    float* gs = gsum_ + 2 * k * nk;
    const Group& gr = groups_[k];
    for (int p = 0; p < gr.n; p++) {
      const float* __restrict a = xn_ + 2 * gr.i[p] * nb;
      const float* __restrict b = xn_ + 2 * gr.j[p] * nb;
      const float sg = gr.sign[p];
      for (int blk = 0, f = 0; blk < nk; blk++) {
        float re = 0.0f, im = 0.0f;
        for (int e = 0; e < B; e++, f++) {
          const float ar = a[2 * f], ai = a[2 * f + 1], br = b[2 * f], bi = b[2 * f + 1];
          re += ar * br + ai * bi;
          im += ai * br - ar * bi;
        }
        gs[2 * blk] += re;
        gs[2 * blk + 1] += sg * im;
      }
    }
  }

  const float norm = 1.0f / (float)(nPairs_ * nk * B);
  float sum = 0.0f, best = -1e30f;
  int bestA = 0;
  for (int a = 0; a < nAngles_; a++) {
    float s = 0.0f;
    for (int k = 0; k < nGroups_; k++) {
      const float* __restrict gs = gsum_ + 2 * k * nk;
      float pr = ph0_[2 * (a * nGroups_ + k)], pi = ph0_[2 * (a * nGroups_ + k) + 1];
      const float sr = phStep_[2 * (a * nGroups_ + k)], si = phStep_[2 * (a * nGroups_ + k) + 1];
      for (int f = 0; f < nk; f++) {
        s += gs[2 * f] * pr - gs[2 * f + 1] * pi;    // Re{G · p}
        const float t = pr * sr - pi * si;           // p ← p · step: next block's phasor
        pi = pr * si + pi * sr;
        pr = t;
      }
    }
    s *= norm;
    scores_[a] = s;
    sum += s;
    if (s > best) { best = s; bestA = a; }
  }

  DoaResult r;
  r.peak = best;
  r.confidence = best - sum / nAngles_;
  // Parabolic interpolation between grid points (wrapping for a plane; at
  // the ends of a line the grid point itself is reported).
  float th = angles_[bestA];
  const float step = nAngles_ > 1 ? angles_[1] - angles_[0] : 0.0f;
  int am = bestA - 1, ap = bestA + 1;
  if (!g_.linear) { am = (am + nAngles_) % nAngles_; ap = ap % nAngles_; }
  if (am >= 0 && ap < nAngles_) {
    const float y0 = scores_[am], y1 = scores_[bestA], y2 = scores_[ap];
    const float den = y0 - 2.0f * y1 + y2;
    if (den < 0.0f) {
      float off = 0.5f * (y0 - y2) / den;
      if (off > 0.5f) off = 0.5f;
      if (off < -0.5f) off = -0.5f;
      th += off * step;
    }
  }
  r.thetaDeg = g_.linear ? th : dsp::wrapDeg(th);
  return r;
}

// ---------- GCC-PHAT pair lead ----------
// R(τ) = Σ_blocks Re{G_b·e^{−j2π f_b τ}}, normalised by the bin count, where
// G_b is x̂_j·conj(x̂_i) summed over block b. Float sin/cos: the S3 has no
// double-precision FPU.
static float gccEval(const float* g, const dsp::Geometry& geo, int nBins, float tauSec) {
  const float f0 = (geo.binLo + (geo.blockBins - 1) * 0.5f) * BIN_HZ;
  const float p0 = -2.0f * dsp::PI_F * f0 * tauSec;
  const float ps = -2.0f * dsp::PI_F * geo.blockBins * BIN_HZ * tauSec;
  float pr = cosf(p0), pi = sinf(p0);
  const float sr = cosf(ps), si = sinf(ps);
  float s = 0.0f;
  for (int f = 0; f < geo.numBlocks; f++) {
    s += g[2 * f] * pr - g[2 * f + 1] * pi;
    const float t = pr * sr - pi * si;
    pi = pr * si + pi * sr;
    pr = t;
  }
  return s / nBins;
}

float gccPhatLead(const float* Xi, const float* Xj, const dsp::Geometry& geo,
                  int i, int j, float* coherence) {
  const int lo = geo.binLo, hi = geo.binHi, nb = hi - lo + 1, B = geo.blockBins;
  // Static rather than 6 KB of stack: the processing task is the only caller.
  static float ni[2 * NUM_BINS], nj[2 * NUM_BINS], g[2 * NUM_BINS];
  phatNormalize(Xi, lo, hi, ni);
  phatNormalize(Xj, lo, hi, nj);
  for (int blk = 0, f = 0; blk < geo.numBlocks; blk++) {   // Σ_block x̂_j · conj(x̂_i)
    float re = 0.0f, im = 0.0f;
    for (int e = 0; e < B; e++, f++) {
      re += nj[2 * f] * ni[2 * f] + nj[2 * f + 1] * ni[2 * f + 1];
      im += nj[2 * f + 1] * ni[2 * f] - nj[2 * f] * ni[2 * f + 1];
    }
    g[2 * blk] = re;
    g[2 * blk + 1] = im;
  }
  // Search the physically possible lags plus a sample of slack, so a sync
  // fault that pushes the lag past the aperture still shows up at the edge.
  const float d = hypotf(geo.pos[i].x - geo.pos[j].x, geo.pos[i].y - geo.pos[j].y);
  const float maxLag = d / SPEED_OF_SOUND * SAMPLE_RATE + 1.5f;
  float bestLag = 0.0f, best = -1e30f;
  for (float l = -maxLag; l <= maxLag; l += 0.25f) {
    const float v = gccEval(g, geo, nb, l / SAMPLE_RATE);
    if (v > best) { best = v; bestLag = l; }
  }
  const float c = bestLag;
  for (float l = c - 0.24f; l <= c + 0.24f; l += 0.02f) {
    const float v = gccEval(g, geo, nb, l / SAMPLE_RATE);
    if (v > best) { best = v; bestLag = l; }
  }
  if (coherence) *coherence = best;
  return bestLag;
}

// ---------- table azimuth ----------
float tableAzimuthDeg(float thetaDeg, bool linear, float elevDeg) {
  if (!linear) return thetaDeg;
  float u = sinf(dsp::deg2rad(thetaDeg)) / cosf(dsp::deg2rad(elevDeg));
  if (u > 1.0f) u = 1.0f;
  if (u < -1.0f) u = -1.0f;
  return dsp::rad2deg(asinf(u));
}

// ---------- energy gate ----------
void EnergyGate::init() {
  const float hopSec = (float)HOP / SAMPLE_RATE;
  rise_ = powf(10.0f, GATE_FLOOR_RISE_DB_S * hopSec / 10.0f);
  openRatio_ = powf(10.0f, GATE_OPEN_DB / 10.0f);
  hangHops_ = (int)ceilf(GATE_HANGOVER_MS / 1000.0f / hopSec);
  hang_ = 0;
  primed_ = false;
}

bool EnergyGate::update(float e) {
  if (!primed_) { floor_ = e; primed_ = true; }
  if (e < floor_) floor_ = e;
  else floor_ *= rise_;
  if (floor_ < 1e-20f) floor_ = 1e-20f;   // digital silence must not pin the floor at 0
  if (e > floor_ * openRatio_) hang_ = hangHops_;
  else if (hang_ > 0) hang_--;
  return open();
}

// ---------- onsets ----------
void OnsetDetector::init() {
  alpha_ = 1.0f - expf(-(float)HOP / SAMPLE_RATE / (DOA_ONSET_REF_MS / 1000.0f));
  ratio_ = powf(10.0f, DOA_ONSET_DB / 10.0f);
  primed_ = false;
}

bool OnsetDetector::update(float e) {
  if (!primed_) { ref_ = e; primed_ = true; }
  const bool onset = e > ref_ * ratio_;
  ref_ += alpha_ * (e - ref_);   // after the test: compared with the hops before
  return onset;
}

// ---------- direction smoothing ----------
void DirectionSmoother::reset(float deg) {
  vx_ = sinf(dsp::deg2rad(deg));
  vy_ = cosf(dsp::deg2rad(deg));
  valid_ = true;
}

void DirectionSmoother::update(float deg, float alpha) {
  if (!valid_) { reset(deg); return; }
  vx_ += alpha * (sinf(dsp::deg2rad(deg)) - vx_);
  vy_ += alpha * (cosf(dsp::deg2rad(deg)) - vy_);
}

float DirectionSmoother::deg() const { return dsp::rad2deg(atan2f(vx_, vy_)); }

}  // namespace doa
