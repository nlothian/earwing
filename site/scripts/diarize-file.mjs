// Run the page's speaker diarizer (src/components/diarizer.js) over a WAV file in
// Node, fed in 100 ms chunks exactly as the live page feeds it, on the same
// onnxruntime-web WASM backend.
//
//   node scripts/diarize-file.mjs recording.wav [--out prefix] [--words words.json]
//        [--rttm reference.rttm] [--threads N] [--set key=value ...]
//        [--anchors voices.json ...] [--voices]
//
// Models come from <repo root>/models/ (npm run models). Writes, each as
// [{ start, end, speaker }]:
//   <prefix>.first.json    each moment's label as first shown live (newest edge, 1 window)
//   <prefix>.settled.json  each moment's label once settled (~10 s later, all windows in)
//   <prefix>.online.json   all labels as the live page had them when the audio ended
//   <prefix>.final.json    after the closing recluster(): what the page shows after Stop
// Live labels are scored with one speaker per moment (the dominant one). With --words ([{ text, start|start_time, end|end_time }]) it also
// prints the transcript split into speaker turns. With --rttm (ground truth) it prints
// the DER of the first-shown, settled, online and final labels (scripts/der.mjs, 0.25 s collar)
// and writes them to <prefix>.score.json (scripts/score-diarization.mjs reads these).
// --voices also writes the final speakers' voice prints to <prefix>.voices.json; each
// --anchors file (one such voices.json, e.g. from another recording) gives the diarizer
// its speakers as known voices (Diarizer.setAnchors), as saved people steer the page.

import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as ort from 'onnxruntime-web';
import { createDiarizer, DEFAULTS, FRAME_HOP, SR } from '../src/components/diarizer.js';
import { DIARIZATION } from '../src/components/models.js';
import { der, formatDer, readRttm } from './der.mjs';

const MODELS = join(dirname(fileURLToPath(import.meta.url)), '..', '..', 'models');

const args = process.argv.slice(2);
let wavPath = null;
let out = null;
let wordsPath = null;
let rttmPath = null;
let threads = 1;
let writeVoices = false;
const anchorPaths = [];
const options = {};
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === '--out') out = args[++i];
  else if (a === '--words') wordsPath = args[++i];
  else if (a === '--rttm') rttmPath = args[++i];
  else if (a === '--threads') threads = Number(args[++i]);
  else if (a === '--anchors') anchorPaths.push(args[++i]);
  else if (a === '--voices') writeVoices = true;
  else if (a === '--set') {
    const [k, v] = args[++i].split('=');
    if (!(k in DEFAULTS)) throw new Error(`unknown option ${k} (have: ${Object.keys(DEFAULTS).join(', ')})`);
    options[k] = Number(v);
  } else wavPath = a;
}
if (!wavPath) {
  console.error('usage: diarize-file.mjs recording.wav [--out prefix] [--words words.json] [--rttm ref.rttm] [--threads N] [--set key=value]');
  process.exit(2);
}
out = out || wavPath.replace(/\.wav$/i, '');

// 16 kHz PCM/float WAV → Float32 mono (channels averaged).
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
  if (fmt.rate !== SR) throw new Error(`need ${SR} Hz audio, got ${fmt.rate} (ffmpeg -i in.wav -ar 16000 out.wav)`);
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
const d = await createDiarizer(ort, { segmentation: file(DIARIZATION.segmentation), embedding: file(DIARIZATION.embedding) }, options);
// Each speaker in each --anchors file is a known person with that one print.
const anchors = anchorPaths.flatMap((p) => JSON.parse(readFileSync(p, 'utf8')).map((v) => ({ prints: [v.centroid] })));
d.setAnchors(anchors);

const CHUNK = SR / 10;
const nFrames = Math.ceil(audio.length / FRAME_HOP);
const first = new Int32Array(nFrames).fill(-1);
const settled = new Int32Array(nFrames).fill(-1);
// relabelled: settled frames whose label later changed while live (a periodic
// recluster rewriting history: on screen, words moving to another speaker).
const relabelled = new Uint8Array(nFrames);
let firstDone = 0;
let settledDone = 0;
const record = (final) => {
  if (!final && settledDone) {
    const now = d.frameLabels(0, settledDone);
    for (let f = 0; f < settledDone; f++) if (settled[f] >= 0 && now[f] !== settled[f]) relabelled[f] = 1;
  }
  const upTo = Math.min(nFrames, d.frames);
  first.set(d.frameLabels(firstDone, upTo), firstDone);
  firstDone = Math.max(firstDone, upTo);
  const s = final ? upTo : Math.min(nFrames, d.settledFrames());
  if (s > settledDone) settled.set(d.frameLabels(settledDone, s), settledDone);
  settledDone = Math.max(settledDone, s);
};
const t0 = performance.now();
for (let i = 0; i < audio.length; i += CHUNK) {
  d.push(audio.subarray(i, Math.min(audio.length, i + CHUNK)));
  if (await d.process()) record(false);
}
await d.process({ final: true });
record(true);
const onlineMs = performance.now() - t0;
const online = d.segments();
const onlineSpeakers = d.speakers.length;
const liveNames = new Set([...settled].filter((x) => x >= 0)).size;
// Label churn, in frames: provisional = the first-shown label differed from the settled one.
const churn = { labelled: 0, provisional: 0, relabelled: 0 };
for (let f = 0; f < nFrames; f++) {
  if (settled[f] < 0) continue;
  churn.labelled++;
  if (first[f] !== settled[f]) churn.provisional++;
  churn.relabelled += relabelled[f];
}
const churnPct = (k) => `${((100 * churn[k]) / (churn.labelled || 1)).toFixed(1)}%`;
const t1 = performance.now();
d.recluster();
const reclusterMs = performance.now() - t1;
const final = d.segments();
if (writeVoices) {
  const voices = d.voices().map((v) => ({ speaker: v.speaker, weight: +v.weight.toFixed(1), centroid: [...v.centroid].map((x) => +x.toFixed(5)) }));
  writeFileSync(`${out}.voices.json`, JSON.stringify(voices));
}

// Per-frame labels → segments (frame f covers [f·hop, (f+1)·hop) + half the receptive field).
const toSegments = (labels) => {
  const out = [];
  for (let f = 0; f < labels.length; f++) {
    const t = (f * FRAME_HOP + 495 - FRAME_HOP / 2) / SR;
    const last = out[out.length - 1];
    if (labels[f] < 0) continue;
    if (last && last.speaker === labels[f] && t - last.end < 0.05) last.end = t + FRAME_HOP / SR;
    else out.push({ start: t, end: t + FRAME_HOP / SR, speaker: labels[f] });
  }
  return out;
};
const labels = { first: toSegments(first), settled: toSegments(settled), online, final };
const round = (segs) => segs.map((s) => ({ start: +s.start.toFixed(3), end: +s.end.toFixed(3), speaker: s.speaker }));
for (const [k, segs] of Object.entries(labels)) writeFileSync(`${out}.${k}.json`, JSON.stringify(round(segs)));

const secs = audio.length / SR;
const talk = (segs) => {
  const t = {};
  for (const s of segs) t[s.speaker] = (t[s.speaker] || 0) + s.end - s.start;
  return Object.entries(t).map(([k, v]) => `#${k} ${v.toFixed(0)}s`).join(', ');
};
const st = d.stats;
console.log(
  `${wavPath}: ${secs.toFixed(0)} s, ${st.windows} windows, ${st.embeddings} embeddings, ${threads} thread(s)\n` +
    `  time: ${(onlineMs / 1000).toFixed(1)} s (RTF ${(onlineMs / 1000 / secs).toFixed(3)}; segmentation ${(st.segMs / 1000).toFixed(1)} s, ` +
    `fbank+embedding ${(st.embMs / 1000).toFixed(1)} s, ${st.reclusters - 1} live reclusters ${(st.reclusterMs / 1000 - reclusterMs / 1000).toFixed(1)} s), ` +
    `final recluster ${reclusterMs.toFixed(0)} ms\n` +
    `  live:   ${liveNames} speaker names used over the take; of labelled speech, ${churnPct('provisional')} changed label ` +
    `on settling, ${churnPct('relabelled')} again later\n` +
    `  online: ${onlineSpeakers} speakers at the end (${talk(online)})\n` +
    `  final:  ${d.speakers.length} speakers (${talk(final)})${anchors.length ? `, ${anchors.length} known voices` : ''}\n` +
    `  wrote ${out}.{first,settled,online,final}.json`,
);

if (rttmPath) {
  const ref = readRttm(readFileSync(rttmPath, 'utf8'));
  const score = { file: wavPath, secs, rtf: onlineMs / 1000 / secs, options, churn };
  for (const k of ['first', 'settled', 'online', 'final']) score[k] = der(ref, labels[k]);
  writeFileSync(`${out}.score.json`, JSON.stringify(score));
  console.log(
    `  DER vs ${rttmPath} (${score.final.refSpeakers} speakers; missed / false alarm / confusion):\n` +
      ['first', 'settled', 'online', 'final'].map((k) => `    ${k.padEnd(8)} ${formatDer(score[k])}, ${score[k].hypSpeakers} speakers`).join('\n'),
  );
}

if (wordsPath) {
  const words = JSON.parse(readFileSync(wordsPath, 'utf8')).map((w) => ({
    text: w.text,
    start_time: w.start ?? w.start_time,
    end_time: w.end ?? w.end_time,
  }));
  const fmt = (t) => `${String(Math.floor(t / 60)).padStart(2, '0')}:${String(Math.floor(t % 60)).padStart(2, '0')}`;
  let cur;
  let line = '';
  const flush = () => line && console.log(line);
  for (const w of d.labelWords(words)) {
    if (w.speaker !== cur) {
      flush();
      cur = w.speaker;
      line = `[${fmt(w.start_time)}] ${w.speaker === null ? '??' : `#${w.speaker}`}: ${w.text}`;
    } else line += ` ${w.text}`;
  }
  flush();
}
