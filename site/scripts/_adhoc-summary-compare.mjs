// Scratch: summarise a plain "Speaker N: text" transcript with the real worker, once
// verbatim and once map-reduced, and show which times each summary cites.
//   node scripts/_adhoc-summary-compare.mjs <transcript.txt> [--model small|large] [--minutes 60] [--chunk 2000]
import { openAsBlob, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { AutoTokenizer, env } from '@huggingface/transformers';
import { LLMS } from '../src/components/models.js';
import {
  assembleSummary,
  chunkLines,
  firstUserMessage,
  fmtTs,
  linesText,
  MAX_TOKENS,
  mapMessages,
  parseCitations,
  reduceMessages,
  NOTES_PREFIX,
  stripThink,
  trimCutNotes,
  SUMMARY_CHUNK_TOKENS,
  SUMMARY_PREFIX,
  SUMMARY_REQUEST,
  SYSTEM_PROMPT,
} from '../src/components/llmPrompt.js';

const SITE = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const MODELS = resolve(SITE, '..', 'models');
const args = process.argv.slice(2);
const opt = (n, d) => (args.includes(n) ? args[args.indexOf(n) + 1] : d);
const file = args[0];
const key = opt('--model', 'small');
const minutes = Number(opt('--minutes', 60));
const chunkTokens = Number(opt('--chunk', SUMMARY_CHUNK_TOKENS));
const out = opt('--out', null);

// Lines: turns split at 120 words, word times spread evenly over the running time.
const turns = readFileSync(file, 'utf8')
  .split(/\n\s*\n/)
  .map((t) => t.trim())
  .filter(Boolean)
  .map((t) => {
    const m = t.match(/^([^:]{1,40}):\s*([\s\S]*)$/);
    return m ? { who: m[1], words: m[2].split(/\s+/) } : { who: null, words: t.split(/\s+/) };
  });
const W = turns.reduce((n, t) => n + t.words.length, 0);
const sec = (i) => (i * minutes * 60) / W;
const lines = [];
let wi = 0;
for (const t of turns) {
  for (let a = 0; a < t.words.length; a += 120) {
    const piece = t.words.slice(a, a + 120);
    const start = sec(wi);
    wi += piece.length;
    const text = piece.join(' ');
    lines.push({ start, end: sec(wi), text, line: `[${fmtTs(start)}] ${t.who ? `${t.who}: ` : ''}${text}` });
  }
}

const m = LLMS[key];
env.allowRemoteModels = false;
const tok = await AutoTokenizer.from_pretrained(join(MODELS, m.dir));
const count = (s) => tok.encode(s, { add_special_tokens: false }).length;
console.log(`${W} words, ${lines.length} lines, transcript ${count(linesText(lines))} tokens (exact)`);

// The worker, as in test-llm-runtime.mjs.
const messages = [];
const waiters = [];
globalThis.self = globalThis;
globalThis.__ORT_VERSION__ = JSON.parse(readFileSync(join(SITE, 'node_modules/onnxruntime-web/package.json'), 'utf8')).version;
Object.defineProperty(globalThis, 'isSecureContext', { value: true, configurable: true });
Object.defineProperty(navigator, 'gpu', {
  configurable: true,
  value: { requestAdapter: async () => ({ features: new Set(['shader-f16']), info: { vendor: 'onnxruntime-node', architecture: 'webgpu' } }) },
});
globalThis.postMessage = (msg) => {
  messages.push(msg);
  for (const w of waiters.slice()) if (w.pred(msg)) {
    waiters.splice(waiters.indexOf(w), 1);
    w.resolve(msg);
  }
};
const next = (pred) => new Promise((resolve) => waiters.push({ pred, resolve }));
env.allowRemoteModels = true;
env.useWasmCache = false;
await import('../src/components/llm.worker.js');
const send = (data) => self.onmessage({ data });
const files = {};
for (const name of Object.keys(m.files)) files[name] = await openAsBlob(join(MODELS, m.dir, name));
send({ type: 'load', model: key, local: { name: 'models', files } });
const r = await next((x) => x.type === 'ready' || (x.type === 'error' && x.during === 'load'));
if (r.type === 'error') throw new Error(r.error);

let gid = 0;
async function gen(msgs, maxTokens, prefix = '') {
  const id = `g${++gid}`;
  const start = messages.length;
  send({ type: 'generate', id, messages: msgs, maxTokens, reuseCache: false, prefix });
  const done = await next((x) => (x.type === 'done' || x.type === 'error') && x.id === id);
  if (done.type === 'error') throw new Error(done.error);
  return trimCutNotes(stripThink(done.raw).text, done.reason); // raw starts with the prefix
}

const cites = (s) => {
  const secs = parseCitations(s).map((c) => c.sec);
  const buckets = Array.from({ length: Math.ceil(minutes / 10) }, (_, i) => secs.filter((x) => x >= i * 600 && x < (i + 1) * 600).length);
  return `${secs.length} citations; per 10 min: ${buckets.join(' ')}; latest ${secs.length ? fmtTs(Math.max(...secs)) : '-'}`;
};

let report = '';
const log = (s) => {
  console.log(s);
  report += s + '\n';
};

const t0 = Date.now();
const verbatim = await gen(
  [
    { role: 'system', content: SYSTEM_PROMPT },
    { role: 'user', content: firstUserMessage(lines, SUMMARY_REQUEST) },
  ],
  MAX_TOKENS.summary,
  SUMMARY_PREFIX,
);
log(`\n=== verbatim (one prompt), ${((Date.now() - t0) / 1000).toFixed(0)} s — ${cites(verbatim)}\n${verbatim}`);

const t1 = Date.now();
const chunks = chunkLines(lines, chunkTokens);
const notes = [];
for (let i = 0; i < chunks.length; i++) notes.push(await gen(mapMessages(chunks[i], i, chunks.length), MAX_TOKENS.notes, NOTES_PREFIX));
const parts = chunks.map((c, i) => ({ start: c.start, end: c.end, notes: notes[i] }));
const merged = assembleSummary(await gen(reduceMessages(parts), MAX_TOKENS.summary, SUMMARY_PREFIX), parts);
log(`\n=== map-reduce (${chunks.length} chunks of ≤${chunkTokens} est. tokens), ${((Date.now() - t1) / 1000).toFixed(0)} s — ${cites(merged)}\n${merged}`);
notes.forEach((n, i) => log(`\n--- notes, part ${i + 1} ${fmtTs(chunks[i].start)}–${fmtTs(chunks[i].end)} — ${cites(n)}\n${n}`));
if (out) writeFileSync(out, report);
process.exit(0);
