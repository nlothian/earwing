// Kaldi-compatible log-mel filterbank ("fbank") features for the speaker-embedding
// model (WeSpeaker ResNet34-LM, input `feats` [B, T, 80]).
//
// Reproduces what pyannote / WeSpeaker compute with torchaudio:
//   torchaudio.compliance.kaldi.fbank(waveform * 32768, num_mel_bins=80,
//     frame_length=25, frame_shift=10, dither=0.0, sample_frequency=16000,
//     window_type='hamming', use_energy=False)
// and Kaldi defaults for everything else: snip_edges (only whole 400-sample
// frames, hop 160), per-frame DC removal, pre-emphasis 0.97, a symmetric Hamming
// window, zero-padding to a 512-point FFT, power spectrum, 80 triangular mel bins
// from 20 Hz to Nyquist, and a natural log floored at float32 epsilon.
// No mean normalisation: the caller applies CMN over the frames it keeps.
//
// Plain ES module with no dependencies, so it runs in the inference worker and
// in Node. The mel weights replicate torchaudio's float32 arithmetic; the
// per-frame maths is float64, so results agree with torchaudio to its float32
// rounding (mean ~4e-6, worst ~1e-4 on the log scale for real speech).

export const FBANK_BINS = 80;

const SAMPLE_RATE = 16000;
const FRAME_LEN = 400; // 25 ms
const FRAME_SHIFT = 160; // 10 ms
const FFT_SIZE = 512; // FRAME_LEN rounded up to a power of two
const HALF = FFT_SIZE / 2; // complex FFT length used for the real-input trick
const PREEMPH = 0.97;
const LOW_FREQ = 20;
const HIGH_FREQ = SAMPLE_RATE / 2; // Kaldi high_freq=0 means Nyquist
const EPSILON = 1.1920928955078125e-7; // float32 epsilon, the log floor
const LOG_FLOOR = Math.log(EPSILON);
const INT16_SCALE = 32768; // Kaldi expects int16-range samples

// Symmetric Hamming window: 0.54 - 0.46 cos(2πn / (N - 1)).
const WINDOW = new Float64Array(FRAME_LEN);
for (let n = 0; n < FRAME_LEN; n++) {
  WINDOW[n] = 0.54 - 0.46 * Math.cos((2 * Math.PI * n) / (FRAME_LEN - 1));
}

// Twiddles and bit-reversal table for the HALF-point complex FFT, plus the
// e^{-2πik/FFT_SIZE} factors that split its output into the real FFT's bins.
const TW_RE = new Float64Array(HALF / 2);
const TW_IM = new Float64Array(HALF / 2);
for (let k = 0; k < HALF / 2; k++) {
  TW_RE[k] = Math.cos((-2 * Math.PI * k) / HALF);
  TW_IM[k] = Math.sin((-2 * Math.PI * k) / HALF);
}
const SPLIT_RE = new Float64Array(HALF);
const SPLIT_IM = new Float64Array(HALF);
for (let k = 0; k < HALF; k++) {
  SPLIT_RE[k] = Math.cos((-2 * Math.PI * k) / FFT_SIZE);
  SPLIT_IM[k] = Math.sin((-2 * Math.PI * k) / FFT_SIZE);
}
const BITREV = new Uint16Array(HALF);
for (let i = 0, bits = Math.log2(HALF); i < HALF; i++) {
  let r = 0;
  for (let b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
  BITREV[i] = r;
}

// Mel filter bank, stored sparsely: filter m covers FFT bins
// [MEL_START[m], MEL_START[m] + MEL_LEN[m]) with weights at MEL_OFFSET[m].
// Kaldi builds the triangles in the mel domain over bins 0..HALF-1 (the
// Nyquist bin never gets any weight).
// The weights are computed in float32, step for step as torchaudio's
// get_mel_banks does. That matters for the narrow low-frequency filters: a
// float64 bank puts, e.g., filter 9's small weight on bin 8 off by 2e-4
// relative, which shows up directly in the log energy.
const f32 = Math.fround;
const melScale = (f) => 1127 * Math.log(1 + f / 700);
const melScale32 = (f) => f32(1127 * f32(Math.log(f32(1 + f32(f / 700)))));
const MEL_START = new Int32Array(FBANK_BINS);
const MEL_LEN = new Int32Array(FBANK_BINS);
const MEL_OFFSET = new Int32Array(FBANK_BINS);
const MEL_WEIGHTS = (() => {
  const binWidth = SAMPLE_RATE / FFT_SIZE;
  // The band edges are float64 Python scalars, cast to float32 when used.
  const melLow64 = melScale(LOW_FREQ);
  const melDelta = f32((melScale(HIGH_FREQ) - melLow64) / (FBANK_BINS + 1));
  const melLow = f32(melLow64);
  const weights = [];
  for (let m = 0; m < FBANK_BINS; m++) {
    const left = f32(melLow + f32(m * melDelta));
    const center = f32(melLow + f32((m + 1) * melDelta));
    const right = f32(melLow + f32((m + 2) * melDelta));
    MEL_OFFSET[m] = weights.length;
    // Each triangle is one contiguous run of positive weights.
    for (let k = 0; k < HALF; k++) {
      const mel = melScale32(binWidth * k);
      const up = f32(f32(mel - left) / f32(center - left));
      const down = f32(f32(right - mel) / f32(right - center));
      const v = Math.min(up, down);
      if (v <= 0) continue;
      if (MEL_LEN[m] === 0) MEL_START[m] = k;
      weights.push(v);
      MEL_LEN[m]++;
    }
  }
  return Float64Array.from(weights);
})();

// Scratch buffers, reused across frames and calls.
const frame = new Float64Array(FRAME_LEN);
const re = new Float64Array(HALF);
const im = new Float64Array(HALF);
const power = new Float64Array(HALF); // bins 0..HALF-1; Nyquist is unused

// In-place iterative radix-2 complex FFT of length HALF over (re, im),
// expecting input already in bit-reversed order.
function fftInPlace() {
  for (let size = 2; size <= HALF; size <<= 1) {
    const halfSize = size >> 1;
    const step = HALF / size;
    for (let base = 0; base < HALF; base += size) {
      for (let j = 0, t = 0; j < halfSize; j++, t += step) {
        const a = base + j;
        const b = a + halfSize;
        const wr = TW_RE[t];
        const wi = TW_IM[t];
        const xr = re[b] * wr - im[b] * wi;
        const xi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
      }
    }
  }
}

// Power spectrum |X[k]|² for k < HALF of the zero-padded, windowed frame.
// The 512 real samples are packed as 256 complex ones (z[n] = x[2n] + i·x[2n+1]),
// transformed, then split: X[k] = E[k] + e^{-2πik/512}·O[k].
function powerSpectrum() {
  for (let n = 0; n < HALF; n++) {
    const r = BITREV[n];
    const i0 = 2 * n;
    // Samples past FRAME_LEN are the zero padding.
    re[r] = i0 < FRAME_LEN ? frame[i0] : 0;
    im[r] = i0 + 1 < FRAME_LEN ? frame[i0 + 1] : 0;
  }
  fftInPlace();
  // k = 0: E = Re Z[0], O = Im Z[0], so X[0] = E + O (real).
  const x0 = re[0] + im[0];
  power[0] = x0 * x0;
  for (let k = 1; k < HALF; k++) {
    const zr = re[k];
    const zi = im[k];
    const cr = re[HALF - k]; // Z[N-k], conjugated below
    const ci = -im[HALF - k];
    const er = 0.5 * (zr + cr);
    const ei = 0.5 * (zi + ci);
    // O[k] = (Z[k] - conj(Z[N-k])) / 2i
    const or = 0.5 * (zi - ci);
    const oi = -0.5 * (zr - cr);
    const xr = er + SPLIT_RE[k] * or - SPLIT_IM[k] * oi;
    const xi = ei + SPLIT_RE[k] * oi + SPLIT_IM[k] * or;
    power[k] = xr * xr + xi * xi;
  }
}

// samples: Float32Array, 16 kHz mono in [-1, 1].
// Returns { data: Float32Array(frames * FBANK_BINS), frames }, row-major.
export function fbank(samples) {
  const n = samples.length;
  const frames = n < FRAME_LEN ? 0 : 1 + Math.floor((n - FRAME_LEN) / FRAME_SHIFT);
  const data = new Float32Array(frames * FBANK_BINS);

  for (let f = 0; f < frames; f++) {
    const off = f * FRAME_SHIFT;

    // Copy at int16 scale and remove the DC offset.
    let sum = 0;
    for (let i = 0; i < FRAME_LEN; i++) {
      const v = samples[off + i] * INT16_SCALE;
      frame[i] = v;
      sum += v;
    }
    const mean = sum / FRAME_LEN;
    for (let i = 0; i < FRAME_LEN; i++) frame[i] -= mean;

    // Kaldi pre-emphasis runs back to front; the first sample uses itself.
    for (let i = FRAME_LEN - 1; i > 0; i--) frame[i] -= PREEMPH * frame[i - 1];
    frame[0] -= PREEMPH * frame[0];

    for (let i = 0; i < FRAME_LEN; i++) frame[i] *= WINDOW[i];

    powerSpectrum();

    const row = f * FBANK_BINS;
    for (let m = 0; m < FBANK_BINS; m++) {
      const start = MEL_START[m];
      const len = MEL_LEN[m];
      const wOff = MEL_OFFSET[m];
      let e = 0;
      for (let j = 0; j < len; j++) e += MEL_WEIGHTS[wOff + j] * power[start + j];
      data[row + m] = e > EPSILON ? Math.log(e) : LOG_FLOOR;
    }
  }

  return { data, frames };
}
