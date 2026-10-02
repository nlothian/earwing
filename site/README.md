# earwing web app

Live speech-to-text for the earwing 4-mic beamforming array. The ESP32-S3 serves
this page from LittleFS over HTTPS (`https://earwing.local/`, self-signed cert) and
plain HTTP (`http://earwing.local/`, which redirects to https when the device is
online). The page streams the device's live beam audio from the same origin
(`/beam.wav`) and transcribes it **in the browser** with NVIDIA Parakeet TDT 0.6B v3,
run by [parakeet.js](https://github.com/ysdede/parakeet.js) on onnxruntime-web:

- **WebGPU, fp16** when the browser can: encoder on the GPU, decoder on WASM.
- **CPU, int8** otherwise: everything on onnxruntime-web's WASM backend.

The device only captures audio and serves files. It never sees the model.

An optional **assistant** panel summarises the transcript and answers questions about
it, with a LiquidAI LFM2.5 language model run on WebGPU in the same browser
(Transformers.js). See [Assistant](#assistant).

Astro 5 static site with one React island. The transcription engine is adapted from
`rigar/site`: the Parakeet worker, the utterance merger, and the cursor-based
overlapping-window scheduler (Keet v4).

```
src/pages/index.astro             page (nav → /status: the firmware's status & controls page)
src/layouts/Layout.astro          shell, nav
src/styles/global.css             all styles (inlined into index.html), light + dark
src/components/Transcriber.jsx    UI + window scheduler + adaptive speech gate
src/components/Assistant.jsx      assistant panel: model picker, chat, citations → transcript
src/components/llmClient.js       useLlm(): assistant state, prompts → worker, KV-cache reuse
src/components/llmPrompt.js       transcript lines, prompts, token budget, citations (pure)
src/components/useAutoSummary.js  re-runs the summary on its own while a model is loaded
src/components/meetingSync.js     live summary → POST /meeting (the device's Meeting tab)
src/components/meetingSummary.js  summary Markdown → sections and items (pure)
src/components/meetingDiff.js     what changed since the last summary: the Follow feed, payload (pure)
src/components/meetingContract.js POST /meeting limits, shared with ../code/meeting_types.h
src/components/ascii.js           toAscii(): text the device's fonts can draw
src/components/llm.worker.js      LLM worker: Transformers.js on ORT's native WebGPU EP
src/components/gpuGate.js         pauses the LLM while Parakeet has GPU work in flight
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
scripts/test-meeting.mjs          live-summary parser, diff, ASCII and payload tests
scripts/download-models.mjs       npm run models: both model variants (+ diarization) → ../models/
scripts/diarize-file.mjs          npm run diarize: diarizer.js over a WAV in Node (+ DER vs an RTTM)
scripts/score-diarization.mjs     npm run diarize:score: DER over a list of recordings (VoxConverse)
scripts/der.mjs                   diarization error rate, md-eval style
scripts/voice-match.mjs           npm run voice-match: cross-recording voice similarity (thresholds)
```

## Develop

```sh
npm install
```

The dev server proxies everything the page fetches from the device (`/beam.wav`,
`/raw.wav`, `/status`, `/status.json`, `/control`, `/meeting`, `/meeting.json`,
`/earwing.crt`) to `DEVICE_URL`, so the page sees them same-origin, as in production.
The proxy streams (it doesn't buffer `/beam.wav`), and closing the page's fetch closes
the upstream request.

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
the audio level), `/control`, a stand-in `/status`, `POST /meeting` (the firmware's
checks and accept policy, one log line per post) with `GET /meeting.json` to see what
it holds, and COOP/COEP on everything. It
also serves the packed site from `../code/data` the way the firmware does (gzip), so
after `npm run build:device` you can open `http://<your LAN IP>:8080/` to test the real
artifact in an insecure context, like `http://earwing.local`. `--audio-port 81`
adds the firmware's legacy plain-HTTP audio port (one client at a time); the page
doesn't use it. On macOS a quick spoken test file is
`say -o speech.wav --data-format=LEI16@16000 "hello from earwing"`.

**Against the real device**:

```sh
npm run dev                                         # proxies to https://earwing.local
DEVICE_URL=https://192.168.1.50 npm run dev         # or by IP
```

The proxy doesn't verify the device's self-signed cert. (Plain `http://` would get a
redirect to https from an online device, which a proxy can't pass through to the page.)
The dev server sends COOP/COEP and localhost is a secure context, so `npm run dev` gets
WebGPU (if the browser has it) and threaded WASM, like `https://earwing.local`.
Add `?backend=wasm` or `?backend=webgpu` to the page URL to force a backend.

Tests: `npm run test:stream` runs the WAV-stream parser against byte streams split at
awkward points. If the mock answers on `http://localhost:8080` (or
`npm run test:stream -- <base URL>`), it also runs live tests over `/beam.wav`: chunk
size and rate, 3 concurrent streams and the 503 for a 4th, `stop()` freeing a slot,
`?mic=2`, and chunked transfer. For the real device, Node must trust its cert:
`NODE_EXTRA_CA_CERTS=earwing.crt npm run test:stream -- https://earwing.local`
(with nothing else streaming from it).

`npm run test:llm` checks the assistant's prompt building (transcript lines, append vs
rebuild, chunking, citations) in Node without a model. `npm run test:meeting` checks
the live summary's Markdown parser, change tracking, ASCII transliteration and payload
caps against `scripts/fixtures/meeting-sample.json`. `npm run test:llm-runtime --
[--model small|large|both] [--tokenizer-only]` runs `llm.worker.js` itself in Node against
`models/` (after `npm run models -- llm`): chat-template/KV-prefix round-trips, chunked
prefill, streaming, cache reuse, abort and pause, on onnxruntime-node's WebGPU EP.

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
The build script's budget is 3.2 MB, leaving room for LittleFS metadata. Before the
assistant the site took **6 files, about 300 KB gzipped**: ORT's JS glue + parakeet.js in
the transcription worker (~110 KB), the same again in the diarization worker (~100 KB) and
React (~65 KB). The assistant adds **about 160 KB gzipped**, almost all of it the LLM
worker (Transformers.js + ORT's WebGPU-EP glue); its panel is a few KB in the page
bundle. `pack-device.mjs` prints the exact table. What keeps it small:

- **onnxruntime-web's `.wasm` never ships.** Its files are 14–28 MB each.
  `astro.config.mjs` aliases `onnxruntime-web` to its extern-wasm WebGPU (JSEP) + WASM
  build, `dist/ort.min.mjs` (~370 KB raw). The default bundle build contains
  `new URL('…jsep.wasm', import.meta.url)`, which makes Vite emit the wasm. The worker
  then sets `ort.env.wasm.wasmPaths` to
  `https://cdn.jsdelivr.net/npm/onnxruntime-web@<installed version>/dist/` (1.30.0 at
  the time of writing; `package.json` asks for `^1.30.0`), and ORT
  fetches `ort-wasm-simd-threaded.jsep.mjs` and `.jsep.wasm` (28 MB, ~5.5 MB brotli)
  from there. The version is read from `node_modules` at build time
  (`__ORT_VERSION__`). `package.json` `overrides` makes parakeet.js share that single
  ORT copy (it pins its own otherwise), so the configured `env` is the one its sessions
  use. JSEP rather than `ort.webgpu.min.mjs` (the newer native WebGPU EP): JSEP is what
  parakeet.js/Keet run fp16 Parakeet on, and its CPU path is the plain, non-asyncify
  WASM, which the int8 fallback uses. `pack-device.mjs` refuses any `.wasm`, anything
  named `ort-wasm*`, anything starting with the WebAssembly magic bytes, and any file
  over 1 MB.
- The assistant's worker gets ORT the same way, but a different build: Transformers.js
  imports `onnxruntime-web/webgpu` (the native WebGPU EP, whose kernels live in the
  asyncify `.wasm`), aliased to the extern-wasm `dist/ort.webgpu.min.mjs`, with
  `ort-wasm-simd-threaded.asyncify.{mjs,wasm}` from the same pinned jsDelivr path. The
  `overrides` entry for `@huggingface/transformers` keeps it on the one installed ORT.
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
  switching backends doesn't evict the other model. `https://earwing.local`,
  `http://earwing.local` and `http://<device IP>` are different origins with
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

Secure contexts (`https://earwing.local`, localhost) use the File System Access API
(`showDirectoryPicker`). The folder handle is saved in IndexedDB
(`earwing-local-models`). On later visits it's used automatically if the browser
still reports permission `granted`. If it reports `prompt`, the page shows a one-click
**Use local model folder (name)** (Start also asks), because re-granting needs a user
gesture. **forget folder** removes it. Plain http has no `showDirectoryPicker`, so the
same link opens an `<input type=file webkitdirectory>` instead. That works, but the
browser can't remember the folder, so you pick it each visit. Chrome asks to confirm
"upload" of the folder; nothing is uploaded, the page only reads the files locally.

For automated tests (the native picker can't be scripted), the dev server exposes
`window.__earwingUseFolderHandle(dirHandle)`, which adopts and saves any directory
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
5. Every 5 s, and when you stop or a file ends, **recluster** all embeddings so far:
   average-linkage clustering, merging while similarity ≥ 0.35. This fixes early
   mistakes, such as a speaker split in two by a noisy first embedding, or a short
   reply given to the person it answered. Speakers keep their number across
   reclusters by time overlap, so labels on screen don't jump around. A recluster is
   cheap: the cosines of every pair of embeddings are kept, each recluster computes
   only the new ones, and the clustering itself is a nearest-neighbour chain over that
   table, O(n²). Matching speakers is cheap too: each local speaker only needs its top
   three candidates, not every global speaker. On a 1 h 44 min take with ~100 speakers,
   the final recluster took 0.5 s, with a 14 MB table. Before this change it took 11 s.
   In very long takes, and for files, the interval stretches so that reclustering stays
   under 10% of the worker's time (`reclusterMaxLoad`). On that take it used 7%.
6. **Known voices steer the recluster** (`setAnchors`). The page sends the diarizer
   the voice prints of everyone saved under Known voices (prints saved in this take
   are left out). Each embedding is given to at most one saved person: the one it
   sounds most like, if that's clear of the runner-up by 0.1. Its affinity ramps from
   0 at cosine 0.45 to that person's best print up to 1 at 0.65. Before clustering,
   two embeddings' cosine is raised by up to 0.3 when they belong to the same saved
   person, and lowered by up to 0.3 when they belong to two different ones. After
   clustering, clusters whose centroids are ≥ 0.45 to the same saved person (and
   0.1 clear of the next) are merged. That joins a known person split in two, and
   keeps two known people apart. Embeddings that sound like no one saved aren't
   changed, so unknown speakers cluster as before, and saved people who aren't in
   the take have no effect. Tunables `anchorLo`, `anchorHi`, `anchorMargin`,
   `anchorPull`, `anchorPush`, `anchorMergeSim` (all three of the last at 0 turn it off).

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
tunables are `DEFAULTS` in `diarizer.js`. Add `--rttm ref.rttm` (ground truth) and it
prints the DER of those labels too (`scripts/der.mjs`: md-eval style, 0.25 s collar,
overlap scored).

`npm run diarize:score [-- --set key=value ...]` scores against a set of recordings with
ground truth. By default it's 15 [VoxConverse](https://github.com/joonson/voxconverse)
dev recordings (`scripts/fixtures/voxconverse-dev-subset.txt`, ~1 h). Twelve have quick
back-and-forth, like TV dialog, and three have slow turns, as controls. It runs several
files at once and prints DER per group for three sets of labels:
- settled: each moment's live label, ~10 s after it was spoken;
- online: the live labels as they stood when the audio ended;
- final: after Stop.

It also prints **relabelled**, the share of settled speech whose label changed again
while live. On screen, those are words jumping to another speaker. Per-file labels and a
`summary.json` go to `datasets/voxconverse/runs/<time>/`. Check a change on
`--list scripts/fixtures/voxconverse-dev-holdout.txt` too: 20 more recordings, picked the
same way, that tuning never saw. The data (2 GB, CC BY 4.0), once:

```sh
mkdir -p ../datasets/voxconverse && cd ../datasets/voxconverse
curl -LO https://www.robots.ox.ac.uk/~vgg/data/voxconverse/data/voxconverse_dev_wav.zip
git clone --depth 1 https://github.com/joonson/voxconverse labels
mkdir dev && unzip -j voxconverse_dev_wav.zip -d dev && cp labels/dev/*.rttm dev/
```

How good is it? Against human labels, on the VoxConverse dev recordings above (debates,
panels and news from YouTube; DER with a 0.25 s collar, missed / false alarm / confusion):

| recordings | live, once settled | after Stop / for a file | relabelled later |
|---|---|---|---|
| 12 fast (tuning) | 19.2% (7.6 / 0.6 / 11.0) | 18.6% (3.9 / 5.9 / 8.9) | 4.8% |
| 15 fast (held out) | 12.4% (8.8 / 0.2 / 3.4) | 10.7% (4.3 / 2.5 / 4.0) | 3.8% |
| 8 slow (both lists) | 2.0% | 1.6–2.0% | 0.2–1.0% |

With 30 s between reclusters, the live figures were 28.3% (confusion 19.3%) on the
fast tuning set and 16.1% (6.9%) on the held-out one, with 9–19% of labels changing
later. Quick back-and-forth was where live labels suffered most: a short reply went to
the person it answered until the next recluster. The after-Stop labels don't depend on
the interval, since the final recluster starts from scratch.

On a real far-field meeting (`pzm12.wav`, 5 min, ICSI-style table mic, about −45 dBFS,
~6 speakers, 17% overlapped speech) there's no ground truth, so the reference is
sherpa-onnx's *offline* diarization with the same model family. That reference is
itself uncertain by about 4–12% DER across its plausible settings. Against it
(collar 0.25 s):

| labels | DER | of which: missed / false alarm / confusion | speakers |
|---|---|---|---|
| live, once settled (~10 s behind), 30 s reclusters | 34% | 17 / 0 / 17% | 6 names used |
| after Stop / for a file | 28% | 7 / 2.5 / 18% | 6 |

That reference wasn't kept, so the live row wasn't re-measured at 5 s. Instead, the
live labels were compared with the same run's after-Stop labels. With 5 s reclusters
they differ on 10% of speech, 3.0% of it confusion. With 30 s reclusters it was 20%
and 11.7%.

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

`npm run diarize -- rec.wav --voices` writes the speakers' prints to
`<prefix>.voices.json`, and `--anchors that.voices.json` diarizes another recording with
them as known voices. `node scripts/score-anchors.mjs [--list ...]` measures what known
voices do: it cuts each recording in half, keeps the first half's speakers with
enough speech (as the page would save them), and scores the second half with no
known voices, with those (`own`), and with another recording's (`foreign`, strangers).
Both halves share the room and mic, so `own` is a little optimistic. Final DER,
with live (settled) in brackets:

| VoxConverse dev | no known voices | own | foreign |
|---|---|---|---|
| 15 tuning | 15.5% (15.7%) | 14.8% (15.3%) | 15.5% (15.7%) |
| 20 held out | 9.5% (13.5%) | 9.7% (12.6%) | 9.5% (13.5%) |

Most recordings score the same either way: on VoxConverse the clustering is rarely
what goes wrong. The gains are confusion (falxo 22.9 → 18.9%, ufpel 28.9 → 26.0% with
6 of its 8 speakers found instead of 4); strangers' prints changed nothing on any
recording. One got worse (udjij 9.5 → 12.6%): once anchors made one speaker pure,
the pieces of three other people it had absorbed formed a 6th, mixed speaker
instead of going back to their own. Merging alone (`anchorPull=0 anchorPush=0`) never
made anything worse but helped less (tuning 15.1%, held out unchanged). On synthetic
meetings from macOS voices, each turn randomly near or far from the mic (5 known
people + 1 unknown; prints from a separate, imperfectly diarized take), the final DER
went from 59% (3 speakers found for 6) to 42% (5). A take with 3 known people,
2 unknown and 2 saved people absent scored the same with and without anchors.

`npm run voice-match -- rec.wav [--splits 2,3,4]` measured those thresholds: it cuts a
recording into pieces, diarizes each as its own recording and compares the speakers'
centroids across pieces (numbers for pzm12 in `voiceStore.js`).

## Assistant

The **Assistant** panel (under the transcript; beside it on screens ≥ 1100 px wide)
writes a summary of the transcript and answers questions about it. Answers cite
times like `[04:12]`; click one to jump there in the transcript (the turn flashes, or
in plain text the sentence is highlighted). It runs a language model **on this
computer's GPU**, in its own worker (`llm.worker.js`, Transformers.js 4.3.0 on ORT's
native WebGPU EP). Nothing leaves the browser. The panel starts collapsed and nothing
downloads until you click **Load**; whether it's open, and the model picked, are
remembered per browser.

| | model | repo | download |
|---|---|---|---|
| Large | LFM2.5 2.6B, q4f16 | `LiquidAI/LFM2.5-2.6B-ONNX` | ~1.55 GB |
| Small | LFM2.5 1.2B Instruct, q4f16 | `LiquidAI/LFM2.5-1.2B-Instruct-ONNX` | ~0.76 GB |

- The default is Large, or Small when `navigator.deviceMemory < 8` or on a touch
  device. The 2.6B is a reasoning model; the worker turns its thinking off.
- **WebGPU only**: a secure context, `navigator.gpu`, an adapter and `shader-f16`.
  There's no CPU fallback (a 1–2 B model on WASM is too slow to be useful), so plain
  `http://` and `?backend=wasm` show "needs WebGPU" (with a link to the https page).
- Models are fetched from Hugging Face on first use and kept in the Cache API for this
  origin. Or load them from the local models folder, like Parakeet:
  `npm run models -- llm` (both), `npm run models -- llm-large` or `-- llm-small`
  (plain `npm run models` doesn't fetch them). Layout:
  `models/LiquidAI--LFM2.5-2.6B-ONNX/{config.json, …, onnx/model_q4f16.onnx, onnx/model_q4f16.onnx_data, …}`.
  **Load** uses the folder picked with **Load model from folder…** if it has the files
  (matched by name and exact size), otherwise it names what's missing and offers
  **Download from Hugging Face instead**. The source is shown under the picker
  (`from folder “models”`, `from Hugging Face (cached)` / `(downloaded)`).
- **Reuse, not re-reading.** The transcript is the first message of the chat, and the
  worker keeps the model's KV (and LFM2's conv) cache between requests. Transcript
  that arrives later, and each question, are *appended* as new messages, so only the
  new tokens are read (prefilled). LFM2's conv state can't be rewound, so reuse needs
  an exact token prefix: if earlier text changed (speaker labels re-clustered, a
  rename), the whole transcript is read again and the answer's meta line says
  "re-read transcript". A long transcript is read in ≤ 512-token chunks with progress
  ("reading transcript 3.1k/9.8k tokens"); past the context budget it's summarised in
  parts first. A new take or file clears the chat.
- **Live transcription comes first.** Both models share one GPU, and a Parakeet window
  queued behind a long prefill would make the live text lag. So while Parakeet runs on
  WebGPU, `gpuGate.js` counts its in-flight jobs (windows, the final pass on Stop, a
  file) and the LLM pauses between prefill chunks or decoded tokens until they're done.
  Pauses over 0.5 s show as "paused for live transcription". The panel's stats line
  shows the effect: `ASR window 0.18 s median (0.34 s while assistant runs) · LLM
  paused 21%`. `?llmgate=off` disables the pausing, for A/B comparison. Loading the
  model while listening can briefly delay the live transcript.
- Dev only: `?llm=mock` swaps in `llm.mock.worker.js`, a stand-in for the worker, to
  work on the UI without a GPU or a download.

### Live summary on the device

Once a model is loaded, the summary **re-runs on its own** (`useAutoSummary.js`) and
is sent to the device's **Meeting** tab. There's no switch: loading the model opts in.

- A run starts when the LLM is idle and there are at least 10 new settled words
  (or any, once the take is final). Runs are at least
  `clamp(last run's duration, 20 s, 180 s)` apart, so a long map-reduce spaces itself
  out. An error doubles the gap, up to 5 minutes; a GPU error stops auto runs until
  the model is loaded again. Auto runs wait while you're typing a question (8 s),
  and **Stop** on one pauses them for 2 minutes. Live transcription still comes
  first (the GPU gate above).
- The chat keeps one **Live summary** entry, which each auto run replaces. **Ask**
  during an auto run stops it (what it wrote stays, as with Stop) and answers first.
  **Summarise** / **Update summary** work as before.
- Each finished summary is parsed into its sections (`meetingSummary.js`), compared
  with the last one (`meetingDiff.js`: rewordings and moves between sections aren't
  news, an item the model drops and re-adds doesn't flap), transliterated to ASCII
  and POSTed as JSON to `/meeting` (`meetingSync.js`; the contract is
  `meetingContract.js` and `../code/meeting_types.h`). The **Follow** feed on the
  device lists new and edited items and summary changes, newest first, up to 12.
- Every 30 s the current state goes again as a heartbeat. With two tabs open the
  device keeps the last real summary and ignores the other tab's heartbeats (the
  panel then says "display is showing another tab"). Firmware without the endpoint
  (404/405) turns sending off for the page. The panel shows
  `live summary rev 4 · synced 12s ago`.
- `UPDATE_REQUEST` asks the model to keep the wording of points that still hold,
  so the feed shows real changes rather than rephrasing.

## Browser constraints

- **Internet is required on first use.** The browser downloads the model from Hugging
  Face (~1.26 GB for WebGPU, ~933 MB for CPU) and the ORT runtime from jsDelivr, not
  from the device, then caches the model in IndexedDB for that origin.
- **Use `https://earwing.local/`.** The device's certificate is self-signed
  (ECDSA, for `earwing.local`). Download it from `/earwing.crt` (linked from
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
