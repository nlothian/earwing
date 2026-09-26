import { useCallback, useEffect, useRef, useState } from 'react';
import { decodeFileTo16kMono, rms, sliceSamples } from './audio.js';
import { startDeviceStream } from './deviceStream.js';
import {
  __adoptHandleForTesting,
  anyComplete,
  currentFolderHandle,
  entriesFromFileList,
  entriesFromHandle,
  forgetFolder,
  fsAccessSupported,
  loadSavedFolder,
  pickFolder,
  requestFolderAccess,
  resolveVariants,
  webkitDirectorySupported,
} from './localModels.js';
import DeviceStrip from './DeviceStrip.jsx';
import { IconCheck, IconCopy, IconListen, IconStop, IconUpload } from './Icons.jsx';

const SR = 16000;

// Speech gate: windows are only (re)transcribed once a 100 ms block since the
// cursor looked like speech, so idle silence doesn't burn CPU. rigar used a fixed
// RMS of 0.01, tuned for a laptop mic with AGC off. The device's beam (and raw
// mics) can run quieter than that, and a gate that never opens means nothing gets
// transcribed at all, so here it's adaptive: track the noise floor (drops at
// once, creeps up ~1% per block, i.e. doubles in ~7 s) and open at ~+8 dB above
// it, clamped to [0.003, 0.01] (≈ -50 … -40 dBFS) — never stricter than rigar's.
const GATE_MIN = 0.003;
const GATE_MAX = 0.01;
const GATE_OVER_FLOOR = 2.5;
const FLOOR_RISE = 1.01;

// Cursor-based overlapping windows (Keet v4 strategy). Each window runs from the
// mature cursor (end of the last finalized sentence) to the live edge and is
// re-transcribed in full, so the un-finalized tail always has full context.
const MIN_INITIAL_SEC = 1.5; // emit the first window once this much audio exists
const MIN_WINDOW_SEC = 3.0; // later windows need at least this span past the cursor
const MAX_WINDOW_SEC = 30.0; // cap the re-transcribed span (drops oldest if longer)
const FIRE_INTERVAL_MS = 1200; // re-fire a window at most this often (per new audio)
// Stability debounce: commit the pending tail once its text has stopped changing
// for this long, regardless of punctuation (so a final sentence doesn't linger).
const STABILITY_MS = 3000;

const SOURCES = [
  { value: 0, label: 'Beam (steered)' },
  { value: 1, label: 'Raw mic 1' },
  { value: 2, label: 'Raw mic 2' },
  { value: 3, label: 'Raw mic 3' },
  { value: 4, label: 'Raw mic 4' },
];

function joinText(a, b) {
  const left = (a || '').replace(/\s+$/, '');
  const right = (b || '').trim();
  if (!left) return right;
  if (!right) return left;
  return `${left} ${right}`;
}

// ?backend=wasm|webgpu forces a backend (for testing); default: pick automatically.
function requestedBackend() {
  try {
    const b = new URLSearchParams(window.location.search).get('backend');
    if (b === 'wasm' || b === 'webgpu') return b;
  } catch {
    /* ignore */
  }
  return 'auto';
}

const fmtBytes = (n) => (n >= 1e9 ? `${(n / 1e9).toFixed(2)} GB` : `${Math.round(n / 1e6)} MB`);

function loadErrorMessage(detail, plan) {
  const size = plan ? `~${fmtBytes(plan.bytes)}, once` : 'about 1 GB, once';
  return (
    `Could not load the speech model. It downloads (${size}) from Hugging Face and ` +
    'jsDelivr, so this browser needs internet access — the device only serves the page.' +
    (detail ? ` (${detail})` : '')
  );
}

function sourceLabel(eng) {
  if (eng.source === 'local') return `model: local folder “${eng.folder}”`;
  if (eng.source === 'hf-cache') return 'model: Hugging Face (cached)';
  if (eng.source === 'hf-download') return 'model: Hugging Face (downloaded)';
  return '';
}

function engineLabel(eng) {
  if (eng.backend === 'webgpu') return `running on WebGPU (fp16${eng.gpu ? `, ${eng.gpu}` : ''})`;
  const t = eng.threads || 1;
  return `running on CPU (WASM, ${eng.quant || 'int8'}, ${t} thread${t === 1 ? '' : 's'})`;
}

export default function Transcriber() {
  // idle | loading | ready | listening | transcribing | unsupported | error
  const [status, setStatus] = useState('idle');
  const [progress, setProgress] = useState(0);
  const [engine, setEngine] = useState(null); // { backend, quant, threads, gpu, note, fallback }
  const [plan, setPlan] = useState(null); // { backend, quant, bytes, source, folder }: loading now
  // Local model folder: null | { kind: 'handle', name, permission } (File System Access,
  // remembered) | { kind: 'files', name, entries } (<input webkitdirectory>, this visit only)
  const [folder, setFolder] = useState(null);
  const [fsSupported, setFsSupported] = useState(false);
  const [offerHf, setOfferHf] = useState(false); // show "Download from Hugging Face instead"
  const useHfRef = useRef(false); // user chose HF over the folder for this visit
  const [hfChosen, setHfChosen] = useState(false); // (the same, for rendering)
  const folderRef = useRef(null);
  folderRef.current = folder;
  const dirInputRef = useRef(null);
  const [transcript, setTranscript] = useState(''); // committed text
  const [partial, setPartial] = useState(''); // segment currently decoding (dimmed)
  const [errorMsg, setErrorMsg] = useState('');
  const [copied, setCopied] = useState(false);
  const [source, setSource] = useState(0); // 0 = beam, 1-4 = raw mic
  const [link, setLink] = useState(''); // connecting | streaming | reconnecting
  const [env, setEnv] = useState({ secure: true, isolated: true, host: '' }); // host: location.hostname

  const workerRef = useRef(null);
  const onMsgRef = useRef(() => {});
  const loadStateRef = useRef('none'); // none | loading | ready
  const readyWaitersRef = useRef([]); // [{ resolve, reject }]
  const statusRef = useRef('idle');
  const fileInputRef = useRef(null);

  // Listening / windowing state. We keep recent captured audio and repeatedly send
  // the worker an overlapping window [cursor .. live edge]; the worker's merger
  // finalizes complete sentences and reports the mature cursor back, which we
  // mirror here so the next window starts where the transcript went stable.
  const streamRef = useRef(null);
  const chunksRef = useRef([]); // 16 kHz chunks from baseRef onward
  const baseRef = useRef(0); // absolute sample index of chunksRef[0] (older audio pruned)
  const totalRef = useRef(0); // head: total samples captured
  const cursorRef = useRef(0); // mature cursor in samples (nothing re-transcribed before it)
  const lastFireRef = useRef(0); // head sample count when we last fired a window
  const busyRef = useRef(false); // a window/finalize is currently in flight
  const maturedRef = useRef(false); // has any sentence finalized? (initial vs normal min span)
  const speechInWindowRef = useRef(false); // speech present since the cursor (gates windowing)
  const floorRef = useRef(null); // running noise-floor RMS for the adaptive speech gate
  const pendingTextRef = useRef(''); // last immature tail seen (for stability debounce)
  const pendingSinceRef = useRef(0); // timestamp the pending tail last changed

  const setStatusBoth = useCallback((s) => {
    statusRef.current = s;
    setStatus(s);
  }, []);

  // Audio in [start, end) (absolute sample indices), from the retained chunks.
  const slice = useCallback(
    (start, end) => sliceSamples(chunksRef.current, start - baseRef.current, end - baseRef.current),
    [],
  );

  // Drop chunks no window can reach any more: windows never start before the
  // cursor, nor more than MAX_WINDOW_SEC behind the head. Keeps memory flat
  // over long listening sessions (rigar kept every sample).
  const prune = useCallback(() => {
    const keepFrom = Math.max(cursorRef.current, totalRef.current - MAX_WINDOW_SEC * SR);
    const chunks = chunksRef.current;
    let drop = 0;
    let base = baseRef.current;
    while (drop < chunks.length - 1 && base + chunks[drop].length <= keepFrom) {
      base += chunks[drop].length;
      drop += 1;
    }
    if (drop) {
      chunks.splice(0, drop);
      baseRef.current = base;
    }
  }, []);

  // Send an overlapping window [cursor .. live edge] to the worker for a fresh
  // transcription. Self-throttled: one window in flight at a time, and only
  // re-fired once enough new audio has arrived and the span clears the minimum.
  const fireWindow = useCallback(() => {
    if (busyRef.current) return;
    if (!speechInWindowRef.current) return; // don't re-transcribe idle silence
    const head = totalRef.current;
    let start = cursorRef.current;
    const minSamples = (maturedRef.current ? MIN_WINDOW_SEC : MIN_INITIAL_SEC) * SR;
    if (head - start < minSamples) return;
    if (head - lastFireRef.current < (FIRE_INTERVAL_MS * SR) / 1000) return;

    // Cap the re-transcribed span; never start before the mature cursor.
    const maxSamples = MAX_WINDOW_SEC * SR;
    if (head - start > maxSamples) start = head - maxSamples;

    busyRef.current = true;
    lastFireRef.current = head;
    const audio = slice(start, head);
    workerRef.current.postMessage({ type: 'window', audio, startTime: start / SR });
  }, [slice]);

  // Called for every ~100 ms block from the device. Drives the window scheduler
  // and the stability debounce: once the pending tail's text has been unchanged
  // for STABILITY_MS, commit it (so a final sentence doesn't stay dimmed forever).
  const onAudioChunk = useCallback(
    (chunk) => {
      chunksRef.current.push(chunk);
      totalRef.current += chunk.length;

      const level = rms(chunk);
      const floor = floorRef.current;
      floorRef.current = floor === null || level < floor ? level : floor * FLOOR_RISE;
      const gate = Math.min(GATE_MAX, Math.max(GATE_MIN, (floor ?? level) * GATE_OVER_FLOOR));
      if (level >= gate) speechInWindowRef.current = true;

      fireWindow();

      if (
        pendingTextRef.current &&
        !busyRef.current &&
        Date.now() - pendingSinceRef.current >= STABILITY_MS
      ) {
        busyRef.current = true; // gate windows/triggers until the commit returns
        workerRef.current.postMessage({ type: 'finalize-pending' });
      }
      prune();
    },
    [fireWindow, prune],
  );

  // Keep the worker message handler fresh without recreating the worker.
  useEffect(() => {
    onMsgRef.current = (msg) => {
      switch (msg.type) {
        case 'plan':
          setPlan({
            backend: msg.backend,
            quant: msg.quant,
            bytes: msg.bytes,
            source: msg.source,
            folder: msg.folder,
          });
          setProgress(0);
          break;
        case 'progress':
          setProgress(msg.progress);
          break;
        case 'ready':
          loadStateRef.current = 'ready';
          setEngine({
            backend: msg.backend,
            quant: msg.quant,
            threads: msg.threads,
            gpu: msg.gpu,
            note: msg.note,
            fallback: msg.fallback,
            source: msg.source,
            folder: msg.folder,
          });
          readyWaitersRef.current.forEach((w) => w.resolve());
          readyWaitersRef.current = [];
          break;
        case 'merged': {
          // A window (or pending-commit) finished: mature text is committed,
          // immature is the live dimmed tail. Mirror the worker's cursor so the
          // next window starts there, and (re)arm the stability debounce on any
          // change to the tail.
          busyRef.current = false;
          cursorRef.current = Math.round((msg.matureCursorTime || 0) * SR);
          if (msg.matureCursorTime > 0) maturedRef.current = true;
          const nextPending = msg.immatureText || '';
          if (nextPending !== pendingTextRef.current) {
            pendingTextRef.current = nextPending;
            pendingSinceRef.current = Date.now();
          }
          // Tail committed/empty → stop windowing until the next speech arrives.
          if (!nextPending) speechInWindowRef.current = false;
          setTranscript(msg.matureText || '');
          setPartial(nextPending);
          break;
        }
        case 'complete':
          // Final transcript from finalize-all (stream) or a finished file.
          busyRef.current = false;
          setPartial('');
          setTranscript(msg.text);
          setStatusBoth('ready');
          break;
        case 'error':
          busyRef.current = false;
          setPartial('');
          if (msg.during === 'load' || loadStateRef.current === 'loading') {
            // Model download/init failed: let the next Start/Upload retry it.
            loadStateRef.current = 'none';
            const fromFolder = msg.code === 'local-missing' || (plan && plan.source === 'local');
            setOfferHf(fromFolder);
            const err = new Error(
              fromFolder && msg.code === 'local-missing'
                ? msg.error
                : fromFolder
                  ? `Could not load the model from the folder “${plan.folder}”: ${msg.error}`
                  : loadErrorMessage(msg.error, plan),
            );
            readyWaitersRef.current.forEach((w) => w.reject(err));
            readyWaitersRef.current = [];
            break; // the waiter's catch shows the message
          }
          if (streamRef.current) {
            streamRef.current.stop(); // don't keep holding one of the device's 3 audio-stream slots
            streamRef.current = null;
            setLink('');
          }
          setErrorMsg(
            (msg.error || 'Something went wrong.') +
              (engine && engine.backend === 'webgpu'
                ? ' If this keeps happening on WebGPU, add ?backend=wasm to the address to use the CPU model.'
                : ''),
          );
          setStatusBoth('error');
          break;
        default:
          break;
      }
    };
  });

  // Create the worker once.
  useEffect(() => {
    setEnv({
      secure: window.isSecureContext,
      isolated: window.crossOriginIsolated === true,
      host: window.location.hostname,
    });
    const supported =
      typeof Worker !== 'undefined' &&
      typeof WebAssembly !== 'undefined' &&
      typeof ReadableStream !== 'undefined';
    if (!supported) {
      setStatusBoth('unsupported');
      return undefined;
    }

    const worker = new Worker(new URL('./parakeet.worker.js', import.meta.url), {
      type: 'module',
    });
    workerRef.current = worker;
    worker.onmessage = (e) => onMsgRef.current(e.data || {});
    worker.onerror = (e) => {
      setErrorMsg(e.message || 'The transcription engine failed to load.');
      setStatusBoth('error');
    };

    return () => {
      if (streamRef.current) streamRef.current.stop();
      worker.terminate();
    };
  }, [setStatusBoth]);

  // The local folder's model files, as { name, sets } for the worker, or null to use
  // Hugging Face. Throws (with a message for the page) if the folder can't be used.
  const localPayload = useCallback(async () => {
    const f = folderRef.current;
    if (!f || useHfRef.current) return null;
    let entries;
    if (f.kind === 'files') entries = f.entries;
    else {
      if (f.permission !== 'granted') {
        throw new Error(
          `This browser needs your OK to read the model folder “${f.name}” again. Click ` +
            '“Use local model folder”, or forget it to download from Hugging Face.',
        );
      }
      entries = await entriesFromHandle(currentFolderHandle());
    }
    const sets = resolveVariants(entries);
    if (!anyComplete(sets)) {
      const e = new Error(
        `No complete Parakeet model in the folder “${f.name}”. Pick the models folder made ` +
          'by `npm run models` (or one of its subfolders). Missing: ' +
          Object.values(sets)
            .map((x) => `${x.dir}/ ${x.missing.join(', ')}`)
            .join('; ') +
          '.',
      );
      e.offerHf = true;
      throw e;
    }
    return { name: f.name, sets };
  }, []);

  const ensureModel = useCallback(
    () =>
      new Promise((resolve, reject) => {
        if (loadStateRef.current === 'ready') {
          resolve();
          return;
        }
        readyWaitersRef.current.push({ resolve, reject });
        if (loadStateRef.current === 'none') {
          loadStateRef.current = 'loading';
          setProgress(0);
          setPlan(null);
          setOfferHf(false);
          setStatusBoth('loading');
          localPayload().then(
            (local) => workerRef.current.postMessage({ type: 'load', backend: requestedBackend(), local }),
            (err) => {
              loadStateRef.current = 'none';
              setOfferHf(true);
              readyWaitersRef.current.forEach((w) => w.reject(err));
              readyWaitersRef.current = [];
            },
          );
        }
      }),
    [localPayload, setStatusBoth],
  );

  // A saved folder whose permission is 'prompt' needs a user gesture to re-grant:
  // call this first thing in a click handler (Start, File, the folder button).
  const regrantFolder = useCallback(() => {
    const f = folderRef.current;
    if (!f || f.kind !== 'handle' || f.permission === 'granted' || useHfRef.current) {
      return Promise.resolve();
    }
    return requestFolderAccess().then((permission) => {
      const next = { ...f, permission };
      folderRef.current = next;
      setFolder(next);
    });
  }, []);

  // Restore a folder picked on an earlier visit (File System Access API only).
  useEffect(() => {
    const supported = fsAccessSupported();
    setFsSupported(supported);
    if (supported) {
      loadSavedFolder().then((saved) => {
        if (saved && !folderRef.current) setFolder({ kind: 'handle', ...saved });
      });
    }
    // Dev-only seam for automated tests: the native directory picker can't be driven,
    // so hand the loader a directory handle (e.g. from OPFS) directly.
    if (import.meta.env && import.meta.env.DEV) {
      window.__gulltopprUseFolderHandle = async (handle) => {
        const f = { kind: 'handle', ...(await __adoptHandleForTesting(handle)) };
        setFolder(f);
        return f;
      };
    }
  }, []);

  const onPickFolder = useCallback(async () => {
    if (!fsSupported) {
      if (dirInputRef.current) dirInputRef.current.click();
      return;
    }
    try {
      const picked = await pickFolder();
      useHfRef.current = false;
      setHfChosen(false);
      setOfferHf(false);
      setErrorMsg('');
      setFolder({ kind: 'handle', ...picked });
    } catch (e) {
      if (e && e.name !== 'AbortError') setErrorMsg(`Couldn’t open that folder: ${e.message || e}`);
    }
  }, [fsSupported]);

  const onDirInput = useCallback((e) => {
    const list = e.target.files;
    if (!list || !list.length) return;
    const name = (list[0].webkitRelativePath || '').split('/')[0] || 'folder';
    useHfRef.current = false;
    setHfChosen(false);
    setOfferHf(false);
    setErrorMsg('');
    setFolder({ kind: 'files', name, entries: entriesFromFileList(list) });
    e.target.value = '';
  }, []);

  const onUseFolder = useCallback(() => {
    regrantFolder();
  }, [regrantFolder]);

  const onForgetFolder = useCallback(async () => {
    if (folderRef.current && folderRef.current.kind === 'handle') await forgetFolder();
    setFolder(null);
    setOfferHf(false);
  }, []);

  // "Download from Hugging Face instead": skip the folder for this visit and load now.
  const onUseHf = useCallback(() => {
    useHfRef.current = true;
    setHfChosen(true);
    setOfferHf(false);
    setErrorMsg('');
    ensureModel().then(
      () => setStatusBoth('ready'),
      (err) => {
        setErrorMsg(err.message);
        setStatusBoth('error');
      },
    );
  }, [ensureModel, setStatusBoth]);

  const stopListening = useCallback(() => {
    if (streamRef.current) {
      streamRef.current.stop();
      streamRef.current = null;
    }
    setLink('');
    // Transcribe whatever remains past the cursor, then commit the pending tail.
    const head = totalRef.current;
    const start = Math.max(cursorRef.current, head - MAX_WINDOW_SEC * SR);
    if (head > start) {
      const audio = slice(start, head);
      workerRef.current.postMessage({ type: 'window', audio, startTime: start / SR });
    }
    workerRef.current.postMessage({ type: 'finalize-all' });
    setStatusBoth('transcribing');
  }, [setStatusBoth, slice]);

  const startListening = useCallback(async () => {
    const regrant = regrantFolder(); // before any await: needs the click's user activation
    setErrorMsg('');
    setCopied(false);
    try {
      await regrant;
      await ensureModel();
    } catch (err) {
      setErrorMsg(err.message);
      setStatusBoth('error');
      return;
    }
    chunksRef.current = [];
    baseRef.current = 0;
    totalRef.current = 0;
    cursorRef.current = 0;
    lastFireRef.current = 0;
    busyRef.current = false;
    maturedRef.current = false;
    speechInWindowRef.current = false;
    floorRef.current = null;
    pendingTextRef.current = '';
    pendingSinceRef.current = 0;
    setPartial('');
    setTranscript('');

    workerRef.current.postMessage({ type: 'stream-start' });
    setStatusBoth('listening');
    const handle = startDeviceStream(onAudioChunk, {
      source,
      onState: (state, info) =>
        setLink(state === 'reconnecting' ? `reconnecting (${info.attempt}/${info.max})…` : state),
      onError: (message) => {
        if (streamRef.current !== handle) return; // a stale stream
        // Keep what was heard: finish the transcript, then show why it stopped.
        stopListening();
        setErrorMsg(message);
      },
    });
    streamRef.current = handle;
  }, [ensureModel, onAudioChunk, regrantFolder, setStatusBoth, source, stopListening]);

  const onListenClick = useCallback(() => {
    if (statusRef.current === 'listening') stopListening();
    else startListening();
  }, [startListening, stopListening]);

  const onFile = useCallback(
    async (e) => {
      const file = e.target.files && e.target.files[0];
      e.target.value = '';
      if (!file) return;
      setErrorMsg('');
      setCopied(false);
      try {
        await ensureModel();
      } catch (err) {
        setErrorMsg(err.message);
        setStatusBoth('error');
        return;
      }
      try {
        setPartial('');
        setTranscript('');
        setStatusBoth('transcribing');
        const audio = await decodeFileTo16kMono(file);
        workerRef.current.postMessage({ type: 'transcribe', audio });
      } catch (err) {
        console.error('[Transcriber] file error', err);
        setErrorMsg('Could not read that audio file — try a WAV, MP3, or M4A.');
        setStatusBoth('error');
      }
    },
    [ensureModel, setStatusBoth],
  );

  const isListening = status === 'listening';
  const isBusy = status === 'loading' || status === 'transcribing';
  const live = isListening || status === 'transcribing';
  const tail = live ? partial : '';
  const hasText = !!(transcript || tail);
  const copyText = joinText(transcript, tail);
  const blocked = status === 'unsupported';

  const onCopy = useCallback(async () => {
    if (!copyText) return;
    try {
      await navigator.clipboard.writeText(copyText);
    } catch {
      // navigator.clipboard needs a secure context; http://gulltoppr.local isn't one.
      const ta = document.createElement('textarea');
      ta.value = copyText;
      ta.setAttribute('readonly', '');
      ta.style.position = 'fixed';
      ta.style.opacity = '0';
      document.body.appendChild(ta);
      ta.select();
      try {
        document.execCommand('copy');
      } catch {
        /* give up quietly */
      }
      ta.remove();
    }
    setCopied(true);
    setTimeout(() => setCopied(false), 1500);
  }, [copyText]);

  const listenLabel = isListening ? 'Stop' : 'Start listening';

  return (
    <>
      <DeviceStrip />

      <main className="main">
        <section className="card">
          <div className="controls">
            <button
              type="button"
              className={`btn ${isListening ? 'btn--stop' : 'btn--primary'}`}
              onClick={onListenClick}
              disabled={blocked || isBusy}
              aria-pressed={isListening}
            >
              {isListening ? <IconStop /> : <IconListen />}
              <span>{listenLabel}</span>
            </button>

            <label className="select">
              <span className="visually-hidden">Audio source</span>
              <select
                name="source"
                value={source}
                onChange={(e) => setSource(Number(e.target.value))}
                disabled={isListening || isBusy}
                title="Audio source on the device"
              >
                {SOURCES.map((s) => (
                  <option key={s.value} value={s.value}>
                    {s.label}
                  </option>
                ))}
              </select>
            </label>

            <button
              type="button"
              className="btn btn--ghost"
              onClick={() => fileInputRef.current && fileInputRef.current.click()}
              disabled={blocked || isBusy || isListening}
              title="Transcribe an audio file instead"
            >
              <IconUpload />
              <span>File</span>
            </button>
            <input ref={fileInputRef} name="file" type="file" accept="audio/*" onChange={onFile} hidden />
            <input
              ref={dirInputRef}
              name="modeldir"
              type="file"
              webkitdirectory=""
              onChange={onDirInput}
              hidden
            />
          </div>

          {!blocked && !engine && status !== 'loading' && (fsSupported || folder || webkitDirectorySupported()) && (
            <div className="folder">
              {!folder ? (
                <>
                  <button type="button" className="linkbtn" onClick={onPickFolder}>
                    Load model from folder…
                  </button>
                  <span>
                    {fsSupported
                      ? ' instead of downloading it from Hugging Face'
                      : ' instead of downloading it (over plain http the browser can’t remember the folder; pick it each visit)'}
                  </span>
                </>
              ) : folder.kind === 'handle' && folder.permission !== 'granted' && !hfChosen ? (
                <>
                  <button type="button" className="linkbtn" onClick={onUseFolder}>
                    Use local model folder ({folder.name})
                  </button>
                  <button type="button" className="linkbtn linkbtn--quiet" onClick={onForgetFolder}>
                    forget folder
                  </button>
                </>
              ) : (
                <>
                  <span>
                    {hfChosen
                      ? `Using Hugging Face this visit, not the folder “${folder.name}”`
                      : `Model from folder “${folder.name}”${folder.kind === 'files' ? ' (this visit only)' : ''}`}
                  </span>
                  <button type="button" className="linkbtn linkbtn--quiet" onClick={onPickFolder}>
                    change
                  </button>
                  <button type="button" className="linkbtn linkbtn--quiet" onClick={onForgetFolder}>
                    {folder.kind === 'handle' ? 'forget folder' : 'clear'}
                  </button>
                </>
              )}
            </div>
          )}

          {blocked ? (
            <p className="notice notice--error">
              This browser can’t run in-browser transcription — it needs Web Workers, WebAssembly
              and streaming fetch. Try a recent Chrome, Edge, Firefox, or Safari.
            </p>
          ) : (
            <>
              {!env.secure && (
                <p className="notice">
                  This page is plain http, so transcription runs on the CPU with one thread
                  (slower, but it works). For WebGPU and multi-threading, open{' '}
                  <a href={`https://${env.host || 'gulltoppr.local'}/`}>
                    https://{env.host || 'gulltoppr.local'}/
                  </a>
                  . The device’s certificate is self-signed: to trust it, see{' '}
                  <a href="/status">Status</a> or download <a href="/gulltoppr.crt">gulltoppr.crt</a>.
                  (Or, in Chrome, add this origin to{' '}
                  <code>chrome://flags/#unsafely-treat-insecure-origin-as-secure</code>.)
                </p>
              )}
              {env.secure && !env.isolated && (
                <p className="notice">
                  This page isn’t cross-origin isolated, so CPU transcription runs single-threaded
                  (slower, but it works).
                </p>
              )}

              {engine && engine.fallback && (
                <p className="notice">
                  {engine.note}. Using the CPU (int8) model for now; reload the page to try
                  WebGPU again.
                </p>
              )}

              {status === 'loading' && (
                <div>
                  <div className="progress" role="progressbar" aria-valuenow={progress}>
                    <div className="progress__bar" style={{ width: `${progress}%` }} />
                  </div>
                  <p className="meta">
                    {!plan
                      ? 'Checking this browser for WebGPU…'
                      : plan.source === 'local'
                        ? `Loading the Parakeet model for ${plan.backend === 'webgpu' ? 'WebGPU (fp16)' : 'CPU (int8)'} from the folder “${plan.folder}” (${fmtBytes(plan.bytes)})…`
                        : plan.source === 'hf-cache'
                          ? `Loading the Parakeet model for ${plan.backend === 'webgpu' ? 'WebGPU (fp16)' : 'CPU (int8)'} from this browser’s cache…`
                          : `Downloading the Parakeet model for ${plan.backend === 'webgpu' ? 'WebGPU (fp16)' : 'CPU (int8)'}… ${progress}% — about ${fmtBytes(plan.bytes)} from Hugging Face on first use (needs internet), then cached in this browser.`}
                  </p>
                </div>
              )}

              {errorMsg && (
                <p className="notice notice--error">
                  {errorMsg}
                  {offerHf && (
                    <>
                      {' '}
                      <button type="button" className="linkbtn" onClick={onUseHf}>
                        Download from Hugging Face instead
                      </button>
                    </>
                  )}
                </p>
              )}

              <div className="transcript-wrap">
                <button
                  type="button"
                  className={`copy${copied ? ' copy--copied' : ''}`}
                  onClick={onCopy}
                  disabled={!copyText}
                  title={copied ? 'Copied' : 'Copy transcript'}
                  aria-label={copied ? 'Copied' : 'Copy transcript'}
                >
                  {copied ? <IconCheck size={18} /> : <IconCopy size={18} />}
                </button>
                <div
                  className={`transcript${hasText ? '' : ' transcript--empty'}`}
                  aria-live="polite"
                >
                  {hasText ? (
                    <>
                      {transcript ? <span>{transcript}</span> : null}
                      {tail ? (
                        <span className="partial">
                          {transcript ? ' ' : ''}
                          {tail}
                        </span>
                      ) : null}
                    </>
                  ) : (
                    'The transcript of what gulltoppr hears will appear here. Press Start listening, or transcribe an audio file.'
                  )}
                </div>
              </div>

              <p className="meta">
                {engine
                  ? `${engineLabel(engine)} · ${sourceLabel(engine)}`
                  : status === 'loading'
                    ? 'loading model…'
                    : 'model loads on first use'}
                {engine && engine.backend !== 'webgpu' && engine.note && !engine.fallback && env.secure ? (
                  <span> · {engine.note}</span>
                ) : null}
                {isListening ? ` · ${link === 'streaming' ? 'listening…' : link || 'connecting…'}` : ''}
                {status === 'transcribing' ? ' · transcribing…' : ''}
              </p>
            </>
          )}
        </section>
      </main>
    </>
  );
}
