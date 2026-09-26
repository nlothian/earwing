// Parakeet-TDT ASR inference worker.
//
// Runs NVIDIA Parakeet-TDT 0.6B v3 in the browser via parakeet.js on
// onnxruntime-web, in one of two configurations:
//
//   webgpu  fp16 encoder on WebGPU, int8 decoder on WASM (parakeet.js always runs
//           the tiny decoder/joiner on WASM). Repo ysdede/parakeet-tdt-0.6b-v3-onnx:
//             encoder-model.fp16.onnx        1,238,960,452 B (weights inline, no .data)
//             decoder_joint-model.int8.onnx     18,202,004 B
//             vocab.txt                            102,132 B      ≈ 1.26 GB total
//   wasm    int8 encoder + decoder on the WASM CPU backend. Repo
//           nasedkinpv/parakeet-tdt-0.6b-v3-onnx-int8:
//             encoder-int8.onnx                  1,443,183 B
//             encoder-int8.onnx.data           878,346,240 B (external weights)
//             decoder_joint-int8.onnx           53,592,358 B
//             vocab.txt                             93,939 B      ≈ 933 MB total
//           (WebGPU has no int8 kernels, and fp16 on CPU is slow, hence two models.)
//
// Selection: the main thread passes { backend: 'auto' | 'webgpu' | 'wasm' } ('auto'
// unless the page URL has ?backend=). 'auto' picks webgpu only if this is a secure
// context with navigator.gpu, an adapter, and the adapter has 'shader-f16'. If the
// WebGPU model fails to compile/create sessions or its first (warm-up) inference
// throws, we drop it and load the int8 WASM model instead, and tell the UI why. A
// failed DOWNLOAD is not a reason to switch (the other model would need the same
// internet); it's reported as a load error.
//
// Local folder: if the main thread passes `local` (see localModels.js), the chosen
// variant's files come from File objects the user picked (blob URLs, no copy, nothing
// cached). If that folder lacks the chosen variant's files, the load fails with
// code 'local-missing' naming them, and the page offers the Hugging Face download.
//
// Otherwise weights download from the Hugging Face CDN on first use and parakeet.js caches
// them in IndexedDB (per origin, keyed by repo + filename, so the two variants
// don't clobber each other). getModelFile resolves each file to a blob URL; we
// download a model's files in parallel and hand them to fromUrls. (fromHub can't
// load the int8 repo: its filename convention differs.)
//
// Live transcription follows Keet's v4 strategy (github.com/ysdede/keet): the
// main thread sends OVERLAPPING windows [mature-cursor .. live-edge] and we
// re-transcribe each one fresh (full encoder context, no boundary artifacts),
// then UtteranceMerger finalizes complete sentences and dedupes the overlap.
//
// gulltoppr: this page is served from the ESP32's small LittleFS partition, so
// onnxruntime-web's 14-28 MB .wasm runtime is NOT bundled. astro.config.mjs
// aliases 'onnxruntime-web' to its extern-wasm WebGPU+WASM build (ort.min.mjs,
// JSEP) and we point ort.env.wasm.wasmPaths at jsDelivr, pinned to the exact
// installed version, BEFORE parakeet.js creates any session. parakeet.js imports
// the same (deduped, aliased) module, so this is the one ORT instance it uses; its
// initOrt() only fills in wasmPaths when unset, so ours wins. Threads: with
// SharedArrayBuffer (cross-origin isolated, i.e. https://gulltoppr.local or
// localhost) we ask for up to 8 WASM threads; plain http gets one. ORT fixes the
// thread count when the WASM runtime first starts, so it's the same for both
// models (the WebGPU path uses WASM only for the decoder).
//
// Message protocol:
//   main -> worker:
//     { type:'load', backend:'auto'|'webgpu'|'wasm', local? } // download/open + init model
//     { type:'transcribe', audio:Float32Array16k }      // uploaded file (batch)
//     { type:'stream-start' }                           // reset merger for a new take
//     { type:'window', audio:Float32Array16k, startTime } // overlapping window
//     { type:'finalize-pending' }                       // commit the pending tail, keep going
//     { type:'finalize-all' }                           // stop: commit everything
//   worker -> main:
//     { type:'plan', backend, quant, bytes, source, folder, note } // about to load this variant
//     { type:'progress', progress }
//     { type:'ready', backend, quant, threads, gpu, note, fallback, source, folder }
//     { type:'merged', matureText, immatureText, matureCursorTime } // live update
//     { type:'complete', text }  // final transcript (file or finalize-all)
//     { type:'error', error, during, code }

import * as ort from 'onnxruntime-web';
import { ParakeetModel, getModelFile } from 'parakeet.js';
import { UtteranceMerger } from './utterance-merger.js';
import { VARIANTS, variantBytes } from './models.js';

// __ORT_VERSION__ is injected at build time from node_modules/onnxruntime-web.
const ORT_CDN = `https://cdn.jsdelivr.net/npm/onnxruntime-web@${__ORT_VERSION__}/dist/`;
ort.env.wasm.wasmPaths = ORT_CDN;
if (ort.env.versions && ort.env.versions.web && ort.env.versions.web !== __ORT_VERSION__) {
  console.warn(
    `[parakeet.worker] onnxruntime-web ${ort.env.versions.web} loaded, but runtime files are ` +
      `pinned to ${__ORT_VERSION__} — rebuild so they match.`,
  );
}

const LONG_AUDIO_SAMPLES = 30 * 16000; // switch to chunked decoding past 30s
const hasSAB = typeof SharedArrayBuffer !== 'undefined' && self.crossOriginIsolated === true;
const CPU_THREADS = hasSAB ? Math.max(1, Math.min(8, navigator.hardwareConcurrency || 4)) : 1;

let model = null;
let loaded = null; // { backend, quant, ... } of the model in use
let loading = null;
const merger = new UtteranceMerger();

// Can this browser run the fp16 encoder on WebGPU? Resolves to { adapter } or
// { reason } (a short, user-facing explanation).
async function probeWebGPU() {
  if (!self.isSecureContext) return { reason: 'WebGPU needs HTTPS' };
  const gpu = self.navigator && navigator.gpu;
  if (!gpu) return { reason: 'no WebGPU in this browser' };
  let adapter = null;
  try {
    adapter = await gpu.requestAdapter({ powerPreference: 'high-performance' });
  } catch (e) {
    return { reason: `no WebGPU adapter (${(e && e.message) || e})` };
  }
  if (!adapter) return { reason: 'no WebGPU adapter' };
  if (!adapter.features.has('shader-f16')) return { reason: 'GPU lacks fp16 shaders (shader-f16)' };
  return { adapter };
}

function adapterName(adapter) {
  const i = adapter && adapter.info;
  if (!i) return '';
  return [i.vendor, i.architecture || i.description].filter(Boolean).join(' ');
}

// Download a variant's files (cached in IndexedDB) and resolve them to blob URLs.
async function download(v) {
  self.postMessage({ type: 'progress', progress: 0 });
  const fileProgress = {};
  const onProgress = (p) => {
    if (!p || !p.file || !p.total) return;
    fileProgress[p.file] = { loaded: p.loaded || 0, total: p.total || 0 };
    let done = 0;
    let total = 0;
    for (const f in fileProgress) {
      done += fileProgress[f].loaded;
      total += fileProgress[f].total;
    }
    // Before every file has reported a length, don't let a small file read as 100%.
    total = Math.max(total, variantBytes(v));
    self.postMessage({ type: 'progress', progress: Math.min(100, Math.floor((done / total) * 100)) });
  };
  const names = Object.values(v.files).map((f) => f.name);
  const urls = await Promise.all(names.map((f) => getModelFile(v.repo, f, { progress: onProgress })));
  return Object.fromEntries(Object.keys(v.files).map((k, i) => [k, urls[i]]));
}

// Are all of a variant's files already in parakeet.js's IndexedDB cache? (Only for
// the status line: "Hugging Face (cached)" vs "(downloaded)".) Same DB, store and key
// format as parakeet.js's hub.js; creates the store the same way if the DB is new.
async function hfCached(v) {
  try {
    const db = await new Promise((resolve, reject) => {
      const req = indexedDB.open('parakeet-cache-db', 1);
      req.onupgradeneeded = () => {
        if (!req.result.objectStoreNames.contains('file-store')) req.result.createObjectStore('file-store');
      };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
    const store = db.transaction('file-store', 'readonly').objectStore('file-store');
    const counts = await Promise.all(
      Object.values(v.files).map(
        (f) =>
          new Promise((resolve) => {
            const r = store.count(`hf-${v.repo}-main--${f.name}`);
            r.onsuccess = () => resolve(r.result);
            r.onerror = () => resolve(0);
          }),
      ),
    );
    db.close();
    return counts.every((n) => n > 0);
  } catch {
    return false;
  }
}

// Blob URLs straight from the local folder's File objects (disk-backed: no copy until
// onnxruntime-web reads them). Throws a 'local-missing' error naming absent files.
function localUrls(v, local) {
  const set = local.sets && local.sets[v.backend];
  if (!set || !set.files) {
    const err = new Error(
      `The model folder "${local.name}" doesn't have the ${v.quant} files for ` +
        `${v.backend === 'webgpu' ? 'WebGPU' : 'the CPU'} (${v.dir}/): ` +
        `missing ${((set && set.missing) || Object.values(v.files).map((f) => f.name)).join(', ')}.`,
    );
    err.stage = 'local-missing';
    throw err;
  }
  self.postMessage({ type: 'progress', progress: 100 });
  return Object.fromEntries(Object.entries(set.files).map(([role, file]) => [role, URL.createObjectURL(file)]));
}

async function createModel(v, urls) {
  const common = {
    encoderUrl: urls.encoder,
    decoderUrl: urls.decoder,
    tokenizerUrl: urls.vocab,
    filenames: { encoder: v.files.encoder.name, decoder: v.files.decoder.name },
    preprocessorBackend: 'js', // pure-JS log-mel; no nemo128.onnx needed
    nMels: 128,
    subsampling: 8,
    windowStride: 0.01,
    cpuThreads: CPU_THREADS,
  };
  if (v.backend === 'wasm') {
    // The int8 graph references its external weights as `<filenames.encoder>.data`.
    return ParakeetModel.fromUrls({ ...common, encoderDataUrl: urls.encoderData, backend: 'wasm' });
  }
  // NOT backend 'webgpu': in parakeet.js 1.4.4 that leaves executionProviders empty, and
  // ORT then picks its highest-priority backend, which is wasm. 'webgpu-hybrid' asks for
  // [webgpu, wasm] for the encoder (decoder on wasm) and creates the sessions one after
  // the other (in parallel they race ORT's WASM init).
  const m = await ParakeetModel.fromUrls({ ...common, backend: 'webgpu-hybrid' });
  // If the WebGPU EP failed to start, ORT silently drops it from [webgpu, wasm] and the
  // fp16 encoder lands on the CPU. JSEP defines env.webgpu.device once it's running.
  if (!ort.env.webgpu || !ort.env.webgpu.device) {
    releaseModel(m);
    throw new Error('the WebGPU execution provider did not start');
  }
  return m;
}

function releaseModel(m) {
  for (const s of [m && m.encoderSession, m && m.joinerSession]) {
    try {
      if (s && s.release) s.release();
    } catch {
      /* ignore */
    }
  }
}

function revoke(urls) {
  for (const u of Object.values(urls || {})) {
    if (typeof u === 'string' && u.startsWith('blob:')) URL.revokeObjectURL(u);
  }
}

// One short inference so a broken fp16/WebGPU setup fails here (where we can fall
// back) rather than on the user's first window. Also compiles the GPU shaders.
async function warmUp(m) {
  const audio = new Float32Array(16000);
  let seed = 1;
  for (let i = 0; i < audio.length; i++) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    audio[i] = (seed / 0x7fffffff - 0.5) * 0.002; // faint noise
  }
  await m.transcribe(audio, 16000, { enableProfiling: false });
}

// Load one variant from the local folder (if the main thread sent one) or Hugging
// Face. Returns { model, source: 'local' | 'hf-cache' | 'hf-download' }.
async function loadVariant(v, note, local) {
  const source = local ? 'local' : (await hfCached(v)) ? 'hf-cache' : 'hf-download';
  self.postMessage({
    type: 'plan',
    backend: v.backend,
    quant: v.quant,
    bytes: variantBytes(v),
    source,
    folder: local ? local.name : '',
    note,
  });
  let urls;
  try {
    urls = local ? localUrls(v, local) : await download(v);
  } catch (e) {
    if (e && e.stage === 'local-missing') throw e;
    // A download error: the other model would need the same internet, so don't fall back.
    const err = e instanceof Error ? e : new Error(String(e));
    err.stage = 'download';
    throw err;
  }
  try {
    const m = await createModel(v, urls);
    try {
      await warmUp(m);
    } catch (e) {
      releaseModel(m);
      throw e;
    }
    return { model: m, source };
  } finally {
    revoke(urls); // ORT has read them; free the blob URLs (the files/IndexedDB copy stay)
  }
}

// requested: 'auto' | 'webgpu' | 'wasm'. local: null, or { name, sets } from
// localModels.js resolveVariants(): sets[backend] = { files: { role: File } } | { missing }.
async function load(requested = 'auto', local = null) {
  if (model) return;
  if (loading) return loading;

  loading = (async () => {
    const t0 = performance.now();
    let note = ''; // why we're on CPU, if we are
    let fallback = false; // WebGPU was tried and failed
    let gpu = '';
    let useGpu = false;
    if (requested !== 'wasm') {
      const probe = await probeWebGPU();
      if (probe.adapter) {
        useGpu = true;
        gpu = adapterName(probe.adapter);
        // Make ORT use the adapter we checked (it would otherwise request its own). Once
        // ORT's WebGPU backend is up, the property is read-only (a retry after a failed load).
        try {
          ort.env.webgpu.adapter = probe.adapter;
        } catch {
          /* already initialised */
        }
      } else if (requested === 'webgpu') {
        useGpu = true; // forced for testing: try anyway, fall back if it fails
        note = probe.reason;
      } else {
        note = probe.reason;
      }
    } else {
      note = 'CPU forced by ?backend=wasm';
    }

    let source = '';
    if (useGpu) {
      try {
        ({ model, source } = await loadVariant(VARIANTS.webgpu, '', local));
        loaded = VARIANTS.webgpu;
      } catch (e) {
        // Not WebGPU's fault: missing local files or no internet. Report, don't switch.
        if (e && (e.stage === 'download' || e.stage === 'local-missing')) throw e;
        console.warn('[parakeet.worker] WebGPU fp16 failed; falling back to WASM int8', e);
        note = `WebGPU failed (${String((e && e.message) || e).slice(0, 160)})`;
        fallback = true;
      }
    }
    if (!model) {
      try {
        ({ model, source } = await loadVariant(VARIANTS.wasm, note, local));
      } catch (e) {
        if (fallback && e && e.stage) e.message = `${note}; then: ${e.message}`;
        throw e;
      }
      loaded = VARIANTS.wasm;
    }

    const threads = (ort.env.wasm && ort.env.wasm.numThreads) || 1;
    console.log(
      `[parakeet.worker] ready: ${loaded.backend} ${loaded.quant}, ${threads} WASM thread(s), ` +
        `${((performance.now() - t0) / 1000).toFixed(1)} s, from ${source}` +
        `${local ? ` "${local.name}"` : ''}${note ? ` (${note})` : ''}`,
    );
    self.postMessage({
      type: 'ready',
      backend: loaded.backend,
      quant: loaded.quant,
      threads,
      gpu: loaded.backend === 'webgpu' ? gpu : '',
      note: loaded.backend === 'webgpu' ? '' : note,
      fallback,
      source,
      folder: local ? local.name : '',
      loadMs: Math.round(performance.now() - t0),
    });
  })();

  // A failed load (typically: no internet to reach Hugging Face / jsDelivr)
  // must not poison later attempts — clear it so the next 'load' retries.
  loading.catch(() => {
    loading = null;
  });

  return loading;
}

// Batch transcription for uploaded files — the whole recording is available up
// front, so a single call (with sentence-aware windowing past 30s) is best.
async function transcribeFile(audio) {
  if (!model) await load();
  if (!audio || audio.length === 0) {
    self.postMessage({ type: 'complete', text: '' });
    return;
  }

  let text = '';
  if (audio.length > LONG_AUDIO_SAMPLES) {
    const r = await model.transcribeLongAudio(audio, 16000, { chunkLengthS: 20 });
    text = (r && r.text) || '';
  } else {
    const r = await model.transcribe(audio, 16000, {});
    text = (r && r.utterance_text) || '';
  }

  self.postMessage({ type: 'complete', text: text.trim() });
}

// Transcribe one overlapping window and feed it to the merger. timeOffset makes
// the returned word times absolute (global), so the merger's cursor and overlap
// dedup line up across consecutive windows.
async function transcribeWindow(audio, startTime) {
  if (!model) await load();
  if (!audio || audio.length === 0) {
    postMerged(merger.snapshot());
    return;
  }
  const r = await model.transcribe(audio, 16000, {
    returnTimestamps: true,
    timeOffset: startTime || 0,
    enableProfiling: false,
  });
  postMerged(merger.process(r && r.words));
}

function postMerged(snap) {
  self.postMessage({
    type: 'merged',
    matureText: snap.matureText,
    immatureText: snap.immatureText,
    matureCursorTime: snap.matureCursorTime,
  });
}

// Serialize work — the pipeline must not run concurrently, and the merger is
// stateful, so windows must be processed strictly in arrival order.
let chain = Promise.resolve();

self.onmessage = (event) => {
  const data = event.data || {};
  const { type } = data;
  chain = chain
    .then(async () => {
      if (type === 'load') {
        await load(data.backend, data.local || null);
      } else if (type === 'transcribe') {
        await transcribeFile(data.audio);
      } else if (type === 'stream-start') {
        merger.reset();
      } else if (type === 'window') {
        await transcribeWindow(data.audio, data.startTime);
      } else if (type === 'finalize-pending') {
        // Stability debounce fired on the main thread: commit the tail and keep
        // the session going (the merger advances its cursor past it).
        postMerged(merger.finalizeAll());
      } else if (type === 'finalize-all') {
        const snap = merger.finalizeAll();
        self.postMessage({ type: 'complete', text: snap.matureText });
      }
    })
    .catch((err) => {
      self.postMessage({
        type: 'error',
        error: String((err && err.message) || err),
        during: type, // lets the UI tell a failed model download from a failed decode
        code: (err && err.stage) || '', // 'download' | 'local-missing' | 
      });
    });
};
