// A fake llm.worker.js for developing the assistant UI without a GPU or a 1.5 GB
// download: `npm run dev`, then open the page with `?llm=mock` (llmClient.js only
// creates it on the dev server; it never ships in a build). Speaks the full worker
// protocol (see the contract in llm.worker.js / llmClient.js):
//
// - load: a plan, then progress over ~2 s, then ready.
// - generate: exact-prefix cache reuse over the message text, prefill in 512-token
//   chunks (~0.1 s each), then a canned Markdown answer streamed at ~25 tok/s. The
//   answer cites real times picked from the transcript lines in the prompt.
// - honours pause / resume / abort / reset-cache / unload; count = chars / 3.5.
// - a question containing "#fail" errors (code 'other'); "#gpu" loses the device.
//
// No imports: the dev server serves this file as-is.

const TOK_CHARS = 3.5;
const PREFILL_CHUNK = 512;
const PREFILL_MS = 100; // per chunk
const TOKEN_MS = 40; // ~25 tok/s

let loaded = null;
let loadGen = 0; // bumped by each load/unload, so an unload stops a load in progress
let cacheText = ''; // what the "KV cache" holds (message text + raw answers)
let paused = false;
let aborted = false;
let busy = false;
let wake = null;

const post = (m) => self.postMessage(m);
const tokens = (s) => Math.ceil(s.length / TOK_CHARS);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function gate() {
  while (paused && !aborted) {
    await new Promise((r) => {
      wake = r;
    });
  }
}

const SIZES = { large: 1_550_000_000, small: 760_000_000 };

async function load({ model, local }) {
  const g = ++loadGen;
  loaded = null;
  const bytes = SIZES[model] || SIZES.large;
  post({
    type: 'plan',
    model,
    bytes,
    source: local ? 'local' : 'hf-download',
    folder: local ? local.name : undefined,
  });
  const steps = 20;
  for (let i = 1; i <= steps; i++) {
    await sleep(100);
    if (g !== loadGen) return;
    post({ type: 'progress', progress: i / steps, loaded: (bytes * i) / steps, total: bytes });
  }
  loaded = model;
  cacheText = '';
  post({ type: 'ready', model, adapter: 'mock (no GPU)' });
}

// The chat template, near enough: what the cache is compared against.
const render = (messages) => messages.map((m) => `<|im_start|>${m.role}\n${m.content}<|im_end|>\n`).join('');

function stamps(text) {
  const out = [];
  for (const m of text.matchAll(/^\[((?:\d+:)?\d{1,2}:\d{2})\] (?:([^:\n]{1,40}): )?(.*)$/gm)) {
    out.push({ ts: m[1], who: m[2] || null, text: m[3] });
  }
  return out;
}

const pick = (list, n) => {
  if (list.length <= n) return list;
  const out = [];
  for (let i = 0; i < n; i++) out.push(list[Math.floor(((i + 0.5) * list.length) / n)]);
  return out;
};

const gist = (l) =>
  l.text
    .split(/\s+/)
    .slice(0, 9)
    .join(' ')
    .replace(/[.,;:!?]+$/, '');

function answerFor(messages) {
  const all = messages.map((m) => m.content).join('\n');
  const last = messages[messages.length - 1].content;
  const lines = stamps(all);
  const cite = (l) => `[${l.ts}]`;
  if (/Write compact notes/.test(last)) {
    const mine = stamps(last);
    return pick(mine, 4)
      .map((l) => `- ${l.who ? `${l.who}: ` : ''}${gist(l)} ${cite(l)}`)
      .join('\n');
  }
  if (/Summarise the transcript|Rewrite the full summary|Merge them into one summary/.test(last)) {
    const src = /Merge them/.test(last)
      ? [...last.matchAll(/^- (?:([^:\n]{1,40}): )?(.*) \[((?:\d+:)?\d{1,2}:\d{2})\]$/gm)].map((m) => ({
          who: m[1] || null,
          text: m[2],
          ts: m[3],
        }))
      : lines;
    if (!src.length) return "The transcript doesn't say.";
    const kp = pick(src, 4);
    const who = kp.find((l) => l.who);
    return [
      '## Summary',
      `(Mock model.) The conversation covers ${src.length} lines, from ${cite(src[0])} to ${cite(src[src.length - 1])}. ` +
        `It opens with “${gist(src[0])}”.`,
      '',
      '## Key points',
      ...kp.map((l) => `- ${gist(l)} ${cite(l)}`),
      '',
      '## Decisions',
      'None recorded.',
      '',
      '## Action items',
      who ? `- ${who.who}: follow up on “${gist(who)}” ${cite(who)}` : `- Unassigned: follow up ${cite(kp[0])}`,
    ].join('\n');
  }
  const q = (last.match(/Question: (.*)$/s) || [])[1] || '';
  if (!lines.length) return "The transcript doesn't say.";
  const words = q
    .toLowerCase()
    .split(/\W+/)
    .filter((w) => w.length > 3);
  const hit = lines.find((l) => words.some((w) => l.text.toLowerCase().includes(w)));
  if (!hit) return "The transcript doesn't say.";
  return `(Mock model.) ${hit.who ? `${hit.who} said` : 'It was said'}: “${gist(hit)}” ${cite(hit)}.`;
}

// Mock "tokens": words with their trailing space.
const toTokens = (text) => text.match(/\S+\s*|\s+/g) || [];

async function generate({ id, messages, maxTokens = 400, reuseCache = true }) {
  if (!loaded) {
    post({ type: 'error', error: 'No model loaded.', during: 'generate', code: 'other' });
    return;
  }
  busy = true;
  aborted = false;
  const prompt = `${render(messages)}<|im_start|>assistant\n`;
  const promptTokens = tokens(prompt);
  const reusable = reuseCache && cacheText && prompt.startsWith(cacheText) ? cacheText : '';
  const reusedTokens = tokens(reusable);
  const t0 = performance.now();
  const last = messages[messages.length - 1].content;

  let done = reusedTokens;
  post({ type: 'prefill', id, done, total: promptTokens, reused: reusedTokens });
  while (done < promptTokens) {
    await gate();
    if (aborted) break;
    await sleep(PREFILL_MS);
    done = Math.min(promptTokens, done + PREFILL_CHUNK);
    post({ type: 'prefill', id, done, total: promptTokens, reused: reusedTokens });
  }
  if (!aborted && /#gpu\b/.test(last)) {
    loaded = null;
    cacheText = '';
    busy = false;
    post({ type: 'error', error: 'GPU device was lost (mock).', during: 'generate', code: 'gpu' });
    return;
  }
  if (!aborted && /#fail\b/.test(last)) {
    busy = false;
    post({ type: 'error', error: 'Generation failed (mock).', during: 'generate', code: 'other' });
    return;
  }
  // The cache now holds the prompt (or the part prefilled before an abort).
  cacheText = aborted ? '' : prompt;
  const prefillMs = performance.now() - t0;
  const t1 = performance.now();
  let raw = '';
  let newTokens = 0;
  let reason = aborted ? 'aborted' : 'eos';
  if (!aborted) {
    const toks = toTokens(answerFor(messages));
    for (const t of toks) {
      await gate();
      if (aborted) {
        reason = 'aborted';
        break;
      }
      if (newTokens >= maxTokens) {
        reason = 'length';
        break;
      }
      await sleep(TOKEN_MS);
      raw += t;
      newTokens += 1;
      post({ type: 'token', id, text: t });
    }
    cacheText = prompt + raw;
  }
  busy = false;
  post({
    type: 'done',
    id,
    raw,
    reason,
    promptTokens,
    reusedTokens,
    newTokens,
    prefillMs: Math.round(prefillMs),
    genMs: Math.round(performance.now() - t1),
  });
}

self.onmessage = (e) => {
  const msg = e.data || {};
  switch (msg.type) {
    case 'load':
      load(msg);
      break;
    case 'generate':
      if (busy) post({ type: 'error', error: 'Already generating.', during: 'generate', code: 'other' });
      else generate(msg);
      break;
    case 'count':
      post({ type: 'count', id: msg.id, tokens: tokens(String(msg.text || '')) });
      break;
    case 'abort':
      aborted = true;
      if (wake) wake();
      break;
    case 'pause':
      if (!paused) {
        paused = true;
        post({ type: 'paused' });
      }
      break;
    case 'resume':
      if (paused) {
        paused = false;
        if (wake) wake();
        post({ type: 'resumed' });
      }
      break;
    case 'reset-cache':
      cacheText = '';
      break;
    case 'unload':
      aborted = true;
      if (wake) wake();
      loadGen += 1;
      loaded = null;
      cacheText = '';
      post({ type: 'unloaded' });
      break;
    default:
      break;
  }
};
