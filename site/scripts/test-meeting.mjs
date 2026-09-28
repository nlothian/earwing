// Tests for the live meeting summary sent to the device: the Markdown parser,
// the diff/feed builder, ASCII transliteration and the payload's caps.
// npm run test:meeting
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as C from '../src/components/meetingContract.js';
import { toAscii } from '../src/components/ascii.js';
import { cleanText, isPlaceholder, parseSummary } from '../src/components/meetingSummary.js';
import { buildPayload, clip, diffSummary, similarity, tokens } from '../src/components/meetingDiff.js';
import { SUMMARY_PREFIX } from '../src/components/llmPrompt.js';

let failed = 0;
function test(name, fn) {
  try {
    fn();
    console.log(`ok   ${name}`);
  } catch (e) {
    failed++;
    console.log(`FAIL ${name}\n     ${e.stack.split('\n').slice(0, 3).join('\n     ')}`);
  }
}

const sample = JSON.parse(readFileSync(new URL('./fixtures/meeting-sample.json', import.meta.url), 'utf8'));

test('fixture follows the contract', () => {
  assert.equal(sample.v, C.MEETING_VERSION);
  assert.match(sample.session, /^[A-Za-z0-9-]{1,23}$/);
  assert.deepEqual(Object.keys(sample.sections), C.SECTION_KEYS);
  for (const e of sample.feed) {
    assert.ok(C.FEED_SECS.includes(e.sec));
    assert.ok(C.FEED_KINDS.includes(e.kind));
  }
  assert.ok(JSON.stringify(sample).length <= C.MAX_BODY);
});

// --- toAscii -------------------------------------------------------------------------
test('toAscii: the lookup table', () => {
  const cases = [
    ['‘single’ “double” „low“ «guillemets»', `'single' "double" "low" "guillemets"`],
    ['a–b — c ‒ d − e', 'a-b - c - d - e'],
    ['wait…', 'wait...'],
    ['• one · two', '* one * two'],
    ['10\u00A0000 and\u202Fthin\u2009space', '10 000 and thin space'],
    ['a → b ← c ⇒ d ↔ e', 'a -> b <- c => d <-> e'],
    ['x ≥ 3, y ≤ 4, z ≠ 5', 'x >= 3, y <= 4, z != 5'],
    ['€5 and £3', 'EUR5 and GBP3'],
    ['Straße', 'Strasse'],
    ['Æsir æther Œuvre œil', 'AEsir aether OEuvre oeil'],
    ['zero\u200Bwidth soft\u00ADhyphen', 'zerowidth softhyphen'],
  ];
  for (const [inp, out] of cases) assert.equal(toAscii(inp), out, inp);
});

test('toAscii: accents dropped, the rest ?, whitespace collapsed', () => {
  assert.equal(toAscii('Zoë’s café in Zürich, São Paulo, Kraków'), "Zoe's cafe in Zurich, Sao Paulo, Krakow");
  assert.equal(toAscii('ﬁne ½ ™'), 'fine 1/2 (TM)'); // NFKD forms, and the table
  assert.equal(toAscii('ok 👍 then 日本'), 'ok ? then ??'); // one ? per character, emoji too
  assert.equal(toAscii('  tabs\tand\nnewlines\r\n  '), 'tabs and newlines');
  assert.equal(toAscii(null), '');
  for (const ch of toAscii('Ω≈∞ “x” — ok')) assert.ok(ch >= ' ' && ch <= '~');
});

// --- parser --------------------------------------------------------------------------
// What llm.mock.worker.js answers to a summary request (answerFor), verbatim.
const MOCK_MD = [
  '## Summary',
  '(Mock model.) The conversation covers 12 lines, from [00:01] to [01:30]. It opens with “we should ship the budget review by friday”.',
  '',
  '## Key points',
  '- we should ship the budget review by friday and [00:01]',
  '- alice will draft the plan for the team we [00:24]',
  '- the budget review by friday and alice will draft [00:47]',
  '- plan for the team we should ship the budget [01:10]',
  '',
  '## Decisions',
  'None recorded.',
  '',
  '## Action items',
  '- Alice: follow up on “alice will draft the plan for the team we” [00:24]',
].join('\n');

test('parser: the mock worker’s summary', () => {
  const p = parseSummary(MOCK_MD);
  assert.equal(
    p.summary,
    '(Mock model.) The conversation covers 12 lines, from to. It opens with "we should ship the budget review by friday".',
  );
  assert.equal(p.sections.key.length, 4);
  assert.deepEqual(p.sections.key[1], { text: 'alice will draft the plan for the team we', t: '00:24' });
  assert.deepEqual(p.sections.decisions, []);
  assert.deepEqual(p.sections.actions, [{ text: 'Alice: follow up on "alice will draft the plan for the team we"', t: '00:24' }]);
  assert.deepEqual(p.sections.questions, []);
  // The real worker starts the answer with SUMMARY_PREFIX; the mock writes its own too.
  assert.deepEqual(parseSummary(SUMMARY_PREFIX + MOCK_MD), p);
});

// Realistic LFM2.5 output: the prefix plus its own "## Summary", bold, citations in
// every style, placeholders in quotes, sub-bullets, a numbered list, headings in
// three styles and a section it made up.
const LFM_MD = `${SUMMARY_PREFIX}## Summary
The team reviewed the **Q3 budget** [03:12] and agreed to move the launch [09:40].
Alice will send a revised budget [11:02].

## Key points
- **Hardware costs** are about 10% over budget [03:12]
- The enclosure passed the drop test [05:40]
  on its second revision [05:55]
* No budget increase this quarter [06:10–06:30]
- Vendor quotes arrive next week [07:15, 07:40]

### Decisions
- "None recorded."

## Attendees
- Alice, Bob, Carol

Action Items:
1. Alice: send the revised budget to Bob by Friday [11:02].
2. Bob: book the room ([12:00], [12:05])
3. **Carol**:
   - draft the plan [13:00]
   - review it with _Dan_ [13:30]

**Open questions**
- None.
`;

test('parser: LFM-style Markdown', () => {
  const p = parseSummary(LFM_MD);
  assert.equal(p.summary, 'The team reviewed the Q3 budget and agreed to move the launch. Alice will send a revised budget.');
  assert.deepEqual(p.sections.key, [
    { text: 'Hardware costs are about 10% over budget', t: '03:12' },
    { text: 'The enclosure passed the drop test on its second revision', t: '05:55' },
    { text: 'No budget increase this quarter', t: '06:10' },
    { text: 'Vendor quotes arrive next week', t: '07:40' },
  ]);
  assert.deepEqual(p.sections.decisions, []);
  assert.deepEqual(p.sections.actions, [
    { text: 'Alice: send the revised budget to Bob by Friday', t: '11:02' },
    { text: 'Bob: book the room', t: '12:05' },
    { text: 'Carol: draft the plan; review it with Dan', t: '13:30' },
  ]);
  assert.deepEqual(p.sections.questions, []);
});

test('parser: missing sections are [], text before a heading is the summary', () => {
  const p = parseSummary('Quick sync about the demo [00:10].\n\n## Key points\n- Demo is Thursday [00:20]');
  assert.equal(p.summary, 'Quick sync about the demo.');
  assert.deepEqual(p.sections.key, [{ text: 'Demo is Thursday', t: '00:20' }]);
  for (const k of ['decisions', 'actions', 'questions']) assert.deepEqual(p.sections[k], []);
  assert.deepEqual(parseSummary(''), { summary: '', sections: { key: [], decisions: [], actions: [], questions: [] } });
});

test('parser: heading variants', () => {
  const p = parseSummary(
    '## Overview\nShort.\n## Main points\n- a [00:01]\n### Decisions made\n- b\n## Next steps\n- c\n## Unresolved\n- d?\n',
  );
  assert.equal(p.summary, 'Short.');
  assert.deepEqual(
    C.SECTION_KEYS.map((k) => p.sections[k].map((i) => i.text)),
    [['a'], ['b'], ['c'], ['d?']],
  );
});

test('parser: placeholders by strict whole-item match', () => {
  for (const s of [
    'None recorded.',
    '"None recorded."',
    'None.',
    'none so far',
    'N/A',
    '(none)',
    '-',
    'No decisions were made.',
    'No action items recorded',
    'No open questions were raised.',
    'There were no decisions.',
    'Nothing yet',
    "The transcript doesn't say.",
    'Only if any were raised',
  ]) {
    assert.ok(isPlaceholder(s), s);
  }
  for (const s of [
    'No budget increase',
    'No launch before QA signs off',
    'None of the vendors replied',
    'Nothing ships without the driver fix',
    'Noted: budget is tight',
  ]) {
    assert.ok(!isPlaceholder(s), s);
  }
});

test('parser: Markdown and citations stripped from text', () => {
  assert.equal(cleanText('**Alice**: send `budget.xlsx` to [Bob](mailto:bob) ~~today~~ [01:02]'), 'Alice: send budget.xlsx to Bob today');
  assert.equal(cleanText('a *really* _big_ snake_case deal [1:02:03]'), 'a really big snake_case deal');
  assert.equal(cleanText('Zoë’s café — 5 € [00:01–00:09]'), "Zoe's cafe - 5 EUR");
  assert.equal(parseSummary('## Key points\n- over an hour in [1:02:03]').sections.key[0].t, '1:02:03');
});

// --- similarity and the diff ---------------------------------------------------------
const it = (text, t = '') => ({ text, t });
const P = (sections = {}, summary = '') => ({
  summary,
  sections: { key: [], decisions: [], actions: [], questions: [], ...sections },
});
// Run a sequence of parsed summaries through diffSummary (60 s apart by default).
function play(list, { stepMs = 60000 } = {}) {
  let st = null;
  const out = [];
  list.forEach((p, i) => {
    st = diffSummary(st, p, { rev: i + 1, nowMs: i * stepMs, coversTo: `0${i}:00` });
    out.push(st);
  });
  return out;
}
const idsOf = (st, sec) => st.items[sec].map((x) => x.id);

test('similarity: stopwords and stems, +0.1 for the same time', () => {
  assert.deepEqual([...tokens('The budgets were reviewed, and moving is decided')].sort(), ['budget', 'decid', 'mov', 'review']);
  const a = { toks: tokens('Hardware costs are about 10% over the Q3 budget'), t: '03:12' };
  const b = { toks: tokens('Hardware costs are roughly 10% over the Q3 budget'), t: '03:12' };
  assert.ok(similarity(a, b) >= 0.85);
  assert.ok(similarity(a, { ...b, t: '' }) < similarity(a, b));
});

test('diff: a reworded item is the same item, a changed one is edited', () => {
  const [r1, r2, r3] = play([
    P({ key: [it('Hardware costs are about 10% over the Q3 budget', '03:12')], decisions: [it('Launch moves to 14 Oct', '09:40')], actions: [it('Ship it')] }),
    P({ key: [it('Hardware costs are roughly 10% over the Q3 budget', '03:12')], decisions: [it('Launch moves to 14 Oct', '09:40')], actions: [it('Ship it now')] }),
    P({ key: [it('Hardware costs are roughly 10% over the Q3 budget', '03:12')], decisions: [it('Launch moves to 21 Oct pending QA sign-off', '09:40')], actions: [it('Ship it now')] }),
  ]);
  assert.equal(r2.changes, 0, 'rewording is not news');
  assert.deepEqual(idsOf(r2, 'key'), idsOf(r1, 'key'));
  assert.deepEqual(idsOf(r2, 'actions'), idsOf(r1, 'actions'), 'one word more: still the same item');
  assert.equal(r2.feed.find((e) => e.sec === 'key').text, 'Hardware costs are roughly 10% over the Q3 budget', 'entries follow the wording');
  assert.equal(r3.changes, 1);
  assert.deepEqual(idsOf(r3, 'decisions'), idsOf(r1, 'decisions'));
  assert.equal(r3.feed[0].kind, 'edited');
  assert.equal(r3.feed[0].text, 'Launch moves to 21 Oct pending QA sign-off');
  assert.equal(r3.feed.filter((e) => e.sec === 'decisions').length, 1, 'one entry per item');
});

test('diff: a shuffle across sections feeds nothing', () => {
  const a = it('Budget is ten percent over on hardware', '03:12');
  const b = it('The enclosure passed the drop test', '05:40');
  const c = it('Launch moves to the fourteenth of October', '09:40');
  const [r1, r2] = play([P({ key: [a, b], decisions: [c] }), P({ key: [c, b], decisions: [a], questions: [] })]);
  assert.equal(r2.changes, 0);
  assert.deepEqual(r2.feed.map((e) => e.id), r1.feed.map((e) => e.id));
  assert.equal(r2.items.decisions[0].id, r1.items.key[0].id, 'a moved item keeps its id');
  assert.equal(r2.feed.find((e) => e.id === r1.items.key[0].id).sec, 'decisions', 'its entry follows it');
});

test('diff: a drop-and-re-add flap feeds nothing (tombstone)', () => {
  const x = it('Alice sends the revised budget to Bob by Friday', '11:02');
  const y = it('Bob books the big meeting room for the review', '12:00');
  const y2 = it('Bob books the big meeting room for the design review', '12:00');
  const [r1, r2, r3] = play([P({ actions: [x, y] }), P({ actions: [x] }), P({ actions: [x, y2] })]);
  assert.equal(r2.changes, 0, 'removed items are never fed');
  assert.equal(r2.tombs.length, 1);
  assert.equal(r3.changes, 0, 'back from the dead is not news');
  assert.equal(r3.items.actions[1].id, r1.items.actions[1].id);
  assert.equal(r3.tombs.length, 0);
  // Gone for longer than TOMB_REVS: new again.
  const empty = P({ actions: [x] });
  const runs = play([P({ actions: [x, y] }), empty, empty, empty, empty, empty, empty, empty, P({ actions: [x, y] })]);
  assert.equal(runs.at(-1).changes, 1);
  assert.equal(runs.at(-1).feed[0].kind, 'new');
});

test('diff: the first rev seeds the feed D, A, Q, K, then the summary', () => {
  const [r1] = play([
    P({ key: [it('k1')], decisions: [it('d1')], actions: [it('a1'), it('a2')], questions: [it('q1?')] }, 'We met.'),
  ]);
  assert.deepEqual(r1.feed.map((e) => e.text), ['d1', 'a1', 'a2', 'q1?', 'k1', 'We met.']);
  assert.ok(r1.feed.every((e) => e.rev === 1 && (e.kind === 'new' || e.kind === 'summary')));
  assert.equal(r1.feed.at(-1).t, '00:00', 'a summary entry is timed by coversTo');
});

test('diff: summary entries (reads differently, 90 s apart, only one)', () => {
  const s1 = 'The team reviewed the budget and agreed to delay the launch.';
  const s2 = 'Vendors were compared; Acme wins on price, Beta on support, and a pilot starts soon.';
  const s3 = 'Hiring for the firmware role is paused until the funding round closes next month.';
  const steps = [
    [0, s1],
    [30000, s2], // too soon
    [100000, s3], // reads differently, 100 s after s1
    [300000, `${s3} Everyone agreed.`], // too similar
  ];
  let st = null;
  const fed = [];
  steps.forEach(([ms, text], i) => {
    st = diffSummary(st, P({}, text), { rev: i + 1, nowMs: ms, coversTo: '' });
    fed.push(st.changes);
  });
  assert.deepEqual(fed, [1, 0, 1, 0]);
  assert.equal(st.feed.filter((e) => e.kind === 'summary').length, 1);
  assert.equal(st.feed[0].text, s3);
  assert.equal(st.summary, `${s3} Everyone agreed.`, 'the payload summary is always the newest');
});

test('diff: lvl fades by change sets, not revs; new marks the latest set', () => {
  const k = (n) => Array.from({ length: n }, (_, i) => it(`point number ${i} about topic ${String.fromCharCode(97 + i)}`));
  const runs = play([P({ key: k(1) }), P({ key: k(2) }), P({ key: k(2) }), P({ key: k(3) }), P({ key: k(4) }), P({ key: k(5) })]);
  const pay = (st) => buildPayload(st, { session: 's-1', seq: 1, hb: false, nowMs: 0 });
  assert.deepEqual(pay(runs[1]).feed.map((e) => e.lvl), [0, 1]);
  assert.deepEqual(pay(runs[2]).feed.map((e) => e.lvl), [0, 1], 'a rev with no changes fades nothing');
  assert.deepEqual(pay(runs[2]).sections.key.map((x) => x.new), [false, true], 'nor clears the highlight');
  assert.deepEqual(pay(runs[5]).feed.map((e) => e.lvl), [0, 1, 2, 3, 3]);
});

test('diff: the feed keeps 12, newest first, and room for a fresh summary', () => {
  const many = Array.from({ length: 7 }, (_, i) => it(`decision ${i} on the ${['alpha', 'beta', 'gamma', 'delta', 'epsilon', 'zeta', 'eta'][i]} work`));
  const acts = Array.from({ length: 7 }, (_, i) => it(`action ${i} for ${['ann', 'ben', 'cat', 'dan', 'eve', 'fay', 'gus'][i]}`));
  const [r1, r2] = play([
    P({ decisions: many, actions: acts }, 'A long meeting.'),
    P({ decisions: many, actions: acts, questions: [it('Who owns the budget?')] }, 'A long meeting.'),
  ]);
  assert.equal(r1.feed.length, C.FEED_MAX);
  assert.equal(r1.feed.at(-1).kind, 'summary');
  assert.equal(r2.feed.length, C.FEED_MAX);
  assert.equal(r2.feed[0].text, 'Who owns the budget?');
});

// --- payload -------------------------------------------------------------------------
const shape = (v) =>
  Array.isArray(v) ? (v.length ? [shape(v[0])] : []) : v && typeof v === 'object'
    ? Object.fromEntries(Object.entries(v).map(([k, x]) => [k, shape(x)]))
    : typeof v;

test('payload: same shape as the fixture, end to end from Markdown', () => {
  const st = diffSummary(null, parseSummary(LFM_MD), { rev: 1, nowMs: 5000, coversTo: '12:34' });
  const p = buildPayload(st, { session: 'q7k2xa-3', seq: 42, hb: false, nowMs: 9000 });
  assert.deepEqual(Object.keys(p), Object.keys(sample));
  assert.deepEqual(shape(p.sections.key[0]), shape(sample.sections.key[0]));
  assert.deepEqual(shape(p.feed[0]), shape(sample.feed[0]));
  for (const k of ['v', 'session', 'rev', 'seq', 'hb', 'coversTo', 'summary']) assert.equal(typeof p[k], typeof sample[k], k);
  assert.deepEqual(Object.keys(p.sections), C.SECTION_KEYS);
  assert.equal(p.feed[0].ago, 4);
  assert.ok(p.sections.key.every((x) => x.new));
  for (const e of p.feed) assert.ok(C.FEED_SECS.includes(e.sec) && C.FEED_KINDS.includes(e.kind));
  assert.match(JSON.stringify(p), /^[\x20-\x7e]*$/, 'printable ASCII only');
});

test('payload: caps, and the body stays under MAX_BODY', () => {
  const long = (n, w) => Array.from({ length: n }, (_, i) => `${w}${i}`).join(' ');
  const sec = (tag) => Array.from({ length: 15 }, (_, i) => it(`${tag} ${i} ${long(80, tag + i)}`, '123:45:67:89'));
  const parsed = P({ key: sec('k'), decisions: sec('d'), actions: sec('a'), questions: sec('q') }, long(400, 's'));
  const st = diffSummary(null, parsed, { rev: 1, nowMs: 0, coversTo: '1:23:45' });
  const p = buildPayload(st, { session: 'bad session!/with a very long name-12', seq: 1, hb: true, nowMs: 1000 });
  const body = JSON.stringify(p);
  assert.ok(Buffer.byteLength(body) <= C.MAX_BODY, `${Buffer.byteLength(body)} bytes`);
  assert.match(p.session, /^[A-Za-z0-9-]{1,23}$/);
  assert.ok(p.summary.length <= C.SUMMARY_MAX && p.summary.endsWith('...'));
  assert.ok(p.feed.length <= C.FEED_MAX);
  for (const k of C.SECTION_KEYS) {
    assert.ok(p.sections[k].length <= C.ITEMS_MAX);
    for (const x of p.sections[k]) assert.ok(x.text.length <= C.TEXT_MAX && x.t.length <= C.T_MAX);
  }
  for (const e of p.feed) assert.ok(e.text.length <= C.FEED_TEXT_MAX && e.lvl >= 0 && e.lvl <= C.LVL_MAX);
  assert.equal(p.hb, true);
  assert.equal(clip('one two three four', 12), 'one two...');
});

if (failed) {
  console.log(`\n${failed} failed`);
  process.exit(1);
}
console.log('\nall passed');
