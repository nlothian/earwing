#!/usr/bin/env node
// Tests for the assistant's runtime (src/components/llm.worker.js) against the real model
// files in <repo-root>/models (`npm run models -- llm`).
//
//   node scripts/test-llm-runtime.mjs [--model small|large|both] [--tokenizer-only]
//
// 1. Tokenizer + chat template (both models, CPU, a few seconds): decode(encode(x)) is
//    exact, and an assistant turn replayed from `raw` re-renders to text that starts with
//    what the cache holds, for finished (eos), cut-off (length/abort) and multi-turn
//    conversations; a later user message only appends.
// 2. The worker itself (default: small model; --model large|both), run in Node: the same
//    module the page bundles, with `self`/`navigator.gpu` shims. Transformers.js's Node
//    build runs it on onnxruntime-node, whose WebGPU EP works on macOS, so this exercises
//    the real load path (local-folder customCache), chunked prefill (cache length === k
//    after every chunk), streaming (token texts concatenate to `raw`), cache reuse across
//    turns, abort mid-prefill (partial prefill kept), pause/resume, count, reset-cache
//    and unload.

import assert from 'node:assert/strict';
import { existsSync, openAsBlob, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { AutoTokenizer, env } from '@huggingface/transformers';
import { LLMS } from '../src/components/models.js';

const SITE = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const MODELS = resolve(SITE, '..', 'models');
const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : dflt;
};
const tokenizerOnly = args.includes('--tokenizer-only');
const runKeys = { small: ['small'], large: ['large'], both: ['small', 'large'] }[opt('--model', 'small')];
if (!runKeys) {
  console.error('usage: test-llm-runtime.mjs [--model small|large|both] [--tokenizer-only]');
  process.exit(2);
}

let failures = 0;
async function test(name, fn) {
  const t0 = Date.now();
  try {
    await fn();
    console.log(`  ok  ${name} (${Date.now() - t0} ms)`);
  } catch (e) {
    failures++;
    console.log(`  FAIL ${name}\n       ${(e && e.stack) || e}`);
  }
}

const have = (m) => Object.entries(m.files).every(([n]) => existsSync(join(MODELS, m.dir, n)));

// ------------------------------------------------------------------ 1. tokenizer + template
// Mirrors llm.worker.js renderPrompt() / remember(): the worker keeps (text, ids) of what's
// in the cache and reuses it iff the next rendered prompt starts with that text.
const TEMPLATE_KWARGS = { preserve_thinking: true, keep_past_thinking: true };
const OPEN = '<|im_start|>assistant\n';
function render(tok, messages) {
  let text = tok.apply_chat_template(messages, { add_generation_prompt: true, tokenize: false, ...TEMPLATE_KWARGS });
  let head = text.slice(text.lastIndexOf(OPEN) + OPEN.length);
  if (head.endsWith('<think>')) {
    text += '</think>';
    head += '</think>';
  }
  return { text, head };
}
const dec = (tok, ids) => tok.decode(ids, { skip_special_tokens: false });

const SAMPLES = [
  'Plain text.',
  '  leading spaces, trailing  \n\n\ttabs\t and\r\nCRLF',
  'Ünïcödé — “quotes”, emoji 🎙️👍🏽, CJK 会议记录, RTL שלום',
  '[12:03] Speaker 1: so, um… the budget is $4,500 (not 4.5k)!',
  '<think>literal tags</think> and <|im_end|> in user text',
];

async function tokenizerTests(m) {
  env.allowRemoteModels = false;
  const tok = await AutoTokenizer.from_pretrained(join(MODELS, m.dir));
  const eos = tok.encode('<|im_end|>', { add_special_tokens: false });
  assert.equal(eos.length, 1);

  await test(`${m.key}: decode(encode(x)) === x`, () => {
    for (const s of SAMPLES) assert.equal(dec(tok, tok.encode(s, { add_special_tokens: false })), s);
  });

  await test(`${m.key}: generation prompt ${m.thinking ? 'opens <think> and is prefilled </think>' : 'has no <think>'}`, () => {
    const { text, head } = render(tok, [{ role: 'user', content: 'hi' }]);
    assert.equal(head, m.thinking ? '<think></think>' : '');
    assert.ok(text.startsWith('<|startoftext|>'));
    const ids = tok.encode(text, { add_special_tokens: false });
    assert.equal(ids.filter((x) => x === tok.encode('<|startoftext|>', { add_special_tokens: false })[0]).length, 1, 'one BOS');
  });

  // Simulate turns: the "model" answers with `answer` tokenized `how` (canonical, or one
  // token per character: a non-canonical sequence a sampler could produce).
  const conv = (answers, how, stop) => {
    const sys = { role: 'system', content: 'You summarise meetings.' };
    let messages = [sys, { role: 'user', content: `Transcript:\n${SAMPLES.join('\n')}` }];
    let cached = null; // { text, ids }
    const stats = { tokenExact: 0, turns: 0 };
    for (const [i, answer] of answers.entries()) {
      const { text, head } = render(tok, messages);
      if (cached) {
        assert.ok(text.startsWith(cached.text), `turn ${i}: prompt doesn't extend the cached text`);
        const full = tok.encode(text, { add_special_tokens: false });
        if (cached.ids.every((x, j) => full[j] === x)) stats.tokenExact++;
      }
      const ids = cached
        ? cached.ids.concat(tok.encode(text.slice(cached.text.length), { add_special_tokens: false }))
        : tok.encode(text, { add_special_tokens: false });
      let out =
        how === 'chars'
          ? [...answer].flatMap((c) => tok.encode(c, { add_special_tokens: false }))
          : tok.encode(answer, { add_special_tokens: false });
      if (stop === 'eos') out = out.concat(eos);
      // generate() leaves the last sampled token out of the cache.
      const inCache = ids.length + out.length - 1;
      cached = { ids: ids.concat(out).slice(0, inCache), text: text + dec(tok, out.slice(0, inCache - ids.length)) };
      const raw = head + dec(tok, out.filter((x) => x !== eos[0]));
      messages = [...messages, { role: 'assistant', content: raw }, { role: 'user', content: `Question ${i + 2}?` }];
      stats.turns++;
    }
    const { text } = render(tok, messages);
    assert.ok(text.startsWith(cached.text), 'final prompt must extend the cached text');
    return stats;
  };
  const answers = ['Summary: the budget is $4,500.', ' Leading space and 🎙️ emoji.\n\n- a list', '会议 ok'];
  for (const how of ['canonical', 'chars']) {
    for (const stop of ['eos', 'length']) {
      await test(`${m.key}: replayed raw extends the cache (${how} tokens, ${stop})`, () => {
        const s = conv(answers, how, stop);
        if (how === 'canonical' && stop === 'eos') {
          assert.equal(s.tokenExact, s.turns - 1, 'canonical answers should also be token-exact prefixes');
        }
      });
    }
  }
  await test(`${m.key}: a changed earlier message breaks the text prefix (→ rebuild)`, () => {
    const a = render(tok, [{ role: 'user', content: 'Transcript v1' }]).text;
    const b = render(tok, [{ role: 'user', content: 'Transcript v2' }, { role: 'assistant', content: 'x' }, { role: 'user', content: 'q' }]).text;
    assert.ok(!b.startsWith(a));
  });
}

// ------------------------------------------------------------------ 2. the worker in Node
const messages = [];
const waiters = [];
function installShims() {
  globalThis.self = globalThis;
  globalThis.__ORT_VERSION__ = JSON.parse(readFileSync(join(SITE, 'node_modules/onnxruntime-web/package.json'), 'utf8')).version;
  Object.defineProperty(globalThis, 'isSecureContext', { value: true, configurable: true });
  Object.defineProperty(navigator, 'gpu', {
    configurable: true,
    value: {
      requestAdapter: async () => ({ features: new Set(['shader-f16']), info: { vendor: 'onnxruntime-node', architecture: 'webgpu' } }),
    },
  });
  globalThis.postMessage = (msg) => {
    messages.push(msg);
    for (const w of waiters.slice()) if (w.pred(msg)) {
      waiters.splice(waiters.indexOf(w), 1);
      w.resolve(msg);
    }
  };
}

const send = (data) => self.onmessage({ data });
function next(pred, ms = 600_000) {
  const hit = messages.find((m) => pred(m) && !m.__seen);
  if (hit) {
    hit.__seen = true;
    return Promise.resolve(hit);
  }
  return new Promise((resolve, reject) => {
    const w = { pred, resolve: (m) => { clearTimeout(t); m.__seen = true; resolve(m); } };
    const t = setTimeout(() => {
      waiters.splice(waiters.indexOf(w), 1);
      reject(new Error('timeout waiting for a worker message'));
    }, ms);
    waiters.push(w);
  });
}
const since = (i) => messages.slice(i);
let gid = 0;
async function ask(msgs, extra = {}) {
  const id = `g${++gid}`;
  const start = messages.length;
  send({ type: 'generate', id, messages: msgs, maxTokens: 96, ...extra });
  const done = await next((m) => (m.type === 'done' || m.type === 'error') && (m.id === id || m.during === 'generate'));
  if (done.type === 'error') throw new Error(`generate error: ${done.error}`);
  const mine = since(start).filter((m) => m.id === id);
  return {
    done,
    text: mine.filter((m) => m.type === 'token').map((m) => m.text).join(''),
    prefill: mine.filter((m) => m.type === 'prefill'),
  };
}

// A transcript long enough for several prefill chunks.
function transcript(n) {
  const who = ['Ana', 'Ben', 'Chloé'];
  const lines = [];
  for (let i = 0; i < n; i++) {
    const t = `${String(Math.floor(i / 6)).padStart(2, '0')}:${String((i * 10) % 60).padStart(2, '0')}`;
    lines.push(`[${t}] ${who[i % 3]}: item ${i} — we agreed the launch moves to week ${10 + (i % 7)}, budget ${1000 + i * 25} dollars.`);
  }
  return lines.join('\n');
}

async function workerTests(key) {
  const m = LLMS[key];
  const files = {};
  for (const name of Object.keys(m.files)) files[name] = await openAsBlob(join(MODELS, m.dir, name)); // disk-backed
  const sys = { role: 'system', content: 'You are a meeting assistant. Answer briefly, in plain text.' };
  const u1 = { role: 'user', content: `Here is a meeting transcript:\n${transcript(90)}\n\nSummarise it in two sentences.` };

  await test(`${key}: local-missing names the absent files`, async () => {
    const { 'config.json': _drop, ...partial } = files;
    send({ type: 'load', model: key, local: { name: 'partial', files: partial } });
    const e = await next((x) => x.type === 'error' && x.during === 'load');
    assert.equal(e.code, 'local-missing');
    assert.deepEqual(e.missing, ['config.json']);
  });

  await test(`${key}: load from a local folder`, async () => {
    const start = messages.length;
    send({ type: 'load', model: key, local: { name: 'models', files } });
    const r = await next((x) => x.type === 'ready' || (x.type === 'error' && x.during === 'load'));
    if (r.type === 'error') throw new Error(`${r.code}: ${r.error}`);
    const plan = since(start).find((x) => x.type === 'plan');
    assert.equal(plan.source, 'local');
    assert.equal(plan.folder, 'models');
    const prog = since(start).filter((x) => x.type === 'progress');
    assert.ok(prog.length >= 2 && prog.at(-1).progress > 0.999, `progress should reach 1 (got ${prog.at(-1)?.progress})`);
    console.log(`       ready in ${r.loadMs} ms (${prog.length} progress events)`);
  });

  await test(`${key}: count`, async () => {
    send({ type: 'count', id: 'c1', text: 'Hello there, general Kenobi.' });
    const c = await next((x) => x.type === 'count' && x.id === 'c1');
    assert.ok(c.tokens > 3 && c.tokens < 20, `tokens ${c.tokens}`);
  });

  let turn1;
  await test(`${key}: first answer — chunked prefill, streaming, raw`, async () => {
    turn1 = await ask([sys, u1]);
    const { done, text, prefill } = turn1;
    assert.equal(text, done.raw, 'token texts must concatenate to raw');
    assert.ok(done.promptTokens > 1024, `prompt should need several chunks (${done.promptTokens} tokens)`);
    assert.equal(done.reusedTokens, 0);
    const chunks = prefill.map((p) => p.done);
    for (let i = 1; i < chunks.length - 1; i++) assert.equal(chunks[i] - chunks[i - 1], 512, `chunk steps ${chunks}`);
    assert.equal(chunks.at(-1), done.promptTokens);
    assert.ok(['eos', 'length'].includes(done.reason));
    if (m.thinking) assert.ok(done.raw.startsWith('<think></think>'), 'thinking prefilled closed');
    const answer = done.raw.replace(/^<think><\/think>/, '');
    assert.ok(!/<think>/.test(answer), 'no thinking in the answer');
    assert.ok(answer.trim().length > 10, 'some answer');
    console.log(`       ${done.promptTokens} prompt tokens, prefill ${done.prefillMs} ms, ${done.newTokens} tokens in ${done.genMs} ms ` +
      `(${(done.newTokens / (done.genMs / 1000)).toFixed(1)} tok/s), ${done.reason}\n       answer: ${JSON.stringify(answer.slice(0, 160))}`);
  });

  let history;
  await test(`${key}: follow-up reuses the whole cache`, async () => {
    history = [sys, u1, { role: 'assistant', content: turn1.done.raw }, { role: 'user', content: 'Who spoke first?' }];
    const { done, text } = await ask(history);
    assert.equal(text, done.raw);
    const expected = turn1.done.promptTokens + turn1.done.newTokens + (turn1.done.reason === 'eos' ? 0 : -1);
    assert.equal(done.reusedTokens, expected, 'reused = previous prompt + answer (minus the uncached last token)');
    assert.ok(done.prefillMs < turn1.done.prefillMs, `reuse should be faster (${done.prefillMs} vs ${turn1.done.prefillMs} ms)`);
    history.push({ role: 'assistant', content: done.raw });
    console.log(`       reused ${done.reusedTokens}/${done.promptTokens}, prefill ${done.prefillMs} ms: ${JSON.stringify(done.raw.slice(0, 100))}`);
  });

  await test(`${key}: reuseCache=false rebuilds`, async () => {
    const { done } = await ask([...history, { role: 'user', content: 'One word: was it a good meeting?' }], { reuseCache: false });
    assert.equal(done.reusedTokens, 0);
  });

  await test(`${key}: abort mid-prefill keeps the partial prefill for the next request`, async () => {
    send({ type: 'reset-cache' });
    const id = `g${++gid}`;
    const start = messages.length;
    send({ type: 'generate', id, messages: [sys, u1], maxTokens: 32 });
    await next((x) => x.type === 'prefill' && x.id === id && x.done >= 512);
    send({ type: 'abort' });
    const done = await next((x) => x.type === 'done' && x.id === id);
    assert.equal(done.reason, 'aborted');
    assert.equal(done.raw, '');
    assert.ok(!since(start).some((x) => x.type === 'token' && x.id === id), 'no tokens after an abort in prefill');
    const again = await ask([sys, u1], { maxTokens: 8 });
    assert.ok(again.done.reusedTokens >= 512 && again.done.reusedTokens % 512 === 0, `reused ${again.done.reusedTokens}`);
  });

  await test(`${key}: abort during decode stops early`, async () => {
    const id = `g${++gid}`;
    const essay = [...history, { role: 'user', content: 'Write a long, detailed essay about the meeting.' }];
    send({ type: 'generate', id, messages: essay, maxTokens: 400 });
    await next((x) => x.type === 'token' && x.id === id && x.text.trim());
    send({ type: 'abort' });
    const done = await next((x) => (x.type === 'done' && x.id === id) || (x.type === 'error' && x.during === 'generate'));
    assert.equal(done.type, 'done', done.error);
    assert.equal(done.reason, 'aborted');
    assert.ok(done.newTokens < 400);
    // The cut-off answer, replayed from raw, still extends the cache.
    const after = await ask([...essay, { role: 'assistant', content: done.raw }, { role: 'user', content: 'Thanks. One word?' }], { maxTokens: 8 });
    assert.equal(after.done.reusedTokens, done.promptTokens + done.newTokens - 1);
  });

  await test(`${key}: pause/resume during generation`, async () => {
    const id = `g${++gid}`;
    send({ type: 'reset-cache' });
    send({ type: 'generate', id, messages: [sys, u1], maxTokens: 16 });
    await next((x) => x.type === 'prefill' && x.id === id && x.done >= 512);
    send({ type: 'pause' });
    await next((x) => x.type === 'paused');
    const at = messages.length;
    await new Promise((r) => setTimeout(r, 500));
    assert.ok(!since(at).some((x) => x.id === id && (x.type === 'prefill' || x.type === 'token')), 'nothing runs while paused');
    send({ type: 'resume' });
    await next((x) => x.type === 'resumed');
    const done = await next((x) => x.type === 'done' && x.id === id);
    assert.notEqual(done.reason, 'aborted');
  });

  await test(`${key}: pause when idle answers at once`, async () => {
    send({ type: 'pause' });
    await next((x) => x.type === 'paused', 2000);
    send({ type: 'resume' });
    await next((x) => x.type === 'resumed', 2000);
  });

  await test(`${key}: unload`, async () => {
    send({ type: 'unload' });
    await next((x) => x.type === 'unloaded');
    send({ type: 'count', id: 'c2', text: 'x' });
    const e = await next((x) => x.type === 'error' && x.during === 'count');
    assert.equal(e.code, 'other');
  });
}

// ------------------------------------------------------------------ main
for (const key of ['small', 'large']) {
  const m = LLMS[key];
  if (!have(m)) {
    console.log(`\n${m.label}: files missing in ${join(MODELS, m.dir)} — run \`npm run models -- llm-${key}\`; skipped`);
    continue;
  }
  console.log(`\n${m.label} — tokenizer + chat template`);
  await tokenizerTests(m);
}

if (!tokenizerOnly) {
  installShims();
  env.allowRemoteModels = true; // the worker's own fetch guard keeps it offline
  env.useWasmCache = false; // onnxruntime-node needs no .wasm; don't fetch the web runtime
  await import('../src/components/llm.worker.js');
  for (const key of runKeys) {
    if (!have(LLMS[key])) continue;
    console.log(`\n${LLMS[key].label} — llm.worker.js on onnxruntime-node WebGPU`);
    await workerTests(key);
  }
}

console.log(failures ? `\n${failures} test(s) failed` : '\nall tests passed');
process.exit(failures ? 1 : 0);
