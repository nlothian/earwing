// The meeting assistant's main-thread side: useLlm() drives the LLM worker
// (llm.worker.js: LFM2.5 on WebGPU via transformers.js) and keeps the chat.
//
// - The worker is created on the first load(), so nothing is downloaded or compiled
//   for people who never open the assistant. Model switch = unload, then load.
// - Every request is planned by llmPrompt.js: append to the conversation the worker
//   has cached (the transcript lines sent so far are unchanged), or rebuild it from
//   the current transcript (reset-cache; earlier chat stays on screen but out of the
//   model's context: meta.rebuilt). Over the verbatim budget, summaries are
//   map-reduced over chunks (notes cached per model + chunk text) and questions get
//   older chunks' notes plus the newest lines.
// - Assistant turns go back into the history as the worker's `raw`, so the next
//   prompt starts with exactly the tokens the worker has cached.
// - While live transcription has a Parakeet job on the GPU (gpuGate.js), the LLM is
//   paused, so the transcript doesn't lag behind a long prefill.
// - Streamed tokens collect in a ref and reach React state once per animation frame.
// - Summaries run on their own too (useAutoSummary.js: run(…, { auto: true })). The
//   chat keeps one "Live summary" pair for them, each auto run replacing the last;
//   preemptAuto() stops one so a question can go first. Every summary that finishes
//   unstopped becomes `lastSummary`, which meetingSync.js sends to the device.
//
// Dev: `?llm=mock` swaps in llm.mock.worker.js (fake model, same protocol; dev server
// only), and window.__earwingAssistant exposes the state and gpuGate stats.

import { useCallback, useEffect, useRef, useState } from 'react';
import * as gate from './gpuGate.js';
import {
  assembleSummary,
  estTokens,
  linesText,
  MAX_TOKENS,
  mapMessages,
  NOTES_PREFIX,
  notesConversation,
  planRequest,
  reduceMessages,
  SUMMARY_PREFIX,
  stripThink,
  trimCutNotes,
  commitAnswer,
} from './llmPrompt.js';

export const LLM_MODEL_KEY = 'earwing-llm-model';
const MODEL_KEYS = ['large', 'small'];
const COUNT_TIMEOUT_MS = 15000;

function params() {
  try {
    return new URLSearchParams(window.location.search);
  } catch {
    return new URLSearchParams();
  }
}

const mockRequested = () => import.meta.env.DEV && params().get('llm') === 'mock';

// Sync check: can this page run the assistant at all? → { ok, reason }
export function llmSupport() {
  if (typeof window === 'undefined' || typeof Worker === 'undefined') {
    return { ok: false, reason: 'The assistant needs Web Workers.' };
  }
  if (mockRequested()) return { ok: true, reason: '' };
  if (params().get('backend') === 'wasm') {
    return { ok: false, reason: 'The assistant runs on WebGPU only, and ?backend=wasm turns WebGPU off.' };
  }
  if (!window.isSecureContext) {
    return {
      ok: false,
      reason: 'The assistant needs WebGPU, which browsers only offer on a secure page (https://earwing.local).',
    };
  }
  if (!navigator.gpu) return { ok: false, reason: 'The assistant needs WebGPU, and this browser doesn’t have it.' };
  return { ok: true, reason: '' };
}

let probe = null;
// Async check: a WebGPU adapter with 16-bit float shaders. → { ok, reason }
export function probeLlmSupport() {
  if (!probe) {
    probe = (async () => {
      const s = llmSupport();
      if (!s.ok || mockRequested()) return s;
      try {
        const adapter = await navigator.gpu.requestAdapter();
        if (!adapter) return { ok: false, reason: 'WebGPU found no usable GPU adapter.' };
        if (!adapter.features.has('shader-f16')) {
          return { ok: false, reason: 'This GPU lacks 16-bit float shaders (shader-f16), which the model needs.' };
        }
        return { ok: true, reason: '' };
      } catch (e) {
        return { ok: false, reason: `WebGPU failed: ${(e && e.message) || e}` };
      }
    })();
  }
  return probe;
}

// The remembered choice, else small on low-memory or touch devices, else large.
export function defaultLlmModel() {
  try {
    const saved = window.localStorage.getItem(LLM_MODEL_KEY);
    if (MODEL_KEYS.includes(saved)) return saved;
  } catch {
    /* ignore */
  }
  try {
    const mem = navigator.deviceMemory;
    if ((mem && mem < 8) || window.matchMedia('(pointer: coarse)').matches) return 'small';
  } catch {
    /* ignore */
  }
  return 'large';
}

function createWorker() {
  if (import.meta.env.DEV && mockRequested()) {
    // Deliberately not `new URL(…, import.meta.url)`: Vite bundles every worker it sees
    // in that form, even in dead code, and the mock must never reach the device build.
    // The dev server serves the file as-is (it has no imports).
    const here = import.meta.url;
    return new Worker(new URL('./llm.mock.worker.js', here), { type: 'module' });
  }
  return new Worker(new URL('./llm.worker.js', import.meta.url), { type: 'module' });
}

const errOf = (msg) => ({
  message: msg.error || 'Something went wrong.',
  code: msg.code || 'other',
  ...(msg.missing ? { missing: msg.missing } : {}),
});

let nextMsgId = 1;

export function useLlm() {
  const [state, setState] = useState('idle'); // idle | loading | ready | busy | error | unsupported
  const [model, setModelState] = useState(defaultLlmModel);
  const [loadedModel, setLoadedModel] = useState(null);
  const [plan, setPlan] = useState(null);
  const [progress, setProgress] = useState(0);
  const [error, setError] = useState(null);
  const [phase, setPhaseState] = useState(null);
  const [messages, setMessages] = useState([]);
  const [busyKind, setBusyKindState] = useState(null); // 'auto' | 'user' while busy
  const [lastSummary, setLastSummary] = useState(null); // { id, text, coversTo, take, auto, ms }
  const [stats, setStats] = useState(() => ({ ...gate.getStats(), tokPerSec: null }));
  const [adapter, setAdapter] = useState('');

  // Mutable state the worker callbacks need without re-rendering.
  const S = useRef(null);
  if (!S.current) {
    S.current = {
      worker: null,
      state: 'idle',
      model: model,
      loadedModel: null,
      wantModel: null, // the model a posted 'load' is for
      conv: null, // llmPrompt.js conversation the worker has cached (or is about to)
      take: undefined,
      epoch: 0, // bumped by clear/unload/new take: a run still finishing mustn't commit
      seq: 0,
      gen: null, // { id, resolve, reject, raw, streamTo, step, sawToken }
      counts: new Map(), // id → { resolve, timer, text }
      gatePaused: false,
      phase: null,
      phaseBeforePause: null,
      stopRequested: false,
      afterGen: [], // posts deferred until the running generate settles
      raf: 0,
      notes: new Map(), // `${model}:${chunk hash}` → notes text
      lastTokPerSec: null,
      statsTimer: 0,
      msgs: [], // the chat, as last set (messages can lag a render behind)
      busyKind: null,
      running: null, // the promise of the run in progress
    };
  }

  const setStateBoth = useCallback((s) => {
    S.current.state = s;
    setState(s);
  }, []);
  const setPhase = useCallback((p) => {
    S.current.phase = p;
    setPhaseState(p);
  }, []);
  const post = useCallback((m) => S.current.worker && S.current.worker.postMessage(m), []);

  const refreshStats = useCallback(() => {
    const s = S.current;
    if (s.statsTimer) return;
    s.statsTimer = setTimeout(() => {
      s.statsTimer = 0;
      setStats({ ...gate.getStats(), tokPerSec: s.lastTokPerSec });
    }, 500);
  }, []);

  // Every chat change goes through here, so S.current.msgs is always current.
  const updateMessages = useCallback((fn) => {
    const s = S.current;
    s.msgs = fn(s.msgs);
    setMessages(s.msgs);
  }, []);

  const patchMsg = useCallback(
    (id, patch) => {
      updateMessages((ms) =>
        ms.map((m) => (m.id === id ? { ...m, ...patch, meta: patch.meta ? { ...m.meta, ...patch.meta } : m.meta } : m)),
      );
    },
    [updateMessages],
  );

  // Streamed text → state, at most once per frame.
  const flush = useCallback(() => {
    const s = S.current;
    s.raf = 0;
    const g = s.gen;
    if (!g || !g.streamTo) return;
    const { text, thinking } = stripThink(g.raw, true);
    patchMsg(g.streamTo, { display: text, thinking });
  }, [patchMsg]);

  // --- worker plumbing -----------------------------------------------------------

  const settleGen = useCallback(() => {
    const s = S.current;
    s.gen = null;
    gate.setLlmActive(false);
    if (s.gatePaused) {
      s.gatePaused = false;
      post({ type: 'resume' });
    }
    for (const m of s.afterGen.splice(0)) post(m);
    refreshStats();
  }, [post, refreshStats]);

  const onWorkerMessage = useCallback(
    (msg) => {
      const s = S.current;
      switch (msg.type) {
        case 'plan':
          setPlan({ model: msg.model, bytes: msg.bytes, source: msg.source, folder: msg.folder });
          setProgress(0);
          break;
        case 'progress':
          setProgress(msg.progress || 0);
          break;
        case 'ready':
          if (s.state !== 'loading' || msg.model !== s.wantModel) {
            post({ type: 'unload' }); // a load we no longer want (the model was switched)
            break;
          }
          s.loadedModel = msg.model;
          setLoadedModel(msg.model);
          setAdapter(msg.adapter || '');
          setProgress(1);
          setError(null);
          setStateBoth('ready');
          break;
        case 'prefill':
          if (s.gen && msg.id === s.gen.id) {
            if (s.phase && s.phase.kind === 'paused') {
              s.phaseBeforePause = { kind: 'prefill', done: msg.done, total: msg.total, ...s.gen.step };
            } else setPhase({ kind: 'prefill', done: msg.done, total: msg.total, ...s.gen.step });
          }
          break;
        case 'token': {
          const g = s.gen;
          if (!g || msg.id !== g.id) break;
          g.raw += msg.text || '';
          if (!g.sawToken) {
            g.sawToken = true;
            if (!(s.phase && s.phase.kind === 'paused')) setPhase({ kind: 'generate', ...g.step });
            else s.phaseBeforePause = { kind: 'generate', ...g.step };
          }
          if (g.streamTo && !s.raf) s.raf = requestAnimationFrame(flush);
          break;
        }
        case 'done': {
          const g = s.gen;
          if (!g || msg.id !== g.id) break;
          if (s.raf) cancelAnimationFrame(s.raf);
          s.raf = 0;
          settleGen();
          g.resolve({ ...msg, raw: typeof msg.raw === 'string' ? msg.raw : g.raw });
          break;
        }
        case 'count': {
          const c = s.counts.get(msg.id);
          if (c) {
            clearTimeout(c.timer);
            s.counts.delete(msg.id);
            c.resolve(Number.isFinite(msg.tokens) ? msg.tokens : estTokens(c.text));
          }
          break;
        }
        case 'paused':
          if (!(s.phase && s.phase.kind === 'paused')) {
            s.phaseBeforePause = s.phase;
            setPhase({ kind: 'paused' });
          }
          break;
        case 'resumed':
          if (s.phase && s.phase.kind === 'paused') setPhase(s.phaseBeforePause);
          s.phaseBeforePause = null;
          break;
        case 'unloaded':
          break;
        case 'error': {
          const err = errOf(msg);
          if (msg.during === 'count') {
            const first = s.counts.entries().next().value;
            if (first) {
              const [id, c] = first;
              clearTimeout(c.timer);
              s.counts.delete(id);
              c.resolve(estTokens(c.text));
            }
            break;
          }
          if (msg.during === 'generate') {
            const g = s.gen;
            if (!g) break;
            if (s.raf) cancelAnimationFrame(s.raf);
            s.raf = 0;
            settleGen();
            g.reject(Object.assign(new Error(err.message), err));
            break;
          }
          if (err.code === 'gpu' && s.state !== 'loading') {
            // The GPU device was lost while idle: the model needs loading again.
            s.loadedModel = null;
            s.conv = null;
            setLoadedModel(null);
            setError(err);
            if (s.state !== 'busy') setStateBoth('idle');
            break;
          }
          // Load failed: the model isn't usable.
          s.loadedModel = null;
          setLoadedModel(null);
          setError(err);
          setStateBoth('error');
          break;
        }
        default:
          break;
      }
    },
    [flush, post, setPhase, setStateBoth, settleGen],
  );

  const ensureWorker = useCallback(() => {
    const s = S.current;
    if (s.worker) return s.worker;
    const w = createWorker();
    w.onmessage = (e) => onWorkerMessage(e.data || {});
    w.onerror = (e) => {
      // The worker script itself failed (e.g. couldn't be fetched): start over next time.
      if (e && e.preventDefault) e.preventDefault();
      w.terminate();
      if (s.worker === w) s.worker = null;
      if (s.gen) {
        const g = s.gen;
        settleGen();
        g.reject(Object.assign(new Error((e && e.message) || 'The assistant stopped.'), { code: 'other' }));
      }
      s.loadedModel = null;
      s.conv = null;
      setLoadedModel(null);
      setError({ message: (e && e.message) || 'The assistant’s worker failed to start.', code: 'other' });
      setStateBoth('error');
    };
    s.worker = w;
    return w;
  }, [onWorkerMessage, setStateBoth, settleGen]);

  // One generate at a time. → done message ({ raw, reason, promptTokens, … })
  const generate = useCallback(
    (messages, maxTokens, reuseCache, { streamTo = null, step = {}, prefix = '' } = {}) => {
      const s = S.current;
      if (s.stopRequested) {
        // Stopped between steps (while counting tokens, say): don't start another.
        return Promise.resolve({ raw: '', reason: 'aborted', promptTokens: 0, reusedTokens: 0, newTokens: 0 });
      }
      const id = ++s.seq;
      return new Promise((resolve, reject) => {
        s.gen = { id, resolve, reject, raw: '', streamTo, step, sawToken: false };
        gate.setLlmActive(true);
        post({ type: 'generate', id, messages, maxTokens, reuseCache, prefix });
        if (gate.gateEnabled && gate.isAsrBusy()) {
          s.gatePaused = true;
          post({ type: 'pause' });
        }
      });
    },
    [post],
  );

  // Exact token count from the worker's tokenizer (estimate if it can't say).
  const count = useCallback(
    (text) => {
      const s = S.current;
      if (!s.worker) return Promise.resolve(estTokens(text));
      const id = ++s.seq;
      return new Promise((resolve) => {
        const timer = setTimeout(() => {
          s.counts.delete(id);
          resolve(estTokens(text));
        }, COUNT_TIMEOUT_MS);
        s.counts.set(id, { resolve, timer, text });
        post({ type: 'count', id, text });
      });
    },
    [post],
  );

  // Post after the running generate (if any) has settled.
  const postAfterGen = useCallback(
    (m) => {
      if (S.current.gen) S.current.afterGen.push(m);
      else post(m);
    },
    [post],
  );

  // --- effects -------------------------------------------------------------------

  useEffect(() => {
    let alive = true;
    const s = llmSupport();
    if (!s.ok) {
      setError({ message: s.reason, code: 'no-webgpu' });
      setStateBoth('unsupported');
      return undefined;
    }
    probeLlmSupport().then((p) => {
      if (!alive || p.ok) return;
      setError({ message: p.reason, code: 'no-webgpu' });
      setStateBoth('unsupported');
    });
    return () => {
      alive = false;
    };
  }, [setStateBoth]);

  // Pause the LLM while Parakeet has GPU work in flight; resume when it's done.
  useEffect(
    () =>
      gate.subscribe((busy) => {
        const s = S.current;
        refreshStats();
        if (!gate.gateEnabled || !s.gen) return;
        if (busy && !s.gatePaused) {
          s.gatePaused = true;
          post({ type: 'pause' });
        } else if (!busy && s.gatePaused) {
          s.gatePaused = false;
          post({ type: 'resume' });
        }
      }),
    [post, refreshStats],
  );

  useEffect(
    () => () => {
      const s = S.current;
      if (s.raf) cancelAnimationFrame(s.raf);
      clearTimeout(s.statsTimer);
      gate.setLlmActive(false);
      if (s.worker) s.worker.terminate();
      s.worker = null;
    },
    [],
  );

  // --- actions -------------------------------------------------------------------

  const forgetConv = useCallback(() => {
    const s = S.current;
    s.conv = null;
    s.epoch += 1;
  }, []);

  const stop = useCallback(() => {
    const s = S.current;
    s.stopRequested = true;
    if (!s.gen) return;
    post({ type: 'abort' });
    if (s.gatePaused) {
      // A paused worker must wake up to see the abort.
      s.gatePaused = false;
      post({ type: 'resume' });
    }
  }, [post]);

  const load = useCallback(
    (local) => {
      const s = S.current;
      if (s.state === 'loading' || s.state === 'busy' || s.state === 'unsupported') return;
      const key = s.model;
      if (s.loadedModel === key) return;
      const w = ensureWorker();
      if (s.loadedModel) {
        w.postMessage({ type: 'unload' });
        s.loadedModel = null;
        setLoadedModel(null);
        forgetConv();
      }
      s.wantModel = key;
      setError(null);
      setPlan(null);
      setProgress(0);
      setStateBoth('loading');
      w.postMessage(local ? { type: 'load', model: key, local } : { type: 'load', model: key });
    },
    [ensureWorker, forgetConv, setStateBoth],
  );

  const unload = useCallback(() => {
    const s = S.current;
    if (s.state === 'unsupported') return;
    stop();
    s.wantModel = null;
    if (s.worker) postAfterGen({ type: 'unload' });
    s.loadedModel = null;
    setLoadedModel(null);
    forgetConv();
    setPhase(null);
    setStateBoth('idle');
  }, [forgetConv, postAfterGen, setPhase, setStateBoth, stop]);

  const setModel = useCallback(
    (key) => {
      const s = S.current;
      if (!MODEL_KEYS.includes(key) || key === s.model) return;
      s.model = key;
      setModelState(key);
      try {
        window.localStorage.setItem(LLM_MODEL_KEY, key);
      } catch {
        /* ignore */
      }
      if (s.state === 'unsupported') return;
      // A loaded model stays loaded (and usable) until the user clicks "Switch to …";
      // load() unloads it then. Only a load still in flight is for the wrong model now.
      if (s.state === 'loading') unload();
      else if (s.state === 'error') {
        setError(null);
        setStateBoth('idle');
      }
    },
    [setStateBoth, unload],
  );

  const clear = useCallback(() => {
    const s = S.current;
    stop();
    updateMessages(() => []);
    forgetConv();
    if (s.worker && s.loadedModel) postAfterGen({ type: 'reset-cache' });
  }, [forgetConv, postAfterGen, stop, updateMessages]);

  // Map step: notes for one chunk (cached per model + chunk text).
  const chunkNotes = useCallback(
    async (chunk, i, n, epoch) => {
      const s = S.current;
      const key = `${s.loadedModel}:${chunk.hash}`;
      if (s.notes.has(key)) return s.notes.get(key);
      const res = await generate(mapMessages(chunk, i, n), MAX_TOKENS.notes, false, {
        step: { step: 'notes', part: i + 1, parts: n },
        prefix: NOTES_PREFIX,
      });
      if (res.reason === 'aborted' || epoch !== s.epoch) return null;
      const notes = trimCutNotes(stripThink(res.raw).text, res.reason);
      s.notes.set(key, notes);
      return notes;
    },
    [generate],
  );

  // → { ok } done; { stopped } (Stop, or cleared/unloaded meanwhile); { error: code };
  //   { skipped } (not idle, nothing asked); { empty } (nothing transcribed to go on).
  const runOnce = useCallback(
    async (kind, question, src, { auto = false } = {}) => {
      const s = S.current;
      if (s.state !== 'ready' || !src) return { ok: false, skipped: true };
      if (kind === 'question' && !String(question || '').trim()) return { ok: false, skipped: true };

      // A new take: its chat and cache start empty.
      const newTake = s.take !== undefined && src.take !== s.take;
      if (newTake && (s.conv || s.msgs.length)) {
        updateMessages(() => []);
        forgetConv();
        post({ type: 'reset-cache' });
      }
      s.take = src.take;
      // Earlier chat on screen: a rebuild leaves it out of the model's context.
      const hadChat = !newTake && s.msgs.length > 0;
      const epoch = s.epoch;
      const userId = nextMsgId++;
      const asstId = nextMsgId++;
      const updating = kind === 'summary' && s.conv && s.conv.hasSummary;
      // An auto run replaces the last auto pair: the chat keeps one live summary.
      updateMessages((ms) => [
        ...(auto ? ms.filter((m) => !m.auto) : ms),
        {
          id: userId,
          role: 'user',
          kind,
          auto,
          display: auto
            ? 'Live summary'
            : kind === 'summary'
              ? updating
                ? 'Update the summary'
                : 'Summarise'
              : String(question).trim(),
          streaming: false,
          thinking: false,
          error: null,
          stopped: false,
          meta: {},
        },
        {
          id: asstId,
          role: 'assistant',
          kind,
          auto,
          display: '',
          streaming: true,
          thinking: false,
          error: null,
          stopped: false,
          meta: {},
        },
      ]);
      s.stopRequested = false;
      s.busyKind = auto ? 'auto' : 'user';
      setBusyKindState(s.busyKind);
      setStateBoth('busy');

      const req = { kind, question };
      const opts = { take: src.take, model: s.loadedModel };
      const finishMsg = (res, meta, extra = {}) => {
        const tokPerSec = res && res.genMs > 0 && res.newTokens ? res.newTokens / (res.genMs / 1000) : null;
        if (tokPerSec) s.lastTokPerSec = tokPerSec;
        const { text: textOverride, ...rest } = extra;
        const text = textOverride ?? (res ? stripThink(res.raw).text : '');
        if (kind === 'summary' && res && res.reason !== 'aborted' && text && !extra.error) {
          setLastSummary({ id: asstId, text, coversTo: meta.coversTo, take: src.take, auto, ms: Date.now() });
        }
        patchMsg(asstId, {
          display: text,
          streaming: false,
          thinking: false,
          stopped: !!(res && res.reason === 'aborted'),
          meta: {
            promptTokens: res ? res.promptTokens : 0,
            reusedTokens: res ? res.reusedTokens : 0,
            newTokens: res ? res.newTokens : 0,
            tokPerSec,
            prefillMs: res ? res.prefillMs : 0,
            ...meta,
          },
          ...rest,
        });
        return res && res.reason === 'aborted' ? { ok: false, stopped: true } : { ok: true };
      };

      // Cleared, unloaded or a new take while this ran: don't touch the conversation.
      const abandon = () => {
        patchMsg(asstId, { streaming: false, thinking: false, stopped: true });
        return { ok: false, stopped: true };
      };

      try {
        let p = planRequest(s.conv, src, req, opts);
        if (p.action !== 'append' && p.lines.length) {
          const exact = await count(linesText(p.lines));
          p = planRequest(s.conv, src, req, { ...opts, transcriptTokens: exact });
        }
        if (p.action !== 'append' && !p.lines.length) {
          const live = !src.final && src.timeline;
          finishMsg(null, {}, {
            error: live
              ? 'Nothing to go on yet: the newest ~12 s wait for their speaker labels.'
              : 'Nothing has been transcribed yet.',
          });
          return { ok: false, empty: true };
        }
        const rebuilt = hadChat && p.action !== 'append';
        const coversTo = p.conv ? p.conv.coversTo : p.lines[p.lines.length - 1].end;
        if (p.action !== 'append') {
          post({ type: 'reset-cache' });
          s.conv = null;
          // Tokens to prefill: the transcript (exact) plus the rest of the prompt (estimated).
          const tokens = p.conv
            ? p.conv.transcriptTokens +
              estTokens(p.messages.map((m) => m.content).join('')) -
              estTokens(linesText(p.lines))
            : p.chunks.reduce((sum, c) => sum + c.tokens, 0);
          setPhase({ kind: 'rebuild', tokens, reason: p.reason });
        }
        const answerPrefix = req.kind === 'summary' ? SUMMARY_PREFIX : '';
        const meta = { coversTo, rebuilt, ...(rebuilt ? { rebuildReason: p.reason } : {}) };

        if (p.action === 'append' || p.action === 'verbatim') {
          const res = await generate(p.messages, p.maxTokens, true, { streamTo: asstId, prefix: answerPrefix });
          if (epoch !== s.epoch) return abandon();
          if (res.reason === 'aborted' && !res.newTokens) {
            // Stopped during prefill: nothing to add. (The worker's own prefix check
            // decides what of its cache the next prompt can reuse.)
            if (p.action === 'verbatim') s.conv = null;
          } else s.conv = commitAnswer(p.conv, res.raw);
          return finishMsg(res, meta);
        }

        // Over budget: notes per chunk (standalone conversations), then either merge
        // them (summary) or ask with notes + the newest lines (question).
        const n = p.action === 'map-reduce' ? p.chunks.length : p.k;
        const notes = [];
        for (let i = 0; i < n; i++) {
          if (s.stopRequested) break;
          const note = await chunkNotes(p.chunks[i], i, p.chunks.length, epoch);
          if (note === null) break;
          notes.push(note);
        }
        if (epoch !== s.epoch) return abandon();
        if (notes.length < n) {
          finishMsg(null, meta, { stopped: true, display: '' });
          return { ok: false, stopped: true };
        }
        if (p.action === 'map-reduce') {
          const parts = p.chunks.map((c, i) => ({ start: c.start, end: c.end, notes: notes[i] }));
          const res = await generate(reduceMessages(parts), p.maxTokens, false, {
            streamTo: asstId,
            prefix: answerPrefix,
            step: { step: 'merge', parts: n },
          });
          if (epoch !== s.epoch) return abandon();
          s.conv = null; // the cache holds the merge conversation: the next request rebuilds
          // Key points come from the notes (the merge only covers the first parts well).
          const text = res.reason === 'aborted' ? undefined : assembleSummary(stripThink(res.raw).text, parts);
          return finishMsg(res, { ...meta, parts: n }, { text });
        }
        const nc = notesConversation(p, notes, src, req, opts);
        post({ type: 'reset-cache' }); // the cache holds the last notes conversation
        const res = await generate(nc.messages, p.maxTokens, true, { streamTo: asstId });
        if (epoch !== s.epoch) return abandon();
        if (res.reason === 'aborted' && !res.newTokens) s.conv = null;
        else s.conv = commitAnswer(nc.conv, res.raw);
        return finishMsg(res, { ...meta, parts: n });
      } catch (e) {
        const code = (e && e.code) || 'other';
        s.conv = null;
        patchMsg(asstId, { streaming: false, thinking: false, error: (e && e.message) || String(e) });
        if (code === 'gpu') {
          // The GPU device was lost: the model has to be loaded again.
          s.loadedModel = null;
          setLoadedModel(null);
          setError({ message: e.message, code });
          setPhase(null);
          setStateBoth('idle');
          return { ok: false, error: code };
        }
        post({ type: 'reset-cache' });
        return { ok: false, error: code };
      } finally {
        s.busyKind = null;
        setBusyKindState(null);
        if (s.state === 'busy') {
          setPhase(null);
          setStateBoth(s.loadedModel ? 'ready' : 'idle');
        }
        refreshStats();
      }
    },
    [chunkNotes, count, forgetConv, generate, patchMsg, post, refreshStats, setPhase, setStateBoth, updateMessages],
  );

  // One run at a time; `running` is its promise (preemptAuto waits on it).
  const run = useCallback(
    (kind, question, src, opts) => {
      const s = S.current;
      const idle = !s.busyKind;
      const p = runOnce(kind, question, src, opts);
      if (idle && s.busyKind) {
        s.running = p;
        p.finally(() => {
          if (s.running === p) s.running = null;
        });
      }
      return p;
    },
    [runOnce],
  );

  const summarise = useCallback((src, opts) => run('summary', null, src, opts), [run]);
  const ask = useCallback((question, src) => run('question', question, src), [run]);

  // Stop an auto run (keeping what it wrote so far, as Stop does) and wait for it to
  // wind down, so a question can go straight after.
  const preemptAuto = useCallback(async () => {
    const s = S.current;
    if (s.busyKind !== 'auto' || !s.running) return;
    const p = s.running;
    stop();
    await p;
  }, [stop]);

  // Dev hook: poke at the assistant from the console / tests.
  const snap = useRef({});
  snap.current = { state, model, loadedModel, plan, progress, error, phase, messages, stats, busyKind, lastSummary };
  useEffect(() => {
    if (!import.meta.env.DEV) return undefined;
    window.__earwingAssistant = {
      get state() {
        return snap.current;
      },
      get conv() {
        return S.current.conv;
      },
      gate: () => gate.getStats(),
      notesCache: () => new Map(S.current.notes),
    };
    return () => {
      delete window.__earwingAssistant;
    };
  }, []);

  return {
    state,
    model,
    loadedModel,
    plan,
    progress,
    error,
    phase,
    messages,
    stats,
    adapter,
    busyKind,
    lastSummary,
    setModel,
    load,
    unload,
    summarise,
    ask,
    preemptAuto,
    stop,
    clear,
  };
}
