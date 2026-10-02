// Speaker-diarization worker: runs diarizer.js (pyannote segmentation-3.0 + WeSpeaker
// ResNet34-LM) beside the transcription worker, so labelling speakers never delays
// the transcript. The two share nothing but the audio: the page sends both the same
// 16 kHz chunks, gets timed words from one and speaker segments from the other, and
// lines them up by time (speakerTimeline.js).
//
// Models (~33 MB, models.js DIARIZATION) come from Hugging Face through parakeet.js's
// getModelFile, which caches them in the same IndexedDB as the Parakeet files.
// onnxruntime-web is this worker's own instance of the same extern-wasm build the
// transcription worker uses (see there), with its runtime from the same jsDelivr URL,
// so the browser's HTTP cache serves the second copy. Always WASM (CPU): at a 2.5 s
// step it costs roughly 0.1-0.4 s of CPU per second of audio, depending on threads.
//
// Message protocol:
//   main -> worker:
//     { type:'load' }                          // download/open both models
//     { type:'reset' }                         // new live take
//     { type:'anchors', anchors:[{ prints:[Float32Array(256)] }] }  // known voices (Diarizer.setAnchors)
//     { type:'audio', samples:Float32Array }   // next live chunk (queued until loaded)
//     { type:'finish' }                        // take ended: last window + recluster
//     { type:'file', audio:Float32Array }      // a whole recording
//   worker -> main:
//     { type:'progress', stage:'download'|'file', progress }   // percent
//     { type:'ready', threads, loadMs }
//     { type:'segments', segments:[{start,end,speaker}], final,   // after each step
//       voices:[{speaker, centroid:Float32Array(256), weight}] }  // per speaker (voiceStore.js)
//     { type:'error', error, during }

import * as ort from 'onnxruntime-web';
import { getModelFile } from 'parakeet.js';
import { createDiarizer, SR } from './diarizer.js';
import { DIARIZATION } from './models.js';

ort.env.wasm.wasmPaths = `https://cdn.jsdelivr.net/npm/onnxruntime-web@${__ORT_VERSION__}/dist/`;
const hasSAB = typeof SharedArrayBuffer !== 'undefined' && self.crossOriginIsolated === true;
// Leave most cores to the transcription worker (it asks for up to 8).
const THREADS = hasSAB ? Math.max(1, Math.min(4, Math.floor((navigator.hardwareConcurrency || 4) / 2))) : 1;
ort.env.wasm.numThreads = THREADS;

let diarizer = null;
let loading = null;
let queued = []; // live chunks that arrived before the models were ready
let anchors = []; // known voices, kept for when the diarizer is created

async function load() {
  if (diarizer) return;
  if (loading) return loading;
  loading = (async () => {
    const t0 = performance.now();
    const parts = Object.values(DIARIZATION);
    const done = parts.map(() => 0);
    const total = parts.reduce((n, m) => n + m.size, 0);
    const urls = await Promise.all(
      parts.map((m, i) =>
        getModelFile(m.repo, m.name, {
          progress: (p) => {
            if (!p || !p.total) return;
            done[i] = p.loaded || 0;
            const pct = Math.min(100, Math.floor((done.reduce((a, b) => a + b, 0) / total) * 100));
            self.postMessage({ type: 'progress', stage: 'download', progress: pct });
          },
        }),
      ),
    );
    try {
      const [segmentation, embedding] = await Promise.all(urls.map((u) => fetch(u).then((r) => r.arrayBuffer())));
      diarizer = await createDiarizer(ort, { segmentation, embedding });
      diarizer.setAnchors(anchors);
    } finally {
      for (const u of urls) if (typeof u === 'string' && u.startsWith('blob:')) URL.revokeObjectURL(u);
    }
    self.postMessage({ type: 'ready', threads: THREADS, loadMs: Math.round(performance.now() - t0) });
  })();
  loading.catch(() => {
    loading = null; // let the next 'load' retry (typically: no internet)
    queued = []; // nothing will drain what came in meanwhile
  });
  return loading;
}

const postSegments = (final) => {
  const voices = diarizer.voices();
  self.postMessage(
    { type: 'segments', segments: diarizer.segments(), final, voices },
    voices.map((v) => v.centroid.buffer),
  );
};

// Every job that touches the diarizer runs here, one at a time: process() awaits the
// models, and a reset or a second file mid-window would corrupt its state. `gen` is
// bumped by each reset; a job from an earlier take stops at its next await and posts
// nothing.
let work = Promise.resolve();
let gen = 0;
const enqueue = (job, during) => {
  work = work.then(job).catch(fail(during));
};

// Live audio: push, then run whatever windows are due. Chunks arrive every ~100 ms
// while a window takes longer, so one queued job drains everything queued.
let drainQueued = false;
function scheduleDrain() {
  if (drainQueued) return;
  drainQueued = true;
  const g = gen;
  enqueue(async () => {
    if (g !== gen) return;
    drainQueued = false;
    while (g === gen && queued.length) {
      for (const c of queued) diarizer.push(c);
      queued = [];
      if ((await diarizer.process()) && g === gen) postSegments(false);
    }
  }, 'audio');
}

async function finish(g) {
  if (g !== gen) return;
  for (const c of queued) diarizer.push(c);
  queued = [];
  await diarizer.process({ final: true });
  if (g !== gen) return;
  diarizer.recluster();
  postSegments(true);
}

async function file(audio, g) {
  if (g !== gen) return;
  diarizer.reset();
  const step = SR * 10; // push in pieces so progress can be reported
  for (let i = 0; i < audio.length; i += step) {
    diarizer.push(audio.subarray(i, Math.min(audio.length, i + step)));
    const ran = await diarizer.process();
    if (g !== gen) return;
    if (ran) postSegments(false); // labels fill in as it goes
    self.postMessage({ type: 'progress', stage: 'file', progress: Math.floor((i / audio.length) * 100) });
  }
  await diarizer.process({ final: true });
  if (g !== gen) return;
  diarizer.recluster();
  postSegments(true);
}

const fail = (during) => (err) =>
  self.postMessage({ type: 'error', error: String((err && err.message) || err), during });

self.onmessage = (event) => {
  const data = event.data || {};
  switch (data.type) {
    case 'load':
      load().catch(fail('load'));
      break;
    case 'reset':
      gen += 1;
      queued = [];
      drainQueued = false;
      enqueue(() => diarizer && diarizer.reset(), 'reset');
      break;
    case 'anchors':
      anchors = data.anchors || [];
      if (diarizer) diarizer.setAnchors(anchors);
      break;
    case 'audio':
      // Kept only while the models are (being) loaded: after a failed load nothing
      // would ever drain it.
      if (diarizer) {
        queued.push(data.samples);
        scheduleDrain();
      } else if (loading) {
        queued.push(data.samples);
        loading.then(scheduleDrain, () => {});
      }
      break;
    case 'finish': {
      const g = gen;
      (loading || load()).then(() => enqueue(() => finish(g), 'finish')).catch(fail('finish'));
      break;
    }
    case 'file': {
      const g = gen;
      (loading || load()).then(() => enqueue(() => file(data.audio, g), 'file')).catch(fail('file'));
      break;
    }
    default:
      break;
  }
};
