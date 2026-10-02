// Do known voices (Diarizer.setAnchors) help? Cuts each recording in half, diarizes
// the first half and keeps the speakers the page would save a print for (≥ the
// voiceStore minWeight), then scores the second half three ways:
//   none     no anchors (the baseline)
//   own      the first half's speakers as known voices (the same people, as when
//            they were named in an earlier take; whoever only speaks in the second
//            half is an unknown speaker)
//   foreign  another recording's first-half speakers (strangers: should change nothing)
// Runs scripts/diarize-file.mjs, one process per run, several at once, like
// score-diarization.mjs.
//
//   node scripts/score-anchors.mjs [--dir ../datasets/voxconverse/dev]
//        [--list scripts/fixtures/voxconverse-dev-subset.txt] [--out dir] [--jobs N]
//        [--set key=value ...] [--reuse earlier-out-dir] [--variants none,own,foreign]
//
// --reuse takes the halves and first-half prints from an earlier run (skips step 1),
// to try settings on the second halves quickly.

import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { availableParallelism } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { formatDer, readRttm, sumDer } from './der.mjs';
import { MATCH } from '../src/components/voiceStore.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
let dir = join(HERE, '..', '..', 'datasets', 'voxconverse', 'dev');
let list = join(HERE, 'fixtures', 'voxconverse-dev-subset.txt');
let out = null;
let jobs = Math.max(1, Math.floor(availableParallelism() / 2));
const sets = [];
let reuse = null;
let variants = ['none', 'own', 'foreign'];
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === '--dir') dir = args[++i];
  else if (a === '--list') list = args[++i];
  else if (a === '--out') out = args[++i];
  else if (a === '--jobs') jobs = Number(args[++i]);
  else if (a === '--set') sets.push('--set', args[++i]);
  else if (a === '--reuse') reuse = resolve(args[++i]);
  else if (a === '--variants') variants = args[++i].split(',');
  else {
    console.error(`unknown argument ${a}`);
    process.exit(2);
  }
}
dir = resolve(dir);
out = resolve(out || join(dir, '..', 'runs', `anchors-${new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19)}`));
mkdirSync(out, { recursive: true });
const halves = reuse || out; // <id>.h{1,2}.{wav,rttm} and <id>.anchors.json

const items = readFileSync(list, 'utf8')
  .split('\n')
  .map((l) => l.replace(/#.*/, '').trim())
  .filter(Boolean)
  .map((l) => {
    const [id, group = 'all'] = l.split(/\s+/);
    return { id, group };
  });

// The WAV's data chunk split at its middle sample → two 16-bit WAVs (header copied,
// sizes fixed). Returns the cut time in seconds.
function splitWav(id) {
  const b = readFileSync(join(dir, `${id}.wav`));
  let off = 12;
  let fmt = null;
  while (off + 8 <= b.length) {
    const tag = b.toString('ascii', off, off + 4);
    const size = b.readUInt32LE(off + 4);
    if (tag === 'fmt ')
      fmt = {
        channels: b.readUInt16LE(off + 10),
        rate: b.readUInt32LE(off + 12),
        bits: b.readUInt16LE(off + 22),
      };
    if (tag === 'data') break;
    off += 8 + size + (size & 1);
  }
  const data = b.subarray(off + 8, off + 8 + Math.min(b.readUInt32LE(off + 4), b.length - off - 8));
  const frame = (fmt.bits / 8) * fmt.channels;
  const half = Math.floor(data.length / frame / 2) * frame;
  const write = (path, pcm) => {
    const h = Buffer.from(b.subarray(0, off + 8));
    h.writeUInt32LE(h.length - 8 + pcm.length, 4);
    h.writeUInt32LE(pcm.length, off + 4);
    writeFileSync(path, Buffer.concat([h, pcm]));
  };
  write(join(out, `${id}.h1.wav`), data.subarray(0, half));
  write(join(out, `${id}.h2.wav`), data.subarray(half));
  return half / frame / fmt.rate;
}

// Reference segments clipped to [t0, t1) and shifted to start at 0, as RTTM.
function clipRttm(ref, id, t0, t1) {
  return ref
    .map((s) => ({
      ...s,
      start: Math.max(s.start, t0) - t0,
      end: Math.min(s.end, t1) - t0,
    }))
    .filter((s) => s.end - s.start > 0.01)
    .map((s) => `SPEAKER ${id} 1 ${s.start.toFixed(3)} ${(s.end - s.start).toFixed(3)} <NA> <NA> ${s.speaker} <NA> <NA>`)
    .join('\n');
}

const diarize = (wav, rttm, prefix, extra = []) =>
  new Promise((ok, fail) => {
    const p = spawn(process.execPath, [join(HERE, 'diarize-file.mjs'), wav, '--rttm', rttm, '--out', prefix, ...extra, ...sets], {
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let log = '';
    p.stdout.on('data', (d) => (log += d));
    p.stderr.on('data', (d) => (log += d));
    p.on('close', (code) => {
      writeFileSync(`${prefix}.log`, log);
      if (code) fail(new Error(`${prefix}: exit ${code}\n${log}`));
      else ok(JSON.parse(readFileSync(`${prefix}.score.json`, 'utf8')));
    });
  });

async function pool(tasks) {
  const results = new Array(tasks.length);
  let next = 0;
  await Promise.all(
    Array.from({ length: Math.min(jobs, tasks.length) }, async () => {
      while (next < tasks.length) {
        const i = next++;
        results[i] = await tasks[i]();
      }
    }),
  );
  return results;
}

console.log(`${items.length} recordings from ${dir}, ${jobs} at a time → ${out}${sets.length ? `\n  ${sets.join(' ')}` : ''}`);
const t0 = performance.now();
for (const it of items) {
  const ref = readRttm(readFileSync(join(dir, `${it.id}.rttm`), 'utf8'));
  if (reuse) it.cut = JSON.parse(readFileSync(join(reuse, 'summary.json'), 'utf8')).files[it.id].cut;
  else {
    it.cut = splitWav(it.id);
    writeFileSync(join(out, `${it.id}.h1.rttm`), clipRttm(ref, it.id, 0, it.cut));
    writeFileSync(join(out, `${it.id}.h2.rttm`), clipRttm(ref, it.id, it.cut, Infinity));
  }
  // Second-half speech by people who never speak in the first half (unknown to "own").
  const talk = (t0, t1) => {
    const m = new Map();
    for (const s of ref) {
      const d = Math.min(s.end, t1) - Math.max(s.start, t0);
      if (d > 0) m.set(s.speaker, (m.get(s.speaker) || 0) + d);
    }
    return m;
  };
  const h1 = talk(0, it.cut);
  const h2 = talk(it.cut, Infinity);
  const total = [...h2.values()].reduce((a, b) => a + b, 0) || 1;
  it.unknownShare = [...h2].filter(([s]) => !h1.has(s)).reduce((a, [, d]) => a + d, 0) / total;
  it.h2Speakers = h2.size;
  it.unknownSpeakers = [...h2.keys()].filter((s) => !h1.has(s)).length;
}

// 1. First halves → voice prints (only speakers with enough speech, as the page saves).
if (reuse) for (const it of items) it.anchors = JSON.parse(readFileSync(join(reuse, `${it.id}.anchors.json`), 'utf8')).length;
else
  await pool(
    items.map((it) => async () => {
      const p = join(out, `${it.id}.h1`);
      await diarize(`${p}.wav`, `${p}.rttm`, p, ['--voices']);
      const voices = JSON.parse(readFileSync(`${p}.voices.json`, 'utf8')).filter((v) => v.weight >= MATCH.minWeight);
      writeFileSync(join(out, `${it.id}.anchors.json`), JSON.stringify(voices));
      it.anchors = voices.length;
    }),
  );

// 2. Second halves, three ways. "foreign" uses the next recording's prints.
const runs = [];
items.forEach((it, i) => {
  const other = items[(i + 1) % items.length];
  const anchorsOf = {
    none: [],
    own: ['--anchors', join(halves, `${it.id}.anchors.json`)],
    foreign: ['--anchors', join(halves, `${other.id}.anchors.json`)],
  };
  for (const v of variants) {
    runs.push(async () => {
      const p = join(halves, `${it.id}.h2`);
      it[v] = await diarize(`${p}.wav`, `${p}.rttm`, join(out, `${it.id}.h2.${v}`), anchorsOf[v]);
    });
  }
});
await pool(runs);

const pct = (x) => `${(100 * x).toFixed(1)}%`;
console.log(`\nsecond halves, final DER (missed / false alarm / confusion), speakers found vs reference:`);
for (const it of items) {
  const f = (v) => `${pct(it[v].final.der).padStart(6)} ${String(it[v].final.hypSpeakers).padStart(2)}`;
  console.log(
    `  ${it.id} ${it.group.padEnd(5)} ${(it[variants[0]].secs / 60).toFixed(1).padStart(4)} min  ${String(it.h2Speakers).padStart(2)} ref spk ` +
      `(${it.unknownSpeakers} unknown, ${pct(it.unknownShare).padStart(5)} of speech), ${String(it.anchors).padStart(2)} known` +
      variants.map((v) => `   ${v} ${f(v)}`).join(''),
  );
}
const groups = [...new Set(items.map((x) => x.group)), 'all'];
const summary = {
  dir,
  list,
  options: sets,
  minutes: (performance.now() - t0) / 60000,
  groups: {},
  files: {},
};
console.log(`\nin ${summary.minutes.toFixed(1)} min:`);
for (const g of groups) {
  const sel = items.filter((x) => g === 'all' || x.group === g);
  const row = {};
  for (const v of variants) for (const k of ['settled', 'final']) row[`${v}.${k}`] = sumDer(sel.map((x) => x[v][k]));
  summary.groups[g] = row;
  console.log(`  ${g} (${sel.length})`);
  for (const k of ['settled', 'final']) {
    console.log(`    ${k.padEnd(7)}  ` + variants.map((v) => `${v} ${formatDer(row[`${v}.${k}`])}`).join('   '));
  }
}
for (const it of items) {
  summary.files[it.id] = {
    group: it.group,
    cut: it.cut,
    anchors: it.anchors,
    unknownShare: it.unknownShare,
    ...Object.fromEntries(variants.map((v) => [v, it[v]])),
  };
}
writeFileSync(join(out, 'summary.json'), JSON.stringify(summary, null, 1));
