// useMeetingSync(): sends the live summary to the device's Meeting tab. Each new
// summary is parsed (meetingSummary.js), diffed against the last one into the Follow
// feed (meetingDiff.js) and POSTed to /meeting as JSON (meetingContract.js); every
// HEARTBEAT_MS the current state goes again with fresh "ago"s, so the display knows
// this tab is alive and a rebooted device is refilled.
//
// - One request at a time; whatever is wanted meanwhile is sent after it, built from
//   the newest state (the latest payload wins). POST_TIMEOUT_MS per request.
// - hb is true once the device has accepted this rev: a heartbeat from a second tab
//   is then ignored by the device (its accept policy, code/meeting_types.h) instead of
//   flipping the display, while a rev it hasn't taken yet goes as a real post.
// - 404 / 405: firmware without the Meeting tab. Sync stays off for this page.
// - The session is "<random per page load>-<take>": a new take starts a new one (the
//   device resets its feed), and so does reloading the page.

import { useCallback, useEffect, useRef, useState } from 'react';
import { HEARTBEAT_MS, MEETING_PATH, POST_TIMEOUT_MS } from './meetingContract.js';
import { buildPayload, diffSummary } from './meetingDiff.js';
import { parseSummary } from './meetingSummary.js';
import { fmtTs } from './llmPrompt.js';

const PAGE_ID = (() => {
  const abc = 'abcdefghijklmnopqrstuvwxyz0123456789';
  let s = '';
  const bytes = new Uint8Array(6);
  try {
    crypto.getRandomValues(bytes);
  } catch {
    for (let i = 0; i < 6; i++) bytes[i] = Math.floor(Math.random() * 256);
  }
  for (const b of bytes) s += abc[b % abc.length];
  return s;
})();
let seq = 0; // posts sent by this page (heartbeats too)

const EMPTY = { rev: 0, syncedAt: 0, result: null, error: null, off: false };

// lastSummary: llmClient's { id, text, coversTo /*s*/, take } or null.
// → { rev, syncedAt /*ms, last accepted post*/, result /*device's last answer*/, error, off }
export function useMeetingSync(lastSummary, take) {
  const [status, setStatus] = useState(EMPTY);
  const R = useRef(null);
  if (!R.current) {
    R.current = {
      take,
      session: `${PAGE_ID}-${take}`,
      diff: null, // meetingDiff state for this session
      delivered: 0, // highest rev the device accepted in this session
      lastId: null, // the lastSummary already taken in
      want: false, // a post is owed
      inflight: false,
      off: false,
      alive: true,
    };
  }

  const pump = useCallback(async () => {
    const r = R.current;
    if (r.inflight || !r.want || r.off || !r.diff || !r.alive) return;
    r.want = false;
    r.inflight = true;
    const { session, diff } = r;
    const payload = buildPayload(diff, { session, seq: ++seq, hb: r.delivered >= diff.rev, nowMs: Date.now() });
    const ac = new AbortController();
    const timer = setTimeout(() => ac.abort(), POST_TIMEOUT_MS);
    let patch;
    try {
      const res = await fetch(MEETING_PATH, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
        cache: 'no-store',
        signal: ac.signal,
      });
      if (res.status === 404 || res.status === 405) {
        r.off = true;
        patch = { off: true, error: null };
      } else if (!res.ok) {
        patch = { error: `the display answered ${res.status}` };
      } else {
        const j = await res.json().catch(() => ({}));
        const result = j && typeof j.result === 'string' ? j.result : 'replaced';
        if (result !== 'ignored' && session === r.session) r.delivered = Math.max(r.delivered, payload.rev);
        patch = { result, error: null, ...(result !== 'ignored' ? { syncedAt: Date.now() } : {}) };
      }
    } catch {
      patch = { error: ac.signal.aborted ? 'the display didn’t answer' : 'the display isn’t reachable' };
    } finally {
      clearTimeout(timer);
      r.inflight = false;
    }
    if (!r.alive) return;
    if (session === r.session) setStatus((s) => ({ ...s, ...patch }));
    pump();
  }, []);

  // A new take: a new session. A new summary for this take: diff it, post it.
  useEffect(() => {
    const r = R.current;
    if (take !== r.take) {
      r.take = take;
      r.session = `${PAGE_ID}-${take}`;
      r.diff = null;
      r.delivered = 0;
      r.want = false;
      setStatus((s) => ({ ...EMPTY, off: s.off }));
    }
    if (!lastSummary || lastSummary.id === r.lastId || lastSummary.take !== take) return;
    r.lastId = lastSummary.id;
    const parsed = parseSummary(lastSummary.text);
    const empty = !parsed.summary && Object.values(parsed.sections).every((l) => !l.length);
    if (empty) return; // not a summary (garbled, or "The transcript doesn't say.")
    const rev = (r.diff ? r.diff.rev : 0) + 1;
    r.diff = diffSummary(r.diff, parsed, { rev, nowMs: Date.now(), coversTo: fmtTs(lastSummary.coversTo) });
    r.want = true;
    setStatus((s) => ({ ...s, rev }));
    pump();
  }, [lastSummary, take, pump]);

  useEffect(() => {
    const r = R.current;
    r.alive = true;
    const t = setInterval(() => {
      if (!r.diff || r.off) return;
      r.want = true;
      pump();
    }, HEARTBEAT_MS);
    return () => {
      r.alive = false;
      clearInterval(t);
    };
  }, [pump]);

  return status;
}
