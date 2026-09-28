// Speaker diarization: who spoke when, for the live beam and for files.
//
// Two small ONNX models on the onnxruntime-web instance the caller passes in:
//
//   segmentation  pyannote/segmentation-3.0 (onnx-community export, ~6 MB). A 10 s
//                 window in, 589 frames (one per 270 samples, ~17 ms) out, each a
//                 powerset over up to 3 LOCAL speakers: {}, {1}, {2}, {3}, {1,2},
//                 {1,3}, {2,3}. "Local" means the labels only hold within that one
//                 window: speaker 1 in one window may be speaker 2 in the next.
//   embedding     WeSpeaker ResNet34-LM (~26 MB), the embedding pyannote 3.1 uses.
//                 80-dim Kaldi fbank frames in, one 256-dim voice print out.
//
// Online pipeline (diart / pyannote style), every STEP (~2.5 s) of new audio:
//   1. segment the last 10 s (zero-padded on the left at the start);
//   2. for each local speaker with enough speech where it talks ALONE, embed those
//      fbank frames (the most recent MAX_EMB_SEC of them, to bound the cost);
//   3. map local speakers to global ones: a one-to-one assignment (two local
//      speakers in a window are never the same person) scored by cosine similarity
//      to each global speaker's embeddings so far, plus how well the local speaker's activity
//      agrees with what earlier windows already said about the overlapping 7.5 s.
//      A local speaker that matches nobody well enough becomes a new global speaker;
//   4. add the window's (now global) activity to a per-frame tally. Each frame is
//      covered by 4 windows; a speaker is active in a frame when at least half of
//      the windows covering it say so.
// Labels for the newest ~10 s are provisional (fewer windows have voted on them).
//
// recluster() is the offline pass for files or when a live take stops: agglomerative
// clustering of every stored embedding (average linkage), then
// the same one-to-one assignment per window against the cluster centroids, and the
// tally is rebuilt. It fixes early mistakes the online pass can't take back (a
// speaker split in two because their first centroid came from one noisy second).
//
// Plain JS apart from `ort`: runs in a Web Worker and in Node (scripts/diarize-file.mjs).

import { FBANK_BINS, fbank } from './fbank.js';

export const SR = 16000;
export const SEG_SAMPLES = 160000; // the segmentation model's 10 s window
export const FRAME_HOP = 270; // samples per segmentation frame
const FRAME_RF = 991; // receptive field of one frame (samples)
export const SEG_FRAMES = 589; // frames per 10 s window
const LOCAL_SPEAKERS = 3;
// powerset class → bit mask of active local speakers
const POWERSET = [0b000, 0b001, 0b010, 0b100, 0b011, 0b101, 0b110];

export const DEFAULTS = {
  stepFrames: 148, // window step: 148 × 270 = 39,960 samples ≈ 2.5 s
  minEmbSec: 0.5, // a local speaker needs this much solo speech in a window to be embedded
  maxEmbSec: 5.0, // embed at most the latest this-much of it (the embedding is the cost)
  // Similarities are average linkage: the mean cosine between an embedding and a
  // speaker's embeddings. On pzm12 (far-field meeting), same-speaker pairs have a
  // median of 0.61 (p10 0.36), different speakers 0.11 (p90 0.28).
  // Online, new speakers are created cautiously (the periodic recluster can still
  // split a speaker; each wrongly created one shows up as a label that later vanishes).
  newSpeakerSim: 0.2, // below this to every speaker so far: a new speaker
  minNewSpeakerSec: 3.0, // ...but only with this much solo speech (else leave it unassigned)
  overlapWeight: 0.5, // weight of "agrees with earlier windows" next to cosine similarity
  minOverlapSec: 0.5, // local activity needed in the already-tallied region to use that cue
  // recluster()
  // While live, re-cluster everything this often (0: only when asked). Frequent
  // reclusters fix a label within seconds instead of leaving it wrong until the next
  // one: on VoxConverse dialog (scripts/score-diarization.mjs), settled live DER was
  // 28% at 30 s, 21% at 10 s, 19% at 5 s (after Stop: 19% either way), and the share
  // of shown labels that later changed fell from 19% to 5%. 2.5 s gained nothing more.
  reclusterEverySec: 5,
  // ...but spend at most this share of the worker's time reclustering. Each recluster
  // grows with the take (O(embeddings²): 0.5 s at 1 h 45 min), so in takes of hours, and
  // for files (processed faster than real time), the interval stretches.
  reclusterMaxLoad: 0.1,
  // keep merging clusters while their average linkage is at least this. On pzm12, 0.3
  // merged 6 voices into 4 and 0.45 split them into 10 (DER ~28% throughout, vs a
  // sherpa-onnx offline reference); 0.35 found 6, the plausible count.
  mergeSim: 0.35,
  minClusterWeight: 6, // clusters lighter than this (in ~seconds of embedded speech) are folded into the nearest big one
  // segments(): pyannote-style smoothing
  minDurationOn: 0.3, // drop active runs shorter than this (s)
  minDurationOff: 0.5, // fill same-speaker gaps shorter than this (s)
};

const frameTime = (f) => (f * FRAME_HOP + FRAME_RF / 2) / SR; // centre of global frame f

function l2normalize(v) {
  let n = 0;
  for (let i = 0; i < v.length; i++) n += v[i] * v[i];
  n = Math.sqrt(n) || 1;
  const out = new Float32Array(v.length);
  for (let i = 0; i < v.length; i++) out[i] = v[i] / n;
  return out;
}

function dot(a, b) {
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a[i] * b[i];
  return s;
}

// Growable Uint8 array addressed by global frame index.
class FrameArray {
  constructor() {
    this.a = new Uint8Array(4096);
  }
  ensure(n) {
    if (n <= this.a.length) return;
    let len = this.a.length;
    while (len < n) len *= 2;
    const b = new Uint8Array(len);
    b.set(this.a);
    this.a = b;
  }
  clear() {
    this.a.fill(0);
  }
}

// Average-linkage agglomerative clustering of weighted unit vectors, merging while the
// linkage is at least mergeSim. Returns the clusters as arrays of item indices.
// cos: the items' pairwise cosines, lower triangle (pair i > j at i(i-1)/2 + j).
//
// A cluster pair's average linkage (the weighted mean cosine between their members) is
// S(A, B) / (wA·wB), with S the weighted sum of the members' pairwise cosines, and
// S(A∪B, C) = S(A, C) + S(B, C): a merge is one pass over a row, no embeddings touched.
// Merges are found by nearest-neighbour chain, O(n²) in all. Average linkage is
// reducible, so the chain builds the same hierarchy as always merging the closest
// pair, and cutting it at mergeSim gives the same clusters.
function averageLinkage(cos, weights, mergeSim) {
  const n = weights.length;
  const w = Float64Array.from(weights);
  const S = new Float32Array((n * (n - 1)) / 2);
  for (let i = 1; i < n; i++) {
    const row = (i * (i - 1)) / 2;
    for (let j = 0; j < i; j++) S[row + j] = cos[row + j] * w[i] * w[j];
  }
  const at = (i, j) => (i > j ? (i * (i - 1)) / 2 + j : (j * (j - 1)) / 2 + i);
  const alive = new Uint8Array(n).fill(1);
  const parent = Int32Array.from({ length: n }, (_, i) => i); // union-find over the cut
  const find = (i) => {
    while (parent[i] !== i) i = parent[i] = parent[parent[i]];
    return i;
  };
  const chain = new Int32Array(n);
  let len = 0;
  let start = 0;
  for (let remaining = n; remaining > 1; ) {
    if (len === 0) {
      while (!alive[start]) start++;
      chain[len++] = start;
    }
    const a = chain[len - 1];
    const prev = len > 1 ? chain[len - 2] : -1;
    let b = prev;
    let best = prev >= 0 ? S[at(a, prev)] / (w[a] * w[prev]) : -Infinity;
    for (let c = 0; c < n; c++) {
      if (c === a || !alive[c]) continue;
      const v = S[at(a, c)] / (w[a] * w[c]);
      if (v > best) {
        best = v;
        b = c;
      }
    }
    if (b !== prev) {
      chain[len++] = b;
      continue;
    }
    // a and b are each other's nearest: merge b into a.
    len -= 2;
    for (let c = 0; c < n; c++) if (c !== a && c !== b && alive[c]) S[at(a, c)] += S[at(b, c)];
    w[a] += w[b];
    alive[b] = 0;
    remaining--;
    // Linkage only falls going up the hierarchy, so the merges at or above the cut are
    // exactly those that happen within clusters of the cut.
    if (best >= mergeSim) parent[find(b)] = find(a);
  }
  const groups = new Map();
  for (let i = 0; i < n; i++) {
    const r = find(i);
    if (!groups.has(r)) groups.set(r, []);
    groups.get(r).push(i);
  }
  return [...groups.values()];
}

// Best one-to-one assignment of local speakers to options by brute force. score(k, g)
// for g in 0..n-1 is the score of joining global g; newScore[k] (or -Infinity) of
// becoming a new speaker. A local may also stay unassigned (score 0). Returns, per
// local, g, 'new' or null. Each local only needs its best L globals (L = number of
// locals, at most 3): if it took one outside them, one of them would still be free
// and at least as good. So at most (L+2)^L combinations, however many speakers.
function assign(locals, n, score, newScore) {
  const L = locals.length;
  const table = locals.map((k) => {
    const row = new Float64Array(n);
    for (let g = 0; g < n; g++) row[g] = score(k, g);
    return row;
  });
  const cands = table.map((row) => {
    const gs = [];
    for (let g = 0; g < n; g++) if (row[g] > -Infinity) gs.push(g);
    return gs
      .sort((a, b) => row[b] - row[a] || a - b)
      .slice(0, L)
      .sort((a, b) => a - b);
  });
  let best = -Infinity;
  let bestPick = null;
  const pick = new Array(L);
  const used = new Set();
  const rec = (i, total) => {
    if (i === locals.length) {
      if (total > best) {
        best = total;
        bestPick = pick.slice();
      }
      return;
    }
    const k = locals[i];
    pick[i] = null;
    rec(i + 1, total);
    if (newScore[k] > -Infinity) {
      pick[i] = 'new';
      rec(i + 1, total + newScore[k]);
    }
    for (const g of cands[i]) {
      if (used.has(g)) continue;
      used.add(g);
      pick[i] = g;
      rec(i + 1, total + table[i][g]);
      used.delete(g);
    }
  };
  rec(0, 0);
  return bestPick || locals.map(() => null);
}

export class Diarizer {
  // segSession / embSession: ort.InferenceSession for the two models.
  constructor(ort, segSession, embSession, options = {}) {
    this.ort = ort;
    this.seg = segSession;
    this.emb = embSession;
    this.opt = { ...DEFAULTS, ...options };
    this.reset();
  }

  reset() {
    this.chunks = []; // retained 16 kHz audio, from sample this.base
    this.base = 0;
    this.total = 0; // samples pushed
    this.nextWindow = 0; // index k of the next scheduled window
    this.lastEnd = 0; // end sample of the last window run
    this.windows = []; // { f0, bits: Uint8Array(589), embs: [{ k, emb, weight }], map: [g|null ×3] }
    this.speakers = []; // global: { sum: Float32Array(256), weight, name }
    this.nextName = 0; // speakers keep their name (a number) across reclusters
    this.act = []; // per global speaker: FrameArray of votes
    this.cnt = new FrameArray(); // windows covering each frame
    this.frames = 0; // frames with audio so far
    this.lastRecluster = 0; // this.total at the last recluster()
    this.reclusterSec = 0; // how long the last recluster() took
    this.reclusterEnd = 0; // performance.now() when it finished
    this.pairCos = { n: 0, tri: new Float32Array(0) }; // recluster(): cosines of stored embeddings
    this.stats = { windows: 0, segMs: 0, embMs: 0, embeddings: 0, reclusters: 0, reclusterMs: 0 };
  }

  push(samples) {
    if (!samples || !samples.length) return;
    this.chunks.push(samples);
    this.total += samples.length;
  }

  // Window k starts at global frame k·step − 592 (so the first window's END is one step
  // in; its left part is padding) and is due once its end sample has arrived.
  windowStartFrame(k) {
    return k * this.opt.stepFrames - Math.ceil(SEG_SAMPLES / FRAME_HOP);
  }

  windowEnd(k) {
    return this.windowStartFrame(k) * FRAME_HOP + SEG_SAMPLES;
  }

  // Run every window that's due. final: also run one last window ending at the live
  // edge, so the tail after the last full step gets labels. Returns windows run.
  async process({ final = false } = {}) {
    let n = 0;
    while (this.windowEnd(this.nextWindow + 1) <= this.total) {
      this.nextWindow += 1;
      await this.runWindow(this.windowStartFrame(this.nextWindow));
      n += 1;
    }
    if (final && this.total > this.lastEnd) {
      const f0 = Math.floor((this.total - SEG_SAMPLES) / FRAME_HOP);
      await this.runWindow(f0);
      n += 1;
    }
    const every = this.opt.reclusterEverySec * SR;
    const rested = performance.now() - this.reclusterEnd >= (1000 * this.reclusterSec) / this.opt.reclusterMaxLoad;
    if (n && every && this.total - this.lastRecluster >= every && rested) this.recluster();
    this.prune();
    return n;
  }

  // Keep only the audio a future window can reach.
  prune() {
    const keepFrom = this.windowStartFrame(this.nextWindow + 1) * FRAME_HOP;
    let base = this.base;
    let drop = 0;
    while (drop < this.chunks.length - 1 && base + this.chunks[drop].length <= keepFrom) {
      base += this.chunks[drop].length;
      drop += 1;
    }
    if (drop) {
      this.chunks.splice(0, drop);
      this.base = base;
    }
  }

  // Audio [start, end) in absolute samples, zero where before 0 (or already pruned).
  slice(start, end) {
    const out = new Float32Array(end - start);
    let pos = this.base;
    for (const c of this.chunks) {
      const cEnd = pos + c.length;
      const a = Math.max(start, pos);
      const b = Math.min(end, cEnd);
      if (b > a) out.set(c.subarray(a - pos, b - pos), a - start);
      if (cEnd >= end) break;
      pos = cEnd;
    }
    return out;
  }

  async runWindow(f0) {
    const opt = this.opt;
    const start = f0 * FRAME_HOP;
    const end = start + SEG_SAMPLES;
    this.lastEnd = Math.max(this.lastEnd, end);
    const audio = this.slice(start, end);

    // 1. Segmentation → local speaker bit masks per frame (hard powerset argmax).
    let t = performance.now();
    const out = await this.seg.run({
      input_values: new this.ort.Tensor('float32', audio, [1, 1, SEG_SAMPLES]),
    });
    const logits = out.logits.data;
    const nFrames = Math.min(SEG_FRAMES, out.logits.dims[1]);
    const bits = new Uint8Array(nFrames);
    const firstReal = Math.max(0, -f0); // frames before sample 0 are padding
    for (let i = firstReal; i < nFrames; i++) {
      let best = 0;
      for (let c = 1; c < 7; c++) if (logits[i * 7 + c] > logits[i * 7 + best]) best = c;
      bits[i] = POWERSET[best];
    }
    this.stats.segMs += performance.now() - t;

    // 2. Embeddings of each local speaker's solo speech.
    t = performance.now();
    const realStart = Math.max(0, start);
    const feats = fbank(audio.subarray(realStart - start));
    const embs = [];
    for (let k = 0; k < LOCAL_SPEAKERS; k++) {
      const mask = 1 << k;
      // fbank frame j (10 ms hop, 25 ms long) → the segmentation frame at its centre
      const rows = [];
      for (let j = 0; j < feats.frames; j++) {
        const centre = realStart - start + j * 160 + 200;
        const i = Math.min(nFrames - 1, Math.max(0, Math.round((centre - FRAME_RF / 2) / FRAME_HOP)));
        if (bits[i] === mask) rows.push(j);
      }
      if (rows.length < opt.minEmbSec * 100) continue;
      const use = rows.slice(-Math.round(opt.maxEmbSec * 100));
      const x = new Float32Array(use.length * FBANK_BINS);
      const mean = new Float32Array(FBANK_BINS);
      use.forEach((j, r) => {
        for (let b = 0; b < FBANK_BINS; b++) {
          const v = feats.data[j * FBANK_BINS + b];
          x[r * FBANK_BINS + b] = v;
          mean[b] += v;
        }
      });
      for (let b = 0; b < FBANK_BINS; b++) mean[b] /= use.length;
      for (let r = 0; r < use.length; r++) for (let b = 0; b < FBANK_BINS; b++) x[r * FBANK_BINS + b] -= mean[b];
      const e = await this.emb.run({ feats: new this.ort.Tensor('float32', x, [1, use.length, FBANK_BINS]) });
      embs.push({ k, emb: l2normalize(e.embs.data), weight: rows.length / 100 }); // weight: solo seconds
      this.stats.embeddings += 1;
    }
    this.stats.embMs += performance.now() - t;

    // 3. Local → global.
    const map = this.assignOnline(f0, bits, embs);
    const win = { f0, bits, embs, map };
    this.windows.push(win);
    this.stats.windows += 1;

    // 4. Tally.
    this.addToTally(win, +1);
  }

  // Seconds each local speaker is active in frames that earlier windows already
  // covered, and how many of those frames each global speaker was active in.
  overlapEvidence(f0, bits) {
    const n = this.speakers.length;
    const local = new Float32Array(LOCAL_SPEAKERS);
    const joint = Array.from({ length: LOCAL_SPEAKERS }, () => new Float32Array(n));
    for (let i = 0; i < bits.length; i++) {
      const f = f0 + i;
      if (f < 0 || f >= this.frames) continue;
      const c = this.cnt.a[f];
      if (!c) continue;
      for (let k = 0; k < LOCAL_SPEAKERS; k++) {
        if (!(bits[i] & (1 << k))) continue;
        local[k] += 1;
        for (let g = 0; g < n; g++) joint[k][g] += this.act[g].a[f] / c;
      }
    }
    return { local, joint };
  }

  assignOnline(f0, bits, embs) {
    const opt = this.opt;
    const n = this.speakers.length;
    const embOf = new Array(LOCAL_SPEAKERS).fill(null);
    for (const e of embs) embOf[e.k] = e;
    const { local, joint } = this.overlapEvidence(f0, bits);
    const minOverlapFrames = (opt.minOverlapSec * SR) / FRAME_HOP;
    const means = this.speakers.map((s) => s.sum.map((v) => v / s.weight));

    const locals = [];
    for (let k = 0; k < LOCAL_SPEAKERS; k++) {
      let any = false;
      for (let i = 0; i < bits.length && !any; i++) any = !!(bits[i] & (1 << k));
      if (any) locals.push(k);
    }
    // mean cosine to that speaker's embeddings so far (average linkage, as in recluster)
    const sim = (k, g) => (embOf[k] ? dot(embOf[k].emb, means[g]) : null);
    const score = (k, g) => {
      const s = sim(k, g);
      const ov = local[k] >= minOverlapFrames ? joint[k][g] / local[k] : null;
      if (s === null && ov === null) return -Infinity;
      // Without an embedding, only the overlap cue can place it; with one, the
      // similarity must clear the new-speaker bar for the pairing to beat 'new'.
      if (s === null) return ov > 0.5 ? ov * opt.overlapWeight : -Infinity;
      return s - opt.newSpeakerSim + (ov === null ? 0 : (ov - 0.5) * opt.overlapWeight);
    };
    const newScore = new Array(LOCAL_SPEAKERS).fill(-Infinity);
    for (const k of locals) {
      // just above 'unassigned' (0), so a long-enough stranger does become a speaker
      if (embOf[k] && embOf[k].weight >= opt.minNewSpeakerSec) newScore[k] = 1e-3;
    }
    const picks = assign(locals, n, score, newScore);

    const map = new Array(LOCAL_SPEAKERS).fill(null);
    locals.forEach((k, i) => {
      let g = picks[i];
      if (g === 'new') {
        g = this.speakers.length;
        this.speakers.push({ sum: new Float32Array(256), weight: 0, name: this.nextName++ });
        this.act.push(new FrameArray());
      }
      map[k] = g;
      if (g !== null && embOf[k]) {
        const s = this.speakers[g];
        const w = Math.min(embOf[k].weight, opt.maxEmbSec);
        for (let d = 0; d < 256; d++) s.sum[d] += embOf[k].emb[d] * w;
        s.weight += w;
      }
    });
    return map;
  }

  addToTally(win, sign) {
    const { f0, bits, map } = win;
    const lastFrame = f0 + bits.length;
    this.cnt.ensure(lastFrame);
    for (const a of this.act) a.ensure(lastFrame);
    for (let i = 0; i < bits.length; i++) {
      const f = f0 + i;
      if (f < 0) continue;
      this.cnt.a[f] += sign;
      for (let k = 0; k < LOCAL_SPEAKERS; k++) {
        if (map[k] !== null && bits[i] & (1 << k)) this.act[map[k]].a[f] += sign;
      }
    }
    // Frames past the audio's end (the final window's right edge) don't count.
    const audioFrames = Math.floor(this.total / FRAME_HOP);
    this.frames = Math.max(this.frames, Math.min(lastFrame, audioFrames));
  }

  // Pairwise cosines of the stored embeddings (lower triangle, for averageLinkage).
  // Embeddings are only ever appended, in window order, so each recluster computes just
  // the rows of the ones added since the last. 4 bytes a pair: ~17 MB for an hour.
  pairCosines(items) {
    const c = this.pairCos;
    const n = items.length;
    const need = (n * (n - 1)) / 2;
    if (c.tri.length < need) {
      const grown = new Float32Array(Math.max(need, 2 * c.tri.length));
      grown.set(c.tri.subarray(0, (c.n * (c.n - 1)) / 2));
      c.tri = grown;
    }
    for (let i = c.n; i < n; i++) {
      const row = (i * (i - 1)) / 2;
      for (let j = 0; j < i; j++) c.tri[row + j] = dot(items[i].emb, items[j].emb);
    }
    c.n = n;
    return c.tri;
  }

  // Offline pass over everything stored: cluster all embeddings, reassign every
  // window's local speakers against the clusters, rebuild the tally and centroids.
  recluster() {
    const opt = this.opt;
    const t0 = performance.now();
    this.lastRecluster = this.total;
    const items = [];
    this.windows.forEach((w, wi) => w.embs.forEach((e) => items.push({ wi, k: e.k, emb: e.emb, weight: e.weight })));
    if (items.length === 0) return;

    const weights = items.map((it) => Math.min(it.weight, opt.maxEmbSec));
    const clusters = averageLinkage(this.pairCosines(items), weights, opt.mergeSim).map((idx) => {
      const sum = new Float32Array(256);
      let weight = 0;
      for (const i of idx) {
        for (let d = 0; d < 256; d++) sum[d] += items[i].emb[d] * weights[i];
        weight += weights[i];
      }
      return { sum, weight, members: idx.map((i) => items[i]) };
    });
    // Small clusters (pyannote's min_cluster_size) don't become speakers; their windows
    // are reassigned to the nearest big one below.
    let big = clusters.filter((c) => c.weight >= opt.minClusterWeight);
    if (big.length === 0) big = [clusters.reduce((x, y) => (x.weight >= y.weight ? x : y))];
    // Order speakers by first appearance, so "Speaker 1" is the first to talk.
    const firstWindow = (c) => Math.min(...c.members.map((m) => m.wi));
    big.sort((x, y) => firstWindow(x) - firstWindow(y));
    const means = big.map((c) => c.sum.map((v) => v / c.weight));

    // Reassign every window, one-to-one, against the final centroids.
    const old = { speakers: this.speakers, act: this.act, cnt: this.cnt, frames: this.frames };
    this.speakers = big.map(() => ({ sum: new Float32Array(256), weight: 0, name: -1 }));
    this.act = big.map(() => new FrameArray());
    this.cnt = new FrameArray();
    this.frames = 0;
    for (const w of this.windows) {
      const embOf = new Array(LOCAL_SPEAKERS).fill(null);
      for (const e of w.embs) embOf[e.k] = e;
      const locals = w.embs.map((e) => e.k);
      const never = new Array(LOCAL_SPEAKERS).fill(-Infinity); // no new speakers now
      const picks = assign(locals, means.length, (k, g) => 2 + dot(embOf[k].emb, means[g]), never);
      w.map = new Array(LOCAL_SPEAKERS).fill(null);
      locals.forEach((k, i) => {
        const g = picks[i];
        w.map[k] = g;
        if (g === null) return;
        const s = this.speakers[g];
        const wt = Math.min(embOf[k].weight, opt.maxEmbSec);
        for (let d = 0; d < 256; d++) s.sum[d] += embOf[k].emb[d] * wt;
        s.weight += wt;
      });
    }
    // Local speakers too short to embed: place them by agreement with the rest of
    // the tally (a second pass, once the embedded ones are in).
    for (const w of this.windows) this.addToTally(w, +1);
    for (const w of this.windows) {
      const missing = [];
      for (let k = 0; k < LOCAL_SPEAKERS; k++) {
        if (w.map[k] !== null || w.embs.some((e) => e.k === k)) continue;
        let any = false;
        for (let i = 0; i < w.bits.length && !any; i++) any = !!(w.bits[i] & (1 << k));
        if (any) missing.push(k);
      }
      if (!missing.length) continue;
      const { local, joint } = this.overlapEvidence(w.f0, w.bits);
      const taken = new Set(w.map.filter((g) => g !== null));
      for (const k of missing) {
        let best = null;
        let bestV = 0.5;
        for (let g = 0; g < this.speakers.length; g++) {
          if (taken.has(g) || !local[k]) continue;
          const v = joint[k][g] / local[k];
          if (v > bestV) {
            bestV = v;
            best = g;
          }
        }
        if (best !== null) {
          // add only this speaker's votes (the window's frame counts are in already)
          for (let i = 0; i < w.bits.length; i++) {
            const f = w.f0 + i;
            if (f >= 0 && w.bits[i] & (1 << k)) this.act[best].a[f] += 1;
          }
          w.map[k] = best;
          taken.add(best);
        }
      }
    }
    this.keepNames(old);
    this.reclusterEnd = performance.now();
    this.reclusterSec = (this.reclusterEnd - t0) / 1000;
    this.stats.reclusters += 1;
    this.stats.reclusterMs += this.reclusterEnd - t0;
  }

  // After a recluster, give each new speaker the name of the old speaker it overlaps
  // most in time (greedy, one-to-one), so labels on screen don't jump around; the
  // rest get new names. When two old speakers merge, the merged one keeps the name
  // of the one with more shared frames.
  keepNames(old) {
    // Frames each (new, old) pair is active in together: one pass, listing who's on.
    const n = this.speakers.length;
    const m = old.speakers.length;
    const shared = new Float64Array(n * m);
    const on = [];
    const oldOn = [];
    for (let f = 0; f < Math.min(this.frames, old.frames); f++) {
      const c = this.cnt.a[f];
      const oc = old.cnt.a[f];
      if (!c || !oc) continue;
      on.length = 0;
      for (let g = 0; g < n; g++) if (this.act[g].a[f] * 2 >= c) on.push(g);
      if (!on.length) continue;
      oldOn.length = 0;
      for (let o = 0; o < m; o++) if (old.act[o].a[f] * 2 >= oc) oldOn.push(o);
      for (const g of on) for (const o of oldOn) shared[g * m + o] += 1;
    }
    const pairs = [];
    for (let g = 0; g < n; g++) for (let o = 0; o < m; o++) if (shared[g * m + o]) pairs.push([shared[g * m + o], g, o]);
    pairs.sort((x, y) => y[0] - x[0]);
    const usedOld = new Set();
    for (const [, g, o] of pairs) {
      if (this.speakers[g].name >= 0 || usedOld.has(o)) continue;
      this.speakers[g].name = old.speakers[o].name;
      usedOld.add(o);
    }
    for (const sp of this.speakers) if (sp.name < 0) sp.name = this.nextName++;
  }

  // Per global speaker, is frame f active? (≥ half the windows covering it agree.)
  activeAt(g, f) {
    const c = this.cnt.a[f];
    return c > 0 && this.act[g].a[f] * 2 >= c;
  }

  // [{ start, end, speaker }] sorted by start; speaker = the speaker's name (a number
  // that survives reclusters; not contiguous: merged-away names disappear).
  // Overlapping speech gives overlapping segments. Smoothed like pyannote.
  segments() {
    const opt = this.opt;
    const out = [];
    for (let g = 0; g < this.speakers.length; g++) {
      const runs = [];
      let s = -1;
      for (let f = 0; f <= this.frames; f++) {
        const on = f < this.frames && this.activeAt(g, f);
        if (on && s < 0) s = f;
        else if (!on && s >= 0) {
          runs.push([frameTime(s) - FRAME_HOP / 2 / SR, frameTime(f - 1) + FRAME_HOP / 2 / SR]);
          s = -1;
        }
      }
      const merged = [];
      for (const r of runs) {
        const last = merged[merged.length - 1];
        if (last && r[0] - last[1] < opt.minDurationOff) last[1] = r[1];
        else merged.push(r.slice());
      }
      const name = this.speakers[g].name;
      for (const [a, b] of merged) if (b - a >= opt.minDurationOn) out.push({ start: a, end: b, speaker: name });
    }
    return out.sort((a, b) => a.start - b.start || a.speaker - b.speaker);
  }

  // [{ speaker, centroid, weight }] per global speaker: the L2-normalised mean of its
  // embeddings (its voice print, for recognising it in another recording) and the
  // weight behind it (capped solo seconds summed over the windows it was embedded in;
  // windows overlap, so roughly 2x the speaker's talk time).
  voices() {
    return this.speakers.filter((s) => s.weight > 0).map((s) => ({ speaker: s.name, centroid: l2normalize(s.sum), weight: s.weight }));
  }

  // Speaker name for a span of time: the one active in most of its frames; if none
  // is (the model called it silence), the nearest active one within 1 s.
  speakerForSpan(t0, t1) {
    const g = this.indexForSpan(t0, t1);
    return g === null ? null : this.speakers[g].name;
  }

  indexForSpan(t0, t1) {
    const f0 = Math.max(0, Math.floor((t0 * SR - FRAME_RF / 2) / FRAME_HOP));
    const f1 = Math.min(this.frames - 1, Math.ceil((t1 * SR - FRAME_RF / 2) / FRAME_HOP));
    const n = this.speakers.length;
    if (!n || f1 < 0 || f0 >= this.frames) return null;
    const votes = new Float32Array(n);
    for (let f = f0; f <= f1; f++) for (let g = 0; g < n; g++) if (this.activeAt(g, f)) votes[g] += 1;
    let best = -1;
    for (let g = 0; g < n; g++) if (votes[g] > 0 && (best < 0 || votes[g] > votes[best])) best = g;
    if (best >= 0) return best;
    const reach = Math.round(SR / FRAME_HOP);
    for (let d = 1; d <= reach; d++) {
      for (const f of [f0 - d, f1 + d]) {
        if (f < 0 || f >= this.frames) continue;
        for (let g = 0; g < n; g++) if (this.activeAt(g, f)) return g;
      }
    }
    return null;
  }

  // Frames before this won't get more votes (no future window covers them).
  settledFrames() {
    return Math.max(0, Math.min(this.frames, this.windowStartFrame(this.nextWindow + 1)));
  }

  // Dominant speaker name per frame in [f0, f1), -1 for none (for scoring live output).
  frameLabels(f0, f1) {
    const out = new Int32Array(Math.max(0, f1 - f0)).fill(-1);
    for (let f = f0; f < f1; f++) {
      const c = this.cnt.a[f];
      let best = -1;
      for (let g = 0; g < this.speakers.length; g++) {
        if (this.activeAt(g, f) && (best < 0 || this.act[g].a[f] > this.act[best].a[f])) best = g;
      }
      if (c && best >= 0) out[f - f0] = this.speakers[best].name;
    }
    return out;
  }

  // words: [{ text, start_time, end_time }] → the same with `speaker` (name or null).
  labelWords(words) {
    return words.map((w) => ({ ...w, speaker: this.speakerForSpan(w.start_time, w.end_time) }));
  }
}

// Load both sessions from ArrayBuffers/Uint8Arrays (or URLs) and build a Diarizer.
export async function createDiarizer(ort, { segmentation, embedding }, options = {}, sessionOptions = {}) {
  const so = { executionProviders: ['wasm'], ...sessionOptions };
  const seg = await ort.InferenceSession.create(segmentation, so);
  const emb = await ort.InferenceSession.create(embedding, so);
  return new Diarizer(ort, seg, emb, options);
}
