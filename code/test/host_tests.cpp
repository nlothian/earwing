// Host-side tests for the portable DSP: FFT, STFT reconstruction, SRP-PHAT,
// delay-and-sum, GCC-PHAT, steering hysteresis and the I2S alignment check.
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
#include "doa.h"
#include "dsp.h"
#include "processor.h"
#include "sync_check.h"

using namespace audio;

static int failures = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      failures++;                                         \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__);       \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
    }                                                     \
  } while (0)

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
static std::vector<std::vector<float>> planeWave(const dsp::Geometry& g, float thetaDeg, int len,
                                                 float rms, float fLo, float fHi,
                                                 std::vector<float>* centre) {
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
    synth(g.delay(m, thetaDeg), rows[m]);
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
  testChannelMap();
  testDelayAndSum();
  testGccLead();
  testDelayTrim();
  testMonoConditioner();
  testGate();
  testSteerTracker();
  testSyncCheck();
  printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
