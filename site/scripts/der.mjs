// Diarization error rate (DER), scored the way NIST md-eval and pyannote.metrics do:
// on a 10 ms grid, skipping ± collar around every reference boundary, overlapped
// speech included, with the one-to-one reference ↔ hypothesis speaker mapping that
// maximises their overlap (Hungarian algorithm).
//
//   missed      reference speech the hypothesis has too few speakers for
//   falseAlarm  hypothesis speakers beyond the reference's count
//   confusion   speech given to the wrong (unmapped) speaker
//   der = (missed + falseAlarm + confusion) / total reference speech
//
// Segments are [{ start, end, speaker }] in seconds; speaker is any key.

const STEP = 0.01;

// RTTM text → segments. Only SPEAKER lines: "SPEAKER file 1 start duration <NA> <NA> name ...".
export function readRttm(text) {
  const segs = [];
  for (const line of text.split('\n')) {
    const p = line.trim().split(/\s+/);
    if (p[0] !== 'SPEAKER') continue;
    const start = Number(p[3]);
    segs.push({ start, end: start + Number(p[4]), speaker: p[7] });
  }
  return segs;
}

// Best one-to-one assignment of rows to columns maximising the summed weight
// (Kuhn-Munkres on the square padding). Returns, per row, its column or -1.
function maxMatching(w, rows, cols) {
  const n = Math.max(rows, cols);
  const cost = (i, j) => (i < rows && j < cols ? -w[i][j] : 0);
  const u = new Float64Array(n + 1);
  const v = new Float64Array(n + 1);
  const p = new Int32Array(n + 1);
  const way = new Int32Array(n + 1);
  for (let i = 1; i <= n; i++) {
    p[0] = i;
    let j0 = 0;
    const minv = new Float64Array(n + 1).fill(Infinity);
    const used = new Uint8Array(n + 1);
    do {
      used[j0] = 1;
      const i0 = p[j0];
      let delta = Infinity;
      let j1 = 0;
      for (let j = 1; j <= n; j++) {
        if (used[j]) continue;
        const cur = cost(i0 - 1, j - 1) - u[i0] - v[j];
        if (cur < minv[j]) {
          minv[j] = cur;
          way[j] = j0;
        }
        if (minv[j] < delta) {
          delta = minv[j];
          j1 = j;
        }
      }
      for (let j = 0; j <= n; j++) {
        if (used[j]) {
          u[p[j]] += delta;
          v[j] -= delta;
        } else minv[j] -= delta;
      }
      j0 = j1;
    } while (p[j0] !== 0);
    do {
      const j1 = way[j0];
      p[j0] = p[j1];
      j0 = j1;
    } while (j0);
  }
  const map = new Int32Array(rows).fill(-1);
  for (let j = 1; j <= n; j++) {
    const i = p[j] - 1;
    if (i >= 0 && i < rows && j - 1 < cols && w[i][j - 1] > 0) map[i] = j - 1;
  }
  return map;
}

// Per-speaker activity on the grid: { names, act: Uint8Array[] }.
function rasterise(segs, n) {
  const names = [...new Set(segs.map((s) => s.speaker))];
  const act = names.map(() => new Uint8Array(n));
  for (const s of segs) {
    const a = act[names.indexOf(s.speaker)];
    for (let i = Math.max(0, Math.round(s.start / STEP)); i < Math.min(n, Math.round(s.end / STEP)); i++) a[i] = 1;
  }
  return { names, act };
}

// Scores hyp against ref. Times in seconds; the sums are seconds of speech.
export function der(ref, hyp, { collar = 0.25 } = {}) {
  const end = Math.max(0, ...ref.map((s) => s.end), ...hyp.map((s) => s.end));
  const n = Math.ceil(end / STEP) + 1;
  const R = rasterise(ref, n);
  const H = rasterise(hyp, n);
  const scored = new Uint8Array(n).fill(1);
  if (collar > 0) {
    for (const s of ref) {
      for (const b of [s.start, s.end]) {
        for (let i = Math.max(0, Math.round((b - collar) / STEP)); i < Math.min(n, Math.round((b + collar) / STEP)); i++) scored[i] = 0;
      }
    }
  }
  const overlap = R.act.map((r) => H.act.map((h) => {
    let c = 0;
    for (let i = 0; i < n; i++) if (scored[i] && r[i] && h[i]) c++;
    return c;
  }));
  const map = maxMatching(overlap, R.names.length, H.names.length);

  let total = 0;
  let missed = 0;
  let falseAlarm = 0;
  let confusion = 0;
  for (let i = 0; i < n; i++) {
    if (!scored[i]) continue;
    let nRef = 0;
    let nHyp = 0;
    let correct = 0;
    for (let r = 0; r < R.act.length; r++) {
      if (!R.act[r][i]) continue;
      nRef++;
      if (map[r] >= 0 && H.act[map[r]][i]) correct++;
    }
    for (const h of H.act) nHyp += h[i];
    total += nRef;
    missed += Math.max(0, nRef - nHyp);
    falseAlarm += Math.max(0, nHyp - nRef);
    confusion += Math.min(nRef, nHyp) - correct;
  }
  const sec = (x) => x * STEP;
  return {
    total: sec(total),
    missed: sec(missed),
    falseAlarm: sec(falseAlarm),
    confusion: sec(confusion),
    der: total ? (missed + falseAlarm + confusion) / total : 0,
    refSpeakers: R.names.length,
    hypSpeakers: H.names.length,
  };
}

// Sums der() results (weights each file by its reference speech).
export function sumDer(results) {
  const s = { total: 0, missed: 0, falseAlarm: 0, confusion: 0 };
  for (const r of results) for (const k in s) s[k] += r[k];
  return { ...s, der: s.total ? (s.missed + s.falseAlarm + s.confusion) / s.total : 0 };
}

// "34.1% (17.0 / 0.2 / 16.9)": DER, then missed / false alarm / confusion.
export const formatDer = (r) =>
  `${(100 * r.der).toFixed(1)}% (${[r.missed, r.falseAlarm, r.confusion].map((x) => ((100 * x) / (r.total || 1)).toFixed(1)).join(' / ')})`;
