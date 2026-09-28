// useAutoSummary(): the live summary. Once a model is loaded, the summary is re-run
// on its own whenever the LLM is idle and the transcript has moved on, and each
// result goes to the device (meetingSync.js via llm.lastSummary). There's no switch:
// loading the model is the opt-in.
//
// A run starts when all of these hold:
// - the LLM is loaded and idle (a user's summary or question comes first), and not
//   stopped for good by a GPU error (until the model is loaded again);
// - MIN_NEW_WORDS more settled words (llmPrompt.js settledCount) than at the last run's
//   start, or any new ones once the take is final;
// - the gap since the last run ended: clamp(last run's duration, MIN_GAP, MAX_GAP)
//   (so a slow map-reduce spaces itself out), doubled per error in a row up to
//   MAX_BACKOFF;
// - nobody typed a question in the last TYPING_MS;
// - no Stop on an auto run in the last STOP_SUSPEND_MS (suspend()).
// Checked on each transcript change (wordsVersion) and every TICK_MS. A new take
// starts over. Speech-to-text keeps the GPU first through gpuGate.js, as for any run.

import { useCallback, useEffect, useRef } from 'react';
import { settledCount, settledUntilFor } from './llmPrompt.js';

export const MIN_NEW_WORDS = 10;
export const MIN_GAP = 20000;
export const MAX_GAP = 180000;
export const MAX_BACKOFF = 300000;
export const TYPING_MS = 8000;
export const STOP_SUSPEND_MS = 120000;
const TICK_MS = 5000;

const fresh = () => ({ lastN: 0, lastEnd: 0, lastDur: 0, errors: 0, suspendUntil: 0, gpuOff: false, running: false });

// The gap after a run: its duration, clamped, doubled per error in a row.
export function autoGap(lastDur, errors) {
  const gap = Math.min(MAX_GAP, Math.max(MIN_GAP, lastDur || 0));
  return errors ? Math.min(MAX_BACKOFF, gap * 2 ** errors) : gap;
}

// llm: useLlm(). getSrc(): the request source ({ words, timeline, labels, final, take }).
// typedAt(): when the question box was last typed in (ms). → { suspend() }
export function useAutoSummary(llm, getSrc, typedAt, { wordsVersion, take }) {
  const L = useRef(llm);
  L.current = llm;
  const src = useRef(getSrc);
  src.current = getSrc;
  const typed = useRef(typedAt);
  typed.current = typedAt;
  const A = useRef(null);
  if (!A.current) A.current = { ...fresh(), take };

  const check = useCallback(async () => {
    const a = A.current;
    const l = L.current;
    if (a.running || a.gpuOff || !l.loadedModel || l.state !== 'ready') return;
    const now = Date.now();
    if (now < a.suspendUntil || now - (typed.current() || 0) < TYPING_MS) return;
    if (now - a.lastEnd < autoGap(a.lastDur, a.errors)) return;
    const s = src.current();
    const words = s.words || [];
    const n = settledCount(words, settledUntilFor(words, s.timeline, s.final));
    if (!(n - a.lastN >= MIN_NEW_WORDS || (s.final && n > a.lastN))) return;

    const take0 = a.take;
    const prevN = a.lastN;
    a.running = true;
    a.lastN = n;
    const t0 = Date.now();
    let out = null;
    try {
      out = await l.summarise(s, { auto: true });
    } finally {
      a.running = false;
    }
    if (A.current !== a || a.take !== take0) return; // a new take meanwhile: its own start
    if (!out || out.skipped) {
      a.lastN = prevN; // didn't run after all (busy, unloaded)
      return;
    }
    a.lastEnd = Date.now();
    a.lastDur = a.lastEnd - t0;
    if (out.error === 'gpu') a.gpuOff = true;
    else if (out.error) a.errors += 1;
    else if (out.ok) a.errors = 0;
    // Stopped (Stop, or preempted by a question) or failed: those words are still owed.
    if (out.stopped || out.error) a.lastN = prevN;
  }, []);

  // A new take: start over.
  useEffect(() => {
    if (A.current.take === take) return;
    A.current = { ...fresh(), take };
  }, [take]);

  // Loading a model (again) clears a GPU stop and the backoff.
  useEffect(() => {
    if (!llm.loadedModel) return;
    const a = A.current;
    a.gpuOff = false;
    a.errors = 0;
  }, [llm.loadedModel]);

  // A manual summary counts as a run for the gap.
  const last = llm.lastSummary;
  useEffect(() => {
    if (last && !last.auto) A.current.lastEnd = Math.max(A.current.lastEnd, Date.now());
  }, [last]);

  useEffect(() => {
    check();
  }, [check, wordsVersion, llm.state, llm.loadedModel]);

  useEffect(() => {
    const t = setInterval(check, TICK_MS);
    return () => clearInterval(t);
  }, [check]);

  const suspend = useCallback(() => {
    A.current.suspendUntil = Date.now() + STOP_SUSPEND_MS;
  }, []);

  return { suspend };
}
