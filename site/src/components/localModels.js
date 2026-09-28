// Load the Parakeet model (and the assistant's LLMs) from a folder on this computer
// instead of Hugging Face.
//
// Two ways in:
//   - File System Access API (showDirectoryPicker): secure contexts only, i.e.
//     https://earwing.local and localhost. The directory handle is kept in IndexedDB,
//     so later visits can reuse it: automatically if the browser still reports
//     permission 'granted', otherwise after one click (requestPermission needs a user
//     gesture). Pattern from gemma-data-agent's sandboxStore.ts.
//   - <input type=file webkitdirectory>: works on plain http too, but the browser
//     gives us File objects for this visit only (nothing to persist without copying
//     the ~2 GB into IndexedDB, which we don't do).
//
// Either way we end up with File objects (disk-backed; getFile() copies nothing) for
// the files the chosen backend needs. They're posted to the worker, which makes blob
// URLs from them for parakeet.js/onnxruntime-web. Nothing is written to IndexedDB.
//
// Accepted layouts (see scripts/download-models.mjs):
//   <picked>/ysdede--parakeet-tdt-0.6b-v3-onnx/encoder-model.fp16.onnx …   (the ./models root)
//   <picked>/encoder-model.fp16.onnx …                                     (one repo folder)
// Files are matched by name AND exact size (both repos have a vocab.txt), preferring
// the repo's folder when several match.
//
// The LLMs (models.js LLMS, resolveLlm below) have repo-relative names with an onnx/
// subfolder, hence three levels: <picked>/LiquidAI--LFM2.5-2.6B-ONNX/onnx/model_q4f16.onnx.
// Their small files (config.json, tokenizer.json …) have generic names, so the exact
// size and the path decide.

import { LLMS, VARIANTS } from './models.js';

const DB_NAME = 'earwing-local-models';
const STORE = 'kv';
const KEY_HANDLE = 'directoryHandle';
const KEY_META = 'meta';
const MAX_DEPTH = 3; // the picked folder and two levels of subfolders (<repo>/onnx/)

export const fsAccessSupported = () =>
  typeof window !== 'undefined' && typeof window.showDirectoryPicker === 'function';

export const webkitDirectorySupported = () =>
  typeof document !== 'undefined' && 'webkitdirectory' in document.createElement('input');

let dbPromise = null;
function openDb() {
  if (!dbPromise) {
    dbPromise = new Promise((resolve, reject) => {
      const req = indexedDB.open(DB_NAME, 1);
      req.onupgradeneeded = () => {
        if (!req.result.objectStoreNames.contains(STORE)) req.result.createObjectStore(STORE);
      };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
    dbPromise.catch(() => {
      dbPromise = null;
    });
  }
  return dbPromise;
}

async function idbGet(key) {
  const db = await openDb();
  return new Promise((resolve, reject) => {
    const req = db.transaction(STORE, 'readonly').objectStore(STORE).get(key);
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

async function idbWrite(ops) {
  const db = await openDb();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, 'readwrite');
    ops(tx.objectStore(STORE));
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
}

// ---------------------------------------------------------------- handle persistence
let currentHandle = null;

export const currentFolderHandle = () => currentHandle;

// Saved folder from an earlier visit → { name, permission } (no prompt), or null.
export async function loadSavedFolder() {
  if (!fsAccessSupported()) return null;
  try {
    const [handle, meta] = await Promise.all([idbGet(KEY_HANDLE), idbGet(KEY_META)]);
    if (!handle) return null;
    currentHandle = handle;
    const permission = await handle.queryPermission({ mode: 'read' });
    return { name: (meta && meta.name) || handle.name, permission };
  } catch (e) {
    console.warn('[localModels] could not restore the saved folder', e);
    return null;
  }
}

// Must be called straight from a click handler (the picker needs a user gesture).
export async function pickFolder() {
  const handle = await window.showDirectoryPicker({ id: 'earwing-models', mode: 'read' });
  currentHandle = handle; // the picker already granted read access
  await idbWrite((s) => {
    s.put(handle, KEY_HANDLE);
    s.put({ name: handle.name, chosenAt: Date.now() }, KEY_META);
  });
  return { name: handle.name, permission: 'granted' };
}

// Call synchronously from a click handler (before any other await) when the saved
// folder's permission is 'prompt'. Resolves to the new permission state.
export function requestFolderAccess() {
  if (!currentHandle) return Promise.resolve('denied');
  return currentHandle.requestPermission({ mode: 'read' }).catch(() => 'denied');
}

export async function forgetFolder() {
  currentHandle = null;
  await idbWrite((s) => {
    s.delete(KEY_HANDLE);
    s.delete(KEY_META);
  });
}

// Dev/test seam: adopt a directory handle (e.g. from OPFS) without the native picker,
// which automation can't drive. Saved like a picked folder, so reloads exercise the
// restore path too.
export async function __adoptHandleForTesting(handle) {
  currentHandle = handle;
  await idbWrite((s) => {
    s.put(handle, KEY_HANDLE);
    s.put({ name: handle.name, chosenAt: Date.now() }, KEY_META);
  });
  return { name: handle.name, permission: await handle.queryPermission({ mode: 'read' }) };
}

// ---------------------------------------------------------------- resolving files
const basename = (p) => p.slice(p.lastIndexOf('/') + 1);
const NEEDED = new Set([
  ...Object.values(VARIANTS).flatMap((v) => Object.values(v.files).map((f) => f.name)),
  ...Object.values(LLMS).flatMap((m) => Object.keys(m.files).map(basename)),
]);

// Candidate entries { name, path, size, file } from a directory handle, statting only
// files whose names we need.
export async function entriesFromHandle(dir) {
  const out = [];
  async function walk(d, prefix, depth) {
    for await (const entry of d.values()) {
      const path = prefix ? `${prefix}/${entry.name}` : entry.name;
      if (entry.kind === 'directory') {
        if (depth < MAX_DEPTH) await walk(entry, path, depth + 1);
      } else if (NEEDED.has(entry.name)) {
        const file = await entry.getFile(); // metadata only; nothing is read yet
        out.push({ name: entry.name, path, size: file.size, file });
      }
    }
  }
  await walk(dir, '', 1);
  return out;
}

// Same, from an <input webkitdirectory> FileList (paths via webkitRelativePath; the
// first segment is the picked folder's own name, which we drop).
export function entriesFromFileList(list) {
  return Array.from(list)
    .filter((f) => NEEDED.has(f.name))
    .map((f) => ({
      name: f.name,
      path: (f.webkitRelativePath || f.name).split('/').slice(1).join('/') || f.name,
      size: f.size,
      file: f,
    }));
}

// For each variant: { files: { role: File } } if complete, else { missing: [text] }.
export function resolveVariants(entries) {
  const sets = {};
  for (const [key, v] of Object.entries(VARIANTS)) {
    const files = {};
    const missing = [];
    for (const [role, want] of Object.entries(v.files)) {
      const named = entries.filter((e) => e.name === want.name);
      const sized = named.filter((e) => e.size === want.size);
      const best =
        sized.find((e) => e.path.split('/').includes(v.dir)) ||
        sized.find((e) => !e.path.includes('/')) ||
        sized[0];
      if (best) files[role] = best.file;
      else if (named.length) {
        missing.push(`${want.name} (wrong size: ${named[0].size.toLocaleString()} bytes, expected ${want.size.toLocaleString()})`);
      } else missing.push(want.name);
    }
    sets[key] = missing.length ? { missing, dir: v.dir } : { files };
  }
  return sets;
}

export const anyComplete = (sets) => Object.values(sets).some((s) => s.files);

// One LLM (models.js LLMS[key]) from the same entries →
//   { key, dir, files: { [repoRelativeName]: File } | null, missing: [repoRelativeName] }
// `files` is null unless every file is there with its exact size. Among same-name,
// same-size candidates, prefer <dir>/<name> (the ./models root was picked), then <name>
// itself (the repo folder was picked), then anything ending in /<name>.
export function resolveLlm(entries, key) {
  const m = LLMS[key];
  if (!m) throw new Error(`unknown LLM ${key}`);
  const files = {};
  const missing = [];
  for (const [name, size] of Object.entries(m.files)) {
    const sized = (entries || []).filter((e) => e.name === basename(name) && e.size === size);
    const best =
      sized.find((e) => e.path === `${m.dir}/${name}` || e.path.endsWith(`/${m.dir}/${name}`)) ||
      sized.find((e) => e.path === name) ||
      sized.find((e) => e.path.endsWith(`/${name}`)) ||
      sized[0];
    if (best) files[name] = best.file;
    else missing.push(name);
  }
  return { key, dir: m.dir, files: missing.length ? null : files, missing };
}
