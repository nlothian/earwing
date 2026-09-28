// Tests for src/components/llmPrompt.js (the assistant's prompts, transcript lines and
// append-vs-rebuild planning). No model: runs in plain Node.
//
//   node scripts/test-llm-prompt.mjs

import assert from 'node:assert/strict';
import {
  chunkLines,
  CHUNK_TOKENS,
  commitAnswer,
  estTokens,
  firstUserMessage,
  fmtTs,
  HISTORY_MAX,
  linesText,
  notesConversation,
  notesSplit,
  parseCitations,
  planAppend,
  planRequest,
  settledUntilFor,
  SETTLE_SEC,
  stripThink,
  SUMMARY_REQUEST,
  transcriptLines,
  UPDATE_REQUEST,
  VERBATIM_MAX,
} from '../src/components/llmPrompt.js';
import { buildTimeline } from '../src/components/speakerTimeline.js';

let passed = 0;
let failed = 0;
function test(name, fn) {
  try {
    fn();
    passed++;
    console.log(`ok   ${name}`);
  } catch (e) {
    failed++;
    console.log(`FAIL ${name}\n     ${e.stack.split('\n').slice(0, 3).join('\n     ')}`);
  }
}

// Words every `step` s from t0 to t1, `dur` long, texts w0, w1, … (or from a vocabulary).
function makeWords(t0, t1, { step = 0.4, dur = 0.3, prefix = 'w', vocab = null } = {}) {
  const out = [];
  let i = 0;
  for (let t = t0; t + dur <= t1 + 1e-9; t += step, i++) {
    const text = vocab ? vocab[i % vocab.length] : `${prefix}${i}`;
    out.push({ text, start_time: +t.toFixed(3), end_time: +(t + dur).toFixed(3) });
  }
  return out;
}
const VOCAB = 'we should ship the budget review by friday and alice will draft the plan for the team'.split(' ');
const concat = (messages) => messages.map((m) => `${m.role}\u0000${m.content}\u0001`).join('');

// --- fmtTs ---------------------------------------------------------------------------
test('fmtTs: mm:ss, then h:mm:ss past an hour', () => {
  assert.equal(fmtTs(0), '00:00');
  assert.equal(fmtTs(5.9), '00:05');
  assert.equal(fmtTs(252), '04:12');
  assert.equal(fmtTs(3599), '59:59');
  assert.equal(fmtTs(3600), '1:00:00');
  assert.equal(fmtTs(3852), '1:04:12');
  assert.equal(fmtTs(-3), '00:00');
});

// --- lines without speakers ------------------------------------------------------------
test('paragraphs: break at a ≥1 s pause only once 15 s long; hard break at 45 s', () => {
  // 0–10 s speech, 1.5 s pause (too early to break), 11.5–20 s, 1.5 s pause (break), then 60 s continuous.
  const words = [...makeWords(0, 10), ...makeWords(11.5, 20, { prefix: 'a' }), ...makeWords(21.5, 81.5, { prefix: 'b' })];
  const lines = transcriptLines(words, null, {});
  assert.equal(lines[0].start, 0);
  assert.ok(lines[0].end > 19 && lines[0].end <= 20, `first paragraph spans the early pause (${lines[0].end})`);
  assert.equal(lines[1].start, 21.5);
  for (const l of lines) assert.ok(l.end - l.start <= 45.5, `≤ 45 s (${l.start}–${l.end})`);
  assert.ok(lines.length >= 3);
  // Lines tile the words exactly.
  assert.equal(lines[0].wordFrom, 0);
  for (let i = 1; i < lines.length; i++) assert.equal(lines[i].wordFrom, lines[i - 1].wordTo);
  assert.equal(lines[lines.length - 1].wordTo, words.length);
  assert.match(lines[0].line, /^\[00:00\] w0 w1 /);
  assert.equal(lines[0].label, null);
});

test('lines past an hour use [h:mm:ss]', () => {
  const words = makeWords(3595, 3610);
  const lines = transcriptLines(words, null, {});
  assert.match(lines[0].line, /^\[59:55\] /);
  const later = transcriptLines(makeWords(3700, 3705), null, {});
  assert.match(later[0].line, /^\[1:01:40\] /);
});

// --- lines with speakers ---------------------------------------------------------------
const SEGS = [
  { start: 0, end: 20, speaker: 0 },
  { start: 20, end: 130, speaker: 1 },
  { start: 130, end: 150, speaker: 0 },
];
test('turns: "[mm:ss] Name: text", names from labels, long turns split at the largest gap', () => {
  const tl = buildTimeline(SEGS);
  // Speaker 1 talks 20–130 s with a 3 s pause at ~70 s.
  const words = [...makeWords(0, 19.8), ...makeWords(20, 70, { prefix: 'b' }), ...makeWords(73, 129.8, { prefix: 'c' }), ...makeWords(130, 149.8, { prefix: 'd' })];
  const lines = transcriptLines(words, tl, { 1: 'Alice' });
  assert.match(lines[0].line, /^\[00:00\] Speaker 1: w0 /);
  const alice = lines.filter((l) => l.label === 'Alice');
  assert.ok(alice.length >= 3, `the 110 s turn is split (${alice.length} pieces)`);
  assert.ok(alice.some((l) => l.start === 73), 'one split is at the 3 s pause');
  for (const l of alice) {
    assert.ok(l.end - l.start <= 45 && l.wordTo - l.wordFrom <= 120, `piece ≤ 45 s / 120 words (${l.start}–${l.end})`);
    assert.match(l.line, new RegExp(`^\\[${fmtTs(l.start)}\\] Alice: `));
  }
  assert.equal(lines[lines.length - 1].label, 'Speaker 1');
  assert.equal(lines[lines.length - 1].wordTo, words.length);
});

test('pending words are left out; live with speakers drops the newest 12 s; final keeps all', () => {
  const tl = buildTimeline(SEGS);
  const words = makeWords(0, 60);
  const withPending = [...words, ...makeWords(60, 62, { prefix: 'p' }).map((w) => ({ ...w, pending: true }))];
  const all = transcriptLines(withPending, tl, {});
  assert.equal(all[all.length - 1].wordTo, words.length, 'stops before the first pending word');
  assert.ok(!linesText(all).includes('p0'));

  const cut = settledUntilFor(words, tl, false);
  assert.equal(cut, words[words.length - 1].end_time - SETTLE_SEC);
  const live = transcriptLines(words, tl, {}, { settledUntil: cut });
  const lastEnd = live[live.length - 1].end;
  assert.ok(lastEnd <= cut && lastEnd > cut - 1, `live lines end by the settled cutoff (${lastEnd} vs ${cut})`);
  assert.equal(settledUntilFor(words, tl, true), Infinity, 'final: everything');
  assert.equal(settledUntilFor(words, null, false), Infinity, 'no speakers: everything');
});

test('`from` emits only later lines, labelled as in a full build', () => {
  const tl = buildTimeline(SEGS);
  const words = makeWords(0, 149.8);
  const full = transcriptLines(words, tl, {});
  const k = full[1].wordTo;
  const tail = transcriptLines(words, tl, {}, { from: k });
  assert.equal(tail[0].wordFrom, k);
  for (const l of tail) {
    for (let i = l.wordFrom; i < l.wordTo; i++) {
      const f = full.find((x) => i >= x.wordFrom && i < x.wordTo);
      assert.equal(l.label, f.label, `word ${i}`);
    }
  }
});

// --- append vs rebuild -------------------------------------------------------------------
const OPTS = { take: 1, model: 'large' };
function convAfter(words, tl, labels, req = { kind: 'summary' }, final = true) {
  const src = { words, timeline: tl, labels, final, take: 1 };
  const p = planRequest(null, src, req, OPTS);
  assert.equal(p.action, 'verbatim');
  return { p, conv: commitAnswer(p.conv, '## Summary\nThey talked [00:01].') };
}

test('first request: system + transcript + request', () => {
  const tl = buildTimeline(SEGS);
  const words = makeWords(0, 40);
  const { p } = convAfter(words, tl, {});
  assert.equal(p.messages.length, 2);
  assert.equal(p.messages[0].role, 'system');
  assert.match(p.messages[1].content, /^<transcript \(\[00:00\]–\[00:39\]\)>\n\[00:00\] Speaker 1: /);
  assert.ok(p.messages[1].content.endsWith(`\n\n${SUMMARY_REQUEST}`));
  assert.equal(p.maxTokens, 900);
});

test('pure growth appends, and the conversation stays prefix-stable', () => {
  const tl = buildTimeline(SEGS);
  const all = makeWords(0, 149.8);
  let conv = convAfter(all.slice(0, 50), tl, {}).conv;
  let prev = concat(conv.messages);
  const firstMessages = conv.messages.length;
  for (const [n, req] of [
    [120, { kind: 'question', question: 'Who spoke first?' }],
    [120, { kind: 'question', question: 'And then?' }],
    [260, { kind: 'summary' }],
    [all.length, { kind: 'question', question: 'Anything else?' }],
  ]) {
    const src = { words: all.slice(0, n), timeline: tl, labels: {}, final: true, take: 1 };
    const p = planRequest(conv, src, req, OPTS);
    assert.equal(p.action, 'append', `n=${n}: ${p.reason}`);
    assert.ok(concat(p.messages).startsWith(prev), 'old messages are an exact prefix');
    const added = p.messages[p.messages.length - 1];
    assert.equal(added.role, 'user');
    if (p.newLines.length) assert.match(added.content, /^<transcript continued \(\[\d\d:\d\d\]–\[\d\d:\d\d\]\)>\n\[/);
    else assert.ok(!added.content.includes('<transcript'), 'no new words: just the request');
    if (req.kind === 'summary') assert.ok(added.content.endsWith(UPDATE_REQUEST), 'a second summary is an update');
    else assert.ok(added.content.endsWith(`Question: ${req.question}`));
    assert.equal(p.conv.sentTo, n);
    conv = commitAnswer(p.conv, `answer for ${n} [00:02]`);
    prev = concat(conv.messages);
  }
  assert.ok(conv.messages.length > firstMessages + 6);
  // Every word went in exactly once.
  const text = conv.messages.filter((m) => m.role === 'user').map((m) => m.content).join('\n');
  for (const w of all) assert.ok(new RegExp(`\\b${w.text}\\b`).test(text), w.text);
});

test('relabel, rename and rewrite force a rebuild', () => {
  const tl = buildTimeline(SEGS);
  const words = makeWords(0, 60);
  const { conv } = convAfter(words, tl, {});
  const src = { words, timeline: tl, labels: {}, final: true, take: 1 };
  const q = { kind: 'question', question: 'x?' };
  assert.equal(planRequest(conv, src, q, OPTS).action, 'append');

  const relabelled = buildTimeline([{ start: 0, end: 10, speaker: 0 }, { start: 10, end: 150, speaker: 1 }]);
  let p = planRequest(conv, { ...src, timeline: relabelled }, q, OPTS);
  assert.equal(p.action, 'verbatim');
  assert.equal(p.reason, 'relabelled');

  p = planRequest(conv, { ...src, labels: { 1: 'Bob' } }, q, OPTS);
  assert.equal(p.reason, 'relabelled', 'rename');
  assert.match(p.messages[1].content, /\] Bob: /);

  const rewritten = words.map((w, i) => (i === 7 ? { ...w, text: 'changed' } : w));
  p = planRequest(conv, { ...src, words: rewritten }, q, OPTS);
  assert.equal(p.reason, 'rewritten');

  p = planRequest(conv, { ...src, words: words.slice(0, 20) }, q, OPTS);
  assert.equal(p.reason, 'rewritten', 'words went away');

  p = planRequest(conv, { ...src, timeline: null }, q, OPTS);
  assert.equal(p.reason, 'relabelled', 'speakers turned off');

  p = planRequest(conv, { ...src, take: 2 }, q, { ...OPTS, take: 2 });
  assert.equal(p.reason, 'take');
  p = planRequest(conv, src, q, { ...OPTS, model: 'small' });
  assert.equal(p.reason, 'take', 'another model');

  // A rebuild starts over: the summary is a fresh one, not an update.
  p = planRequest(conv, { ...src, labels: { 1: 'Bob' } }, { kind: 'summary' }, OPTS);
  assert.ok(p.messages[1].content.endsWith(SUMMARY_REQUEST));
});

test('live: only settled lines go in; a later final request appends the rest', () => {
  const tl = buildTimeline(SEGS);
  const words = makeWords(0, 100);
  const liveSrc = { words, timeline: tl, labels: {}, final: false, take: 1 };
  const p = planRequest(null, liveSrc, { kind: 'summary' }, OPTS);
  assert.ok(p.conv.coversTo <= words[words.length - 1].end_time - SETTLE_SEC);
  const conv = commitAnswer(p.conv, 'ok');
  const a = planAppend(conv, { ...liveSrc, final: true });
  assert.equal(a.ok, true);
  assert.equal(a.newLines[a.newLines.length - 1].wordTo, words.length);
});

test('without speakers: growth appends, rewrite rebuilds', () => {
  const words = makeWords(0, 80);
  const src = { words: words.slice(0, 100), timeline: null, labels: {}, final: false, take: 1 };
  const p = planRequest(null, src, { kind: 'summary' }, OPTS);
  assert.equal(p.conv.sentTo, 100, 'no settling delay without speakers');
  const conv = commitAnswer(p.conv, 'ok');
  const q = { kind: 'question', question: 'x?' };
  assert.equal(planRequest(conv, { ...src, words }, q, OPTS).action, 'append');
  const rw = words.map((w, i) => (i === 99 ? { ...w, text: 'other' } : w));
  assert.equal(planRequest(conv, { ...src, words: rw }, q, OPTS).reason, 'rewritten');
});

test('history over HISTORY_MAX rebuilds', () => {
  const words = makeWords(0, 30);
  const src = { words, timeline: null, labels: {}, final: true, take: 1 };
  let conv = commitAnswer(planRequest(null, src, { kind: 'summary' }, OPTS).conv, 'x'.repeat(3.5 * 900));
  let rounds = 0;
  for (;;) {
    const p = planRequest(conv, src, { kind: 'question', question: 'why?' }, OPTS);
    if (p.action !== 'append') {
      assert.equal(p.reason, 'history');
      break;
    }
    conv = commitAnswer(p.conv, 'y'.repeat(3.5 * 400));
    assert.ok(conv.historyTokens <= HISTORY_MAX + 400 + 10);
    rounds++;
    assert.ok(rounds < 20);
  }
  assert.ok(rounds >= 1);
});

// --- budget, chunking -------------------------------------------------------------------
// ~2 words/s of real-looking text: 16k tokens ≈ 56k chars ≈ 10k words ≈ 80+ min.
const LONG = makeWords(0, 3 * 3600, { step: 0.5, dur: 0.4, vocab: VOCAB });

test('over budget: summaries map-reduce, questions use notes + newest lines', () => {
  const src = { words: LONG, timeline: null, labels: {}, final: true, take: 1 };
  const total = estTokens(linesText(transcriptLines(LONG, null, {})));
  assert.ok(total > VERBATIM_MAX, `long enough (${total})`);
  const s = planRequest(null, src, { kind: 'summary' }, OPTS);
  assert.equal(s.action, 'map-reduce');
  assert.ok(s.chunks.length >= Math.ceil(total / CHUNK_TOKENS));
  for (const c of s.chunks) assert.ok(c.tokens <= CHUNK_TOKENS, `chunk ≤ ${CHUNK_TOKENS} (${c.tokens})`);
  assert.deepEqual(
    s.chunks.flatMap((c) => c.lines),
    s.lines,
    'chunks tile the lines in order',
  );

  const q = planRequest(null, src, { kind: 'question', question: 'Who drafts the plan?' }, OPTS);
  assert.equal(q.action, 'notes');
  assert.ok(q.k > 0 && q.k < q.chunks.length);
  const notes = q.chunks.slice(0, q.k).map((c, i) => `- note ${i} [${fmtTs(c.start)}]`);
  const nc = notesConversation(q, notes, src, { kind: 'question', question: 'Who drafts the plan?' }, OPTS);
  const content = nc.messages[1].content;
  assert.match(content, /^Notes on the earlier part of the meeting \(\[00:00\]–\[/);
  assert.match(content, /\n\n<transcript \(\[\d+:\d\d(:\d\d)?\]–\[/);
  assert.ok(content.endsWith('Question: Who drafts the plan?'));
  assert.ok(estTokens(content) <= VERBATIM_MAX + 500, `fits (${estTokens(content)})`);
  assert.equal(nc.conv.mode, 'notes');
  assert.equal(nc.conv.sentTo, LONG.length);

  // The exact count decides: an estimate under budget with an exact count over it.
  const mid = { ...src, words: LONG.slice(0, 2000) };
  assert.equal(planRequest(null, mid, { kind: 'summary' }, OPTS).action, 'verbatim');
  assert.equal(planRequest(null, mid, { kind: 'summary' }, { ...OPTS, transcriptTokens: VERBATIM_MAX + 1 }).action, 'map-reduce');
});

test('chunks stay the same as the transcript grows (all but the last)', () => {
  const a = chunkLines(transcriptLines(LONG.slice(0, 15000), null, {}));
  const b = chunkLines(transcriptLines(LONG, null, {}));
  for (let i = 0; i < a.length - 1; i++) assert.equal(a[i].hash, b[i].hash, `chunk ${i}`);
});

test('appending past VERBATIM_MAX rebuilds (budget)', () => {
  const src = { words: LONG.slice(0, 6000), timeline: null, labels: {}, final: true, take: 1 };
  const p = planRequest(null, src, { kind: 'summary' }, OPTS);
  assert.equal(p.action, 'verbatim');
  const conv = commitAnswer(p.conv, 'ok');
  const q = planRequest(conv, { ...src, words: LONG }, { kind: 'question', question: 'q?' }, OPTS);
  assert.equal(q.reason, 'budget');
  assert.equal(q.action, 'notes');
});

test('notesSplit: smallest k that fits', () => {
  const chunks = [6000, 6000, 6000, 6000].map((tokens) => ({ tokens }));
  assert.equal(notesSplit(chunks, 16000, 350), 2); // 12000 + 700
  assert.equal(notesSplit([{ tokens: 5000 }], 16000), 0);
});

test('firstUserMessage range', () => {
  const lines = transcriptLines(makeWords(0, 1390), null, {});
  assert.match(firstUserMessage(lines, 'R'), /^<transcript \(\[00:00\]–\[23:09\]\)>/);
});

// --- output parsing ------------------------------------------------------------------------
test('parseCitations', () => {
  const text = 'A [04:12], B [1:04:12] and C [04:12–05:30] or [4:05-4:10]; not [99:99] or [x] or [12].';
  const c = parseCitations(text);
  assert.deepEqual(
    c.map((x) => [x.sec, x.label]),
    [
      [252, '[04:12]'],
      [3852, '[1:04:12]'],
      [252, '[04:12–05:30]'],
      [245, '[4:05-4:10]'],
    ],
  );
  assert.equal(text.slice(c[1].start, c[1].end), '[1:04:12]');
});

test('stripThink', () => {
  assert.deepEqual(stripThink('<think>hmm\nok</think>\n\nThe answer [00:03].<|im_end|>'), {
    text: 'The answer [00:03].',
    thinking: false,
  });
  assert.deepEqual(stripThink('<think>still going', true), { text: '', thinking: true });
  assert.deepEqual(stripThink('Hi <think>x</think>there'), { text: 'Hi there', thinking: false });
  // Cut mid-tag while streaming: hold the partial tag back.
  assert.deepEqual(stripThink('<thi', true), { text: '', thinking: false });
  assert.deepEqual(stripThink('Done.<|im_e', true), { text: 'Done.', thinking: false });
  assert.deepEqual(stripThink('<think>a</thi', true), { text: '', thinking: true });
  assert.deepEqual(stripThink('reasoning</think>Answer'), { text: 'Answer', thinking: false });
  assert.deepEqual(stripThink('A < B', true), { text: 'A < B', thinking: false });
  assert.deepEqual(stripThink('A <', true), { text: 'A ', thinking: false });
  assert.deepEqual(stripThink('A <'), { text: 'A <', thinking: false });
});

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
