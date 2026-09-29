// Prompts for the in-browser meeting assistant (llmClient.js), and the transcript
// lines they carry. Pure functions: no browser or worker APIs, so
// scripts/test-llm-prompt.mjs runs them in Node.
//
// The transcript goes to the model as timestamped lines, one per speaker turn
// ("[04:12] Alice: …"), or per paragraph when speakers are off ("[04:12] …"). Long
// turns are split at their largest pause so every line's time stays close to what it
// says, which is what citations point at.
//
// The worker can only reuse its KV/conv cache when the new prompt starts with exactly
// the tokens it has already seen (LFM2's conv state can't be rolled back). So the
// conversation only ever grows: the transcript is the first user message, and later
// transcript and each request are appended as new user messages. `planAppend` checks
// that every line already sent is still what the transcript says now (same words,
// same speaker name); if any changed (a word rewritten, a relabel after a recluster,
// a rename) the whole conversation is rebuilt from the current transcript.
//
// Beyond SUMMARY_VERBATIM_MAX tokens of transcript, summaries are map-reduced: notes
// per ~1k-token chunk (cached by the caller), then one merge. Beyond VERBATIM_MAX,
// questions get notes on the older ~6k-token chunks plus as many of the newest lines,
// verbatim, as fit. Summaries switch much earlier because a small model summarising one
// long prompt covers the start and stops (both models, Sep 2026: a 4.3k-token hour of
// TV summarised only to minute 16–22); a question only has to find its answer.

import { speakerName, toTurns } from './speakerTimeline.js';

export const VERBATIM_MAX = 16000; // tokens of transcript sent verbatim (questions)
export const SUMMARY_VERBATIM_MAX = 1000; // the same, for summaries (one summary chunk)
export const HISTORY_MAX = 3000; // tokens of Q&A kept in context after the transcript
export const CHUNK_TOKENS = 6000; // chunk size for questions' notes
export const SUMMARY_CHUNK_TOKENS = 1000; // chunk size for map-reduce summaries
export const MAX_TOKENS = { summary: 900, question: 400, notes: 350 };
export const SETTLE_SEC = 12; // live, with speakers: labels this close to the newest word may still change
const TURN_MAX_SEC = 45;
const TURN_MAX_WORDS = 120;
const PARA_GAP_SEC = 1;
const PARA_MIN_SEC = 15;
const PARA_MAX_SEC = 45;

// ---------------------------------------------------------------------------------
// Times and sizes

// 252 → "04:12"; 3852 → "1:04:12" (no brackets).
export function fmtTs(sec) {
  const s = Math.max(0, Math.floor(Number(sec) || 0));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const ss = String(s % 60).padStart(2, '0');
  return h ? `${h}:${String(m).padStart(2, '0')}:${ss}` : `${String(m).padStart(2, '0')}:${ss}`;
}

export const tsTag = (sec) => `[${fmtTs(sec)}]`;

// Rough token count for display and chunking (LFM2's tokenizer averages ~3.5–4 chars
// per token on English). Exact numbers come from the worker's `count`.
export const estTokens = (text) => Math.ceil(String(text || '').length / 3.5);

// cyrb53: a fast 53-bit string hash (for the notes cache key).
export function hashText(str) {
  let h1 = 0xdeadbeef;
  let h2 = 0x41c6ce57;
  for (let i = 0; i < str.length; i++) {
    const ch = str.charCodeAt(i);
    h1 = Math.imul(h1 ^ ch, 2654435761);
    h2 = Math.imul(h2 ^ ch, 1597334677);
  }
  h1 = Math.imul(h1 ^ (h1 >>> 16), 2246822507) ^ Math.imul(h2 ^ (h2 >>> 13), 3266489909);
  h2 = Math.imul(h2 ^ (h2 >>> 16), 2246822507) ^ Math.imul(h1 ^ (h1 >>> 13), 3266489909);
  return (4294967296 * (2097151 & h2) + (h1 >>> 0)).toString(36);
}

// ---------------------------------------------------------------------------------
// Transcript lines

const joinText = (words, a, b) => {
  let s = '';
  for (let i = a; i < b; i++) {
    const t = String(words[i].text || '').trim();
    if (t) s = s ? `${s} ${t}` : t;
  }
  return s;
};

// How many leading words to use: up to the first `pending` (not yet final) word, and
// only those ending by settledUntil.
export function settledCount(words, settledUntil = Infinity) {
  let n = 0;
  while (n < words.length && !words[n].pending && !(words[n].end_time > settledUntil)) n++;
  return n;
}

// The `settledUntil` a request should use: live with speakers, drop the newest
// SETTLE_SEC (their labels are provisional); final, or without speakers, use all.
export function settledUntilFor(words, timeline, final) {
  if (final || !timeline || !words.length) return Infinity;
  return words[words.length - 1].end_time - SETTLE_SEC;
}

// Speaker name for each of the first n words (null without a timeline, or where the
// diarizer hasn't labelled anyone yet). Turns are computed over words[0..n), which
// labels a prefix exactly as it would label it inside the longer list.
export function wordLabels(words, timeline, labels, n = words.length) {
  const out = new Array(n).fill(null);
  if (!timeline || !n) return out;
  const idx = [];
  for (let i = 0; i < n; i++) idx.push({ start_time: words[i].start_time, end_time: words[i].end_time, i });
  for (const t of toTurns(idx, timeline)) {
    const name = t.speaker ? speakerName(t, labels) : null;
    for (const w of t.words) out[w.i] = name;
  }
  return out;
}

// Split words[a..b) into pieces no longer than TURN_MAX_SEC / TURN_MAX_WORDS, each
// time at the largest gap between words (never leaving a piece of < 1/4 the words).
function splitRange(words, a, b, out) {
  const n = b - a;
  const dur = words[b - 1].end_time - words[a].start_time;
  if (n < 2 || (dur <= TURN_MAX_SEC && n <= TURN_MAX_WORDS)) {
    out.push([a, b]);
    return;
  }
  const edge = Math.max(1, Math.floor(n / 4));
  let best = a + Math.floor(n / 2);
  let bestGap = -Infinity;
  for (let k = a + edge; k <= b - edge; k++) {
    const gap = words[k].start_time - words[k - 1].end_time;
    if (gap > bestGap) {
      bestGap = gap;
      best = k;
    }
  }
  splitRange(words, a, best, out);
  splitRange(words, best, b, out);
}

// Paragraphs for a transcript without speakers: break at a pause ≥ PARA_GAP_SEC once
// the paragraph is PARA_MIN_SEC long, and always by PARA_MAX_SEC.
function paragraphs(words, a, b) {
  const out = [];
  let p = a;
  for (let i = a + 1; i < b; i++) {
    const gap = words[i].start_time - words[i - 1].end_time;
    const len = words[i].start_time - words[p].start_time;
    if ((gap >= PARA_GAP_SEC && len >= PARA_MIN_SEC) || len >= PARA_MAX_SEC || i - p >= TURN_MAX_WORDS) {
      out.push([p, i]);
      p = i;
    }
  }
  if (b > p) out.push([p, b]);
  return out;
}

const lineOf = (words, a, b, label) => {
  const text = joinText(words, a, b);
  const start = words[a].start_time;
  const end = words[b - 1].end_time;
  return { wordFrom: a, wordTo: b, label, text, start, end, line: `${tsTag(start)} ${label ? `${label}: ` : ''}${text}` };
};

// words: mature timed words [{ text, start_time, end_time }]; timeline: buildTimeline()
// or null; labels: { [diarizer speaker name]: display name }.
// opts.settledUntil: only words ending by then; opts.from: first word index to emit
// lines for (turns are still worked out from word 0, so a word's speaker is the same
// as in a full build); opts.to: stop before this word index.
// → [{ wordFrom, wordTo /*exclusive*/, label /*string|null*/, text, start, end, line }]
export function transcriptLines(words, timeline, labels, opts = {}) {
  const { settledUntil = Infinity, from = 0 } = opts;
  if (!words || !words.length) return [];
  const to = Math.min(opts.to ?? words.length, settledCount(words, settledUntil));
  if (to <= from) return [];
  const lines = [];
  if (!timeline) {
    for (const [a, b] of paragraphs(words, from, to)) {
      const l = lineOf(words, a, b, null);
      if (l.text) lines.push(l);
    }
    return lines;
  }
  const names = wordLabels(words, timeline, labels, to);
  let a = from;
  for (let i = from + 1; i <= to; i++) {
    if (i < to && names[i] === names[a]) continue;
    const pieces = [];
    splitRange(words, a, i, pieces);
    for (const [p, q] of pieces) {
      const l = lineOf(words, p, q, names[a]);
      if (l.text) lines.push(l);
    }
    a = i;
  }
  return lines;
}

export const linesText = (lines) => lines.map((l) => l.line).join('\n');

// ---------------------------------------------------------------------------------
// Prompts

export const SYSTEM_PROMPT =
  "You are earwing's meeting assistant. You are given a transcript made by automatic speech " +
  'recognition, so expect misheard words; speaker names may be generic ("Speaker 2") or wrong. ' +
  'Each line starts with a [mm:ss] time from the start of the recording, then the speaker. ' +
  'Rules: use only the transcript, never outside knowledge. Cite the time of every line you ' +
  'rely on in square brackets, e.g. [04:12]. If the transcript doesn\'t contain the answer, say ' +
  '"The transcript doesn\'t say." Be concise and answer in the language of the question.';

// Measured on both models (Sep 2026): a one-line format spec made LFM2.5-1.2B copy the
// transcript back and fill in the spec literally ("Owner: task due by …"). A spelled-out
// layout, "don't repeat the transcript", and starting the answer with SUMMARY_PREFIX fix
// that; the 2.6B was fine either way.
const SUMMARY_FORMAT = `Use exactly this Markdown layout:

## Summary
2–4 sentences.

## Key points
- one point per bullet, ending with the [mm:ss] time it comes from

## Decisions
- each decision with its [mm:ss] time, or "None recorded."

## Action items
- Name: what they will do (when, if said) [mm:ss], or "None recorded."

## Open questions
- only if any were raised`;

export const SUMMARY_REQUEST =
  `Write a summary of the transcript. Do not repeat or quote the transcript. ${SUMMARY_FORMAT}\n\n` +
  'Only include what was actually said.';

// The worker starts every summary answer with this (it becomes part of the answer).
export const SUMMARY_PREFIX = '## Summary\n';

// The live summary is re-run every minute or so and the device shows what changed
// (meetingDiff.js), so ask for stable wording. The leading "Rewrite the full summary"
// is what llm.mock.worker.js recognises.
export const UPDATE_REQUEST =
  'Rewrite the full summary in the same format, covering the whole transcript so far. ' +
  'Keep the wording of points that still hold; add new points at the end of each section.';

export const questionRequest = (q) =>
  'Answer this question about the transcript in your own words, in 1–3 sentences. Do not ' +
  'quote or repeat transcript lines. End each sentence with the [mm:ss] time it comes from.' +
  `\n\nQuestion: ${String(q || '').trim()}`;

const range = (a, b) => `(${tsTag(a)}–${tsTag(b)})`;

// The first user message: the whole transcript, then the request.
export function firstUserMessage(lines, request, fromSec = 0) {
  const to = lines.length ? lines[lines.length - 1].end : fromSec;
  return `<transcript ${range(fromSec, to)}>\n${linesText(lines)}\n</transcript>\n\n${request}`;
}

// A later user message: the transcript since fromSec (if any), then the request.
export function moreUserMessage(lines, request, fromSec) {
  if (!lines.length) return request;
  return `<transcript continued ${range(fromSec, lines[lines.length - 1].end)}>\n${linesText(lines)}\n</transcript>\n\n${request}`;
}

// Over budget, for questions: notes on the older chunks, then the newest lines.
export function notesUserMessage(notes, lines, request) {
  const parts = notes.map((n) => `Part ${range(n.start, n.end)}:\n${n.notes.trim()}`).join('\n\n');
  const lastNote = notes[notes.length - 1];
  const head = `Notes on the earlier part of the meeting ${range(0, lastNote ? lastNote.end : 0)}:\n${parts}`;
  if (!lines.length) return `${head}\n\n${request}`;
  return `${head}\n\n${firstUserMessage(lines, request, lines[0].start)}`;
}

// Measured on both models (Sep 2026, a one-hour TV transcript): asked for "compact
// notes", the 1.2B copied transcript lines one by one until MAX_TOKENS.notes ran out,
// covering only the start of the part. A bullet count, "in your own words" and the
// part's end time make it cover the whole part, if the part is small (~1k tokens: at
// ~2k both models still stopped a third of the way in). Starting the answer with
// NOTES_PREFIX stops preambles and makes every bullet carry its time. The tags
// (Decision: / Action: / Question:) are what the summary's other sections are built
// from (assembleSummary).
export const NOTES_PREFIX = '- [';

export function mapUserMessage(chunk, i, n) {
  const r = range(chunk.start, chunk.end);
  return (
    `Transcript ${r}:\n${chunk.text}\n\n` +
    `This is part ${i + 1} of ${n} ${r}. Write 3–6 bullet points that together cover this ` +
    `whole part, from ${tsTag(chunk.start)} to ${tsTag(chunk.end)}, in order. Start each bullet with ` +
    'the [mm:ss] time it comes from, then one point in your own words. After the time, write ' +
    '"Decision:" if the point is something decided, "Action: Name:" if someone will do ' +
    'something, or "Question:" if a question is left open; most points are none of these. Do ' +
    'not copy or quote transcript lines. No introduction.'
  );
}

// Measured on both models (Sep 2026, five parts of notes, ~750 tokens): asked to merge
// notes into the full layout, they walk the notes in order, copying them into the
// sections, and stop after two or three parts. Only the summary sentences come out of
// the merge; every list is built from the notes (assembleSummary).
export function reduceUserMessage(notes) {
  const parts = notes.map((n, i) => `Part ${i + 1} ${range(n.start, n.end)}:\n${n.notes.trim()}`).join('\n\n');
  return (
    `${parts}\n\nAbove are notes on ${notes.length} consecutive parts of one meeting. Write a ` +
    'summary of the whole meeting in 2–4 sentences, covering it from the first part to the ' +
    'last. Include only what the notes say. Answer with a "## Summary" heading, then the ' +
    'sentences, and nothing else.'
  );
}

// The summary's lists from notes, without a model. Every timed bullet is a key point,
// a decision, an action item or an open question (by its tag). Key points: at most
// KEY_POINTS_MAX in all, at least one per part, spread evenly across each part.
export const KEY_POINTS_MAX = 15;
export const TAGGED_MAX = 8; // decisions, action items, open questions: each
const BULLET_RE = /^\s*(?:[-*•]|\d+[.)])\s+(.+)$/;
const LEAD_TS_RE = /^\*{0,2}(\[(?:\d+:)?\d{1,2}:\d{2}\])\*{0,2}\s*[-–—:]?\s*/;
const TAIL_TS_RE = /\s*[(]?(\[(?:\d+:)?\d{1,2}:\d{2}\])[)]?[.]?$/;
// A tag anywhere in a bullet: the 2.6B often writes it after the point ("… Decision:
// restated.", "[Decision:] …"). Capitalised only, so prose ("the question: who …") isn't one.
const TAG_RE = /\[?\*{0,2}\b(Decision|Action(?: item)?|Open [Qq]uestion|Question)s?\*{0,2}\]?\s*:\s*\]?\s*(?:\*{1,2}\s*)?/g;
const KIND = { D: 'decision', A: 'action', Q: 'question', O: 'question' };
const tidy = (t) => t.replace(/^[\s\-–—:;,.]+|[\s\-–—:;,]+$/g, '').replace(/^Name:\s*/, '');

// "- [04:12] text" or "- text [04:12]" → items { ts: '[04:12]', text, kind }: the text
// before any tag is a 'point'; the text after a tag (Decision: / Action: / Question:)
// is a 'decision' / 'action' / 'question' (dropped if empty or "None…"). Untimed
// bullets are skipped.
export function noteBullets(notes) {
  const out = [];
  for (const line of String(notes || '').split('\n')) {
    const b = line.match(BULLET_RE);
    if (!b) continue;
    let text = b[1].trim();
    let ts = null;
    const lead = text.match(LEAD_TS_RE);
    if (lead) {
      ts = lead[1];
      text = text.slice(lead[0].length).replace(TAIL_TS_RE, ''); // the 2.6B often repeats it
    } else {
      const tail = text.match(TAIL_TS_RE);
      if (tail) {
        ts = tail[1];
        text = text.slice(0, tail.index);
      }
    }
    if (!ts) continue;
    const tags = [...text.matchAll(TAG_RE)];
    const pre = tidy(tags.length ? text.slice(0, tags[0].index) : text);
    if (pre) out.push({ ts, text: pre, kind: 'point' });
    tags.forEach((m, i) => {
      const t = tidy(text.slice(m.index + m[0].length, i + 1 < tags.length ? tags[i + 1].index : text.length));
      if (t && !/^\(?(none|n\/a)\b/i.test(t)) out.push({ ts, text: t, kind: KIND[m[1][0]] });
    });
  }
  return out;
}

// Notes cut off by MAX_TOKENS.notes (reason 'length'): drop the unfinished last line.
export function trimCutNotes(text, reason) {
  const s = String(text || '').trimEnd();
  if (reason !== 'length') return s;
  const nl = s.lastIndexOf('\n');
  return nl < 0 ? s : s.slice(0, nl).trimEnd();
}

// k of xs, evenly spread (first and last included).
const spread = (xs, k) =>
  k >= xs.length ? xs : Array.from({ length: k }, (_, j) => xs[k === 1 ? 0 : Math.round((j * (xs.length - 1)) / (k - 1))]);

// parts: [{ notes }] in order → { points, decisions, actions, questions }, each a list
// of "- text [mm:ss]" lines. A part's key points are its untagged bullets (all of its
// bullets if every one is tagged).
export function listsFromNotes(parts, { max = KEY_POINTS_MAX, taggedMax = TAGGED_MAX } = {}) {
  const per = Math.max(1, Math.floor(max / Math.max(1, parts.length)));
  const line = (b) => `- ${b.text} ${b.ts}`;
  const all = parts.map((p) => noteBullets(p.notes));
  const points = all.flatMap((bs) => {
    const plain = bs.filter((b) => b.kind === 'point');
    return spread(plain.length ? plain : bs, per).map(line);
  });
  const tagged = (kind) => spread(all.flat().filter((b) => b.kind === kind), taggedMax).map(line);
  return { points, decisions: tagged('decision'), actions: tagged('action'), questions: tagged('question') };
}

// A map-reduce summary: the merge's summary sentences (under "## Summary", or its
// text up to the first heading), then the lists from the notes. Anything else the
// model wrote is dropped.
export function assembleSummary(merged, parts, opts) {
  const lines = String(merged || '').split('\n');
  const heading = (l) => /^#{1,6}\s+\S/.test(l);
  const at = lines.findIndex((l) => /^#{1,6}\s+summary\b/i.test(l));
  const from = at >= 0 ? at + 1 : 0;
  let to = lines.findIndex((l, i) => i >= from && heading(l));
  if (to < 0) to = lines.length;
  const summary = lines.slice(from, to).join('\n').trim();
  const { points, decisions, actions, questions } = listsFromNotes(parts, opts);
  const none = ['- None recorded.'];
  const out = ['## Summary', summary || "The notes don't say.", ''];
  if (points.length) out.push('## Key points', ...points, '');
  out.push('## Decisions', ...(decisions.length ? decisions : none), '');
  out.push('## Action items', ...(actions.length ? actions : none), '');
  if (questions.length) out.push('## Open questions', ...questions, '');
  return out.join('\n').trimEnd();
}

// ---------------------------------------------------------------------------------
// Chunking (map-reduce)

// Consecutive runs of lines of about maxTokens (estimated) each. Greedy from the start,
// so as the transcript grows every chunk but the last stays the same (and its notes
// stay cached).
export function chunkLines(lines, maxTokens = CHUNK_TOKENS) {
  const chunks = [];
  let cur = [];
  let tokens = 0;
  const flush = () => {
    if (!cur.length) return;
    const text = linesText(cur);
    chunks.push({ lines: cur, text, tokens, start: cur[0].start, end: cur[cur.length - 1].end, hash: hashText(text) });
    cur = [];
    tokens = 0;
  };
  for (const l of lines) {
    const t = estTokens(l.line) + 1;
    if (cur.length && tokens + t > maxTokens) flush();
    cur.push(l);
    tokens += t;
  }
  flush();
  return chunks;
}

// For a question over budget: how many leading chunks go in as notes (the rest
// verbatim). The smallest k whose verbatim tail plus ~notesPerChunk per note fits.
export function notesSplit(chunks, budget = VERBATIM_MAX, notesPerChunk = MAX_TOKENS.notes) {
  let tail = chunks.reduce((n, c) => n + c.tokens, 0);
  for (let k = 0; k < chunks.length; k++) {
    if (tail + k * notesPerChunk <= budget) return k;
    tail -= chunks[k].tokens;
  }
  return Math.max(0, chunks.length - 1);
}

// ---------------------------------------------------------------------------------
// Append vs rebuild

// conv: what the worker has seen, or null:
//   { take, model, mode: 'verbatim'|'notes', messages: [{ role, content }] (as sent,
//     assistant turns as the worker's `raw`), lines: every transcript line the
//     conversation covers, from word 0 (with the words' texts at the time: `texts`),
//     sentTo: word index covered, coversTo: seconds, transcriptTokens, historyTokens,
//     hasSummary }
// → { ok: true, newLines } or { ok: false, reason }.
// ok means every line already covered still reads the same, so the new lines (and
// the request) can be appended as a new user message.
export function planAppend(conv, src) {
  if (!conv) return { ok: false, reason: 'new' };
  const { words = [], timeline = null, labels = {}, final = false } = src;
  if (words.length < conv.sentTo) return { ok: false, reason: 'rewritten' };
  const names = conv.lines.some((l) => l.label !== null) || timeline ? wordLabels(words, timeline, labels, conv.sentTo) : null;
  for (const l of conv.lines) {
    for (let i = l.wordFrom; i < l.wordTo; i++) {
      if (String(words[i].text || '').trim() !== l.texts[i - l.wordFrom]) return { ok: false, reason: 'rewritten' };
      if (names && names[i] !== l.label) return { ok: false, reason: 'relabelled' };
    }
  }
  const settledUntil = settledUntilFor(words, timeline, final);
  const newLines = transcriptLines(words, timeline, labels, { settledUntil, from: conv.sentTo });
  return { ok: true, newLines };
}

// Lines as stored in conv.lines (each with its words' texts, to spot rewrites).
export const withTexts = (lines, words) =>
  lines.map((l) => ({ ...l, texts: words.slice(l.wordFrom, l.wordTo).map((w) => String(w.text || '').trim()) }));

// ---------------------------------------------------------------------------------
// Model output

const THINK_OPEN = '<think>';
const THINK_CLOSE = '</think>';
const SPECIALS = ['<|im_end|>', '<|endoftext|>'];

// Drop <think>…</think> blocks and end-of-turn tokens. While streaming, an unclosed
// <think> hides everything after it (thinking: true), and a partly streamed tag at
// the very end is held back.
export function stripThink(raw, streaming = false) {
  let s = String(raw || '');
  for (const sp of SPECIALS) s = s.split(sp).join('');
  s = s.replace(/<think>[\s\S]*?<\/think>/g, '');
  let thinking = false;
  const open = s.indexOf(THINK_OPEN);
  if (open >= 0) {
    s = s.slice(0, open);
    thinking = true;
  }
  // A stray close tag (template put <think> in the prompt): keep what follows it.
  const close = s.lastIndexOf(THINK_CLOSE);
  if (close >= 0) s = s.slice(close + THINK_CLOSE.length);
  if (streaming) {
    for (const tag of [THINK_OPEN, THINK_CLOSE, ...SPECIALS]) {
      for (let k = Math.min(tag.length - 1, s.length); k > 0; k--) {
        if (s.endsWith(tag.slice(0, k))) {
          s = s.slice(0, s.length - k);
          break;
        }
      }
    }
  }
  return { text: streaming ? s.trimStart() : s.trim(), thinking };
}

// Citations like [04:12], [1:04:12] or [04:12–05:30] → [{ start, end, sec, label }]
// (start/end: offsets in text; sec: the first time; label: the matched text).
const CITE_RE = /\[(?:(\d+):)?(\d{1,2}):(\d{2})(?:\s*[–-][^\]]*)?\]/g;
export function parseCitations(text) {
  const out = [];
  const s = String(text || '');
  for (const m of s.matchAll(CITE_RE)) {
    const h = m[1] ? Number(m[1]) : 0;
    const mm = Number(m[2]);
    const ss = Number(m[3]);
    if (ss > 59 || (m[1] && mm > 59)) continue;
    out.push({ start: m.index, end: m.index + m[0].length, sec: h * 3600 + mm * 60 + ss, label: m[0] });
  }
  return out;
}

// ---------------------------------------------------------------------------------
// Planning a request (pure; llmClient.js runs the plan against the worker)

// req: { kind: 'summary'|'question', question? }. opts: { take, model,
//   tokens: text → number (default estTokens), transcriptTokens: exact count of the
//   whole transcript's text, if known (for the budget decision on a rebuild) }.
// → append:     { action: 'append', messages, conv /*after sending*/, newLines, maxTokens }
//   rebuild:    { action: 'verbatim', reason, messages, conv, lines, maxTokens }
//   over budget:{ action: 'map-reduce', reason, lines, chunks, maxTokens }   (summary)
//               { action: 'notes', reason, lines, chunks, k, maxTokens }     (question;
//                 then notesConversation() with the notes of chunks[0..k))
export function planRequest(conv, src, req, opts = {}) {
  const tokens = opts.tokens || estTokens;
  const { words = [], timeline = null, labels = {}, final = false } = src;
  const maxTokens = MAX_TOKENS[req.kind === 'summary' ? 'summary' : 'question'];
  const budget = req.kind === 'summary' ? SUMMARY_VERBATIM_MAX : VERBATIM_MAX;
  let reason = 'new';
  if (conv && (conv.take !== opts.take || conv.model !== opts.model)) reason = 'take';
  else if (conv) {
    const check = planAppend(conv, src);
    reason = check.reason;
    if (check.ok) {
      const request = req.kind === 'summary' ? (conv.hasSummary ? UPDATE_REQUEST : SUMMARY_REQUEST) : questionRequest(req.question);
      const newText = check.newLines.length ? linesText(check.newLines) : '';
      const addTranscript = newText ? tokens(newText) : 0;
      const addHistory = tokens(request) + maxTokens;
      if (conv.transcriptTokens + addTranscript > budget) reason = 'budget';
      else if (conv.historyTokens + addHistory > HISTORY_MAX) reason = 'history';
      else {
        const content = moreUserMessage(check.newLines, request, conv.coversTo);
        const last = check.newLines[check.newLines.length - 1];
        const next = {
          ...conv,
          messages: [...conv.messages, { role: 'user', content }],
          lines: conv.lines.concat(withTexts(check.newLines, words)),
          sentTo: last ? last.wordTo : conv.sentTo,
          coversTo: last ? last.end : conv.coversTo,
          transcriptTokens: conv.transcriptTokens + addTranscript,
          historyTokens: conv.historyTokens + tokens(request),
          hasSummary: conv.hasSummary || req.kind === 'summary',
        };
        return { action: 'append', messages: next.messages, conv: next, newLines: check.newLines, maxTokens };
      }
    }
  }
  const settledUntil = settledUntilFor(words, timeline, final);
  const lines = transcriptLines(words, timeline, labels, { settledUntil });
  const total = opts.transcriptTokens ?? (lines.length ? tokens(linesText(lines)) : 0);
  if (total > budget) {
    const chunks = chunkLines(lines, req.kind === 'summary' ? SUMMARY_CHUNK_TOKENS : CHUNK_TOKENS);
    if (req.kind === 'summary') return { action: 'map-reduce', reason, lines, chunks, maxTokens };
    return { action: 'notes', reason, lines, chunks, k: notesSplit(chunks), maxTokens };
  }
  const request = req.kind === 'summary' ? SUMMARY_REQUEST : questionRequest(req.question);
  const messages = [
    { role: 'system', content: SYSTEM_PROMPT },
    { role: 'user', content: firstUserMessage(lines, request) },
  ];
  const last = lines[lines.length - 1];
  const next = {
    take: opts.take,
    model: opts.model,
    mode: 'verbatim',
    messages,
    lines: withTexts(lines, words),
    sentTo: last ? last.wordTo : 0,
    coversTo: last ? last.end : 0,
    transcriptTokens: total,
    historyTokens: tokens(request), // requests + answers (not the system prompt or transcript)
    hasSummary: req.kind === 'summary',
  };
  return { action: 'verbatim', reason, messages, conv: next, lines, maxTokens };
}

// The conversation for a question over budget: notes on chunks[0..k) (notes[i] is
// chunk i's notes text), then chunks[k..] verbatim.
export function notesConversation(plan, notes, src, req, opts = {}) {
  const tokens = opts.tokens || estTokens;
  const { chunks, k, lines } = plan;
  const noteParts = chunks.slice(0, k).map((c, i) => ({ start: c.start, end: c.end, notes: notes[i] || '' }));
  const tail = chunks.slice(k).flatMap((c) => c.lines);
  const request = questionRequest(req.question);
  const content = notesUserMessage(noteParts, tail, request);
  const messages = [
    { role: 'system', content: SYSTEM_PROMPT },
    { role: 'user', content },
  ];
  const last = lines[lines.length - 1];
  const conv = {
    take: opts.take,
    model: opts.model,
    mode: 'notes',
    messages,
    lines: withTexts(lines, src.words || []),
    sentTo: last ? last.wordTo : 0,
    coversTo: last ? last.end : 0,
    transcriptTokens: tokens(content) - tokens(request),
    historyTokens: tokens(request), // requests + answers (not the system prompt or transcript)
    hasSummary: false,
  };
  return { messages, conv };
}

// The standalone conversations of a map-reduce summary.
export const mapMessages = (chunk, i, n) => [
  { role: 'system', content: SYSTEM_PROMPT },
  { role: 'user', content: mapUserMessage(chunk, i, n) },
];
export const reduceMessages = (notes) => [
  { role: 'system', content: SYSTEM_PROMPT },
  { role: 'user', content: reduceUserMessage(notes) },
];

// After the answer: the assistant turn goes into the history exactly as the worker
// produced it (`raw`), so the next request's prompt starts with the cached tokens.
export function commitAnswer(conv, raw, tokens = estTokens) {
  return {
    ...conv,
    messages: [...conv.messages, { role: 'assistant', content: raw }],
    historyTokens: conv.historyTokens + tokens(raw),
  };
}
