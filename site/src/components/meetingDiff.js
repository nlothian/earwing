// The live summary's history, for the device's Follow feed. Each summary the
// assistant writes is a complete rewrite, so to show "what changed" this matches
// every item against the previous summary's, and builds the payload for POST /meeting
// (meetingContract.js). Pure functions, for Node tests; meetingSync.js runs them.
//
// Matching uses Dice similarity on token sets (lowercased, stopwords out, crude
// stemming), +0.1 when the citation times agree:
// - Within a section, greedily, best pair first: ≥ SAME is the same item reworded
//   (no feed entry); ≥ EDITED with at least 2 words different is an edit (feed); a
//   smaller difference counts as the same item. Unmatched is new (feed).
// - An item that left one section and turns up in another at ≥ SAME moved: no entry.
// - A dropped item is kept as a tombstone for TOMB_REVS summaries; one that comes back
//   (≥ REVIVE against a tombstone) isn't news, so an item the model drops and re-adds
//   doesn't flap into the feed.
// - The summary paragraph is fed when it reads differently (< SUMMARY_DIFF against the
//   last one fed) and SUMMARY_MIN_MS have passed; the feed holds one summary entry.
// - Removed items are never fed. The feed has one entry per item (an edit moves it to
//   the top), newest first, FEED_MAX long; lvl is how many change sets old it is.
//
// Items keep an id across summaries (matched, moved and revived items inherit it), so
// feed entries follow their item.

import {
  FEED_MAX,
  FEED_TEXT_MAX,
  ITEMS_MAX,
  LVL_MAX,
  MAX_BODY,
  MEETING_VERSION,
  SECTION_KEYS,
  SESSION_MAX,
  SUMMARY_MAX,
  T_MAX,
  TEXT_MAX,
} from './meetingContract.js';

export const SAME = 0.85;
export const EDITED = 0.4;
export const REVIVE = 0.7;
export const TOMB_REVS = 5;
export const SUMMARY_DIFF = 0.7;
export const SUMMARY_MIN_MS = 90000;
// A rev's changes go into the feed in this order (the first summary seeds it so).
const FEED_ORDER = ['decisions', 'actions', 'questions', 'key'];

// ---------------------------------------------------------------------------------
// Similarity

const STOP = new Set(
  (
    'a an the and or but of to in on at for by with from as is are was were be been being it its ' +
    'this that these those will would should shall can could may might must has have had do does did ' +
    'we they he she i you our their his her them us about into so if then than also there which ' +
    'who what when up out all any some just very more most per via'
  ).split(' '),
);

const stem = (w) => {
  let s = w;
  if (s.length > 4 && s.endsWith('ies')) s = `${s.slice(0, -3)}y`;
  else if (s.length > 5 && s.endsWith('ing')) s = s.slice(0, -3);
  else if (s.length > 4 && s.endsWith('ed')) s = s.slice(0, -2);
  else if (s.length > 3 && s.endsWith('s') && !s.endsWith('ss')) s = s.slice(0, -1);
  if (s.length > 3 && s.endsWith('e')) s = s.slice(0, -1);
  return s;
};

export function tokens(text) {
  const out = new Set();
  for (const w of String(text || '').toLowerCase().split(/[^a-z0-9]+/)) {
    if (w && !STOP.has(w)) out.add(stem(w));
  }
  return out;
}

export function dice(a, b) {
  if (!a.size && !b.size) return 1;
  let n = 0;
  for (const w of a) if (b.has(w)) n++;
  return (2 * n) / (a.size + b.size);
}

// Words in one set and not the other (both ways).
function wordsDifferent(a, b) {
  let n = 0;
  for (const w of a) if (!b.has(w)) n++;
  for (const w of b) if (!a.has(w)) n++;
  return n;
}

export function similarity(x, y) {
  const s = dice(x.toks, y.toks) + (x.t && x.t === y.t ? 0.1 : 0);
  return Math.min(1, s);
}

// ---------------------------------------------------------------------------------
// The diff

// state: null (a new session) or what the last call returned:
//   { rev, changeRev /*latest rev that fed anything*/, nextId, summary, coversTo,
//     items: { [sec]: [{ id, text, t, toks, rev /*when last new or edited*/ }] },
//     feed: [{ id, sec, kind, text, t, rev, ms }],
//     tombs: [{ id, sec, text, t, toks, itemRev, rev /*when dropped*/ }],
//     fed: { text, toks, ms } | null /*the last summary fed*/, changes /*this rev*/ }
// parsed: parseSummary(). opts: { rev, nowMs, coversTo /*"mm:ss"*/ }.
export function diffSummary(state, parsed, { rev, nowMs, coversTo = '' }) {
  const prev = state || {
    changeRev: 0,
    nextId: 1,
    items: Object.fromEntries(SECTION_KEYS.map((k) => [k, []])),
    feed: [],
    tombs: [],
    fed: null,
  };
  let nextId = prev.nextId;
  const fresh = (sec) => (parsed.sections[sec] || []).map((it) => ({ ...it, toks: tokens(it.text), id: 0 }));
  const cur = Object.fromEntries(SECTION_KEYS.map((k) => [k, fresh(k)]));
  const kind = new Map(); // item → 'new' | 'edited' (fed); matched/moved/revived: absent
  const leftover = []; // previous items not matched in their own section

  // Within each section: greedy, best pair first.
  for (const sec of SECTION_KEYS) {
    const olds = prev.items[sec];
    const news = cur[sec];
    const pairs = [];
    for (let i = 0; i < olds.length; i++) {
      for (let j = 0; j < news.length; j++) {
        const s = similarity(olds[i], news[j]);
        if (s >= EDITED) pairs.push([s, i, j]);
      }
    }
    pairs.sort((a, b) => b[0] - a[0] || a[1] - b[1] || a[2] - b[2]);
    const usedOld = new Set();
    for (const [s, i, j] of pairs) {
      if (usedOld.has(i) || news[j].id) continue;
      usedOld.add(i);
      news[j].id = olds[i].id;
      news[j].rev = olds[i].rev;
      if (s < SAME && wordsDifferent(olds[i].toks, news[j].toks) >= 2) kind.set(news[j], 'edited');
    }
    olds.forEach((o, i) => usedOld.has(i) || leftover.push({ ...o, sec }));
  }

  // Unmatched: moved from another section, back from the dead, or new.
  const tombs = prev.tombs.filter((t) => rev - t.rev <= TOMB_REVS).map((t) => ({ ...t }));
  for (const sec of SECTION_KEYS) {
    for (const it of cur[sec]) {
      if (it.id) continue;
      let best = null;
      let bestS = SAME;
      for (const o of leftover) {
        if (o.sec === sec || o.taken) continue;
        const s = similarity(o, it);
        if (s >= bestS) [best, bestS] = [o, s];
      }
      if (!best) {
        bestS = REVIVE;
        for (const o of tombs) {
          if (o.taken) continue;
          const s = similarity(o, it);
          if (s >= bestS) [best, bestS] = [o, s];
        }
      }
      if (best) {
        best.taken = true;
        it.id = best.id;
        it.rev = best.itemRev ?? best.rev; // a tombstone's rev is when it was dropped
      } else {
        it.id = nextId++;
        kind.set(it, 'new');
      }
    }
  }
  const nextTombs = tombs
    .filter((t) => !t.taken)
    .concat(
      leftover
        .filter((o) => !o.taken)
        .map(({ id, sec, text, t, toks, rev: itemRev }) => ({ id, sec, text, t, toks, itemRev, rev })),
    )
    .map(({ taken, ...t }) => t);

  // This rev's feed entries, in FEED_ORDER, then the summary paragraph.
  const changes = [];
  for (const sec of FEED_ORDER) {
    for (const it of cur[sec]) {
      const k = kind.get(it);
      if (!k) continue;
      it.rev = rev;
      changes.push({ id: it.id, sec, kind: k, text: it.text, t: it.t, rev, ms: nowMs });
    }
  }
  const summary = parsed.summary || '';
  const sumToks = tokens(summary);
  let fed = prev.fed;
  if (summary && (!fed || (dice(sumToks, fed.toks) < SUMMARY_DIFF && nowMs - fed.ms >= SUMMARY_MIN_MS))) {
    fed = { text: summary, toks: sumToks, ms: nowMs };
    changes.push({ id: 'summary', sec: 'summary', kind: 'summary', text: summary, t: coversTo, rev, ms: nowMs });
  }

  // Older entries follow their item's current wording and section (a move or a
  // small rewording shows up in place, without moving the entry).
  const where = new Map();
  for (const sec of SECTION_KEYS) for (const it of cur[sec]) where.set(it.id, { sec, it });
  const changed = new Set(changes.map((c) => c.id));
  const older = prev.feed
    .filter((e) => !changed.has(e.id))
    .map((e) => {
      const w = where.get(e.id);
      return w && e.kind !== 'summary' ? { ...e, sec: w.sec, text: w.it.text, t: w.it.t } : e;
    });
  let feed = changes.concat(older);
  if (feed.length > FEED_MAX) {
    const s = feed.findIndex((e) => e.kind === 'summary');
    const keepSummary = s >= FEED_MAX && changed.has('summary');
    feed = feed.slice(0, keepSummary ? FEED_MAX - 1 : FEED_MAX);
    if (keepSummary) feed.push(changes[changes.length - 1]);
  }

  const items = Object.fromEntries(
    SECTION_KEYS.map((k) => [k, cur[k].map(({ id, text, t, toks, rev: r }) => ({ id, text, t, toks, rev: r ?? rev }))]),
  );
  return {
    rev,
    changeRev: changes.length ? rev : prev.changeRev,
    nextId,
    summary,
    coversTo,
    items,
    feed,
    tombs: nextTombs,
    fed,
    changes: changes.length,
  };
}

// ---------------------------------------------------------------------------------
// The payload

// Cut to n characters at a word boundary where there is one, with "...".
export function clip(text, n) {
  const s = String(text || '');
  if (s.length <= n) return s;
  const cut = s.slice(0, n - 3);
  const sp = cut.lastIndexOf(' ');
  return `${(sp >= cut.length / 2 ? cut.slice(0, sp) : cut).replace(/[\s.,;:]+$/, '')}...`;
}

const clipT = (t) => String(t || '').slice(0, T_MAX);

// → the MeetingPayload (meetingContract.js) for state, within every cap and under
// MAX_BODY bytes (dropping the oldest feed entries, then the last items of the
// longest lists, if the text alone would go over).
export function buildPayload(state, { session, seq, hb, nowMs }) {
  const lvl = (e) => Math.max(0, Math.min(LVL_MAX, state.changeRev - e.rev));
  const payload = {
    v: MEETING_VERSION,
    session: String(session).replace(/[^A-Za-z0-9-]/g, '').slice(0, SESSION_MAX),
    rev: state.rev,
    seq,
    hb: !!hb,
    coversTo: clipT(state.coversTo),
    summary: clip(state.summary, SUMMARY_MAX),
    sections: Object.fromEntries(
      SECTION_KEYS.map((k) => [
        k,
        state.items[k]
          .slice(0, ITEMS_MAX)
          .map((it) => ({ text: clip(it.text, TEXT_MAX), t: clipT(it.t), new: it.rev === state.changeRev })),
      ]),
    ),
    feed: state.feed.slice(0, FEED_MAX).map((e) => ({
      sec: e.sec,
      kind: e.kind,
      text: clip(e.text, FEED_TEXT_MAX),
      t: clipT(e.t),
      lvl: lvl(e),
      ago: Math.max(0, Math.round((nowMs - e.ms) / 1000)),
    })),
  };
  const size = () => new TextEncoder().encode(JSON.stringify(payload)).length;
  while (size() > MAX_BODY) {
    const lists = SECTION_KEYS.map((k) => payload.sections[k]);
    const longest = lists.reduce((a, b) => (b.length > a.length ? b : a));
    if (payload.feed.length > 4) payload.feed.pop();
    else if (longest.length) longest.pop();
    else if (payload.feed.length) payload.feed.pop();
    else break;
  }
  return payload;
}
