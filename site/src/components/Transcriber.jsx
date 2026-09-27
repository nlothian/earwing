import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
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
import { DIARIZATION } from './models.js';
import { buildTimeline, speakerName, toTurns, turnsToText, turnText } from './speakerTimeline.js';
import {
  addPrint,
  forgetAllVoices,
  forgetVoice,
  listVoices,
  MATCH,
  nextMatches,
  removePrint,
  renameVoice,
  sameName,
} from './voiceStore.js';
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

// "Label speakers" (diarize.worker.js) is remembered per browser; on by default.
const SPEAKERS_KEY = 'earwing-speakers';
function loadSpeakersPref() {
  try {
    return window.localStorage.getItem(SPEAKERS_KEY) !== 'off';
  } catch {
    return true;
  }
}
const DIAR_BYTES = Object.values(DIARIZATION).reduce((n, m) => n + m.size, 0);

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

function speakersLabel(diar, n, final, live) {
  if (diar.state === 'loading') return `speakers: loading models ${diar.progress || 0}%`;
  if (diar.state === 'error') return `speakers: off (${diar.error})`;
  if (diar.stage === 'file') return `speakers: labelling ${diar.progress || 0}%`;
  if (diar.stage === 'finish') return 'speakers: tidying up…';
  if (!n) return live ? 'speakers: listening for voices' : 'speakers: ready';
  return `${n} speaker${n === 1 ? '' : 's'}${final ? '' : ' so far'}`;
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

  // Speaker labels. The diarization worker is created on first use (and only if the
  // toggle is on), so nothing is downloaded for people who don't want it.
  const [speakersOn, setSpeakersOn] = useState(true);
  const speakersOnRef = useRef(true);
  speakersOnRef.current = speakersOn;
  const [diar, setDiar] = useState({ state: 'idle', progress: 0, error: '' }); // idle | loading | ready | error
  const [segments, setSegments] = useState([]); // [{ start, end, speaker }]
  const [segmentsFinal, setSegmentsFinal] = useState(false);
  // Names the user gave the speakers, keyed by the diarizer's speaker name (stable
  // across reclusters, unlike display numbers); cleared with each new take.
  const [speakerNames, setSpeakerNames] = useState({});
  const speakerNamesRef = useRef({});
  speakerNamesRef.current = speakerNames;
  const [renaming, setRenaming] = useState(null); // { who, turn, draft } while a label is being edited
  // Known voices (voiceStore.js): people named in earlier takes, recognised by voice.
  const [knownVoices, setKnownVoices] = useState([]);
  const [takeVoices, setTakeVoices] = useState([]); // this take's centroids: [{ speaker, centroid, weight }]
  const takeVoicesRef = useRef([]);
  takeVoicesRef.current = takeVoices;
  const [matches, setMatches] = useState({}); // { [speaker]: { id, sim, auto } }
  const matchesRef = useRef({});
  matchesRef.current = matches;
  const dismissedRef = useRef({}); // { [speaker]: Set of person ids } matches the user overrode
  const savedRef = useRef({}); // { [speaker]: { id } | { name } } prints saved (or owed) this take
  const sessionRef = useRef(''); // this take's id: one print per person per take
  const [voiceEdit, setVoiceEdit] = useState(null); // { id, draft } while renaming a known voice
  const diarWorkerRef = useRef(null);
  const diarLoadRef = useRef(false); // 'load' posted
  const onDiarMsgRef = useRef(() => {});
  // Timed words from the transcription worker, to line up with the segments.
  const matureWordsRef = useRef([]);
  const [wordsVersion, setWordsVersion] = useState(0);
  const [pendingWords, setPendingWords] = useState([]);

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
      if (speakersOnRef.current && diarWorkerRef.current) {
        diarWorkerRef.current.postMessage({ type: 'audio', samples: chunk });
      }

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

  // Finalized words arrive as a delta: replace the list from msg.matureFrom on.
  const takeWords = useCallback((msg) => {
    if (!Array.isArray(msg.matureWords)) return;
    const list = matureWordsRef.current;
    list.length = Math.min(list.length, msg.matureFrom || 0);
    for (const w of msg.matureWords) list.push(w);
    setWordsVersion((v) => v + 1);
  }, []);

  // Diarization worker: created, and its models loaded, on the first Start/File with
  // the toggle on. Failing to load it (e.g. no internet) only turns the labels off.
  const diarWorker = useCallback(() => {
    if (!diarWorkerRef.current) {
      const w = new Worker(new URL('./diarize.worker.js', import.meta.url), { type: 'module' });
      w.onmessage = (e) => onDiarMsgRef.current(e.data || {});
      w.onerror = (e) => setDiar({ state: 'error', progress: 0, error: e.message || 'failed to start' });
      diarWorkerRef.current = w;
    }
    if (!diarLoadRef.current) {
      diarLoadRef.current = true;
      setDiar({ state: 'loading', progress: 0, error: '' });
      diarWorkerRef.current.postMessage({ type: 'load' });
    }
    return diarWorkerRef.current;
  }, []);

  useEffect(() => {
    onDiarMsgRef.current = (msg) => {
      switch (msg.type) {
        case 'progress':
          setDiar((d) => ({ ...d, progress: msg.progress, stage: msg.stage }));
          break;
        case 'ready':
          setDiar({ state: 'ready', progress: 100, error: '', threads: msg.threads });
          break;
        case 'segments':
          setSegments(msg.segments || []);
          setSegmentsFinal(!!msg.final);
          setTakeVoices(msg.voices || []);
          if (msg.final) {
            setDiar((d) => ({ ...d, stage: '' }));
            updatePrints(msg.voices || []);
          }
          break;
        case 'error':
          if (msg.during === 'load') diarLoadRef.current = false; // retry on the next Start
          setDiar({ state: 'error', progress: 0, error: msg.error });
          break;
        default:
          break;
      }
    };
  });

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
          takeWords(msg);
          setPendingWords(msg.pendingWords || []);
          break;
        }
        case 'complete':
          // Final transcript from finalize-all (stream) or a finished file.
          busyRef.current = false;
          setPartial('');
          setTranscript(msg.text);
          takeWords(msg);
          setPendingWords([]);
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

    setSpeakersOn(loadSpeakersPref());

    return () => {
      if (streamRef.current) streamRef.current.stop();
      worker.terminate();
      if (diarWorkerRef.current) diarWorkerRef.current.terminate();
    };
  }, [setStatusBoth]);

  const onSpeakersToggle = useCallback((e) => {
    const on = e.target.checked;
    setSpeakersOn(on);
    try {
      window.localStorage.setItem(SPEAKERS_KEY, on ? 'on' : 'off');
    } catch {
      /* private mode: just this visit */
    }
  }, []);

  // A new take or file: clear the words and labels (and this take's names and voice
  // matches; the known voices stay), and (toggle on) reset the diarizer.
  const resetSpeakers = useCallback(() => {
    matureWordsRef.current = [];
    setWordsVersion((v) => v + 1);
    setPendingWords([]);
    setSegments([]);
    setSegmentsFinal(false);
    setSpeakerNames({});
    setRenaming(null);
    setTakeVoices([]);
    setMatches({});
    dismissedRef.current = {};
    savedRef.current = {};
    sessionRef.current = `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
    if (speakersOnRef.current) diarWorker().postMessage({ type: 'reset' });
  }, [diarWorker]);

  useEffect(() => {
    listVoices().then(setKnownVoices);
  }, []);

  // Recognise known voices whenever centroids, names or the known voices change.
  useEffect(() => {
    const named = new Set(Object.keys(speakerNames));
    const given = Object.values(speakerNames);
    const taken = new Set(knownVoices.filter((p) => given.some((n) => sameName(n, p.name))).map((p) => p.id));
    const opts = { named, taken, dismissed: dismissedRef.current, session: sessionRef.current };
    setMatches((prev) => nextMatches(prev, takeVoices, knownVoices, opts));
  }, [takeVoices, knownVoices, speakerNames]);

  // Save a named speaker's current centroid as this take's print for a person
  // ({ id } or { name }); too little speech yet → owed, saved when the take ends.
  const savePrint = useCallback((who, person) => {
    const key = String(who);
    const v = takeVoicesRef.current.find((x) => String(x.speaker) === key);
    if (!v || v.weight < MATCH.minWeight) {
      savedRef.current[key] = person;
      return;
    }
    const print = { session: sessionRef.current, speaker: v.speaker, centroid: v.centroid, weight: v.weight };
    addPrint(person, print).then(({ list, id }) => {
      if (id) savedRef.current[key] = { id };
      if (list) setKnownVoices(list);
    });
  }, []);

  // Take ended (final centroids after the recluster): refresh the prints of the
  // speakers named in it, now with all their speech behind them.
  const updatePrints = useCallback(
    (voices) => {
      takeVoicesRef.current = voices;
      for (const [key, person] of Object.entries(savedRef.current)) {
        if (speakerNamesRef.current[key]) savePrint(key, person);
      }
    },
    [savePrint],
  );

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
      window.__earwingUseFolderHandle = async (handle) => {
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
    // Last window + a full recluster: the labels may still tidy up after the text is done.
    if (speakersOnRef.current && diarWorkerRef.current) {
      diarWorkerRef.current.postMessage({ type: 'finish' });
      setDiar((d) => ({ ...d, stage: 'finish' }));
    }
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
    resetSpeakers();

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
  }, [ensureModel, onAudioChunk, regrantFolder, resetSpeakers, setStatusBoth, source, stopListening]);

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
        resetSpeakers();
        setStatusBoth('transcribing');
        const audio = await decodeFileTo16kMono(file);
        workerRef.current.postMessage({ type: 'transcribe', audio });
        if (speakersOnRef.current) diarWorker().postMessage({ type: 'file', audio });
      } catch (err) {
        console.error('[Transcriber] file error', err);
        setErrorMsg('Could not read that audio file — try a WAV, MP3, or M4A.');
        setStatusBoth('error');
      }
    },
    [diarWorker, ensureModel, resetSpeakers, setStatusBoth],
  );

  const isListening = status === 'listening';
  const isBusy = status === 'loading' || status === 'transcribing';
  const live = isListening || status === 'transcribing';
  const tail = live ? partial : '';
  const hasText = !!(transcript || tail);
  const blocked = status === 'unsupported';

  // Speaker turns, once the diarizer has labelled anything.
  const timeline = useMemo(() => (segments.length ? buildTimeline(segments) : null), [segments]);
  const turns = useMemo(() => {
    if (!speakersOn || !timeline) return null;
    const words = matureWordsRef.current.concat(live ? pendingWords.map((w) => ({ ...w, pending: true })) : []);
    return words.length ? toTurns(words, timeline) : null;
    // wordsVersion: matureWordsRef changed
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [speakersOn, timeline, wordsVersion, pendingWords, live]);
  const nSpeakers = timeline ? timeline.order.size : 0;
  // What each speaker is called: the user's names win over recognised voices.
  const voiceById = useMemo(() => new Map(knownVoices.map((p) => [p.id, p])), [knownVoices]);
  const labels = useMemo(() => {
    const out = {};
    for (const [k, m] of Object.entries(matches)) if (m.auto && voiceById.has(m.id)) out[k] = voiceById.get(m.id).name;
    return { ...out, ...speakerNames };
  }, [matches, voiceById, speakerNames]);
  const copyText = turns ? turnsToText(turns, labels) : joinText(transcript, tail);

  // Renaming a speaker: click its label in any turn, type, Enter (or click away) to
  // keep, Escape to cancel. An empty name goes back to "Speaker N". A name also saves
  // the speaker's voice print under it (voiceStore.js), to recognise them next time.
  // (A ref, so the blur that follows Enter/Escape as the input unmounts is a no-op.)
  const renamingRef = useRef(null);
  renamingRef.current = renaming;
  // The user decided what this speaker is called: any recognised-voice match for it
  // is overridden (and won't come back if the name is cleared later).
  const dismissMatch = useCallback((key) => {
    const m = matchesRef.current[key];
    if (!m) return;
    (dismissedRef.current[key] ||= new Set()).add(m.id);
  }, []);
  const setName = useCallback(
    (who, name, person) => {
      const key = String(who);
      dismissMatch(key);
      // Renamed to someone else: take back the print this take saved under the old
      // name (it wasn't them). Clearing the name keeps it.
      const old = savedRef.current[key];
      delete savedRef.current[key];
      const oldName = old && old.id && voiceById.has(old.id) ? voiceById.get(old.id).name : null;
      if (name && oldName && person.id !== old.id && !sameName(name, oldName)) {
        removePrint(old.id, sessionRef.current).then((list) => list && setKnownVoices(list));
      }
      setSpeakerNames((names) => {
        const next = { ...names };
        if (name) next[key] = name;
        else delete next[key];
        return next;
      });
      if (name) savePrint(key, person);
    },
    [dismissMatch, savePrint, voiceById],
  );
  const commitRename = useCallback(() => {
    const r = renamingRef.current;
    if (!r) return;
    renamingRef.current = null;
    setRenaming(null);
    const name = r.draft.trim();
    if (name === r.shown) return; // unchanged
    const custom = name && name !== `Speaker ${r.speaker}`;
    setName(r.who, custom ? name : '', { name });
  }, [setName]);
  // One click on a suggestion ("Alice?"): use that name.
  const acceptMatch = useCallback(
    (who) => {
      const m = matchesRef.current[String(who)];
      const p = m && voiceById.get(m.id);
      if (p) setName(who, p.name, { id: p.id });
    },
    [setName, voiceById],
  );

  // Known voices list: rename or forget. Forgetting also drops any name it gave in
  // this take (names the user typed stay) and stops this take re-saving the voice.
  const onForgetVoice = useCallback((id) => {
    for (const [k, v] of Object.entries(savedRef.current)) if (v.id === id) delete savedRef.current[k];
    forgetVoice(id).then((list) => list && setKnownVoices(list));
  }, []);
  const onForgetAll = useCallback(() => {
    savedRef.current = {};
    forgetAllVoices().then((list) => list && setKnownVoices(list));
  }, []);
  const commitVoiceEdit = useCallback(() => {
    const e = voiceEdit;
    setVoiceEdit(null);
    if (!e || !e.draft.trim()) return;
    renameVoice(e.id, e.draft).then((list) => {
      if (!list) return;
      // renamed onto another saved name: the two were merged under that one's id
      const merged = list.find((p) => sameName(p.name, e.draft));
      if (merged && merged.id !== e.id) {
        for (const v of Object.values(savedRef.current)) if (v.id === e.id) v.id = merged.id;
      }
      setKnownVoices(list);
    });
  }, [voiceEdit]);
  const onRenameKey = useCallback(
    (e) => {
      if (e.key === 'Enter') {
        e.preventDefault();
        commitRename();
      } else if (e.key === 'Escape') {
        e.preventDefault();
        renamingRef.current = null;
        setRenaming(null);
      }
    },
    [commitRename],
  );

  const onCopy = useCallback(async () => {
    if (!copyText) return;
    try {
      await navigator.clipboard.writeText(copyText);
    } catch {
      // navigator.clipboard needs a secure context; http://earwing.local isn't one.
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

  // A turn's speaker label: the name (click to rename), or the rename box; plus a
  // suggested name ("Alice?", click to accept) when the voice sounds like someone saved.
  const renderWho = (t, i) => {
    const shown = speakerName(t, labels);
    if (renaming && renaming.turn === i && renaming.who === t.who) {
      return (
        <input
          className="turn__who turn__rename"
          type="text"
          value={renaming.draft}
          placeholder={`Speaker ${t.speaker}`}
          aria-label={`Name for speaker ${t.speaker}`}
          maxLength={40}
          // eslint-disable-next-line jsx-a11y/no-autofocus
          autoFocus
          onFocus={(e) => e.target.select()}
          onChange={(e) => setRenaming({ ...renaming, draft: e.target.value })}
          onKeyDown={onRenameKey}
          onBlur={commitRename}
        />
      );
    }
    const m = matches[t.who];
    const recognised = m && voiceById.get(m.id);
    const suggest = recognised && !m.auto && !speakerNames[t.who] ? recognised : null;
    const auto = recognised && m.auto && !speakerNames[t.who];
    return (
      <span className="turn__head">
        <button
          type="button"
          className="turn__who"
          title={
            auto
              ? `Recognised voice (similarity ${m.sim.toFixed(2)}). Click to rename`
              : 'Rename this speaker (their voice is remembered in this browser)'
          }
          onClick={() => setRenaming({ who: t.who, speaker: t.speaker, turn: i, draft: shown, shown })}
        >
          {shown}
        </button>
        {suggest ? (
          <button
            type="button"
            className="turn__suggest"
            title={`Sounds like ${suggest.name} (similarity ${m.sim.toFixed(2)}). Click to use this name`}
            onClick={() => acceptMatch(t.who)}
          >
            {suggest.name}?
          </button>
        ) : null}
      </span>
    );
  };

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
            <label
              className="toggle"
              title={`Label who is speaking (downloads ${fmtBytes(DIAR_BYTES)} of models once)`}
            >
              <input
                type="checkbox"
                name="speakers"
                checked={speakersOn}
                onChange={onSpeakersToggle}
                disabled={blocked || isBusy || isListening}
              />
              <span>Speakers</span>
            </label>
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
                  <a href={`https://${env.host || 'earwing.local'}/`}>
                    https://{env.host || 'earwing.local'}/
                  </a>
                  . The device’s certificate is self-signed: to trust it, see{' '}
                  <a href="/status" target="_blank" rel="noopener">Status</a> or download <a href="/earwing.crt">earwing.crt</a>.
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
                  {hasText && turns ? (
                    turns.map((t, i) => {
                      const done = t.words.filter((w) => !w.pending);
                      const pend = t.words.filter((w) => w.pending);
                      return (
                        // eslint-disable-next-line react/no-array-index-key
                        <p key={i} className={`turn${t.speaker ? ` turn--${((t.speaker - 1) % 6) + 1}` : ''}`}>
                          {t.speaker ? renderWho(t, i) : null}
                          {done.length ? <span>{turnText({ words: done })}</span> : null}
                          {pend.length ? (
                            <span className="partial">
                              {done.length ? ' ' : ''}
                              {turnText({ words: pend })}
                            </span>
                          ) : null}
                        </p>
                      );
                    })
                  ) : hasText ? (
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
                    'The transcript of what earwing hears will appear here. Press Start listening, or transcribe an audio file.'
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
                {speakersOn && diar.state !== 'idle' ? ` · ${speakersLabel(diar, nSpeakers, segmentsFinal, live)}` : ''}
                {isListening ? ` · ${link === 'streaming' ? 'listening…' : link || 'connecting…'}` : ''}
                {status === 'transcribing' ? ' · transcribing…' : ''}
              </p>

              {knownVoices.length > 0 && (
                <details className="voices">
                  <summary>Known voices ({knownVoices.length})</summary>
                  <ul className="voices__list">
                    {knownVoices.map((p) => (
                      <li key={p.id}>
                        {voiceEdit && voiceEdit.id === p.id ? (
                          <input
                            className="turn__rename"
                            type="text"
                            value={voiceEdit.draft}
                            aria-label={`New name for ${p.name}`}
                            maxLength={40}
                            // eslint-disable-next-line jsx-a11y/no-autofocus
                            autoFocus
                            onFocus={(e) => e.target.select()}
                            onChange={(e) => setVoiceEdit({ ...voiceEdit, draft: e.target.value })}
                            onKeyDown={(e) => {
                              if (e.key === 'Enter') {
                                e.preventDefault();
                                commitVoiceEdit();
                              } else if (e.key === 'Escape') {
                                e.preventDefault();
                                setVoiceEdit(null);
                              }
                            }}
                            onBlur={commitVoiceEdit}
                          />
                        ) : (
                          <button
                            type="button"
                            className="voices__name"
                            title="Rename"
                            onClick={() => setVoiceEdit({ id: p.id, draft: p.name })}
                          >
                            {p.name}
                          </button>
                        )}
                        <span className="voices__n">
                          {p.prints.length} recording{p.prints.length === 1 ? '' : 's'}
                        </span>
                        <button type="button" className="linkbtn linkbtn--quiet" onClick={() => onForgetVoice(p.id)}>
                          forget
                        </button>
                      </li>
                    ))}
                  </ul>
                  <p className="voices__note">
                    Naming a speaker saves a voice print, so later recordings can recognise them. Voice
                    prints are stored only in this browser.{' '}
                    <button type="button" className="linkbtn linkbtn--quiet" onClick={onForgetAll}>
                      Forget all
                    </button>
                  </p>
                </details>
              )}
            </>
          )}
        </section>
      </main>
    </>
  );
}
