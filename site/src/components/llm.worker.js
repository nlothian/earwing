// The assistant's language-model worker: LiquidAI LFM2.5 (models.js LLMS) on
// Transformers.js 4.3 and onnxruntime-web's native WebGPU execution provider.
//
// WebGPU only (secure context + navigator.gpu + an adapter with shader-f16): the q4f16
// export keeps activations and the KV cache in fp16. There is no CPU fallback; without
// WebGPU the load fails with code 'no-webgpu'.
//
// earwing serves this page from the ESP32's small LittleFS partition, so ORT's .wasm
// runtime is never bundled: astro.config.mjs aliases 'onnxruntime-web/webgpu' (what
// Transformers.js imports) to the extern-wasm dist/ort.webgpu.min.mjs, and below we point
// wasmPaths at jsDelivr's ort-wasm-simd-threaded.asyncify.{mjs,wasm}, pinned to the
// installed version (__ORT_VERSION__). Transformers.js caches those two files (and the
// model files) in the Cache API ('transformers-cache').
//
// Model files come either from Hugging Face (Cache API cached, see `plan.source`) or from
// a local folder: `local.files` maps each repo-relative name (models.js LLMS[key].files)
// to a File. For that we install env.customCache: it answers Transformers.js's
// cache lookups for https://huggingface.co/<repo>/resolve/main/<name> with the File
// (a disk-backed Response; no copy until ORT reads it), passes every other key (the ORT
// runtime files) through to the Cache API, and while a local folder is in use any other
// Hugging Face request gets a 404 without touching the network (optional files are then
// simply absent, as they'd be on the Hub).
//
// Prompting and the KV cache
// - `messages` are rendered with the model's chat template. LFM2.5-2.6B is a reasoning
//   model: its template opens every answer with `<think>`. Thinking is off by default, so
//   the worker prefills `</think>` (an empty thought) after it. The template is rendered
//   with preserve_thinking / keep_past_thinking so earlier assistant turns are replayed
//   verbatim, which is what makes the cache reusable.
// - `done.raw` is the assistant turn exactly as the template must replay it: everything
//   after `<|im_start|>assistant\n` that's in the model's context (for the 2.6B that
//   starts `<think></think>`), minus the final <|im_end|>. The streamed `token` texts
//   concatenate to exactly `raw` (the first one carries that template/prefill head).
// - Reuse: LFM2's conv state can't be rewound, so a cache is only reused when the new
//   prompt EXTENDS the old context. The worker remembers the context as text + token ids;
//   if the new rendered prompt starts with that text, the cached ids are kept and only the
//   rest is tokenized and prefilled. (Comparing text rather than re-tokenized ids means a
//   non-canonical tokenization the model sampled can't spoil reuse.) Anything else, or
//   reuseCache=false, rebuilds from scratch.
// - Prefill runs in chunks of ≤ PREFILL_CHUNK tokens: model.generate() on ids[0:k] with
//   the same DynamicCache and max_new_tokens 1 (generate() slices the input to the
//   uncached part; the sampled token is dropped, so afterwards cache length === k, which
//   we check). Between chunks we honour pause and abort, so a long transcript never holds
//   the GPU for more than one chunk. The last ≤ PREFILL_CHUNK tokens go into the
//   streaming generate() call.
// - Pause (from the main thread's gpuGate, so Parakeet gets the GPU): model.forward is
//   wrapped (an own property) to wait before each forward pass while paused; 'paused' is
//   posted once the GPU is actually idle. Abort interrupts via InterruptableStoppingCriteria
//   and, if we're parked in the pause gate or between chunks, by throwing there.
//
// Message protocol (see the shared contract):
//   main → worker
//     { type:'load', model:'large'|'small', local?: { name, files: { [repoRelativeName]: File } } }
//     { type:'generate', id, messages:[{role, content}], maxTokens, reuseCache=true, think=false,
//       prefix='' /* the answer's opening words, written for the model */ }
//     { type:'count', id, text }        → { type:'count', id, tokens }
//     { type:'abort' } { type:'pause' } { type:'resume' } { type:'reset-cache' } { type:'unload' }
//   worker → main
//     { type:'plan', model, bytes, source:'local'|'hf-cache'|'hf-download', folder }
//     { type:'progress', progress /*0..1*/, loaded, total }
//     { type:'ready', model, adapter, loadMs }
//     { type:'prefill', id, done, total, reused }
//     { type:'token', id, text }
//     { type:'done', id, raw, reason:'eos'|'length'|'aborted', promptTokens, reusedTokens,
//       newTokens, prefillMs, genMs }
//     { type:'paused' } { type:'resumed' } { type:'unloaded' }
//     { type:'error', error, during:'load'|'generate'|'count', code:'no-webgpu'|'local-missing'|
//       'download'|'gpu'|'other', missing?:[names] }

import {
  AutoModelForCausalLM,
  AutoTokenizer,
  DynamicCache,
  InterruptableStoppingCriteria,
  Tensor,
  env,
} from '@huggingface/transformers';
import { LLMS, llmBytes } from './models.js';

// ---------------------------------------------------------------- ORT runtime files
const ORT_CDN = `https://cdn.jsdelivr.net/npm/onnxruntime-web@${__ORT_VERSION__}/dist/`;
const onnx = env.backends.onnx; // shares its .wasm / .webgpu objects with ORT's own env
onnx.wasm.wasmPaths = {
  mjs: `${ORT_CDN}ort-wasm-simd-threaded.asyncify.mjs`,
  wasm: `${ORT_CDN}ort-wasm-simd-threaded.asyncify.wasm`,
};
onnx.wasm.numThreads = 1; // the asyncify build never spawns threads; everything runs on the GPU
if (onnx.versions && onnx.versions.web && onnx.versions.web !== __ORT_VERSION__) {
  console.warn(
    `[llm.worker] onnxruntime-web ${onnx.versions.web} loaded, but runtime files are pinned to ` +
      `${__ORT_VERSION__} — rebuild so they match.`,
  );
}

const PREFILL_CHUNK = 512;
// LiquidAI's recommended sampling for LFM2.5 (model cards); repetition_penalty per model.
const SAMPLING = { do_sample: true, temperature: 0.1, top_k: 50 };
const REPETITION_PENALTY = { large: 1.1, small: 1.05 };
// Template kwargs: replay earlier assistant turns verbatim (the 2.6B's and the 1.2B's names).
const TEMPLATE_KWARGS = { preserve_thinking: true, keep_past_thinking: true };

const post = (msg) => self.postMessage(msg);

// ---------------------------------------------------------------- local files + cache
const HF = () => env.remoteHost.replace(/\/+$/, '') + '/';
const isHf = (url) => url.startsWith(HF()) || /^https:\/\/(hf\.co|[a-z0-9-]+\.huggingface\.co|[a-z0-9.-]+\.hf\.co)\//.test(url);
const fileUrl = (m, name) => `${HF()}${m.repo}/resolve/main/${name}`;
const keyOf = (req) => (typeof req === 'string' ? req : req && req.url) || '';

let localFiles = null; // Map(url → File) while a local folder is in use
let fetchFailures = 0; // network/HTTP failures since the load started (to tell 'download' from 'gpu')

let cachePromise = null;
function browserCache() {
  if (!cachePromise) {
    cachePromise = (typeof caches !== 'undefined' ? caches.open(env.cacheKey) : Promise.reject(new Error('no Cache API')))
      .catch((e) => {
        console.warn(`[llm.worker] Cache API unavailable; nothing will be cached (${(e && e.message) || e})`);
        return null;
      });
  }
  return cachePromise;
}

env.useCustomCache = true;
env.customCache = {
  async match(req) {
    const url = keyOf(req);
    if (localFiles) {
      const f = localFiles.get(url);
      // content-length matters: Transformers.js sizes its read buffer and progress from it.
      if (f) return new Response(f, { headers: { 'content-length': String(f.size) } });
      if (isHf(url)) return undefined;
    }
    if (!/^https?:\/\//.test(url)) return undefined; // e.g. its '/models/…' local-path probe
    const c = await browserCache();
    return c ? c.match(url) : undefined;
  },
  async put(req, response) {
    const url = keyOf(req);
    if ((localFiles && isHf(url)) || !/^https?:\/\//.test(url)) return;
    const c = await browserCache();
    if (c) await c.put(url, response);
  },
  async delete(req) {
    const c = await browserCache();
    return c ? c.delete(keyOf(req)) : false;
  },
};

const baseFetch = env.fetch;
env.fetch = async (input, init) => {
  const url = keyOf(input) || String(input);
  if (localFiles && isHf(url)) {
    return new Response(null, { status: 404, statusText: 'Not Found (loading from a local folder)' });
  }
  try {
    const res = await baseFetch(input, init);
    if (!res.ok && res.status !== 404 && res.status !== 416) fetchFailures++;
    return res;
  } catch (e) {
    fetchFailures++;
    throw e;
  }
};

async function hfCached(m) {
  const c = await browserCache();
  if (!c) return false;
  try {
    const hits = await Promise.all(Object.keys(m.files).map((n) => c.match(fileUrl(m, n))));
    return hits.every(Boolean);
  } catch {
    return false;
  }
}

// ---------------------------------------------------------------- WebGPU probe
// Same checks as parakeet.worker.js: { adapter } or { reason } (short, user-facing).
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

// ---------------------------------------------------------------- state
let spec = null; // LLMS[key] of the loaded model
let tokenizer = null;
let model = null;
let eosIds = new Set();
let loading = null; // Promise while loading
let loadedAdapter = '';
let cache = null; // DynamicCache
let cachedIds = []; // token ids whose state is in `cache`
let cachedText = ''; // …and the prompt text they spell

// Pause gate + abort.
let paused = false;
let pausedPosted = false;
let resumeWaiters = [];
let busy = false; // a generate() is running
let genSeq = 0; // generate requests received
let abortedUpTo = 0; // generate requests with seq ≤ this are aborted
let currentSeq = 0;
let stopper = null; // InterruptableStoppingCriteria of the running generate

class Aborted extends Error {
  constructor() {
    super('aborted');
    this.aborted = true;
  }
}
const isAborted = () => currentSeq > 0 && currentSeq <= abortedUpTo;

function waitWhilePaused() {
  if (!paused) return null;
  if (!pausedPosted) {
    pausedPosted = true;
    post({ type: 'paused' });
  }
  return new Promise((resolve) => resumeWaiters.push(resolve));
}

async function gate() {
  while (paused && !isAborted()) await waitWhilePaused();
  if (isAborted()) throw new Aborted();
}

function wrapForward(m) {
  const forward = m.forward.bind(m);
  m.forward = async (inputs) => {
    await gate();
    return forward(inputs);
  };
}

// ---------------------------------------------------------------- load / unload
function codedError(message, code, extra = {}) {
  const e = new Error(message);
  e.code = code;
  Object.assign(e, extra);
  return e;
}

async function disposeCache() {
  const c = cache;
  cache = null;
  cachedIds = [];
  cachedText = '';
  if (c) {
    try {
      await c.dispose();
    } catch {
      /* ignore */
    }
  }
}

async function unload() {
  await disposeCache();
  const m = model;
  model = null;
  tokenizer = null;
  spec = null;
  localFiles = null;
  if (m) {
    try {
      await m.dispose();
    } catch (e) {
      console.warn('[llm.worker] dispose failed', e);
    }
  }
}

function toLocalFiles(m, local) {
  const files = (local && local.files) || {};
  const missing = Object.entries(m.files)
    .filter(([name, size]) => !files[name] || files[name].size !== size)
    .map(([name]) => name);
  if (missing.length) {
    throw codedError(
      `The model folder "${local.name || ''}" doesn't have the complete ${m.label} files (${m.dir}/): ` +
        `missing ${missing.join(', ')}.`,
      'local-missing',
      { missing },
    );
  }
  return new Map(Object.keys(m.files).map((name) => [fileUrl(m, name), files[name]]));
}

async function load(key, local) {
  const m = LLMS[key];
  if (!m) throw codedError(`unknown model "${key}"`, 'other');
  if (model && spec === m) {
    post({ type: 'ready', model: m.key, adapter: loadedAdapter, loadMs: 0 });
    return;
  }
  if (model) await unload();

  const t0 = performance.now();
  const probe = await probeWebGPU();
  if (!probe.adapter) throw codedError(`The assistant needs WebGPU: ${probe.reason}.`, 'no-webgpu');

  localFiles = local ? toLocalFiles(m, local) : null;
  const source = local ? 'local' : (await hfCached(m)) ? 'hf-cache' : 'hf-download';
  post({ type: 'plan', model: m.key, bytes: llmBytes(m), source, folder: local ? local.name || '' : '' });

  // Progress over all files, by bytes (tokenizer + model load in parallel).
  const total = llmBytes(m);
  const perFile = {};
  let lastPost = 0;
  let lastLoaded = -1;
  const postProgress = (force) => {
    const now = performance.now();
    if (!force && now - lastPost < 100) return;
    let loaded = 0;
    for (const f in perFile) loaded += perFile[f];
    loaded = Math.min(loaded, total);
    if (loaded === lastLoaded) return;
    lastPost = now;
    lastLoaded = loaded;
    post({ type: 'progress', progress: total ? loaded / total : 0, loaded, total });
  };
  const progress_callback = (p) => {
    if (!p || !p.file) return;
    if (p.status === 'progress' && typeof p.loaded === 'number') perFile[p.file] = p.loaded;
    else if (p.status === 'done' && m.files[p.file]) perFile[p.file] = m.files[p.file];
    else return;
    postProgress(p.status === 'done');
  };
  postProgress(true);

  fetchFailures = 0;
  let tok;
  let mdl;
  try {
    [tok, mdl] = await Promise.all([
      AutoTokenizer.from_pretrained(m.repo, { progress_callback }),
      AutoModelForCausalLM.from_pretrained(m.repo, { dtype: m.dtype, device: 'webgpu', progress_callback }),
    ]);
  } catch (e) {
    localFiles = null;
    const msg = String((e && e.message) || e);
    const fileMissing = e && (e.name === 'ModelFileNotFoundError' || /not found|404|Could not locate/i.test(msg));
    if (fetchFailures > 0 || (!local && fileMissing) || /fetch|network/i.test(msg)) {
      throw codedError(`Couldn't download ${m.label} from Hugging Face: ${msg}`, 'download');
    }
    if (local && fileMissing) throw codedError(`${m.label}: ${msg}`, 'local-missing');
    throw codedError(`${m.label} failed to start on the GPU: ${msg}`, 'gpu');
  }
  postProgress(true);

  // One token on a tiny prompt: compiles the shaders now and makes a broken GPU setup fail
  // here, as a load error, rather than on the first question.
  try {
    wrapForward(mdl);
    const ids = tok.encode('Hello', { add_special_tokens: false });
    busy = true; // a 'pause' now is answered once this pass is done
    await mdl.generate({ ...inputsFor(ids), max_new_tokens: 1, do_sample: false });
  } catch (e) {
    try {
      await mdl.dispose();
    } catch {
      /* ignore */
    }
    localFiles = null;
    throw codedError(`${m.label} failed its first run on the GPU: ${(e && e.message) || e}`, 'gpu');
  } finally {
    busy = false;
  }

  spec = m;
  tokenizer = tok;
  model = mdl;
  const gc = mdl.generation_config || {};
  const eos = gc.eos_token_id ?? mdl.config.eos_token_id;
  eosIds = new Set((Array.isArray(eos) ? eos : [eos]).filter((x) => x != null).map(Number));
  loadedAdapter = adapterName(probe.adapter);
  const loadMs = Math.round(performance.now() - t0);
  console.log(`[llm.worker] ready: ${m.label} ${m.dtype} on WebGPU (${loadedAdapter}), ${(loadMs / 1000).toFixed(1)} s, from ${source}`);
  post({ type: 'ready', model: m.key, adapter: loadedAdapter, loadMs });
}

// ---------------------------------------------------------------- generation
function inputsFor(ids) {
  const n = ids.length;
  return {
    input_ids: new Tensor('int64', BigInt64Array.from(ids, (x) => BigInt(x)), [1, n]),
    attention_mask: new Tensor('int64', new BigInt64Array(n).fill(1n), [1, n]),
  };
}

const ASSISTANT_OPEN = '<|im_start|>assistant\n';

// The prompt text for `messages`, plus the assistant turn's head: whatever of the answer
// the template (and our </think> prefill) already wrote after <|im_start|>assistant\n.
function renderPrompt(messages, think, prefix = '') {
  let text = tokenizer.apply_chat_template(messages, {
    add_generation_prompt: true,
    tokenize: false,
    ...TEMPLATE_KWARGS,
  });
  const at = text.lastIndexOf(ASSISTANT_OPEN);
  let head = at >= 0 ? text.slice(at + ASSISTANT_OPEN.length) : '';
  if (!think && head.endsWith('<think>')) {
    text += '</think>';
    head += '</think>';
  }
  // The start of the answer, written for the model (e.g. "## Summary\n" keeps small
  // models to the requested layout). Part of the head, so it's streamed and in raw.
  text += prefix;
  head += prefix;
  return { text, head };
}

const decode = (ids) => (ids.length ? tokenizer.decode(ids, { skip_special_tokens: false }) : '');

// Record what the cache now holds (its first get_seq_length() tokens of prompt + output)
// as ids + text, for the next request's reuse check. Byte-level BPE decodes each token to
// its exact bytes, so the text is exact unless the cut splits a UTF-8 character (then
// the next render won't match it and we rebuild).
async function remember(ids, promptText, out) {
  const n = cache ? cache.get_seq_length() : 0;
  if (n >= ids.length) {
    cachedIds = ids.concat(out).slice(0, n);
    cachedText = promptText + decode(out.slice(0, n - ids.length));
    return;
  }
  cachedIds = ids.slice(0, n);
  cachedText = n ? decode(cachedIds) : '';
  if (!n || !promptText.startsWith(cachedText)) await disposeCache();
}

async function generate({ id, messages, maxTokens, reuseCache = true, think = false, prefix = '' }, seq) {
  if (!model) throw codedError('No model is loaded.', 'other');
  currentSeq = seq;
  busy = true;
  const t0 = performance.now();
  let tFirst = 0;
  let reused = 0;
  let ids = [];
  const out = []; // generated ids (numbers)
  let emitted = ''; // text already sent as token events (after the head)
  let head = '';

  try {
    const r = renderPrompt(messages, think, prefix);
    head = r.head;
    const text = r.text;

    if (reuseCache && cache && cachedText && text.length > cachedText.length && text.startsWith(cachedText)) {
      ids = cachedIds.concat(tokenizer.encode(text.slice(cachedText.length), { add_special_tokens: false }));
      reused = cachedIds.length;
    } else {
      await disposeCache();
      ids = tokenizer.encode(text, { add_special_tokens: false });
    }
    if (!cache) cache = new DynamicCache();
    // From here on the cache holds a prefix of `ids`; keep the bookkeeping right even if
    // we stop part-way (an aborted prefill keeps its progress for the next request).
    const promptText = text;
    const total = ids.length;
    post({ type: 'prefill', id, done: reused, total, reused });

    // Chunked prefill, leaving ≤ PREFILL_CHUNK tokens for the streaming call.
    let k = cache.get_seq_length();
    try {
      while (total - k > PREFILL_CHUNK) {
        await gate();
        const next = k + PREFILL_CHUNK;
        await model.generate({
          ...inputsFor(ids.slice(0, next)),
          past_key_values: cache,
          max_new_tokens: 1,
          do_sample: false,
        });
        k = cache.get_seq_length();
        if (k !== next) throw new Error(`prefill: cache holds ${k} tokens, expected ${next}`);
        post({ type: 'prefill', id, done: k, total, reused });
      }
    } catch (e) {
      if (e && e.aborted) await remember(ids, promptText, out); // keep the partial prefill
      throw e;
    }

    // Stream the answer. The head (e.g. "<think></think>") goes out first so the token
    // texts concatenate to exactly `raw`.
    if (head) post({ type: 'token', id, text: head });
    stopper = new InterruptableStoppingCriteria();
    if (isAborted()) stopper.interrupt();
    let first = true;
    const streamer = {
      put(value) {
        if (first) {
          first = false; // generate() first hands over the prompt
          return;
        }
        const t = Number(value[0][0]);
        out.push(t);
        if (!tFirst) {
          tFirst = performance.now();
          post({ type: 'prefill', id, done: total, total, reused });
        }
        if (eosIds.has(t)) return;
        const s = decode(out.filter((x) => !eosIds.has(x)));
        if (s.endsWith('�')) return; // an incomplete UTF-8 sequence; wait for the rest
        if (s.length > emitted.length && s.startsWith(emitted)) {
          post({ type: 'token', id, text: s.slice(emitted.length) });
          emitted = s;
        }
      },
      end() {},
    };
    let reason = 'length';
    try {
      await model.generate({
        ...inputsFor(ids),
        past_key_values: cache,
        max_new_tokens: Math.max(1, maxTokens | 0 || 512),
        ...SAMPLING,
        repetition_penalty: REPETITION_PENALTY[spec.key] || 1,
        stopping_criteria: [stopper],
        streamer,
      });
      if (out.length && eosIds.has(out[out.length - 1])) reason = 'eos';
      else if (isAborted()) reason = 'aborted';
    } catch (e) {
      if (!(e && e.aborted)) throw e;
      reason = 'aborted';
    }

    // The cache now holds the prompt plus every generated token but the last (or, if
    // aborted before the first token, part of the prompt).
    await remember(ids, promptText, out);
    const body = decode(out.filter((x) => !eosIds.has(x)));
    // Flush anything held back (e.g. a stream that stopped mid-character).
    if (body.length > emitted.length && body.startsWith(emitted)) post({ type: 'token', id, text: body.slice(emitted.length) });
    const tEnd = performance.now();
    post({
      type: 'done',
      id,
      raw: head + body,
      reason,
      promptTokens: ids.length,
      reusedTokens: reused,
      newTokens: out.filter((x) => !eosIds.has(x)).length,
      prefillMs: Math.round((tFirst || tEnd) - t0),
      genMs: Math.round(tFirst ? tEnd - tFirst : 0),
    });
  } catch (e) {
    if (e && e.aborted) {
      // Aborted during prefill (before any token): an empty answer.
      post({
        type: 'done',
        id,
        raw: '',
        reason: 'aborted',
        promptTokens: ids.length,
        reusedTokens: reused,
        newTokens: 0,
        prefillMs: Math.round(performance.now() - t0),
        genMs: 0,
      });
      return;
    }
    await disposeCache(); // unknown state
    throw e;
  } finally {
    busy = false;
    stopper = null;
    currentSeq = 0;
  }
}

// ---------------------------------------------------------------- messages
const GPU_FAILURE = /device (was |is )?lost|GPUDevice|webgpu|out of memory|OOM/i;
function postError(err, during) {
  post({
    type: 'error',
    error: String((err && err.message) || err),
    during,
    // Only a lost/broken device is 'gpu' (the page then drops the model and offers
    // Load again); anything else leaves the model loaded for the next question.
    code: (err && err.code) || (GPU_FAILURE.test(String((err && err.message) || err)) ? 'gpu' : 'other'),
    ...(err && err.missing ? { missing: err.missing } : {}),
  });
}

let chain = Promise.resolve();
const enqueue = (fn) => {
  chain = chain.then(fn, fn);
  return chain;
};

self.onmessage = (event) => {
  const data = event.data || {};
  switch (data.type) {
    case 'load':
      enqueue(async () => {
        loading = load(data.model, data.local || null);
        try {
          await loading;
        } catch (e) {
          postError(e, 'load');
        } finally {
          loading = null;
        }
      });
      break;
    case 'generate': {
      const seq = ++genSeq;
      enqueue(async () => {
        if (seq <= abortedUpTo) {
          post({ type: 'done', id: data.id, raw: '', reason: 'aborted', promptTokens: 0, reusedTokens: 0, newTokens: 0, prefillMs: 0, genMs: 0 });
          return;
        }
        try {
          await generate(data, seq);
        } catch (e) {
          console.error('[llm.worker] generate failed', e);
          postError(e, 'generate');
        }
      });
      break;
    }
    case 'count':
      // Tokenizer only (CPU): don't queue behind a running generation.
      (async () => {
        try {
          if (!tokenizer && loading) await loading.catch(() => {});
          if (!tokenizer) throw codedError('No model is loaded.', 'other');
          const tokens = tokenizer.encode(String(data.text || ''), { add_special_tokens: false }).length;
          post({ type: 'count', id: data.id, tokens });
        } catch (e) {
          postError(e, 'count');
        }
      })();
      break;
    case 'abort':
      abortedUpTo = genSeq;
      if (stopper) stopper.interrupt();
      for (const r of resumeWaiters.splice(0)) r(); // leave the pause gate (it then throws)
      break;
    case 'pause':
      if (!paused) {
        paused = true;
        pausedPosted = false;
        if (!busy) {
          pausedPosted = true;
          post({ type: 'paused' });
        }
      }
      break;
    case 'resume':
      if (paused) {
        paused = false;
        pausedPosted = false;
        for (const r of resumeWaiters.splice(0)) r();
        post({ type: 'resumed' });
      }
      break;
    case 'reset-cache':
      enqueue(() => disposeCache());
      break;
    case 'unload':
      abortedUpTo = genSeq;
      if (stopper) stopper.interrupt();
      for (const r of resumeWaiters.splice(0)) r();
      enqueue(async () => {
        await unload();
        post({ type: 'unloaded' });
      });
      break;
    default:
      break;
  }
};
