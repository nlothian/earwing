// Score the diarizer against ground truth over a list of recordings: runs
// scripts/diarize-file.mjs --rttm on each (several at once, one process per file) and
// sums the DER per group and overall: for the settled live labels, the live labels as
// they stood when the audio ended (online, after the periodic reclusters), and the
// final ones (after Stop). "relabelled" is the share of settled speech whose label
// changed again while live: words jumping to another speaker on screen.
//
//   node scripts/score-diarization.mjs [--dir ../datasets/voxconverse/dev]
//        [--list scripts/fixtures/voxconverse-dev-subset.txt] [--out dir] [--jobs N]
//        [--set key=value ...]
//
// --dir holds <id>.wav (16 kHz) and <id>.rttm for each id. --list: one "id group"
// per line, # comments. --set is passed through to every run. Per-file outputs and a
// summary.json go to --out (default <dir>/../runs/<time>), so runs can be compared.

import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { availableParallelism } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { formatDer, sumDer } from './der.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
let dir = join(HERE, '..', '..', 'datasets', 'voxconverse', 'dev');
let list = join(HERE, 'fixtures', 'voxconverse-dev-subset.txt');
let out = null;
let jobs = Math.max(1, Math.floor(availableParallelism() / 2));
const sets = [];
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === '--dir') dir = args[++i];
  else if (a === '--list') list = args[++i];
  else if (a === '--out') out = args[++i];
  else if (a === '--jobs') jobs = Number(args[++i]);
  else if (a === '--set') sets.push('--set', args[++i]);
  else {
    console.error(`unknown argument ${a}`);
    process.exit(2);
  }
}
dir = resolve(dir);
out = resolve(out || join(dir, '..', 'runs', new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19)));
mkdirSync(out, { recursive: true });

const items = readFileSync(list, 'utf8')
  .split('\n')
  .map((l) => l.replace(/#.*/, '').trim())
  .filter(Boolean)
  .map((l) => {
    const [id, group = 'all'] = l.split(/\s+/);
    return { id, group };
  });

const run = ({ id }) =>
  new Promise((ok, fail) => {
    const p = spawn(
      process.execPath,
      [join(HERE, 'diarize-file.mjs'), join(dir, `${id}.wav`), '--rttm', join(dir, `${id}.rttm`), '--out', join(out, id), ...sets],
      { stdio: ['ignore', 'pipe', 'pipe'] },
    );
    let log = '';
    p.stdout.on('data', (b) => (log += b));
    p.stderr.on('data', (b) => (log += b));
    p.on('close', (code) => {
      writeFileSync(join(out, `${id}.log`), log);
      if (code) fail(new Error(`${id}: exit ${code}\n${log}`));
      else ok(JSON.parse(readFileSync(join(out, `${id}.score.json`), 'utf8')));
    });
  });

console.log(`${items.length} recordings from ${dir}, ${jobs} at a time → ${out}${sets.length ? `\n  ${sets.join(' ')}` : ''}`);
const t0 = performance.now();
const results = new Array(items.length);
let next = 0;
await Promise.all(
  Array.from({ length: Math.min(jobs, items.length) }, async () => {
    while (next < items.length) {
      const i = next++;
      const s = await run(items[i]);
      results[i] = s;
      const { id, group } = items[i];
      console.log(
        `  ${id} ${group.padEnd(5)} ${(s.secs / 60).toFixed(1).padStart(4)} min  ${s.final.refSpeakers} ref speakers` +
          `  settled ${formatDer(s.settled)}, ${s.settled.hypSpeakers}  final ${formatDer(s.final)}, ${s.final.hypSpeakers}`,
      );
    }
  }),
);

const groups = [...new Set(items.map((x) => x.group)), 'all'];
const summary = { dir, list, options: sets, minutes: (performance.now() - t0) / 60000, groups: {}, files: {} };
console.log(`\nDER (missed / false alarm / confusion), 0.25 s collar, in ${summary.minutes.toFixed(1)} min:`);
for (const g of groups) {
  const sel = results.filter((_, i) => g === 'all' || items[i].group === g);
  const row = { recordings: sel.length };
  for (const k of ['settled', 'online', 'final']) row[k] = sumDer(sel.map((s) => s[k]));
  const labelled = sel.reduce((a, s) => a + s.churn.labelled, 0) || 1;
  row.provisional = sel.reduce((a, s) => a + s.churn.provisional, 0) / labelled;
  row.relabelled = sel.reduce((a, s) => a + s.churn.relabelled, 0) / labelled;
  summary.groups[g] = row;
  console.log(
    `  ${g.padEnd(5)} (${sel.length})  settled ${formatDer(row.settled)}   online ${formatDer(row.online)}   final ${formatDer(row.final)}` +
      `   relabelled ${(100 * row.relabelled).toFixed(1)}%`,
  );
}
items.forEach(({ id }, i) => (summary.files[id] = results[i]));
writeFileSync(join(out, 'summary.json'), JSON.stringify(summary, null, 1));
