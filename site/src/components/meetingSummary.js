// parseSummary(): the assistant's Markdown summary (llmPrompt.js SUMMARY_FORMAT) →
// the structure the device's Meeting tab shows. Pure, for Node tests.
//
// The models follow the layout only roughly, so this is forgiving:
// - Headings are matched by their words (## / ### / **bold** / "Label:" lines), and
//   a repeated one (SUMMARY_PREFIX plus the model's own "## Summary") just carries on
//   the same section. Unknown headings are skipped with their content.
// - Items are bullets (-, *, +, •, 1.); an indented line or sub-bullet joins the item
//   above it; a plain line in a list section is an item of its own.
// - Each item's last [mm:ss] citation becomes its `t`; all citations and Markdown are
//   stripped from the text, which is transliterated to ASCII (ascii.js).
// - Placeholders ("None recorded.", "None.", "No decisions were made") are dropped by a
//   strict whole-item match, so a real item like "No budget increase" survives.
// → { summary, sections: { key, decisions, actions, questions } } (items { text, t }),
//   every section present, possibly [].

import { toAscii } from './ascii.js';
import { SECTION_KEYS } from './meetingContract.js';
import { fmtTs, parseCitations } from './llmPrompt.js';

// Heading text (lowercased, letters only) → section. First match wins.
const HEADINGS = [
  ['summary', /^(summary|overview|tl ?dr|meeting summary|executive summary)$/],
  ['key', /\b(key points?|key takeaways?|takeaways?|highlights|main points?|discussion( points)?|points discussed)\b/],
  ['decisions', /\bdecisions?( made)?\b/],
  ['actions', /\b(action items?|actions?|next steps|to ?dos?|tasks|follow ?ups?)\b/],
  ['questions', /\b(open questions?|questions?|unresolved|open issues|outstanding)\b/],
];

function sectionOf(heading) {
  const h = String(heading)
    .toLowerCase()
    .replace(/[^a-z ]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
  if (!h) return null;
  for (const [key, re] of HEADINGS) if (re.test(h)) return key;
  return undefined; // a heading, but not one of ours
}

// A whole item that says "nothing here".
const NOUNS = '(decisions?|action items?|actions?|open questions?|questions?|key points?|points?|items?|tasks?|next steps)';
const VERBS = '(recorded|noted|mentioned|made|raised|identified|discussed|assigned|given|stated|specified|so far|yet|at this (time|point)|in the transcript)';
const PLACEHOLDER = new RegExp(
  [
    `^(none|nothing|nil|n/?a|-+)( (were|was|have been|has been))?( ${VERBS})*$`,
    `^no ${NOUNS}( (were|was|have been|has been))?( ${VERBS})*$`,
    `^there (were|are) no ${NOUNS}( ${VERBS})*$`,
    '^(not (mentioned|specified|recorded|discussed|stated))$',
    "^the transcript (doesn't|does not) say$",
    '^only if any were raised$',
  ].join('|'),
);

export function isPlaceholder(text) {
  const t = String(text)
    .toLowerCase()
    .replace(/[‘’]/g, "'")
    .replace(/[“”"()[\]*_]/g, '')
    .replace(/^[\s'.!:;,-]+|[\s'.!:;,]+$/g, '')
    .replace(/\s+/g, ' ');
  return !t || PLACEHOLDER.test(t);
}

// Lists of times in one pair of brackets ("[03:12, 04:00]"), which parseCitations
// doesn't read, as one citation each ("[03:12] [04:00]").
const TIME = '(?:\\d+:)?\\d{1,2}:\\d{2}';
const SEP = '\\s*(?:[,;]|and)\\s*';
const CITE_LIST_RE = new RegExp(`\\[\\s*(${TIME}(?:${SEP}${TIME})+)\\s*\\]`, 'g');
const splitLists = (s) =>
  String(s).replace(CITE_LIST_RE, (_, inner) =>
    inner
      .split(new RegExp(SEP))
      .map((x) => `[${x}]`)
      .join(' '),
  );

// The item's time: its last citation (a range's start), as "mm:ss" / "h:mm:ss".
function lastCite(text) {
  const cites = parseCitations(splitLists(text));
  return cites.length ? fmtTs(cites[cites.length - 1].sec) : '';
}

// Markdown and citations out, ASCII, tidy spacing.
export function cleanText(text) {
  let s = splitLists(text);
  for (const c of parseCitations(s).reverse()) s = s.slice(0, c.start) + s.slice(c.end);
  s = s
    .replace(/!?\[([^\]]*)\]\([^)]*\)/g, '$1') // [link](url)
    .replace(/`+([^`]*)`+/g, '$1')
    .replace(/(\*\*|__|~~)(.+?)\1/g, '$2')
    .replace(/(^|[\s(])[*_]([^*_\s][^*_]*?)[*_](?=[\s).,;:!?]|$)/g, '$1$2')
    .replace(/\*\*|~~/g, '')
    .replace(/^\s*>\s?/, '');
  s = toAscii(s)
    .replace(/\(\s*[,;]*\s*\)/g, '') // "( )" left by "([03:12], [04:00])"
    .replace(/\s+([.,;:!?])/g, '$1')
    .replace(/([,;:])(?=[.,;:])/g, '')
    .replace(/\s+/g, ' ')
    .trim();
  return s;
}

const BULLET_RE = /^(\s*)(?:[-*+•]|\d+[.)])\s+(.*)$/;
const HEADING_RE = /^\s{0,3}#{1,6}(?:\s+|(?=[A-Za-z]))(.*?)\s*#*\s*$/;
const BOLD_HEADING_RE = /^\s*(?:\*\*|__)([^*_]+?)(?:\*\*|__)\s*:?\s*$/;
const LABEL_HEADING_RE = /^\s*([A-Za-z][A-Za-z ]{1,30}):\s*$/;

// A line that is a heading → its section key, null (empty), undefined (unknown
// heading); or false when it isn't a heading at all.
function headingOf(line) {
  let m = HEADING_RE.exec(line);
  if (m) return sectionOf(m[1]);
  m = BOLD_HEADING_RE.exec(line) || LABEL_HEADING_RE.exec(line);
  if (m) {
    const key = sectionOf(m[1]);
    if (key) return key; // a bold line that isn't a section name is content
  }
  return false;
}

export function parseSummary(md) {
  const summary = [];
  const raw = Object.fromEntries(SECTION_KEYS.map((k) => [k, []])); // [{ text }] before cleaning
  let cur = 'summary'; // text before any heading is the summary (the prefix starts it anyway)
  let last = null; // the item a continuation line joins
  for (const line of String(md || '').replace(/\r/g, '').split('\n')) {
    if (!line.trim() || /^\s*([-*_])\s*(\1\s*){2,}$/.test(line)) {
      last = null;
      continue;
    }
    const h = headingOf(line);
    if (h !== false) {
      cur = h || null;
      last = null;
      continue;
    }
    if (!cur) continue; // under a heading we don't know
    const b = BULLET_RE.exec(line);
    const indent = (b ? b[1] : /^\s*/.exec(line)[0]).replace(/\t/g, '  ').length;
    const body = (b ? b[2] : line).trim();
    if (cur === 'summary') {
      summary.push(body);
      continue;
    }
    if (last && indent >= 2) {
      // wrapped continuation, or a sub-bullet: part of the item above
      last.text += b && !/:\s*$/.test(last.text) ? `; ${body}` : ` ${body}`;
      continue;
    }
    last = { text: body };
    raw[cur].push(last);
  }
  const sections = {};
  for (const k of SECTION_KEYS) {
    sections[k] = [];
    for (const it of raw[k]) {
      const text = cleanText(it.text).replace(/[.;,]+$/, '');
      if (isPlaceholder(text)) continue;
      sections[k].push({ text, t: lastCite(it.text) });
    }
  }
  const sum = cleanText(summary.join(' '));
  return { summary: isPlaceholder(sum) ? '' : sum, sections };
}
