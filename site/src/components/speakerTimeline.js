// Line up the transcription worker's timed words with the diarization worker's
// speaker segments, and group them into turns for display.
//
// Segments are painted onto a 50 ms grid, longest last, so in overlapping speech the
// speaker holding the floor wins (the ASR transcribes the dominant voice; a
// backchannel "yeah" under it is the short segment). A word gets the speaker covering
// most of its span, or, if the diarizer heard nobody there, the nearest one within
// 1 s. Words the diarizer hasn't reached yet (the newest few seconds) stay unlabelled
// and join the turn before them.

const BIN = 0.05; // s
const REACH = Math.round(1 / BIN);

// segments: [{ start, end, speaker }] → { bins: Int32Array (speaker name or -1), order }
// order: speaker name → display number (1, 2, …) by first appearance.
export function buildTimeline(segments) {
  const end = segments.reduce((m, s) => Math.max(m, s.end), 0);
  const bins = new Int32Array(Math.ceil(end / BIN) + 1).fill(-1);
  const bySize = [...segments].sort((a, b) => b.end - b.start - (a.end - a.start));
  for (const s of bySize) {
    for (let i = Math.floor(s.start / BIN); i < Math.ceil(s.end / BIN); i++) bins[i] = s.speaker;
  }
  const order = new Map();
  for (const s of [...segments].sort((a, b) => a.start - b.start)) {
    if (!order.has(s.speaker)) order.set(s.speaker, order.size + 1);
  }
  return { bins, order };
}

function speakerFor(tl, t0, t1) {
  const { bins } = tl;
  const a = Math.max(0, Math.floor(t0 / BIN));
  const b = Math.min(bins.length - 1, Math.max(a, Math.ceil(t1 / BIN) - 1));
  if (a >= bins.length) return null;
  const votes = new Map();
  for (let i = a; i <= b; i++) if (bins[i] >= 0) votes.set(bins[i], (votes.get(bins[i]) || 0) + 1);
  let best = null;
  for (const [k, v] of votes) if (best === null || v > votes.get(best)) best = k;
  if (best !== null) return best;
  for (let d = 1; d <= REACH; d++) {
    if (a - d >= 0 && bins[a - d] >= 0) return bins[a - d];
    if (b + d < bins.length && bins[b + d] >= 0) return bins[b + d];
  }
  return null;
}

// words: [{ text, start_time, end_time, pending? }]
//   → [{ speaker: display number | null, who: diarizer speaker name | null, words }]
export function toTurns(words, tl) {
  const turns = [];
  for (const w of words) {
    const who = tl ? speakerFor(tl, w.start_time, w.end_time) : null;
    const speaker = who === null ? null : tl.order.get(who);
    const last = turns[turns.length - 1];
    if (last && (speaker === null || speaker === last.speaker)) last.words.push(w);
    else if (last && last.speaker === null && turns.length === 1) {
      last.speaker = speaker; // leading unlabelled words belong to the first speaker
      last.who = who;
      last.words.push(w);
    } else turns.push({ speaker, who, words: [w] });
  }
  return turns;
}

export const turnText = (t) =>
  t.words
    .map((w) => w.text)
    .join(' ')
    .trim();

// A turn's speaker's display name: its name (given by the user, or a recognised
// voice), or "Speaker N". names: { [diarizer speaker name]: string }. Names are keyed
// by the diarizer's name, not the display number, because display numbers follow
// first appearance and can shift when a recluster merges or splits speakers.
export const speakerName = (turn, names) => (names && names[turn.who]) || `Speaker ${turn.speaker}`;

// "Speaker 1: …" (or "Alice: …") paragraphs, for Copy.
export const turnsToText = (turns, names) =>
  turns.map((t) => (t.speaker ? `${speakerName(t, names)}: ${turnText(t)}` : turnText(t))).join('\n\n');
