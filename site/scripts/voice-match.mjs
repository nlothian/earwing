// How well do speaker centroids from one recording match the same people in another?
// Measures the thresholds voiceStore.js uses to recognise saved voices.
//
//   npm run voice-match -- recording.wav [--splits 2,3,4] [--threads N]
//
// Diarizes the whole file once (the reference: which speaker is who), then cuts it
// into 2, 3, 4… pieces and diarizes each piece as a SEPARATE recording, the way the
// page does a file (10 s pushes, then a final recluster). Each piece speaker's
// centroid (the normalised mean of its embeddings, what the page saves as a voice
// print) is compared with every speaker's centroid in the other pieces of the same
// split. "Same person" = both overlap the same reference speaker most in time.
// Prints the same/different similarity distributions, the pairs, and a "store"
// simulation: each piece's speakers matched against prints saved from the others.

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as ort from 'onnxruntime-web';
import { createDiarizer, FRAME_HOP, SR } from '../src/components/diarizer.js';
import { DIARIZATION } from '../src/components/models.js';

const MODELS = join(dirname(fileURLToPath(import.meta.url)), '..', '..', 'models');

const args = process.argv.slice(2);
let wavPath = null;
let splits = [2, 3, 4];
let threads = 4;
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--splits') splits = args[++i].split(',').map(Number);
  else if (args[i] === '--threads') threads = Number(args[++i]);
  else wavPath = args[i];
}
if (!wavPath) {
  console.error('usage: voice-match.mjs recording.wav [--splits 2,3,4] [--threads N]');
  process.exit(2);
}

// 16 kHz 16-bit PCM or 32-bit float WAV → Float32 mono (as in diarize-file.mjs).
function readWav(path) {
  const b = readFileSync(path);
  if (b.toString('ascii', 0, 4) !== 'RIFF' || b.toString('ascii', 8, 12) !== 'WAVE') throw new Error('not a WAV file');
  let fmt = null;
  let data = null;
  for (let off = 12; off + 8 <= b.length; ) {
    const id = b.toString('ascii', off, off + 4);
    const size = Math.min(b.readUInt32LE(off + 4), b.length - off - 8);
    if (id === 'fmt ') {
      fmt = { format: b.readUInt16LE(off + 8), channels: b.readUInt16LE(off + 10), rate: b.readUInt32LE(off + 12), bits: b.readUInt16LE(off + 22) };
    } else if (id === 'data') data = b.subarray(off + 8, off + 8 + size);
    off += 8 + size + (size & 1);
  }
  if (!fmt || !data) throw new Error('missing fmt or data chunk');
  if (fmt.rate !== SR) throw new Error(`need ${SR} Hz audio, got ${fmt.rate}`);
  if (!((fmt.format === 1 && fmt.bits === 16) || (fmt.format === 3 && fmt.bits === 32))) throw new Error('need 16-bit PCM or 32-bit float');
  const bps = fmt.bits / 8;
  const n = Math.floor(data.length / (bps * fmt.channels));
  const mono = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    let s = 0;
    for (let c = 0; c < fmt.channels; c++) {
      const o = (i * fmt.channels + c) * bps;
      s += fmt.format === 3 ? data.readFloatLE(o) : data.readInt16LE(o) / 32768;
    }
    mono[i] = s / fmt.channels;
  }
  return mono;
}

const audio = readWav(wavPath);
ort.env.wasm.numThreads = threads;
const file = (m) => readFileSync(join(MODELS, m.dir, m.name));
const d = await createDiarizer(ort, { segmentation: file(DIARIZATION.segmentation), embedding: file(DIARIZATION.embedding) });

// Diarize audio[a, b) as its own recording → speakers with centroid, weight, and the
// dominant speaker name per frame (frame f of the piece ≈ frame a/FRAME_HOP + f overall).
async function diarize(a, b) {
  d.reset();
  const step = SR * 10;
  for (let i = a; i < b; i += step) {
    d.push(audio.subarray(i, Math.min(b, i + step)));
    await d.process();
  }
  await d.process({ final: true });
  d.recluster();
  const speakers = d.speakers.map((s) => {
    let n = 0;
    for (const v of s.sum) n += v * v;
    n = Math.sqrt(n) || 1;
    return { name: s.name, centroid: s.sum.map((v) => v / n), weight: s.weight };
  });
  return { speakers, labels: d.frameLabels(0, d.frames), f0: Math.round(a / FRAME_HOP) };
}

const dot = (x, y) => x.reduce((s, v, i) => s + v * y[i], 0);
const pct = (xs, p) => xs[Math.min(xs.length - 1, Math.max(0, Math.round((p / 100) * (xs.length - 1))))];
const stats = (xs) => {
  const s = [...xs].sort((x, y) => x - y);
  if (!s.length) return '(none)';
  const f = (v) => v.toFixed(2);
  return `n=${s.length} min ${f(s[0])} p10 ${f(pct(s, 10))} median ${f(pct(s, 50))} p90 ${f(pct(s, 90))} max ${f(s[s.length - 1])}`;
};

const ref = await diarize(0, audio.length);
const refWeight = Object.fromEntries(ref.speakers.map((s) => [s.name, s.weight]));
console.log(`reference (whole file): ${ref.speakers.map((s) => `#${s.name} ${s.weight.toFixed(0)}s`).join(', ')}`);

const same = [];
const diff = [];
const pairs = [];
const store = [];
for (const k of splits) {
  const len = Math.floor(audio.length / k / FRAME_HOP) * FRAME_HOP;
  const pieces = [];
  for (let p = 0; p < k; p++) {
    const piece = await diarize(p * len, p === k - 1 ? audio.length : (p + 1) * len);
    // Map each piece speaker to the reference speaker it overlaps most in time.
    for (const s of piece.speakers) {
      const votes = new Map();
      let total = 0;
      piece.labels.forEach((l, f) => {
        if (l !== s.name) return;
        total += 1;
        const r = ref.labels[piece.f0 + f];
        if (r >= 0) votes.set(r, (votes.get(r) || 0) + 1);
      });
      let best = null;
      for (const [r, v] of votes) if (best === null || v > votes.get(best)) best = r;
      s.ref = best;
      s.purity = total ? (votes.get(best) || 0) / total : 0;
      s.split = [...votes].sort((x, y) => y[1] - x[1]).map(([r, v]) => `#${r} ${((v / total) * 100).toFixed(0)}%`).join(' ');
      s.tag = `${k}:${p}#${s.name}`;
    }
    pieces.push(piece);
    console.log(
      `split ${k} piece ${p}: ` +
        piece.speakers
          .map((s) => `#${s.name} ${s.weight.toFixed(0)}s → ref ${s.purity < 0.8 ? `(${s.split})` : `#${s.ref} (${(s.purity * 100).toFixed(0)}%)`}`)
          .join(', '),
    );
  }
  for (let p = 0; p < k; p++) {
    for (let q = p + 1; q < k; q++) {
      for (const a of pieces[p].speakers) {
        for (const b of pieces[q].speakers) {
          const sim = dot(a.centroid, b.centroid);
          const isSame = a.ref !== null && a.ref === b.ref;
          (isSame ? same : diff).push(sim);
          pairs.push({ sim, isSame, a, b });
        }
      }
    }
  }
  // Store simulation: prints for each reference person from every OTHER piece; match
  // each piece speaker by the max similarity over a person's prints.
  for (let p = 0; p < k; p++) {
    const people = new Map();
    pieces.forEach((pc, q) => {
      if (q === p) return;
      for (const s of pc.speakers) {
        if (!people.has(s.ref)) people.set(s.ref, []);
        people.get(s.ref).push(s.centroid);
      }
    });
    for (const s of pieces[p].speakers) {
      const scored = [...people].map(([r, prints]) => ({ r, sim: Math.max(...prints.map((c) => dot(c, s.centroid))) }));
      scored.sort((x, y) => y.sim - x.sim);
      const top = scored[0];
      const second = scored[1] ? scored[1].sim : -1;
      store.push({ tag: s.tag, weight: s.weight, ref: s.ref, known: people.has(s.ref), top: top.r, sim: top.sim, second });
    }
  }
}

console.log(`\nsame person:      ${stats(same)}`);
console.log(`different people: ${stats(diff)}`);
for (const w of [5, 10, 15, 20]) {
  const keep = pairs.filter((x) => x.a.weight >= w && x.b.weight >= w);
  console.log(
    `  both ≥ ${w}s: same ${stats(keep.filter((x) => x.isSame).map((x) => x.sim))}\n` +
      `            diff ${stats(keep.filter((x) => !x.isSame).map((x) => x.sim))}`,
  );
}
// A piece speaker that's a mix of reference speakers (< 80% one of them) isn't one
// person; nor, perhaps, is the reference speaker it overlaps.
const pure = pairs.filter((x) => x.a.purity >= 0.8 && x.b.purity >= 0.8 && x.a.weight >= 15 && x.b.weight >= 15);
console.log(
  `  both ≥ 15s and ≥ 80% one reference speaker: same ${stats(pure.filter((x) => x.isSame).map((x) => x.sim))}\n` +
    `            diff ${stats(pure.filter((x) => !x.isSame).map((x) => x.sim))}`,
);
console.log('\nhighest different-person pairs:');
for (const x of pairs.filter((x) => !x.isSame).sort((x, y) => y.sim - x.sim).slice(0, 8)) {
  console.log(`  ${x.sim.toFixed(3)}  ${x.a.tag} (${x.a.weight.toFixed(0)}s → #${x.a.ref}) vs ${x.b.tag} (${x.b.weight.toFixed(0)}s → #${x.b.ref})`);
}
console.log('lowest same-person pairs:');
for (const x of pairs.filter((x) => x.isSame).sort((x, y) => x.sim - y.sim).slice(0, 8)) {
  console.log(`  ${x.sim.toFixed(3)}  ${x.a.tag} (${x.a.weight.toFixed(0)}s) vs ${x.b.tag} (${x.b.weight.toFixed(0)}s) → #${x.a.ref}`);
}
console.log('\nstore simulation (best person by max over prints; * = wrong person, - = person not in store):');
for (const s of store.sort((x, y) => y.sim - x.sim)) {
  const mark = !s.known ? '-' : s.top === s.ref ? ' ' : '*';
  console.log(
    `  ${mark} ${s.sim.toFixed(3)} (next ${s.second.toFixed(3)})  ${s.tag} ${s.weight.toFixed(0)}s ref #${s.ref} (${(refWeight[s.ref] || 0).toFixed(0)}s) → #${s.top}`,
  );
}
