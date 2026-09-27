// Known voices: remember who a speaker was across takes and files.
//
// When the user names a speaker, that speaker's voice print (the diarizer's centroid:
// the L2-normalised mean of its 256-dim WeSpeaker embeddings, diarizer.voices()) is
// saved under the name. In later takes, a diarized speaker whose centroid is close
// enough to a saved person's gets the name automatically, or as a suggestion.
//
// Stored in this browser's IndexedDB only (voice prints are biometric data: the page
// never sends them anywhere, and forgetting is one click). One record per person:
//   { id, name, prints: [{ session, speaker, centroid: Float32Array(256), weight, savedAt }], updatedAt }
// with at most one print per session (a take or file), the latest MAX_PRINTS kept.
// A person matches by their best print, so voices recorded in different rooms or
// through different mics each get a chance.
//
// If IndexedDB is unavailable (private mode, blocked storage) it falls back to memory:
// names are still recognised for the rest of the visit.

const DB_NAME = 'heimdallr-voices';
const STORE = 'voices';
const MAX_PRINTS = 8;

// Thresholds, measured with scripts/voice-match.mjs on pzm12 (far-field meeting, 5 min,
// cut into 2-5 pieces, each diarized as its own recording; "same person" = both piece
// speakers overlap the same speaker of a whole-file run most in time). Cosine between
// piece centroids (weight: see minWeight):
//   all                    same person median 0.76, p10 0.64, min 0.45 (n=52)
//                          different   median 0.20, p90 0.37, max 0.71 (n=196)
//   both ≥ 15 weight, each ≥ 80% one whole-file speaker
//                          same person median 0.78, p10 0.72, min 0.65 (n=36)
//                          different   median 0.17, p90 0.28, max 0.48 (n=114)
// The same-person lows are light speakers (7-11 weight); the different-person highs
// (0.5-0.7) are piece speakers that were a mix of voices (a diarization error), which
// the one-to-one assignment usually leaves for the purer speaker. Simulating the store
// (each piece against prints from the others, best print per person), every speaker
// ≥ 15 weight found the right person at ≥ 0.67; the runner-up was < 0.1 behind twice
// (0.69 vs 0.61-0.63, the runner-up a fragment of the same voice): suggested, not applied.
// Halves of one recording share the room and mic, so across real sessions same-person
// similarities will run lower: when unsure, suggest rather than apply.
export const MATCH = {
  autoSim: 0.65, // at least this (and clear of the runner-up): apply the name
  suggestSim: 0.45, // at least this: offer the name ("Alice?") for one-click accept
  margin: 0.1, // auto-apply only if the next-best person is this much less similar
  keepSim: 0.35, // a suggestion already shown stays until it drops below this (no flicker)
  // Speech behind a centroid before it's matched or saved, in diarizer weight
  // (≈ 2× talk time, so 15 ≈ 7 s of speech). Lighter centroids are noisy.
  minWeight: 15,
};

// ------------------------------------------------------------------------- storage
let dbPromise = null;
let memory = null; // Map id → person, once IndexedDB has failed

function openDb() {
  if (!dbPromise) {
    dbPromise = new Promise((resolve, reject) => {
      const req = indexedDB.open(DB_NAME, 1);
      req.onupgradeneeded = () => {
        if (!req.result.objectStoreNames.contains(STORE)) req.result.createObjectStore(STORE, { keyPath: 'id' });
      };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
      req.onblocked = () => reject(new Error('IndexedDB blocked'));
    });
  }
  return dbPromise;
}

// Run fn(people) → changed people (and ids to delete) inside one transaction, so
// read-modify-write can't interleave. fn gets and may mutate a Map id → person.
// Resolves with the full list afterwards.
async function update(fn) {
  let db = null;
  if (!memory) {
    try {
      db = await openDb();
    } catch (err) {
      console.warn('[voiceStore] IndexedDB unavailable, keeping voices for this visit only', err);
      memory = new Map();
    }
  }
  if (memory) {
    fn(memory);
    return sorted([...memory.values()]);
  }
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, 'readwrite');
    const store = tx.objectStore(STORE);
    let result = [];
    const req = store.getAll();
    req.onsuccess = () => {
      const people = new Map(req.result.map((p) => [p.id, p]));
      const { put = [], del = [] } = fn(people) || {};
      for (const id of del) store.delete(id);
      for (const p of put) store.put(p);
      result = [...people.values()];
    };
    tx.oncomplete = () => resolve(sorted(result));
    tx.onerror = () => reject(tx.error);
    tx.onabort = () => reject(tx.error);
  });
}

const sorted = (people) => people.sort((a, b) => a.name.localeCompare(b.name));
export const sameName = (a, b) => a.trim().toLowerCase() === b.trim().toLowerCase();
const newId = () => `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;

// All saved people, by name ([] if storage is unavailable).
export function listVoices() {
  return update(() => null).catch(() => []);
}

// Add a print to a person: by `id` (and nothing happens if they've been forgotten
// since), else by `name` (case-insensitive), creating them if new. A print from the
// same session replaces that session's earlier print when it's the same speaker or
// has more speech behind it. print: { session, speaker, centroid, weight }.
// Resolves with the full list and the person's id (null if nothing was saved).
export async function addPrint({ id, name }, print) {
  let savedId = null;
  const list = await update((people) => {
    let p = id ? people.get(id) : null;
    if (!p && !id && name && name.trim()) {
      p = [...people.values()].find((x) => sameName(x.name, name));
      if (!p) {
        p = { id: newId(), name: name.trim(), prints: [], updatedAt: 0 };
        people.set(p.id, p);
      }
    }
    if (!p) return null;
    const entry = {
      session: print.session,
      speaker: print.speaker,
      centroid: Float32Array.from(print.centroid),
      weight: print.weight,
      savedAt: Date.now(),
    };
    const i = p.prints.findIndex((x) => x.session === print.session);
    if (i < 0) p.prints.push(entry);
    else if (p.prints[i].speaker === print.speaker || p.prints[i].weight <= print.weight) p.prints[i] = entry;
    else return null;
    p.prints = p.prints.sort((a, b) => a.savedAt - b.savedAt).slice(-MAX_PRINTS);
    p.updatedAt = Date.now();
    savedId = p.id;
    return { put: [p] };
  }).catch(() => null);
  return { list, id: savedId };
}

// Take back the print a session saved for a person (the user renamed that speaker to
// someone else: it wasn't them). A person left with no prints is removed: they were
// only ever that one mistake (typically a typo'd name). Other sessions' prints stay.
export function removePrint(id, session) {
  return update((people) => {
    const p = people.get(id);
    if (!p || !p.prints.some((x) => x.session === session)) return null;
    p.prints = p.prints.filter((x) => x.session !== session);
    if (p.prints.length) return { put: [p] };
    people.delete(id);
    return { del: [id] };
  }).catch(() => null);
}

// Rename a person. Renaming onto another saved name merges the two (prints pooled,
// latest MAX_PRINTS kept).
export function renameVoice(id, name) {
  return update((people) => {
    const p = people.get(id);
    const to = name.trim();
    if (!p || !to) return null;
    const other = [...people.values()].find((x) => x.id !== id && sameName(x.name, to));
    if (!other) {
      p.name = to;
      p.updatedAt = Date.now();
      return { put: [p] };
    }
    other.prints = [...other.prints, ...p.prints].sort((a, b) => a.savedAt - b.savedAt).slice(-MAX_PRINTS);
    other.updatedAt = Date.now();
    people.delete(id);
    return { put: [other], del: [id] };
  }).catch(() => null);
}

export function forgetVoice(id) {
  return update((people) => {
    people.delete(id);
    return { del: [id] };
  }).catch(() => null);
}

export function forgetAllVoices() {
  return update((people) => {
    const del = [...people.keys()];
    people.clear();
    return { del };
  }).catch(() => null);
}

// ------------------------------------------------------------------------- matching
function dot(a, b) {
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a[i] * b[i];
  return s;
}

// How much a centroid sounds like a person: the best of their prints, leaving out
// those saved from `session` (the take being matched: its prints are its own speakers).
export const personSim = (centroid, person, session) =>
  Math.max(-1, ...person.prints.filter((p) => p.session !== session).map((p) => dot(centroid, p.centroid)));

// This take's matches, updated for new centroids. Sticky, so names don't flicker as
// live centroids move:
//   - an auto-applied name stays for the rest of the take (while its speaker exists,
//     the person isn't forgotten, and the similarity stays above suggestSim: it only
//     drops that far when a recluster gave the diarizer's name to another voice);
//   - a suggestion stays while it's above keepSim, unless it becomes an auto match or
//     that person is auto-matched to another speaker;
//   - otherwise, the best remaining pairs (greedy by similarity, one-to-one: one
//     person is never given to two speakers of the same take).
// prev:      { [speaker]: { id, sim, auto } } (speaker = diarizer name)
// speakers:  diarizer.voices() — [{ speaker, centroid, weight }]
// people:    listVoices()
// named:     Set of speakers (as strings) the user named (never matched: manual names win)
// taken:     Set of person ids whose name the user gave a speaker in this take
// dismissed: { [speaker]: Set of person ids } matches the user overrode
// session:   this take's id (its own prints don't count, so clearing a name the user
//            just gave doesn't bring it straight back as a match)
// → the next { [speaker]: { id, sim, auto } } (prev itself if nothing changed)
export function nextMatches(prev, speakers, people, { named, taken, dismissed, session }, opt = MATCH) {
  const byId = new Map(people.map((p) => [p.id, p]));
  const present = new Map(speakers.map((s) => [String(s.speaker), s])); // object keys are strings
  const next = {};
  const used = new Set(taken);
  const isDismissed = (sp, id) => !!(dismissed[sp] && dismissed[sp].has(id));
  const valid = (sp, m) =>
    present.has(sp) && byId.has(m.id) && !named.has(sp) && !used.has(m.id) && !isDismissed(sp, m.id);

  // 1. Auto matches already made, while the voice still fits (a recluster can hand a
  // diarizer name to a different voice early in a take).
  for (const [sp, m] of Object.entries(prev)) {
    if (!m.auto || !valid(sp, m)) continue;
    const sim = personSim(present.get(sp).centroid, byId.get(m.id), session);
    if (sim < opt.suggestSim) continue;
    next[sp] = { ...m, sim };
    used.add(m.id);
  }

  // 2. Fresh pairs for everyone else with enough speech.
  const pairs = [];
  for (const s of speakers) {
    const sp = String(s.speaker);
    if (next[sp] || named.has(sp) || s.weight < opt.minWeight) continue;
    const sims = people
      .filter((p) => !isDismissed(sp, p.id))
      .map((p) => ({ id: p.id, sim: personSim(s.centroid, p, session) }));
    for (const { id, sim } of sims) {
      if (used.has(id) || sim < opt.suggestSim) continue;
      const runnerUp = Math.max(-1, ...sims.filter((x) => x.id !== id).map((x) => x.sim));
      pairs.push({ speaker: sp, id, sim, auto: sim >= opt.autoSim && sim - runnerUp >= opt.margin });
    }
  }
  pairs.sort((a, b) => b.sim - a.sim);
  const fresh = {};
  const freshUsed = new Set();
  for (const p of pairs) {
    if (fresh[p.speaker] || freshUsed.has(p.id)) continue;
    fresh[p.speaker] = p;
    freshUsed.add(p.id);
  }

  // 3. New auto matches win; then suggestions already shown; then new suggestions.
  for (const [sp, p] of Object.entries(fresh)) {
    if (p.auto && !used.has(p.id)) {
      next[sp] = { id: p.id, sim: p.sim, auto: true };
      used.add(p.id);
    }
  }
  for (const [sp, m] of Object.entries(prev)) {
    if (m.auto || next[sp] || !valid(sp, m)) continue;
    const sim = personSim(present.get(sp).centroid, byId.get(m.id), session);
    if (sim < opt.keepSim) continue;
    next[sp] = { id: m.id, sim, auto: false };
    used.add(m.id);
  }
  for (const [sp, p] of Object.entries(fresh)) {
    if (next[sp] || used.has(p.id)) continue;
    next[sp] = { id: p.id, sim: p.sim, auto: false };
    used.add(p.id);
  }

  const same = (a, b) => a && b && a.id === b.id && a.auto === b.auto && Math.abs(a.sim - b.sim) < 0.01;
  const keys = Object.keys(next);
  const unchanged = keys.length === Object.keys(prev).length && keys.every((k) => same(next[k], prev[k]));
  return unchanged ? prev : next;
}
