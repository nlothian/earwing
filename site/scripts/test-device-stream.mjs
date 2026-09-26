#!/usr/bin/env node
// Tests for src/components/deviceStream.js under plain Node (22+: global fetch).
//
//   node scripts/test-device-stream.mjs              parser tests + live tests vs the mock
//                                                    (http://localhost:8080, `npm run mock`)
//   node scripts/test-device-stream.mjs <base URL>   live tests against another server, e.g.
//                                                    http://gulltoppr.local (see below)
//
// The live part needs a device or `npm run mock` running; it's skipped if nothing
// answers. It fetches /beam.wav from the base URL, the way the page does same-origin,
// and checks: header parsed, 1600-sample chunks at ~10/s, sane values, 3 concurrent
// streams work and a 4th is refused (503) with a friendly message, stop() frees a
// slot for the next client, and ?mic=2.
//
// Real device: when it's online, http://gulltoppr.local redirects to https with a
// self-signed cert, which Node rejects. Either trust the cert for this run
// (NODE_EXTRA_CA_CERTS=gulltoppr.crt, downloaded from /gulltoppr.crt) and pass
// https://gulltoppr.local, or pass the device's IP over http if it doesn't redirect.
// Nothing else may be streaming from the device while the slot tests run.

import assert from 'node:assert/strict';
import { createWavParser, startDeviceStream, CHUNK_SAMPLES } from '../src/components/deviceStream.js';

const base = (process.argv[2] || 'http://localhost:8080').replace(/\/+$/, '');
let failed = 0;
async function test(name, fn) {
  try {
    await fn();
    console.log(`ok   ${name}`);
  } catch (e) {
    failed++;
    console.log(`FAIL ${name}\n     ${e.stack || e}`);
  }
}

function wav({ rate = 16000, channels = 1, bits = 16, samples, extraChunk = false }) {
  const extra = extraChunk ? 8 + 5 + 1 : 0; // odd-sized LIST chunk + pad byte
  const h = Buffer.alloc(44 + extra);
  h.write('RIFF', 0);
  h.writeUInt32LE(0x7fff0000, 4);
  h.write('WAVEfmt ', 8);
  h.writeUInt32LE(16, 16);
  h.writeUInt16LE(1, 20);
  h.writeUInt16LE(channels, 22);
  h.writeUInt32LE(rate, 24);
  h.writeUInt32LE((rate * channels * bits) / 8, 28);
  h.writeUInt16LE((channels * bits) / 8, 32);
  h.writeUInt16LE(bits, 34);
  let o = 36;
  if (extraChunk) {
    h.write('LIST', o);
    h.writeUInt32LE(5, o + 4);
    h.write('abcde', o + 8);
    o += 14;
  }
  h.write('data', o);
  h.writeUInt32LE(0x7fff0000, o + 4);
  const body = Buffer.alloc(samples.length * 2);
  samples.forEach((s, i) => body.writeInt16LE(s, i * 2));
  return new Uint8Array(Buffer.concat([h, body]));
}

// Feed bytes in random-sized pieces (1..max), including odd splits.
function feed(parser, bytes, max, rnd) {
  for (let i = 0; i < bytes.length; ) {
    const n = 1 + Math.floor(rnd() * max);
    parser.push(bytes.subarray(i, i + n));
    i += n;
  }
}

let seed = 42;
const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);

// ---------------------------------------------------------------- parser
const N = CHUNK_SAMPLES * 5 + 123; // 5 full chunks + a partial (not emitted)
const samples = Array.from({ length: N }, (_, i) => ((i * 7919) % 65536) - 32768);
samples[0] = -32768;
samples[1] = 32767;
samples[2] = 0;
samples[3] = -1;

for (const [label, max, extraChunk] of [
  ['1-byte pieces', 1, false],
  ['random odd splits ≤7', 7, false],
  ['random splits ≤3000', 3000, false],
  ['header split + extra RIFF chunk', 5, true],
]) {
  await test(`parser: ${label}`, () => {
    const got = [];
    const p = createWavParser((c) => got.push(c));
    feed(p, wav({ samples, extraChunk }), max, rnd);
    assert.equal(p.headerDone, true);
    assert.deepEqual(p.format, { format: 1, channels: 1, sampleRate: 16000, bits: 16 });
    assert.equal(got.length, 5);
    for (const c of got) assert.equal(c.length, CHUNK_SAMPLES);
    const flat = got.flatMap((c) => Array.from(c));
    for (let i = 0; i < flat.length; i++) assert.equal(flat[i], samples[i] / 32768, `sample ${i}`);
    assert.equal(flat[0], -1);
    assert.equal(flat[1], 32767 / 32768);
    assert.equal(flat[3], -1 / 32768);
  });
}

await test('parser: rejects 44.1 kHz', () => {
  const p = createWavParser(() => {});
  assert.throws(() => p.push(wav({ rate: 44100, samples: [0] })), /44100 Hz/);
});
await test('parser: rejects stereo / 24-bit', () => {
  assert.throws(() => createWavParser(() => {}).push(wav({ channels: 2, samples: [0] })), /2 ch/);
  assert.throws(() => createWavParser(() => {}).push(wav({ bits: 24, samples: [0] })), /24-bit/);
});
await test('parser: rejects non-WAV', () => {
  const p = createWavParser(() => {});
  assert.throws(() => p.push(new TextEncoder().encode('HTTP/1.1 404 Not Found\r\n')), /not a WAV/);
});

// ---------------------------------------------------------------- live
const reachable = await fetch(`${base}/status.json`, { signal: AbortSignal.timeout(2000) })
  .then((r) => r.ok)
  .catch(() => false);

if (!reachable) {
  console.log(`skip live tests: no ${base}/status.json (start \`npm run mock\`)`);
} else {
  const collect = (ms, opts = {}) =>
    new Promise((resolve) => {
      const chunks = [];
      const times = [];
      const states = [];
      let error = null;
      const h = startDeviceStream(
        (c) => {
          chunks.push(c);
          times.push(Date.now());
        },
        { base, onState: (s) => states.push(s), onError: (m) => (error = m), ...opts },
      );
      setTimeout(() => resolve({ h, chunks, times, states, get error() { return error; } }), ms);
    });
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  let first;
  await test('live: header parsed, 1600-sample chunks at ~10/s, sane values', async () => {
    first = await collect(4000);
    assert.equal(first.error, null, first.error);
    assert.deepEqual(first.states.slice(0, 2), ['connecting', 'streaming']);
    const c = first.chunks;
    assert.ok(c.length >= 30, `only ${c.length} chunks in 4 s`);
    for (const x of c) assert.equal(x.length, 1600);
    // Rate over the steady part (skip the first second's connection setup).
    const steady = first.times.filter((t) => t - first.times[0] >= 1000);
    const secs = (steady.at(-1) - steady[0]) / 1000;
    const rate = (steady.length - 1) / secs;
    assert.ok(rate > 8.5 && rate < 11.5, `rate ${rate.toFixed(2)}/s`);
    let peak = 0;
    let sum = 0;
    let n = 0;
    for (const x of c) for (const v of x) {
      assert.ok(v >= -1 && v < 1 && Number.isFinite(v));
      peak = Math.max(peak, Math.abs(v));
      sum += v * v;
      n++;
    }
    const rmsDb = 10 * Math.log10(sum / n);
    assert.ok(peak > 0.01, 'audio is all silence?');
    console.log(`     ${c.length} chunks, ${rate.toFixed(2)} chunks/s, peak ${peak.toFixed(3)}, rms ${rmsDb.toFixed(1)} dBFS`);
  });

  let others = [];
  await test('live: 3 streams at once; a 4th gets the "3 clients" 503 message at once', async () => {
    others = await Promise.all([collect(1500), collect(1500, { source: 1 })]);
    for (const o of others) {
      assert.equal(o.error, null, o.error);
      assert.ok(o.chunks.length >= 8, `concurrent client got ${o.chunks.length} chunks`);
    }
    const fourth = await collect(1000);
    assert.equal(fourth.chunks.length, 0);
    assert.match(fourth.error || '', /3 clients/);
    assert.ok(!fourth.states.includes('reconnecting'), 'a first-connect 503 must not be retried');
    console.log(`     4th client: "${fourth.error.slice(0, 70)}…"`);
    const before = first.chunks.length;
    await sleep(500);
    assert.ok(first.chunks.length > before, 'first client stalled');
  });

  await test('live: stop() aborts and frees a slot for the next client', async () => {
    first.h.stop();
    const n = first.chunks.length;
    await sleep(300); // let the server see the close
    const t = Date.now();
    const next = await collect(1500);
    assert.equal(first.chunks.length, n, 'stopped stream still delivering');
    assert.equal(next.error, null, next.error);
    assert.ok(next.chunks.length >= 8, `next client got ${next.chunks.length} chunks in 1.5 s`);
    console.log(`     next client streaming ${next.times[0] - t} ms after connecting`);
    next.h.stop();
    for (const o of others) o.h.stop();
    await sleep(300);
  });

  await test('live: ?mic=2 streams too', async () => {
    const m = await collect(1500, { source: 2 });
    assert.equal(m.error, null, m.error);
    assert.ok(m.chunks.length >= 8);
    m.h.stop();
  });

  await test('live: transfer is chunked or length-less (parser must not need Content-Length)', async () => {
    const ctl = new AbortController();
    const res = await fetch(`${base}/beam.wav`, { signal: ctl.signal });
    const te = res.headers.get('transfer-encoding');
    const cl = res.headers.get('content-length');
    ctl.abort();
    console.log(`     transfer-encoding: ${te || '-'}, content-length: ${cl || '-'}`);
    assert.ok(res.ok);
  });
}

await new Promise((r) => setTimeout(r, 200));
console.log(failed ? `\n${failed} test(s) failed` : '\nall tests passed');
process.exit(failed ? 1 : 0);
