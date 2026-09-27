# gulltoppr web app

Live speech-to-text for the gulltoppr 4-mic beamforming array. The ESP32-S3 serves
this page from LittleFS over HTTPS (`https://gulltoppr.local/`, self-signed cert) and
plain HTTP (`http://gulltoppr.local/`, which redirects to https when the device is
online). The page streams the device's live beam audio from the same origin
(`/beam.wav`) and transcribes it **in the browser** with NVIDIA Parakeet TDT 0.6B v3,
run by [parakeet.js](https://github.com/ysdede/parakeet.js) on onnxruntime-web:

- **WebGPU, fp16** when the browser can: encoder on the GPU, decoder on WASM.
- **CPU, int8** otherwise: everything on onnxruntime-web's WASM backend.

The device only captures audio and serves files. It never sees the model.

Astro 5 static site with one React island. The transcription engine is adapted from
`rigar/site`: the Parakeet worker, the utterance merger, and the cursor-based
overlapping-window scheduler (Keet v4).

```
src/pages/index.astro             page (nav → /status: the firmware's status & controls page)
src/layouts/Layout.astro          shell, nav
src/styles/global.css             all styles (inlined into index.html), light + dark
src/components/Transcriber.jsx    UI + window scheduler + adaptive speech gate
src/components/DeviceStrip.jsx    polls /status.json once a second while visible
src/components/deviceStream.js    fetch()es /beam.wav (same origin), parses the WAV stream
src/components/parakeet.worker.js inference worker: picks WebGPU fp16 or WASM int8
src/components/diarize.worker.js  speaker-diarization worker (runs diarizer.js)
src/components/diarizer.js        who spoke when: pyannote segmentation + WeSpeaker + clustering
src/components/fbank.js           Kaldi-compatible 80-bin log-mel fbank (WeSpeaker's input)
src/components/speakerTimeline.js timed words + speaker segments → speaker turns
src/components/voiceStore.js      known voices: named speakers' voice prints (IndexedDB) + matching
src/components/models.js          model repos, files and sizes (shared)
src/components/localModels.js     "Load model from folder…" (File System Access / webkitdirectory)
src/components/utterance-merger.js
src/components/audio.js           file decode, sliceSamples, rms
scripts/pack-device.mjs           dist/ → ../code/data/**.gz (+ size checks)
scripts/mock-device.mjs           fake device for development
scripts/test-device-stream.mjs    stream-parser tests (+ live tests vs mock/device)
scripts/download-models.mjs       npm run models: both model variants (+ diarization) → ../models/
scripts/diarize-file.mjs          npm run diarize: diarizer.js over a WAV in Node
scripts/voice-match.mjs           npm run voice-match: cross-recording voice similarity (thresholds)
```

## Develop

```sh
npm install
```

The dev server proxies everything the page fetches from the device (`/beam.wav`,
`/raw.wav`, `/status`, `/status.json`, `/control`, `/gulltoppr.crt`) to `DEVICE_URL`,
so the page sees them same-origin, as in production. The proxy streams (it doesn't
buffer `/beam.wav`), and closing the page's fetch closes the upstream request.

**Against the mock** (no hardware needed):

```sh
npm run mock -- path/to/speech.wav                # terminal 1: device on :8080
DEVICE_URL=http://localhost:8080 npm run dev      # terminal 2
```

`mock-device.mjs` loops any WAV file in real time (any sample rate or channel count,
PCM or float, converted to 16 kHz mono). With no file it plays a synthetic buzz/pause
pattern. On one HTTP port it copies the firmware's web server: `/beam.wav[?mic=N]`
(chunked, **up to 3 streams at once; a 4th gets 503 "too many audio streams"**),
`/raw.wav?s=N` (one capture at a time, 503 while busy), `/status.json` (fields track
the audio level), `/control`, a stand-in `/status`, and COOP/COEP on everything. It
also serves the packed site from `../code/data` the way the firmware does (gzip), so
after `npm run build:device` you can open `http://<your LAN IP>:8080/` to test the real
artifact in an insecure context, like `http://gulltoppr.local`. `--audio-port 81`
adds the firmware's legacy plain-HTTP audio port (one client at a time); the page
doesn't use it. On macOS a quick spoken test file is
`say -o speech.wav --data-format=LEI16@16000 "hello from gulltoppr"`.

**Against the real device**:

```sh
npm run dev                                         # proxies to https://gulltoppr.local
DEVICE_URL=https://192.168.1.50 npm run dev         # or by IP
```

The proxy doesn't verify the device's self-signed cert. (Plain `http://` would get a
redirect to https from an online device, which a proxy can't pass through to the page.)
The dev server sends COOP/COEP and localhost is a secure context, so `npm run dev` gets
WebGPU (if the browser has it) and threaded WASM, like `https://gulltoppr.local`.
Add `?backend=wasm` or `?backend=webgpu` to the page URL to force a backend.

Tests: `npm run test:stream` runs the WAV-stream parser against byte streams split at
awkward points. If the mock answers on `http://localhost:8080` (or
`npm run test:stream -- <base URL>`), it also runs live tests over `/beam.wav`: chunk
size and rate, 3 concurrent streams and the 503 for a 4th, `stop()` freeing a slot,
`?mic=2`, and chunked transfer. For the real device, Node must trust its cert:
`NODE_EXTRA_CA_CERTS=gulltoppr.crt npm run test:stream -- https://gulltoppr.local`
(with nothing else streaming from it).

## Build and deploy to the device

```sh
npm run build:device   # astro build, then gzip dist/ into ../code/data/
npm run deploy         # the above + pio run -d ../code -t uploadfs
```

`pack-device.mjs` empties `../code/data` (only its contents), gzips every file in
`dist/` at level 9 into `../code/data/<path>.gz`, and prints a size table. It fails if
it finds an ONNX Runtime/WebAssembly file or any file over 1 MB, if the total goes over
budget, or if a path component is longer than 31 characters (it checks before touching
`../code/data`). The firmware serves `/x` from `/x.gz` with `Content-Encoding: gzip`
on both HTTPS and HTTP, caches `/_astro/*` as immutable, and sends COOP/COEP.
`uploadfs` flashes only the filesystem, not the firmware.

### Flash budget

The LittleFS partition (the `spiffs` entry in `default_16MB.csv`) is 3,538,944 bytes.
The build script's budget is 3.2 MB, leaving room for LittleFS metadata. The site
currently takes **6 files, about 300 KB gzipped**: ORT's JS glue + parakeet.js in the
transcription worker (~110 KB), the same again in the diarization worker (~100 KB) and
React (~65 KB). What keeps it small:

- **onnxruntime-web's `.wasm` never ships.** Its files are 14–28 MB each.
  `astro.config.mjs` aliases `onnxruntime-web` to its extern-wasm WebGPU (JSEP) + WASM
  build, `dist/ort.min.mjs` (~370 KB raw). The default bundle build contains
  `new URL('…jsep.wasm', import.meta.url)`, which makes Vite emit the wasm. The worker
  then sets `ort.env.wasm.wasmPaths` to
  `https://cdn.jsdelivr.net/npm/onnxruntime-web@<installed version>/dist/`, and ORT
  fetches `ort-wasm-simd-threaded.jsep.mjs` and `.jsep.wasm` (28 MB, ~5.5 MB brotli)
  from there. The version is read from `node_modules` at build time
  (`__ORT_VERSION__`). `package.json` `overrides` makes parakeet.js share that single
  ORT copy (it pins its own otherwise), so the configured `env` is the one its sessions
  use. JSEP rather than `ort.webgpu.min.mjs` (the newer native WebGPU EP): JSEP is what
  parakeet.js/Keet run fp16 Parakeet on, and its CPU path is the plain, non-asyncify
  WASM, which the int8 fallback uses. `pack-device.mjs` refuses any `.wasm`, anything
  named `ort-wasm*`, anything starting with the WebAssembly magic bytes, and any file
  over 1 MB.
- System fonts and inline SVG icons. There are no font files and no external CSS.
- CSS is inlined into `index.html`. Assets get short hash-only names
  (`/_astro/<hash>.js`), which means fewer requests to the device's single-threaded
  server and short LittleFS names.

## Models and backends

`parakeet.worker.js` picks a model when it first loads (on the first Start or File):

| | WebGPU (fp16) | CPU (int8) |
|---|---|---|
| repo | `ysdede/parakeet-tdt-0.6b-v3-onnx` | `nasedkinpv/parakeet-tdt-0.6b-v3-onnx-int8` |
| encoder | `encoder-model.fp16.onnx` (1.24 GB, on WebGPU) | `encoder-int8.onnx` + `.data` (880 MB) |
| decoder | `decoder_joint-model.int8.onnx` (18 MB, on WASM) | `decoder_joint-int8.onnx` (54 MB) |
| vocab | `vocab.txt` | `vocab.txt` |
| download | **~1.26 GB** | **~933 MB** |

- **WebGPU fp16** is used only if the page is a secure context, `navigator.gpu` exists,
  `requestAdapter()` returns an adapter, and it has `shader-f16`. Otherwise CPU int8.
- If the WebGPU model fails to create its sessions, ORT's WebGPU backend doesn't
  start, or the warm-up inference throws, the worker drops it, loads the int8 model,
  and the page says why. A failed *download* doesn't switch models (the other one
  needs the same internet); it's reported as an error and the next Start retries.
- `?backend=wasm` forces the CPU model; `?backend=webgpu` tries WebGPU even when the
  checks fail (and still falls back on errors).
- parakeet.js caches each file in IndexedDB, per origin, keyed by repo and filename, so
  switching backends doesn't evict the other model. `https://gulltoppr.local`,
  `http://gulltoppr.local` and `http://<device IP>` are different origins with
  separate caches.
- parakeet.js gotcha (1.4.4): `backend: 'webgpu'` leaves the encoder's execution
  providers empty, and ORT then picks WASM. The worker uses `'webgpu-hybrid'`
  ([webgpu, wasm], sessions created one after the other) and checks that ORT's WebGPU
  device exists afterwards.
- WASM threads: with cross-origin isolation (https or localhost) up to 8 threads
  (`min(8, hardwareConcurrency)`), otherwise 1. ORT fixes the count when its WASM
  runtime starts, so it's the same for both models; on WebGPU only the decoder uses it.
- Measured on an Apple Silicon Mac (Chrome, 10 cores), 26 s of speech as a file:
  WebGPU fp16 0.56 s (RTF ≈ 0.02), WASM int8 with 8 threads 1.8 s (RTF ≈ 0.07),
  WASM int8 single-threaded (plain http) 11 s (RTF ≈ 0.42). Cached model load ≈ 2 s either way.

## Loading the model from a local folder

Instead of downloading from Hugging Face in each browser (and for each origin), the
page can read the model files from a folder on the computer running the browser.

```sh
npm run models        # both variants → <repo root>/models (~2.2 GB); webgpu|wasm for one
```

`download-models.mjs` writes one subfolder per Hugging Face repo, named `<org>--<repo>`
(the names are in `src/components/models.js`):

```
models/ysdede--parakeet-tdt-0.6b-v3-onnx/          encoder-model.fp16.onnx  decoder_joint-model.int8.onnx  vocab.txt
models/nasedkinpv--parakeet-tdt-0.6b-v3-onnx-int8/ encoder-int8.onnx  encoder-int8.onnx.data  decoder_joint-int8.onnx  vocab.txt
```

It shows progress, resumes interrupted downloads from `<file>.part` with HTTP Range
requests, and checks each file against the Hugging Face tree API: its size, plus the
LFS sha256 (or the git blob sha1 for small non-LFS files like `vocab.txt`). Verified
files are recorded in `models/.verified.json`, so re-runs skip them. `/models/` is
git-ignored.

In the page, click **Load model from folder…** (under the buttons, before the model has
loaded) and pick either `models/` or one of its repo subfolders. The page then:

- finds the files for the backend it picked (fp16 set for WebGPU, int8 set for CPU,
  including the automatic WebGPU → CPU fallback), matching by name **and exact size**,
  since both repos have a `vocab.txt`;
- hands them to the worker as `File` objects from `getFile()` (disk-backed, nothing is
  copied or cached in IndexedDB); the worker makes blob URLs for parakeet.js;
- if the folder lacks the needed files, names them and offers **Download from Hugging
  Face instead** (for this visit);
- shows the source in the status line: `model: local folder “models”`,
  `model: Hugging Face (cached)` or `model: Hugging Face (downloaded)`.

Secure contexts (`https://gulltoppr.local`, localhost) use the File System Access API
(`showDirectoryPicker`). The folder handle is saved in IndexedDB
(`gulltoppr-local-models`). On later visits it's used automatically if the browser
still reports permission `granted`. If it reports `prompt`, the page shows a one-click
**Use local model folder (name)** (Start also asks), because re-granting needs a user
gesture. **forget folder** removes it. Plain http has no `showDirectoryPicker`, so the
same link opens an `<input type=file webkitdirectory>` instead. That works, but the
browser can't remember the folder, so you pick it each visit. Chrome asks to confirm
"upload" of the folder; nothing is uploaded, the page only reads the files locally.

For automated tests (the native picker can't be scripted), the dev server exposes
`window.__gulltopprUseFolderHandle(dirHandle)`, which adopts and saves any directory
handle, such as an OPFS directory.

## Speaker labels (diarization)

With **Speakers** ticked (the default, remembered per browser), the transcript is split
into turns labelled Speaker 1, 2, … Labelling runs in its own worker
(`diarize.worker.js`), beside the transcription worker, so it never delays the text.
The page sends both workers the same audio. Parakeet's word timestamps and the
diarizer's speaker segments are lined up by time on the main thread
(`speakerTimeline.js`). If the diarization models can't load, you get the plain
transcript and the status line says why.

Models (~33 MB, downloaded from Hugging Face on first use and cached in IndexedDB
alongside Parakeet; always run on WASM):

| | repo | file | size |
|---|---|---|---|
| segmentation | `onnx-community/pyannote-segmentation-3.0` | `onnx/model.onnx` | 6.0 MB |
| embedding | `Wespeaker/wespeaker-voxceleb-resnet34-LM` | `voxceleb_resnet34_LM.onnx` | 26.5 MB |

Pipeline (`diarizer.js`, online, pyannote/diart style):

1. Every ~2.5 s, segment the last 10 s. pyannote gives up to 3 *local* speakers per
   window, and their numbering only holds within that window.
2. Embed each local speaker's solo speech. The input is an 80-bin Kaldi fbank
   (`fbank.js`, matches torchaudio's `kaldi.fbank` to 1.5e-4), from at least 0.5 s
   and at most the latest 5 s of that speech, since the embedding is the cost.
3. Map local speakers one-to-one to global speakers. The score is the mean cosine
   to each speaker's embeddings so far, plus agreement with what earlier windows
   said about the overlapping 7.5 s. A new speaker is created only below 0.2
   similarity and with ≥ 3 s of solo speech.
4. Tally the votes: each moment is covered by 4 windows, and a speaker is active
   where at least half of them agree.
5. Every 30 s, and when you stop or a file ends, **recluster** all embeddings so far:
   average-linkage clustering, merging while similarity ≥ 0.35. This fixes early
   mistakes, such as a speaker split in two by a noisy first embedding. Speakers
   keep their number across reclusters by time overlap, so labels on screen don't
   jump around.

The newest ~10 s of labels are provisional, and the last few words stay in the
previous turn until they're labelled. Labels can still tidy up after Stop ("speakers:
tidying up…").

Cost: about 0.1 s of CPU per second of audio with 4 WASM threads, and ~0.4 s
single-threaded (plain http). Almost all of it is the embedding. The worker uses up
to 4 threads (half the cores), leaving the rest to transcription.

`npm run diarize -- rec.wav [--words words.json] [--set key=value]` runs the same code
in Node on 16 kHz WAV files, fed in 100 ms chunks as the page feeds it. Run
`npm run models -- diarization` first. It writes the labels as they were live (first
shown, settled) and after the final recluster, as `[{start, end, speaker}]` JSON. The
tunables are `DEFAULTS` in `diarizer.js`.

How good is it? It was tested on 5 min of a real far-field meeting (`pzm12.wav`,
ICSI-style table mic, about −45 dBFS, ~6 speakers, 17% overlapped speech). There's no
ground truth, so the reference is sherpa-onnx's *offline* diarization with the same
model family. That reference is itself uncertain by about 4–12% DER across its
plausible settings. Against it (collar 0.25 s):

| labels | DER | of which: missed / false alarm / confusion | speakers |
|---|---|---|---|
| live, once settled (~10 s behind) | 34% | 17 / 0 / 17% | 6 names used |
| after Stop / for a file | 28% | 7 / 2.5 / 18% | 6 |

- **Missed speech in the live view** is mostly overlapped speech, because the live
  view shows one speaker per moment.
- **Confusion** comes mainly from two quieter voices that WeSpeaker scores as similar
  as some same-speaker pairs (mean cosine 0.43 vs 0.54–0.62). They get merged. Longer
  embeddings or a 1.25 s step didn't separate them and cost 2–5× the CPU.
- **The browser matched Node:** both file mode and a live take through the mock
  device found 5 speakers, with similar shares.

### Naming speakers, and remembered voices

Click a speaker's label to rename it (Enter keeps, Escape cancels, empty goes back to
"Speaker N"). Naming a speaker also saves its voice print (the diarizer's centroid, a
256-dim vector) under that name in this browser's IndexedDB (`voiceStore.js`, one print
per person per take, the latest 8 kept). In later takes and files, a speaker with
enough speech (≈ 7 s) whose voice is close to a saved person gets the name
automatically (cosine ≥ 0.65 and ≥ 0.1 clear of the next person), or a one-click
suggestion ("Alice?", ≥ 0.45). Each person goes to at most one speaker per take, and
names you type always win. **Known voices** under the transcript lists the saved people
(click a name to rename it) with Forget / Forget all. Prints never leave the browser.

`npm run voice-match -- rec.wav [--splits 2,3,4]` measured those thresholds: it cuts a
recording into pieces, diarizes each as its own recording and compares the speakers'
centroids across pieces (numbers for pzm12 in `voiceStore.js`).

## Browser constraints

- **Internet is required on first use.** The browser downloads the model from Hugging
  Face (~1.26 GB for WebGPU, ~933 MB for CPU) and the ORT runtime from jsDelivr, not
  from the device, then caches the model in IndexedDB for that origin.
- **Use `https://gulltoppr.local/`.** The device's certificate is self-signed
  (ECDSA, for `gulltoppr.local`). Download it from `/gulltoppr.crt` (linked from
  `/status`) and trust it in the OS/browser, or click through the browser's warning.
  Over https the page is a secure context, so the browser honours the firmware's
  COOP/COEP: WebGPU and `SharedArrayBuffer` (threaded WASM) become available. The page
  tests for them rather than assuming.
- **Plain `http://` is not a secure context**, so there's no WebGPU and no
  `SharedArrayBuffer`: CPU int8, single-threaded. Slower, but it works, and the page
  links to the https version. (Chrome can also treat an http origin as secure via
  `chrome://flags/#unsafely-treat-insecure-origin-as-secure`.) Copy falls back to
  `execCommand` there because `navigator.clipboard` needs a secure context.
- **Same-origin audio, up to 3 streams.** The page fetches `/beam.wav[?mic=N]` from
  the origin that served it (port 81's plain-HTTP audio would be blocked as mixed
  content on https). The device allows 3 streams at once; a 4th, or a stream while a
  `/raw.wav` capture is running, gets a 503 and the page says which. Stop listening, or
  close the tab, to free a slot. If the stream drops mid-session, the page reconnects
  with backoff for about 25 s before giving up. It keeps the transcript either way.
- **Speech gate.** The device's beam can be quieter than a laptop mic, so windows are
  gated by an adaptive RMS threshold: about +8 dB over a running noise floor, clamped to
  0.003–0.01. rigar used a fixed 0.01. See `Transcriber.jsx`.
