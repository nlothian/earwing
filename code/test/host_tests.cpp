// Host-side tests for the portable DSP: FFT, STFT reconstruction, SRP-PHAT,
// delay-and-sum, GCC-PHAT, steering hysteresis, the I2S alignment check, and
// DOA tracking with a raised talker and in a synthetic reverberant room.
// Synthetic plane waves are built with exact fractional delays (a phase ramp
// on a long FFT), so these tests are what settle the phase-sign convention in
// audio_config.h. Build and run: make -C code/test
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <random>
#include <vector>

#include "beamformer.h"
#include "check.h"
#include "doa.h"
#include "dsp.h"
#include "processor.h"
#include "sync_check.h"

using namespace audio;

static std::mt19937 rng(12345);

static std::vector<float> noise(int n, float rms) {
  std::normal_distribution<float> d(0.0f, rms);
  std::vector<float> v(n);
  for (auto& x : v) x = d(rng);
  return v;
}

// Plane wave from thetaDeg on the given geometry: row m gets s(t + τ_m), done
// exactly with a phase ramp on one long FFT. Optionally band-limit the source.
// Returns rows[m][n]; `centre` receives s(t) itself (what the centre hears).
// elevDeg raises the source above the mics' plane, which scales every τ_m by
// cos(el) (the mics all sit at z = 0).
static std::vector<std::vector<float>> planeWave(const dsp::Geometry& g, float thetaDeg, int len,
                                                 float rms, float fLo, float fHi,
                                                 std::vector<float>* centre, float elevDeg = 0.0f) {
  dsp::Fft big;
  big.init(len);
  std::vector<float> s = noise(len, 1.0f);
  std::vector<float> S(2 * len);
  for (int i = 0; i < len; i++) { S[2 * i] = s[i]; S[2 * i + 1] = 0; }
  big.forward(S.data());
  // Band-limit (and drop Nyquist, which can't carry a fractional delay).
  for (int k = 0; k < len; k++) {
    const int kk = k <= len / 2 ? k : len - k;
    const float f = (float)kk * SAMPLE_RATE / len;
    if (f < fLo || f > fHi || k == len / 2) { S[2 * k] = 0; S[2 * k + 1] = 0; }
  }
  auto synth = [&](float tau, std::vector<float>& out) {
    std::vector<float> X(S);
    for (int k = 0; k < len; k++) {
      const double f = (k <= len / 2 ? k : k - len) * (double)SAMPLE_RATE / len;
      const double ph = 2.0 * M_PI * f * tau;   // X_m = S·e^{+j2πfτ_m}
      const float c = (float)cos(ph), sn = (float)sin(ph);
      const float re = X[2 * k], im = X[2 * k + 1];
      X[2 * k] = re * c - im * sn;
      X[2 * k + 1] = re * sn + im * c;
    }
    big.inverse(X.data());
    out.resize(len);
    for (int i = 0; i < len; i++) out[i] = X[2 * i];
  };
  std::vector<float> ref;
  synth(0.0f, ref);
  double p = 0;
  for (float x : ref) p += x * x;
  const float scale = rms / (float)sqrt(p / len);
  std::vector<std::vector<float>> rows(NUM_MICS);
  for (int m = 0; m < NUM_MICS; m++) {
    synth(g.delay(m, thetaDeg) * cosf(dsp::deg2rad(elevDeg)), rows[m]);
    for (auto& x : rows[m]) x *= scale;
  }
  if (centre) {
    *centre = ref;
    for (auto& x : *centre) x *= scale;
  }
  return rows;
}

// Interleave rows into capture-channel frames through a channel→row map.
static std::vector<float> interleave(const std::vector<std::vector<float>>& rows,
                                     const uint8_t* posOfChannel) {
  const int len = (int)rows[0].size();
  std::vector<float> in(len * NUM_MICS);
  for (int n = 0; n < len; n++)
    for (int c = 0; c < NUM_MICS; c++) in[n * NUM_MICS + c] = rows[posOfChannel[c]][n];
  return in;
}

static double power(const float* x, int n) {
  double p = 0;
  for (int i = 0; i < n; i++) p += (double)x[i] * x[i];
  return p / n;
}

// ---------- tests ----------
static void testFft() {
  printf("fft\n");
  for (int n : {8, 64, 512}) {
    dsp::Fft f;
    CHECK(f.init(n), "init %d", n);
    std::vector<float> x(2 * n), X(2 * n);
    auto v = noise(2 * n, 1.0f);
    x = v;
    X = x;
    f.forward(X.data());
    double maxErr = 0;
    for (int k = 0; k < n; k++) {
      double re = 0, im = 0;
      for (int t = 0; t < n; t++) {
        const double a = -2.0 * M_PI * k * t / n;
        re += x[2 * t] * cos(a) - x[2 * t + 1] * sin(a);
        im += x[2 * t] * sin(a) + x[2 * t + 1] * cos(a);
      }
      maxErr = std::max(maxErr, std::max(fabs(re - X[2 * k]), fabs(im - X[2 * k + 1])));
    }
    CHECK(maxErr < 1e-4 * sqrt(n), "n=%d forward vs naive DFT err %g", n, maxErr);
    f.inverse(X.data());
    double rt = 0;
    for (int i = 0; i < 2 * n; i++) rt = std::max(rt, (double)fabsf(X[i] - x[i]));
    CHECK(rt < 1e-5, "n=%d round trip err %g", n, rt);
  }
}

static void testGeometry() {
  printf("geometry\n");
  dsp::Geometry g;
  CHECK(g.init(GEOMETRY_LINEAR), "linear init");
  CHECK(g.linear, "linear flagged");
  CHECK(fabsf(g.minSpacing - 0.030f) < 1e-6f, "min spacing %g", g.minSpacing);
  CHECK(fabsf(g.aliasHz - 5716.7f) < 1.0f, "alias %g", g.aliasHz);
  CHECK(fabsf(g.bandHiHz - 5488.0f) < 1.0f, "band hi %g", g.bandHiHz);
  // Band 300..5488 Hz = bins 10..175, in blocks of 9 (0.5 rad / 0.0515 rad
  // per bin at 90 mm), rounded down to whole blocks: 18 blocks, top bin 171.
  CHECK(g.blockBins == 9 && g.numBlocks == 18, "blocks %d x %d", g.numBlocks, g.blockBins);
  CHECK(g.binLo == 10 && g.binHi == 171, "bins %d..%d", g.binLo, g.binHi);
  CHECK(g.scanMinDeg == -90 && g.scanMaxDeg == 90, "scan range");
  // 90 mm end to end at c = 343: 262 µs, 4.2 samples at 16 kHz.
  const float endLead = (g.delay(3, 90) - g.delay(0, 90)) * SAMPLE_RATE;
  CHECK(fabsf(endLead - 4.198f) < 0.01f, "endfire lead %g samples", endLead);

  dsp::Geometry q;
  CHECK(q.init(GEOMETRY_SQUARE), "square init");
  CHECK(!q.linear, "square is planar");
  CHECK(fabsf(q.minSpacing - 0.040f) < 1e-5f, "square spacing %g", q.minSpacing);
  CHECK(fabsf(q.aliasHz - 4287.5f) < 1.0f, "square alias %g", q.aliasHz);
  CHECK(q.scanMinDeg == -180 && q.scanMaxDeg == 180, "square scan range");
}

// Identical inputs, beam at 0°: STFT → DAS → overlap-add must give back the
// input, LATENCY samples late, to float precision.
static void testReconstruction() {
  printf("stft reconstruction\n");
  dsp::Geometry g;
  g.init(GEOMETRY_LINEAR);
  dsp::Fft fft;
  fft.init(FFT_N);
  std::vector<float> win(FFT_N);
  dsp::sqrtHannPeriodic(win.data(), FFT_N);
  dsp::StftAnalyzer ana[NUM_MICS];
  dsp::StftSynth syn;
  for (auto& a : ana) a.init(&fft, win.data());
  syn.init(&fft, win.data());
  bf::DelayAndSum das;
  das.init(g);
  das.steer(0.0f);

  const int hops = 40, len = hops * HOP;
  auto x = noise(len, 0.1f);
  std::vector<float> y(len);
  std::vector<float> spec[NUM_MICS];
  for (auto& s : spec) s.resize(2 * FFT_N);
  std::vector<float> Y(2 * FFT_N);
  for (int h = 0; h < hops; h++) {
    for (int m = 0; m < NUM_MICS; m++) ana[m].push(&x[h * HOP], spec[m].data());
    const float* rows[NUM_MICS] = {spec[0].data(), spec[1].data(), spec[2].data(), spec[3].data()};
    das.apply(rows, Y.data());
    dsp::makeHermitian(Y.data(), FFT_N);
    syn.pull(Y.data(), &y[h * HOP]);
  }
  double err = 0;
  for (int n = FFT_N; n < len; n++) err = std::max(err, (double)fabsf(y[n] - x[n - LATENCY]));
  CHECK(err < 1e-6, "max reconstruction error %g", err);
  printf("  max error %.2e (latency %d samples)\n", err, LATENCY);
}

// Same through the whole Processor (high-pass included) in FIXED_BEAM at 0°.
static void testProcessorPassthrough() {
  printf("processor passthrough\n");
  proc::Processor p;
  CHECK(p.init(GEOMETRY_LINEAR, DEFAULT_CALIBRATION), "init");
  p.setFixedAngle(0.0f);
  p.setMode(proc::Mode::FixedBeam);
  const int hops = 40, len = hops * HOP;
  auto x = noise(len, 0.05f);
  std::vector<std::vector<float>> rows(NUM_MICS, x);
  auto in = interleave(rows, DEFAULT_CALIBRATION.posOfChannel);
  std::vector<float> out(len), mic(NUM_MICS * HOP), ref(len);
  dsp::Biquad hpf;
  hpf.highpass(SAMPLE_RATE, HPF_HZ);
  for (int n = 0; n < len; n++) ref[n] = hpf.step(x[n]);
  proc::HopResult r;
  double micErr = 0;
  for (int h = 0; h < hops; h++) {
    p.process(&in[h * HOP * NUM_MICS], &out[h * HOP], mic.data(), &r);
    for (int n = 0; n < HOP && h * HOP + n >= LATENCY; n++)
      micErr = std::max(micErr, (double)fabsf(mic[n] - ref[h * HOP + n - LATENCY]));
  }
  double err = 0;
  for (int n = FFT_N; n < len; n++) err = std::max(err, (double)fabsf(out[n] - ref[n - LATENCY]));
  CHECK(err < 1e-5, "beam vs delayed input error %g", err);
  CHECK(micErr < 1e-7, "raw mic delay line error %g", micErr);
}

// Run a plane wave through the processor and return the per-hop scan angles.
static std::vector<float> scanAngles(const MicPos* table, float thetaDeg, float snrDb,
                                     const Calibration& cal = DEFAULT_CALIBRATION,
                                     float fLo = 100, float fHi = 8000) {
  dsp::Geometry g;
  g.init(table);
  const int hops = 64, len = hops * HOP;
  auto rows = planeWave(g, thetaDeg, 1 << 14, 0.02f, fLo, fHi, nullptr);
  if (snrDb < 100) {
    const float nrms = 0.02f * powf(10.0f, -snrDb / 20.0f);
    for (auto& r : rows) { auto n = noise(len, nrms); for (int i = 0; i < len; i++) r[i] += n[i]; }
  }
  proc::Processor p;
  p.init(table, cal);
  p.setMode(proc::Mode::DoaOnly);
  auto in = interleave(rows, cal.posOfChannel);
  std::vector<float> out(HOP);
  std::vector<float> ang;
  proc::HopResult r;
  for (int h = 0; h < hops; h++) {
    p.process(&in[h * HOP * NUM_MICS], out.data(), nullptr, &r);
    if (h >= 4) ang.push_back(r.doa.thetaDeg);   // skip the frames still filling the window
  }
  return ang;
}

static float median(std::vector<float> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

static void testSrpLinear() {
  printf("srp-phat, linear array\n");
  for (float th : {-80.f, -60.f, -45.f, -30.f, -10.f, 0.f, 7.f, 10.f, 30.f, 45.f, 60.f, 80.f}) {
    auto a = scanAngles(GEOMETRY_LINEAR, th, 200);
    float worst = 0;
    for (float x : a) worst = std::max(worst, fabsf(x - th));
    auto an = scanAngles(GEOMETRY_LINEAR, th, 10);
    const float med = median(an);
    printf("  %+5.0f°: clean worst err %.2f°, 10 dB SNR median %+6.2f°\n", th, worst, med);
    CHECK(worst < 2.0f, "θ=%g clean worst error %g", th, worst);
    CHECK(fabsf(med - th) < 2.0f, "θ=%g noisy median %g", th, med);
  }
}

static void testSrpSquare() {
  printf("srp-phat, square array\n");
  for (float th : {-150.f, -90.f, -30.f, 0.f, 45.f, 100.f, 170.f}) {
    auto a = scanAngles(GEOMETRY_SQUARE, th, 200);
    float worst = 0;
    for (float x : a) worst = std::max(worst, fabsf(dsp::wrapDeg(x - th)));
    printf("  %+5.0f°: worst err %.2f°\n", th, worst);
    CHECK(worst < 2.0f, "square θ=%g worst error %g", th, worst);
  }
}

// ---------- reverberant room ----------
// One arrival of the talker at the array: from (az, el), `delay` seconds after
// the direct sound, scaled by `gain`. The direct sound is {az, el, 0, 1}.
struct Arrival { float az, el, delay, gain; };

// Speech-like source in a synthetic room: voiced syllables (~150-300 ms on,
// 80-200 ms off, 10 ms attack) after a quiet lead-in, each a harmonic series
// whose pitch glides somewhere in 100-220 Hz, plus a little breath noise.
// The glide matters: the tail still carries the previous pitch's harmonics,
// so reverberation fills the gaps between the direct sound's, as in a real
// room. Every arrival is added at every mic with its exact fractional delay;
// the source stops early enough that the longest tail doesn't wrap.
static std::vector<std::vector<float>> roomWave(const dsp::Geometry& g, const std::vector<Arrival>& arr,
                                                int len, std::mt19937& r) {
  std::uniform_real_distribution<float> U(0.0f, 1.0f);
  std::normal_distribution<float> N(0.0f, 1.0f);
  std::vector<float> src(len, 0.0f);
  const int lead = SAMPLE_RATE * 3 / 10, stop = len - SAMPLE_RATE / 4, attack = SAMPLE_RATE / 100;
  for (int n = lead; n < stop;) {
    const int on = (int)((0.15f + 0.15f * U(r)) * SAMPLE_RATE), off = (int)((0.08f + 0.12f * U(r)) * SAMPLE_RATE);
    const float level = 0.6f + 0.4f * U(r);
    const float f0a = 100 + 120 * U(r), f0b = 100 + 120 * U(r);
    double phase = 0;
    for (int i = 0; i < on && n + i < stop; i++) {
      const float a = i < attack ? (float)i / attack : 1.0f, d = on - i < attack ? (float)(on - i) / attack : 1.0f;
      const float f0 = f0a + (f0b - f0a) * i / on;
      phase += 2.0 * M_PI * f0 / SAMPLE_RATE;
      float v = 0.1f * N(r);
      for (int k = 1; k * f0 < 7000; k++) v += sinf((float)(k * phase)) / sqrtf((float)k);
      src[n + i] = level * fminf(a, d) * v;
    }
    n += on + off;
  }
  dsp::Fft big;
  big.init(len);
  std::vector<float> S(2 * len);
  for (int i = 0; i < len; i++) { S[2 * i] = src[i]; S[2 * i + 1] = 0; }
  big.forward(S.data());
  for (int k = 0; k < len; k++) {
    const int kk = k <= len / 2 ? k : len - k;
    const float f = (float)kk * SAMPLE_RATE / len;
    if (f < 100 || f > 7000 || k == len / 2) { S[2 * k] = 0; S[2 * k + 1] = 0; }
  }
  std::vector<std::vector<float>> rows(NUM_MICS, std::vector<float>(len, 0.0f));
  std::vector<float> X(2 * len);
  for (int m = 0; m < NUM_MICS; m++)
    for (const Arrival& a : arr) {
      const double tau = g.delay(m, a.az) * cos(dsp::deg2rad(a.el)) - a.delay;
      for (int k = 0; k < len; k++) {
        const double f = (k <= len / 2 ? k : k - len) * (double)SAMPLE_RATE / len;
        const double ph = 2.0 * M_PI * f * tau;
        const float c = (float)cos(ph), sn = (float)sin(ph);
        X[2 * k] = a.gain * (S[2 * k] * c - S[2 * k + 1] * sn);
        X[2 * k + 1] = a.gain * (S[2 * k] * sn + S[2 * k + 1] * c);
      }
      big.inverse(X.data());
      for (int i = 0; i < len; i++) rows[m][i] += X[2 * i];
    }
  double p = 0;
  for (float x : rows[0]) p += x * x;
  const float scale = 0.02f / (float)sqrt(p / len);
  for (auto& row : rows) {
    auto n = noise(len, 2e-4f);   // self-noise, 40 dB under the talker
    for (int i = 0; i < len; i++) row[i] = row[i] * scale + n[i];
  }
  return rows;
}

// A meeting room: the direct sound, five early reflections 2-15 ms behind
// it (walls, a laptop lid, the ceiling) at `early` ± 0.15 of its level, and
// a 40-arrival tail decaying with an RT60 of 0.4 s, from everywhere at or
// above the table. early = 0.35 is a typical small furnished room at ~1.5 m;
// 0.55 is a hard one (glass, a laptop lid right by the array).
static std::vector<Arrival> room(float az, float el, float early, std::mt19937& r) {
  std::uniform_real_distribution<float> U(0.0f, 1.0f);
  std::vector<Arrival> a = {{az, el, 0.0f, 1.0f}};
  for (int i = 0; i < 5; i++)
    a.push_back({-180 + 360 * U(r), -5 + 50 * U(r), 0.002f + 0.013f * U(r), early - 0.15f + 0.3f * U(r)});
  for (int i = 0; i < 40; i++) {
    const float d = 0.015f + 0.135f * U(r);
    a.push_back({-180 + 360 * U(r), -5 + 65 * U(r), d, 0.5f * expf(-6.9f * d / 0.4f) * (0.5f + U(r))});
  }
  return a;
}

// Mean |error| of the tracked cone angle over the last 60% of the hops, and
// how many hops the tracker accepted, for a talker at (az, el) in a room.
static float reverbError(float az, float el, float early, unsigned seed, int* accepted) {
  dsp::Geometry g;
  g.init(GEOMETRY_LINEAR);
  std::mt19937 r(seed);
  const int len = 1 << 16;   // ~4 s
  auto rows = roomWave(g, room(az, el, early, r), len, r);
  proc::Processor p;
  p.init(GEOMETRY_LINEAR, DEFAULT_CALIBRATION);
  p.setMode(proc::Mode::DoaOnly);
  auto in = interleave(rows, DEFAULT_CALIBRATION.posOfChannel);
  std::vector<float> out(HOP);
  proc::HopResult res;
  const float want = dsp::rad2deg(asinf(sinf(dsp::deg2rad(az)) * cosf(dsp::deg2rad(el))));
  const int hops = len / HOP - 1, from = hops * 2 / 5;
  double err = 0;
  int n = 0;
  *accepted = 0;
  for (int h = 0; h < hops; h++) {
    p.process(&in[h * HOP * NUM_MICS], out.data(), nullptr, &res);
    if (res.doaAccepted) ++*accepted;
    if (h >= from) { err += res.smoothValid ? fabsf(res.smoothDeg - want) : 90.0f; n++; }
  }
  return (float)(err / n);
}

static void testReverb() {
  printf("tracking in a reverberant room (linear array, talker 25° up)\n");
  for (float early : {0.35f, 0.55f}) {
    printf("  early reflections at %.2f of the direct sound\n", early);
    float worstMean = 0;
    for (float az : {0.f, 30.f, 60.f, 90.f, -75.f}) {
      float sum = 0;
      int acc = 0;
      for (unsigned seed : {1u, 2u, 3u, 4u, 5u, 6u}) {
        int a;
        sum += reverbError(az, 25.0f, early, seed, &a);
        acc += a;
      }
      printf("    az %+4.0f°: mean tracked cone error %5.2f° (%d hops accepted per room)\n", az, sum / 6, acc / 6);
      worstMean = std::max(worstMean, sum / 6);
    }
    printf("    worst %.2f°\n", worstMean);
    // Every scan at a syllable's start, and only then: without the onset
    // test these rooms average 24° and 33° off for a talker on the axis.
    CHECK(worstMean < (early < 0.5f ? 8.0f : 10.0f), "early %.2f: worst mean error %g", early, worstMean);
  }
}

// A talker raised above the table: the linear array's scan reads the cone
// angle, pulled toward broadside, and smoothAzDeg undoes that when the talker
// is at the assumed elevation. Run through the whole processor, so the
// smoother and the reported field are covered too.
static float trackedAzimuth(const MicPos* table, float azDeg, float elevDeg, float* coneDeg) {
  dsp::Geometry g;
  g.init(table);
  // Speech-like syllables in a dry room: the tracker only listens at onsets.
  std::mt19937 rr(7);
  auto rows = roomWave(g, {{azDeg, elevDeg, 0.0f, 1.0f}}, 1 << 15, rr);
  const int hops = (1 << 15) / HOP - 1;
  proc::Processor p;
  p.init(table, DEFAULT_CALIBRATION);
  p.setMode(proc::Mode::DoaOnly);
  auto in = interleave(rows, DEFAULT_CALIBRATION.posOfChannel);
  std::vector<float> out(HOP);
  proc::HopResult r;
  for (int h = 0; h < hops; h++) p.process(&in[h * HOP * NUM_MICS], out.data(), nullptr, &r);
  *coneDeg = r.smoothDeg;
  return r.smoothAzDeg;
}

static void testTableAzimuth() {
  printf("table azimuth from a raised talker\n");
  // The conversion itself: no-ops, the clamp, and an exact inverse.
  CHECK(fabsf(doa::tableAzimuthDeg(37.0f, true, 0.0f) - 37.0f) < 1e-3f, "el 0 must be identity");
  CHECK(doa::tableAzimuthDeg(37.0f, false, 40.0f) == 37.0f, "planar must be identity");
  CHECK(fabsf(doa::tableAzimuthDeg(70.0f, true, 25.0f) - 90.0f) < 1e-3f, "past cos(el) must clamp to +90");
  CHECK(fabsf(doa::tableAzimuthDeg(-70.0f, true, 25.0f) + 90.0f) < 1e-3f, "past cos(el) must clamp to -90");
  for (float az : {-60.f, 20.f, 75.f}) {
    const float th = dsp::rad2deg(asinf(sinf(dsp::deg2rad(az)) * cosf(dsp::deg2rad(30.0f))));
    CHECK(fabsf(doa::tableAzimuthDeg(th, true, 30.0f) - az) < 0.01f, "inverse at az=%g", az);
  }

  const float el = DOA_ASSUMED_ELEV_DEG;
  for (float az : {-90.f, -60.f, -30.f, 0.f, 15.f, 45.f, 70.f, 90.f}) {
    float cone;
    const float got = trackedAzimuth(GEOMETRY_LINEAR, az, el, &cone);
    const float wantCone = dsp::rad2deg(asinf(sinf(dsp::deg2rad(az)) * cosf(dsp::deg2rad(el))));
    printf("  az %+4.0f° el %.0f°: cone %+6.2f° (expect %+6.2f°), table azimuth %+6.2f°\n",
           az, el, cone, wantCone, got);
    CHECK(fabsf(cone - wantCone) < 2.0f, "az=%g cone %g, want %g", az, cone, wantCone);
    CHECK(fabsf(got - az) < 2.0f, "az=%g el=%g table azimuth %g", az, el, got);
  }

  // Off the assumed height the correction is approximate; it should still
  // beat the raw cone angle for a talker on the axis, where the bias is worst.
  for (float e : {10.f, 40.f}) {
    float cone;
    const float got = trackedAzimuth(GEOMETRY_LINEAR, 90.0f, e, &cone);
    printf("  az  +90° el %.0f°: cone %+6.2f°, table azimuth %+6.2f°\n", e, cone, got);
    CHECK(fabsf(got - 90.0f) < fabsf(cone - 90.0f), "el=%g: corrected %g not nearer 90 than %g", e, got, cone);
  }

  // A flat planar array's azimuth isn't biased by height, and isn't corrected.
  for (float az : {-120.f, 30.f, 160.f}) {
    float tracked;
    const float got = trackedAzimuth(GEOMETRY_SQUARE, az, el, &tracked);
    printf("  square az %+4.0f° el %.0f°: %+6.2f°\n", az, el, got);
    CHECK(fabsf(dsp::wrapDeg(got - az)) < 2.0f, "square az=%g el=%g got %g", az, el, got);
  }
}

// A wrong channel→position map scrambles the DOA; the right map in
// Calibration puts it back.
static void testChannelMap() {
  printf("channel map\n");
  Calibration cal = DEFAULT_CALIBRATION;
  const uint8_t map[NUM_MICS] = {2, 0, 3, 1};
  memcpy(cal.posOfChannel, map, sizeof(map));
  auto a = scanAngles(GEOMETRY_LINEAR, 40.0f, 200, cal);
  CHECK(fabsf(median(a) - 40.0f) < 2.0f, "mapped DOA %g", median(a));
}

// Output power relative to the source s(t) at the centre, for a band of noise.
static float beamGainDb(float srcDeg, float steerDeg, float fLo, float fHi) {
  dsp::Geometry g;
  g.init(GEOMETRY_LINEAR);
  const int len = 1 << 14;
  std::vector<float> centre;
  auto rows = planeWave(g, srcDeg, len, 0.02f, fLo, fHi, &centre);
  proc::Processor p;
  p.init(GEOMETRY_LINEAR, DEFAULT_CALIBRATION);
  p.setFixedAngle(steerDeg);
  p.setMode(proc::Mode::FixedBeam);
  auto in = interleave(rows, DEFAULT_CALIBRATION.posOfChannel);
  std::vector<float> out(len);
  proc::HopResult r;
  for (int h = 0; h < len / HOP; h++) p.process(&in[h * HOP * NUM_MICS], &out[h * HOP], nullptr, &r);
  const int skip = 8 * HOP;
  return 10.0f * log10f(power(&out[skip], len - skip) / power(&centre[skip - LATENCY], len - skip));
}

static void testDelayAndSum() {
  printf("delay-and-sum\n");
  for (float th : {-60.f, -20.f, 0.f, 35.f, 70.f}) {
    const float gdb = beamGainDb(th, th, 300, 7000);
    printf("  steered at source %+4.0f°: %+.3f dB\n", th, gdb);
    CHECK(fabsf(gdb) < 0.1f, "on-target gain at %g: %g dB", th, gdb);
  }
  // Source at broadside, beam at 60°: the high band must drop, the low band
  // barely (the aperture is tiny next to a 1 m wavelength).
  const float lo = beamGainDb(0, 60, 250, 450);
  const float hi = beamGainDb(0, 60, 3500, 4500);
  printf("  source 0°, beam 60°: 250-450 Hz %+.2f dB, 3.5-4.5 kHz %+.2f dB\n", lo, hi);
  CHECK(lo > -1.5f, "low band off-target %g dB", lo);
  CHECK(hi < -6.0f, "high band off-target %g dB", hi);
  // Wrong sign would show up as a steered-at-mirror source being the one that passes.
  const float mirror = beamGainDb(-40, 40, 3000, 5000);
  CHECK(mirror < -6.0f, "mirror-image source passes: %g dB", mirror);
}

static void testGccLead() {
  printf("gcc-phat pair lead\n");
  dsp::Geometry g;
  g.init(GEOMETRY_LINEAR);
  for (float th : {90.f, 0.f, -30.f}) {
    auto rows = planeWave(g, th, 1 << 13, 0.02f, 100, 8000, nullptr);
    dsp::Fft fft;
    fft.init(FFT_N);
    std::vector<float> win(FFT_N);
    dsp::sqrtHannPeriodic(win.data(), FFT_N);
    std::vector<float> X[NUM_MICS];
    for (int m = 0; m < NUM_MICS; m++) {
      X[m].resize(2 * FFT_N);
      for (int n = 0; n < FFT_N; n++) { X[m][2 * n] = rows[m][2048 + n] * win[n]; X[m][2 * n + 1] = 0; }
      fft.forward(X[m].data());
    }
    float coh;
    const float lead = doa::gccPhatLead(X[0].data(), X[3].data(), g, 0, 3, &coh);
    const float expect = (g.delay(3, th) - g.delay(0, th)) * SAMPLE_RATE;
    printf("  θ=%+4.0f°: Mic 4 leads Mic 1 by %+.3f samples (expect %+.3f), coherence %.2f\n",
           th, lead, expect, coh);
    CHECK(fabsf(lead - expect) < 0.05f, "lead %g vs %g", lead, expect);
  }
}

// Per-mic time trim: a channel recorded 40 µs late, corrected by advanceUs.
static void testDelayTrim() {
  printf("delay trim\n");
  dsp::Geometry g;
  g.init(GEOMETRY_LINEAR);
  auto rows = planeWave(g, 0.0f, 1 << 14, 0.02f, 100, 8000, nullptr);
  // Delay row 1 by 40 µs = 0.64 samples, exactly, via a second plane-wave trick:
  // regenerate the row from its spectrum with an extra phase ramp.
  dsp::Fft big;
  const int len = 1 << 14;
  big.init(len);
  std::vector<float> X(2 * len);
  for (int i = 0; i < len; i++) { X[2 * i] = rows[1][i]; X[2 * i + 1] = 0; }
  big.forward(X.data());
  for (int k = 0; k < len; k++) {
    const double f = (k <= len / 2 ? k : k - len) * (double)SAMPLE_RATE / len;
    const double ph = -2.0 * M_PI * f * 40e-6;
    const float c = (float)cos(ph), s = (float)sin(ph), re = X[2 * k], im = X[2 * k + 1];
    X[2 * k] = re * c - im * s;
    X[2 * k + 1] = re * s + im * c;
  }
  big.inverse(X.data());
  for (int i = 0; i < len; i++) rows[1][i] = X[2 * i];

  // Stationary noise keeps the speech gate shut, so judge the trim by the
  // scan's peak coherence, which is computed every hop.
  Calibration bad = DEFAULT_CALIBRATION, good = DEFAULT_CALIBRATION;
  good.advanceUs[1] = 40.0f;
  proc::Processor pb, pg;
  pb.init(GEOMETRY_LINEAR, bad);
  pg.init(GEOMETRY_LINEAR, good);
  pb.setMode(proc::Mode::DoaOnly);
  pg.setMode(proc::Mode::DoaOnly);
  auto in = interleave(rows, DEFAULT_CALIBRATION.posOfChannel);
  std::vector<float> out(HOP);
  proc::HopResult rb, rg;
  float peakB = 0, peakG = 0;
  int n = 0;
  for (int h = 0; h < 40; h++) {
    pb.process(&in[h * HOP * NUM_MICS], out.data(), nullptr, &rb);
    pg.process(&in[h * HOP * NUM_MICS], out.data(), nullptr, &rg);
    if (h >= 4) { peakB += rb.doa.peak; peakG += rg.doa.peak; n++; }
  }
  peakB /= n; peakG /= n;
  printf("  scan peak coherence: untrimmed %.3f, trimmed %.3f\n", peakB, peakG);
  CHECK(peakG > 0.99f, "trimmed coherence %g", peakG);
  CHECK(peakG > peakB + 0.02f, "trim didn't help: %g vs %g", peakG, peakB);
}

// The inbuilt-mic path: the array's high-pass, nothing else. A DC offset must
// go, the voice band must pass at unity, rumble must be cut, and the reported
// level must be the high-passed signal's.
static void testMonoConditioner() {
  printf("board-mic conditioner\n");
  const int hops = 64, settle = 16;   // ~250 ms for the 90 Hz high-pass to forget the step
  auto run = [&](float dc, float amp, float hz, float* levelDb) {
    proc::MonoConditioner m;
    m.init();
    std::vector<float> in(HOP), out(HOP);
    double p = 0, mean = 0, lvl = 0;
    int n = 0;
    for (int h = 0; h < hops; h++) {
      for (int i = 0; i < HOP; i++)
        in[i] = dc + amp * sinf(2.0f * (float)M_PI * hz * (h * HOP + i) / SAMPLE_RATE);
      const float db = m.process(in.data(), out.data());
      if (h < settle) continue;
      for (float y : out) { p += (double)y * y; mean += y; }
      lvl += pow(10.0, db / 10.0);
      n++;
    }
    *levelDb = (float)(10.0 * log10(lvl / n));
    mean /= (double)n * HOP;
    CHECK(fabs(mean) < 1e-4, "dc %.2f, %g Hz: residual mean %g", dc, hz, mean);
    return 10.0 * log10(p / ((double)n * HOP) / (amp * amp / 2.0));   // gain, dB
  };
  float lvl;
  const double g1k = run(0.1f, 0.1f, 1000.0f, &lvl);
  CHECK(fabs(g1k) < 0.05, "1 kHz gain %.3f dB", g1k);
  CHECK(fabsf(lvl - 20.0f * log10f(0.1f / sqrtf(2.0f))) < 0.05f, "1 kHz level %.2f dBFS", lvl);
  const double g20 = run(0.0f, 0.1f, 20.0f, &lvl);
  CHECK(g20 < -20.0, "20 Hz gain %.1f dB (2nd-order high-pass at %.0f Hz)", g20, HPF_HZ);
  printf("  1 kHz %+.3f dB, 20 Hz %+.1f dB\n", g1k, g20);
  // In place is how the firmware calls it.
  proc::MonoConditioner a, b;
  a.init();
  b.init();
  auto x = noise(HOP, 0.05f);
  std::vector<float> y(HOP);
  a.process(x.data(), y.data());
  b.process(x.data(), x.data());
  CHECK(!memcmp(x.data(), y.data(), sizeof(float) * HOP), "in-place result differs");
}

static void testGate() {
  printf("energy gate\n");
  doa::EnergyGate g;
  g.init();
  int opened = -1, closed = -1;
  for (int h = 0; h < 200; h++) {
    const float e = (h >= 100 && h < 130) ? 1e-4f : 1e-8f;   // 40 dB burst
    const bool o = g.update(e);
    if (o && opened < 0) opened = h;
    if (!o && opened >= 0 && closed < 0) closed = h;
  }
  CHECK(opened == 100, "opened at hop %d", opened);
  // 200 ms hangover = 13 hops at 16 ms.
  CHECK(closed >= 130 + 12 && closed <= 130 + 14, "closed at hop %d", closed);
}

static void testSteerTracker() {
  printf("auto-beam steering\n");
  proc::SteerTracker t;
  t.init(true, 0.0f);
  const float hopMs = 1000.0f * HOP / SAMPLE_RATE;
  // Inside the hysteresis band: never moves.
  for (int h = 0; h < 200; h++) t.update(8.0f);
  CHECK(t.steer() == 0.0f, "moved inside band: %g", t.steer());
  // A 200 ms excursion: too short.
  for (int h = 0; h < (int)(200 / hopMs); h++) t.update(40.0f);
  for (int h = 0; h < 10; h++) t.update(0.0f);
  CHECK(t.steer() == 0.0f, "moved on a short excursion: %g", t.steer());
  // A sustained move: waits ~300 ms, then slews without jumping.
  int startHop = -1;
  float prev = 0, maxStep = 0;
  for (int h = 0; h < 200; h++) {
    const float s = t.update(40.0f);
    if (s != 0.0f && startHop < 0) startHop = h;
    maxStep = std::max(maxStep, fabsf(s - prev));
    prev = s;
  }
  const float waitMs = startHop * hopMs;
  printf("  started after %.0f ms, max step %.2f°/hop, final %.1f°\n", waitMs, maxStep, t.steer());
  CHECK(waitMs >= 280 && waitMs <= 340, "hold time %g ms", waitMs);
  CHECK(maxStep <= STEER_SLEW_DEG_S * hopMs / 1000.0f + 1e-4f, "jumped %g°", maxStep);
  CHECK(fabsf(t.steer() - 40.0f) < 1e-4f, "didn't arrive: %g", t.steer());
}

static void testSyncCheck() {
  printf("i2s alignment analysis\n");
  const int n = 2048;
  std::uniform_int_distribution<int32_t> d(-(1 << 23), (1 << 23) - 1);
  std::vector<int32_t> base(n + 2000);
  for (auto& w : base) w = d(rng) * 256;   // 24-bit data, low byte zero like the INMP441
  for (int s : {0, 1, -1, 2, -3, 64, -129, 511}) {
    std::vector<int32_t> m(n), sl(n);
    const int o = 1000;
    for (int i = 0; i < n; i++) { m[i] = base[o + i]; sl[i] = base[o + i + s]; }
    auto r = sync_check::analyze(m.data(), sl.data(), n, 512);
    CHECK(r.status == sync_check::Status::Ok && r.slotOffset == s, "offset %d: %s %d", s,
          sync_check::statusName(r.status), r.slotOffset);
    CHECK(r.dropMaster % 2 == 0 && r.dropMaster >= 0 && r.dropSlave >= 0, "drops %d/%d",
          r.dropMaster, r.dropSlave);
    bool aligned = true;
    for (int i = 0; i + std::max(r.dropMaster, r.dropSlave) < n; i++)
      if (sl[i + r.dropSlave] != m[i + r.dropMaster]) { aligned = false; break; }
    CHECK(aligned, "offset %d: drops %d/%d don't align", s, r.dropMaster, r.dropSlave);
  }
  std::vector<int32_t> z(n, 0);
  CHECK(sync_check::analyze(z.data(), z.data(), n, 512).status == sync_check::Status::Inconclusive,
        "silence should be inconclusive");
  std::vector<int32_t> m(n), sl(n);
  for (int i = 0; i < n; i++) { m[i] = base[i]; sl[i] = (int32_t)((uint32_t)base[i + 2] << 1); }
  auto r = sync_check::analyze(m.data(), sl.data(), n, 512);
  CHECK(r.status == sync_check::Status::BitShift && r.bitShift == 1 && r.slotOffset == 2,
        "bit shift: %s shift %d offset %d", sync_check::statusName(r.status), r.bitShift, r.slotOffset);
  for (int i = 0; i < n; i++) sl[i] = d(rng) * 256;
  CHECK(sync_check::analyze(m.data(), sl.data(), n, 512).status == sync_check::Status::NoMatch,
        "unrelated streams should not match");
}

int main() {
  testFft();
  testGeometry();
  testReconstruction();
  testProcessorPassthrough();
  testSrpLinear();
  testSrpSquare();
  testTableAzimuth();
  testReverb();
  testChannelMap();
  testDelayAndSum();
  testGccLead();
  testDelayTrim();
  testMonoConditioner();
  testGate();
  testSteerTracker();
  testSyncCheck();
  return report("host_tests");
}
