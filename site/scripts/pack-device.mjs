#!/usr/bin/env node
// Pack dist/ for the device: gzip every file (zlib level 9) into ../code/data/<path>.gz,
// which PlatformIO's `uploadfs` turns into the LittleFS image. The firmware serves
// /x from /x.gz with Content-Encoding: gzip, so the tree mirrors dist/ exactly.
//
// Fails (exit 1) if dist/ contains any .wasm / ORT runtime file or any file over 1 MB
// (onnxruntime-web's runtime must come from the CDN, never the flash), if the gzipped
// total exceeds the budget, or if a path component is too long for comfort on LittleFS.

import { gzipSync, constants } from 'node:zlib';
import {
  existsSync, lstatSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync,
} from 'node:fs';
import { basename, dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const SITE = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const DIST = join(SITE, 'dist');
const CODE = resolve(SITE, '..', 'code');
const OUT = join(CODE, 'data');

// default_16MB.csv: the spiffs/LittleFS partition is 0x360000 = 3,538,944 bytes.
// Leave headroom for LittleFS metadata (2 blocks for the superblock, per-file
// metadata pairs, and file data rounded up to 4 KB blocks).
const PARTITION_BYTES = 0x360000;
const BUDGET_BYTES = 3_200_000;
// esp_littlefs / pioarduino's image builder use name_max = 64; stay well under.
const MAX_COMPONENT = 31;

const fail = (msg) => {
  console.error(`\npack-device: ${msg}`);
  process.exit(1);
};

// ---- sanity: are we pointing where we think we are? ----
if (!existsSync(DIST) || !statSync(DIST).isDirectory()) fail(`no ${DIST} — run \`astro build\` first.`);
if (!existsSync(join(CODE, 'platformio.ini'))) fail(`${CODE} doesn't look like the firmware (no platformio.ini).`);
if (basename(OUT) !== 'data' || dirname(OUT) !== CODE) fail(`refusing to write to ${OUT}`);

function walk(dir) {
  const out = [];
  for (const name of readdirSync(dir).sort()) {
    const p = join(dir, name);
    const st = lstatSync(p);
    if (st.isDirectory()) out.push(...walk(p));
    else if (st.isFile()) out.push(p);
  }
  return out;
}

const files = walk(DIST);
// onnxruntime-web's runtime (ort-wasm-simd-threaded*.{mjs,wasm}) must load from jsDelivr,
// never from flash. Catch it by extension, by name (a hashed name hides that, so also
// by content: a WebAssembly module starts with "\0asm"), and by size.
const MAX_FILE_BYTES = 1_000_000;
const bad = [];
for (const f of files) {
  const rel = relative(DIST, f);
  const size = statSync(f).size;
  const head = readFileSync(f).subarray(0, 4);
  if (f.endsWith('.wasm') || /ort-wasm/.test(basename(f)) || head.equals(Buffer.from([0, 0x61, 0x73, 0x6d]))) {
    bad.push(`${rel} (ONNX Runtime / WebAssembly runtime file)`);
  } else if (size > MAX_FILE_BYTES) {
    bad.push(`${rel} (${(size / 1e6).toFixed(1)} MB > ${MAX_FILE_BYTES / 1e6} MB per file)`);
  }
}
if (bad.length) {
  fail(
    `dist/ contains files that must not go on the device:\n  ${bad.join('\n  ')}\n` +
      "onnxruntime-web's runtime must load from the CDN — check the alias in astro.config.mjs.",
  );
}

// ---- empty code/data (its contents only; never the directory itself) ----
mkdirSync(OUT, { recursive: true });
for (const name of readdirSync(OUT)) {
  const p = join(OUT, name);
  if (!resolve(p).startsWith(OUT + sep)) fail(`unexpected path ${p}`);
  rmSync(p, { recursive: true, force: true }); // a symlink is removed, not followed
}

// ---- gzip ----
const rows = [];
let totalRaw = 0;
let totalGz = 0;
for (const f of files) {
  const rel = relative(DIST, f).split(sep).join('/');
  const target = `${rel}.gz`;
  const long = target.split('/').filter((c) => c.length > MAX_COMPONENT);
  if (long.length) fail(`path component too long for LittleFS comfort (> ${MAX_COMPONENT}): ${long.join(', ')}`);
  const raw = readFileSync(f);
  const gz = gzipSync(raw, { level: constants.Z_BEST_COMPRESSION });
  const dest = join(OUT, ...target.split('/'));
  mkdirSync(dirname(dest), { recursive: true });
  writeFileSync(dest, gz);
  rows.push({ path: `/${target}`, raw: raw.length, gz: gz.length });
  totalRaw += raw.length;
  totalGz += gz.length;
}

// LittleFS stores data in whole 4 KB blocks (inline files < ~1/8 block excepted), so
// estimate on-flash use too.
const BLOCK = 4096;
const onFlash = rows.reduce((n, r) => n + Math.max(1, Math.ceil(r.gz / BLOCK)) * BLOCK, 0) + 2 * BLOCK;

const w = Math.max(...rows.map((r) => r.path.length), 4);
const kb = (n) => `${(n / 1024).toFixed(1)} KB`.padStart(10);
console.log(`\n${'file'.padEnd(w)}  ${'raw'.padStart(10)}  ${'gzip'.padStart(10)}`);
for (const r of rows) console.log(`${r.path.padEnd(w)}  ${kb(r.raw)}  ${kb(r.gz)}`);
console.log(`${'-'.repeat(w + 24)}`);
console.log(`${`total (${rows.length} files)`.padEnd(w)}  ${kb(totalRaw)}  ${kb(totalGz)}`);
console.log(
  `\n~${(onFlash / 1024).toFixed(0)} KB on flash of ${(PARTITION_BYTES / 1024).toFixed(0)} KB ` +
    `(budget ${(BUDGET_BYTES / 1024).toFixed(0)} KB) → ${relative(SITE, OUT)}/`,
);

if (totalGz > BUDGET_BYTES || onFlash > BUDGET_BYTES) {
  fail(`over budget: ${totalGz} bytes gzipped (~${onFlash} on flash) > ${BUDGET_BYTES}.`);
}
