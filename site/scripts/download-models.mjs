#!/usr/bin/env node
// Download the Parakeet model files the app uses into <repo-root>/models, so the page
// can load them from disk ("Load model from folder…") instead of Hugging Face, plus
// the two small speaker-diarization models (for scripts/diarize-file.mjs; the page
// always fetches those from Hugging Face and caches them).
//
//   node scripts/download-models.mjs [webgpu|wasm|diarization ...] [--out <dir>]   (npm run models)
//
// Layout (the app matches the folder names, see src/components/models.js):
//   models/ysdede--parakeet-tdt-0.6b-v3-onnx/          fp16 set for WebGPU  (~1.26 GB)
//     encoder-model.fp16.onnx  decoder_joint-model.int8.onnx  vocab.txt
//   models/nasedkinpv--parakeet-tdt-0.6b-v3-onnx-int8/ int8 set for CPU     (~933 MB)
//     encoder-int8.onnx  encoder-int8.onnx.data  decoder_joint-int8.onnx  vocab.txt
//   models/onnx-community--pyannote-segmentation-3.0/onnx/model.onnx    diarization (~6 MB)
//   models/Wespeaker--wespeaker-voxceleb-resnet34-LM/voxceleb_resnet34_LM.onnx    (~27 MB)
//
// Each file downloads to <name>.part and resumes from there (HTTP Range) if
// interrupted. It's checked against the Hugging Face tree API: size, plus the LFS
// sha256 (large files) or the git blob sha1 (small files like vocab.txt). Only then
// is it renamed into place. Verified files are remembered in models/.verified.json
// (by size + mtime), so a re-run skips them without re-hashing.

import { createHash } from 'node:crypto';
import { createReadStream, createWriteStream, existsSync, mkdirSync, readFileSync, renameSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { Readable } from 'node:stream';
import { pipeline } from 'node:stream/promises';
import { fileURLToPath } from 'node:url';
import { DIARIZATION, VARIANTS, variantBytes } from '../src/components/models.js';

// The diarization models, one "variant" per repo, so the loop below treats them alike.
const SETS = {
  ...VARIANTS,
  diarization: Object.entries(DIARIZATION).map(([role, m]) => ({
    backend: 'diarization',
    quant: role,
    repo: m.repo,
    dir: m.dir,
    files: { [role]: { name: m.name, size: m.size } },
  })),
};

const SITE = resolve(dirname(fileURLToPath(import.meta.url)), '..');
let OUT = resolve(SITE, '..', 'models');
const pick = [];
const args = process.argv.slice(2);
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--out') OUT = resolve(args[++i]);
  else if (args[i] === '-h' || args[i] === '--help') {
    console.log('usage: download-models.mjs [webgpu|wasm|diarization ...] [--out <dir>]');
    process.exit(0);
  } else if (SETS[args[i]]) pick.push(args[i]);
  else {
    console.error(`unknown argument ${args[i]} (expected webgpu, wasm, diarization or --out <dir>)`);
    process.exit(2);
  }
}
const variants = (pick.length ? pick : Object.keys(SETS)).flatMap((k) => SETS[k]);
const REVISION = 'main';
const TTY = process.stdout.isTTY;

const mb = (n) => `${(n / 1e6).toFixed(1)} MB`;
const fail = (msg) => {
  console.error(`\ndownload-models: ${msg}`);
  process.exit(1);
};

const VERIFIED = join(OUT, '.verified.json');
let verified = {};
try {
  verified = JSON.parse(readFileSync(VERIFIED, 'utf8'));
} catch {
  /* first run */
}
const saveVerified = () => writeFileSync(VERIFIED, `${JSON.stringify(verified, null, 2)}\n`);

async function tree(repo) {
  const url = `https://huggingface.co/api/models/${repo}/tree/${REVISION}?recursive=1`;
  const res = await fetch(url);
  if (!res.ok) fail(`${url}: HTTP ${res.status}`);
  const map = new Map();
  for (const e of await res.json()) if (e.type === 'file') map.set(e.path, e);
  return map;
}

// LFS files: sha256 of the content. Plain git files: sha1 of "blob <size>\0" + content.
async function hashFile(path, entry) {
  const lfs = !!entry.lfs;
  const h = createHash(lfs ? 'sha256' : 'sha1');
  if (!lfs) h.update(`blob ${statSync(path).size}\0`);
  await pipeline(createReadStream(path, { highWaterMark: 4 << 20 }), async function* (src) {
    for await (const chunk of src) h.update(chunk);
  });
  return { algo: lfs ? 'sha256' : 'git-sha1', got: h.digest('hex'), want: lfs ? entry.lfs.oid : entry.oid };
}

function progressLine(label, done, total, t0, startBytes) {
  const secs = (Date.now() - t0) / 1000;
  const rate = secs > 0 ? (done - startBytes) / secs : 0;
  const eta = rate > 0 ? Math.round((total - done) / rate) : 0;
  const pct = total ? ((done / total) * 100).toFixed(1) : '?';
  return `  ${label}: ${pct}%  ${mb(done)} / ${mb(total)}  ${(rate / 1e6).toFixed(1)} MB/s  ETA ${eta}s`;
}

async function downloadTo(url, part, total, label) {
  for (let attempt = 1; ; attempt++) {
    const have = existsSync(part) ? statSync(part).size : 0;
    if (have > total) rmSync(part);
    if (have === total) return;
    const headers = have > 0 ? { Range: `bytes=${have}-` } : {};
    try {
      const res = await fetch(url, { headers, redirect: 'follow' });
      if (have > 0 && res.status === 200) {
        console.log(`  ${label}: server ignored the Range request; restarting from 0`);
        rmSync(part, { force: true });
      } else if (!(res.status === 200 || res.status === 206)) {
        throw new Error(`HTTP ${res.status} ${res.statusText}`);
      }
      const start = res.status === 206 ? have : 0;
      if (start) console.log(`  ${label}: resuming at ${mb(start)}`);
      let done = start;
      const t0 = Date.now();
      let lastPrint = 0;
      let lastPct = -1;
      const counter = async function* (src) {
        for await (const chunk of src) {
          done += chunk.length;
          const now = Date.now();
          if (TTY && now - lastPrint > 250) {
            process.stdout.write(`\r${progressLine(label, done, total, t0, start)}\x1b[K`);
            lastPrint = now;
          } else if (!TTY && total > 5e6) {
            const pct = Math.floor((done / total) * 10) * 10;
            if (pct !== lastPct) {
              console.log(progressLine(label, done, total, t0, start));
              lastPct = pct;
            }
          }
          yield chunk;
        }
      };
      await pipeline(Readable.fromWeb(res.body), counter, createWriteStream(part, { flags: start ? 'a' : 'w' }));
      if (TTY) process.stdout.write(`\r${progressLine(label, done, total, t0, start)}\x1b[K\n`);
      const size = statSync(part).size;
      if (size !== total) throw new Error(`got ${size} bytes, expected ${total}`);
      return;
    } catch (e) {
      if (TTY) process.stdout.write('\n');
      if (attempt >= 5) throw e;
      const wait = attempt * 2000;
      console.log(`  ${label}: ${e.message}; retrying in ${wait / 1000}s (attempt ${attempt + 1}/5)`);
      await new Promise((r) => setTimeout(r, wait));
    }
  }
}

mkdirSync(OUT, { recursive: true });
let grand = 0;
for (const v of variants) {
  const dir = join(OUT, v.dir);
  mkdirSync(dir, { recursive: true });
  console.log(`\n${v.repo} (${v.backend}, ${v.quant}, ${mb(variantBytes(v))}) → ${relative(process.cwd(), dir) || '.'}`);
  const listing = await tree(v.repo);
  for (const f of Object.values(v.files)) {
    const entry = listing.get(f.name);
    if (!entry) fail(`${v.repo} has no ${f.name} any more; update src/components/models.js`);
    if (entry.size !== f.size) {
      fail(`${v.repo}/${f.name} is ${entry.size} bytes on Hugging Face, but the app expects ${f.size}; update src/components/models.js`);
    }
    const dest = join(dir, f.name);
    mkdirSync(dirname(dest), { recursive: true }); // names may include a folder (onnx/model.onnx)
    const key = `${v.dir}/${f.name}`;
    if (existsSync(dest)) {
      const st = statSync(dest);
      const mark = verified[key];
      if (st.size === f.size && mark && mark.size === st.size && mark.mtimeMs === st.mtimeMs) {
        console.log(`  ${f.name}: ok (verified earlier)`);
        grand += f.size;
        continue;
      }
      if (st.size === f.size) {
        const hv = await hashFile(dest, entry);
        if (hv.got === hv.want) {
          verified[key] = { size: st.size, mtimeMs: st.mtimeMs, [hv.algo]: hv.got };
          saveVerified();
          console.log(`  ${f.name}: ok (${hv.algo} verified)`);
          grand += f.size;
          continue;
        }
        console.log(`  ${f.name}: ${hv.algo} mismatch, downloading again`);
      } else {
        console.log(`  ${f.name}: wrong size (${st.size}), downloading again`);
      }
      rmSync(dest);
    }
    const part = `${dest}.part`;
    const url = `https://huggingface.co/${v.repo}/resolve/${REVISION}/${f.name.split('/').map(encodeURIComponent).join('/')}`;
    await downloadTo(url, part, f.size, f.name);
    const hv = await hashFile(part, entry);
    if (hv.got !== hv.want) {
      rmSync(part);
      fail(`${f.name}: ${hv.algo} ${hv.got} != expected ${hv.want} (deleted; run again)`);
    }
    renameSync(part, dest);
    const st = statSync(dest);
    verified[key] = { size: st.size, mtimeMs: st.mtimeMs, [hv.algo]: hv.got };
    saveVerified();
    console.log(`  ${f.name}: ${hv.algo} verified`);
    grand += f.size;
  }
}
console.log(`\ndone: ${mb(grand)} in ${OUT}\nIn the app: "Load model from folder…" and pick ${OUT} (or one of its subfolders).`);
