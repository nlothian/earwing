#include "beamformer.h"

#include <math.h>

namespace bf {

using namespace audio;

bool DelayAndSum::init(const dsp::Geometry& g) {
  g_ = g;
  w_ = (float*)dsp::alloc(sizeof(float) * 2 * NUM_MICS * NUM_BINS);
  if (!w_) return false;
  steer(0.0f);
  return true;
}

void DelayAndSum::steer(float thetaDeg) {
  theta_ = thetaDeg;
  for (int m = 0; m < NUM_MICS; m++) {
    // Walk the phase ramp bin by bin (one complex multiply each) instead of
    // 257 sin/cos calls; this runs every hop while AUTO_BEAM slews.
    const double ps = -2.0 * M_PI * BIN_HZ * g_.delay(m, thetaDeg);
    const float sr = (float)cos(ps), si = (float)sin(ps);
    float pr = 1.0f / NUM_MICS, pi = 0.0f;
    float* w = w_ + 2 * m * NUM_BINS;
    for (int k = 0; k < NUM_BINS; k++) {
      w[2 * k] = pr;
      w[2 * k + 1] = pi;
      const float t = pr * sr - pi * si;
      pi = pr * si + pi * sr;
      pr = t;
    }
  }
}

void DelayAndSum::apply(const float* const X[NUM_MICS], float* Y) {
  for (int k = 0; k < NUM_BINS; k++) { Y[2 * k] = 0.0f; Y[2 * k + 1] = 0.0f; }
  for (int m = 0; m < NUM_MICS; m++) {
    const float* x = X[m];
    const float* w = w_ + 2 * m * NUM_BINS;
    for (int k = 0; k < NUM_BINS; k++) {
      const float xr = x[2 * k], xi = x[2 * k + 1], wr = w[2 * k], wi = w[2 * k + 1];
      Y[2 * k] += xr * wr - xi * wi;
      Y[2 * k + 1] += xr * wi + xi * wr;
    }
  }
}

}  // namespace bf
