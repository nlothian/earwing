// GPU gate: live transcription and the assistant's LLM share one GPU, and a Parakeet
// window queued behind a long LLM prefill makes the live transcript lag. So the
// transcription side reports its in-flight GPU jobs here (Transcriber.jsx: asrBegin
// when it posts a window/final pass/file, asrEnd when the result comes back), and
// llmClient.js pauses the LLM (between prefill chunks, or between decoded tokens)
// while any are in flight, resuming when the count drops back to 0.
//
// Also keeps Parakeet window latency samples, split by whether the LLM was active,
// so the effect of running both can be measured (getStats; window.__earwingAssistant
// in dev). `?llmgate=off` turns the pausing off for A/B comparison; the counters and
// samples still run.
//
// A main-thread singleton: plain module state, no React.

const KEEP = 30; // latency samples kept, of each kind
const STUCK_MS = 120_000; // a job that never reported back: stop blocking the LLM

let inflight = 0;
const byKind = { window: 0, final: 0, file: 0 };
let busySince = 0;
let stuckTimer = null;
let llmActive = false;
let llmActiveSince = 0;
let llmActiveMs = 0;
let pausedSince = 0; // > 0 while the LLM is active and held by the gate
let pausedMs = 0;
const samples = []; // window latencies (ms) without the LLM active
const samplesWithLlm = [];
const listeners = new Set();

export const gateEnabled = (() => {
  try {
    return new URLSearchParams(globalThis.location?.search || '').get('llmgate') !== 'off';
  } catch {
    return true;
  }
})();

const now = () => (globalThis.performance ? performance.now() : Date.now());

function updatePaused() {
  const t = now();
  const held = gateEnabled && llmActive && inflight > 0;
  if (held && !pausedSince) pausedSince = t;
  if (!held && pausedSince) {
    pausedMs += t - pausedSince;
    pausedSince = 0;
  }
}

function notify(busy) {
  for (const fn of listeners) {
    try {
      fn(busy);
    } catch (e) {
      console.error(e);
    }
  }
}

function setInflight(n) {
  const was = inflight > 0;
  inflight = Math.max(0, n);
  const is = inflight > 0;
  if (is && !was) busySince = now();
  if (!is) for (const k of Object.keys(byKind)) byKind[k] = 0;
  clearTimeout(stuckTimer);
  stuckTimer = is ? setTimeout(() => asrReset(), STUCK_MS) : null;
  updatePaused();
  if (is !== was) notify(is);
}

// kind: 'window' | 'final' | 'file'
export function asrBegin(kind = 'window') {
  if (kind in byKind) byKind[kind] += 1;
  setInflight(inflight + 1);
}

export function asrEnd() {
  if (inflight > 0) setInflight(inflight - 1);
}

// The transcription worker finished or failed: nothing is in flight any more.
export function asrReset() {
  setInflight(0);
}

export const isAsrBusy = () => inflight > 0;

// A Parakeet window took ms (post → merged).
export function recordWindow(ms, { audioSec } = {}) {
  if (!(ms >= 0)) return;
  const list = llmActive ? samplesWithLlm : samples;
  list.push({ ms, audioSec: audioSec ?? null });
  if (list.length > KEEP) list.splice(0, list.length - KEEP);
}

// llmClient: the LLM is loading work onto the GPU (a generate is running) or not.
export function setLlmActive(on) {
  on = !!on;
  if (on === llmActive) return;
  const t = now();
  if (on) llmActiveSince = t;
  else llmActiveMs += t - llmActiveSince;
  llmActive = on;
  updatePaused();
}

// fn(busy) whenever the ASR goes busy (0 → >0 jobs) or idle. → unsubscribe
export function subscribe(fn) {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

function median(list) {
  if (!list.length) return null;
  const v = list.map((s) => s.ms).sort((a, b) => a - b);
  const m = v.length >> 1;
  return v.length % 2 ? v[m] : (v[m - 1] + v[m]) / 2;
}

export function getStats() {
  const t = now();
  return {
    asrMedianMs: median(samples),
    asrMedianMsWithLlm: median(samplesWithLlm),
    samples: samples.length,
    samplesWithLlm: samplesWithLlm.length,
    pausedMs: Math.round(pausedMs + (pausedSince ? t - pausedSince : 0)),
    llmActiveMs: Math.round(llmActiveMs + (llmActive ? t - llmActiveSince : 0)),
    // extras (debugging)
    inflight,
    inflightByKind: { ...byKind },
    busyForMs: inflight ? Math.round(t - busySince) : 0,
    gate: gateEnabled,
  };
}
