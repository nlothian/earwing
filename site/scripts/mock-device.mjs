#!/usr/bin/env node
// Zero-dependency stand-in for the earwing device, for developing the site
// without hardware.
//
//   node scripts/mock-device.mjs [audio.wav] [--http-port 8080] [--audio-port 81]
//   (npm run mock -- speech.wav)
//
// HTTP server (default port 8080), like the firmware's web servers (:80 / :443):
//   GET /beam.wav[?mic=N]  endless 16 kHz / 16-bit / mono WAV at real time: a
//                          44-byte header, then samples in 1024-sample (64 ms)
//                          writes, sent with Transfer-Encoding: chunked (Node's
//                          default for a body without a length), which the firmware
//                          may also use. The WAV argument is looped (any rate /
//                          channel count / PCM or float format; converted and
//                          downmixed); without one, a synthetic buzz-and-pause
//                          pattern plays. Up to 3 streams at once, as on the device:
//                          a 4th gets 503 "too many audio streams".
//   GET /raw.wav?s=N       N seconds (1-20) of 4-channel 24-bit audio, captured in
//                          real time first; one at a time (503 while one runs).
//   GET /status.json       plausible fake fields (levels follow the audio)
//   POST /meeting          the live meeting summary, with the firmware's checks and
//                          accept policy (replaced/touched/old/ignored; one log line
//                          per post); GET /meeting.json shows what's stored
//   /control?source=&mode=&angle=[&json], /status (a tiny stand-in page)
//   GET /earwing.crt     404 here (the device serves its self-signed cert)
//   and, if `npm run build:device` has filled ../code/data, the packed site itself,
//   served gzipped like the firmware does (--site <dir> to point elsewhere).
//   Everything is sent with COOP/COEP, like the firmware.
//
// Legacy audio port (off unless --audio-port is given): the firmware still streams
// on plain-HTTP port 81 for VLC and scripts; this mimics it (raw socket, one client
// at a time, extra connections wait unanswered). The site no longer uses it.
//
// Point the dev server at it:  DEVICE_URL=http://localhost:8080 npm run dev

import net from 'node:net';
import http from 'node:http';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, extname, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const SR = 16000;
const BATCH = 1024; // samples per socket write (firmware: 4 hops × 256)
const MAX_STREAMS = 3; // concurrent /beam.wav clients on the web servers (firmware limit)
const MAX_RAW_SECONDS = 20;

// ---------------------------------------------------------------- args
const args = process.argv.slice(2);
let wavPath = null;
let audioPort = Number(process.env.MOCK_AUDIO_PORT) || 0; // 0 = no legacy audio port
let httpPort = Number(process.env.MOCK_HTTP_PORT) || 8080;
let siteDir = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..', 'code', 'data');
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === '--audio-port') audioPort = Number(args[++i]);
  else if (a === '--http-port') httpPort = Number(args[++i]);
  else if (a === '--site') siteDir = resolve(args[++i]);
  else if (a === '-h' || a === '--help') {
    console.log(
      'usage: mock-device.mjs [audio.wav] [--http-port 8080] [--audio-port 81] [--site ../code/data]',
    );
    process.exit(0);
  } else wavPath = a;
}

// ---------------------------------------------------------------- audio source
function readWav(path) {
  const b = readFileSync(path);
  if (b.toString('ascii', 0, 4) !== 'RIFF' || b.toString('ascii', 8, 12) !== 'WAVE') {
    throw new Error(`${path}: not a RIFF/WAVE file`);
  }
  let fmt = null;
  let data = null;
  for (let off = 12; off + 8 <= b.length; ) {
    const id = b.toString('ascii', off, off + 4);
    let size = b.readUInt32LE(off + 4);
    if (id === 'data' && off + 8 + size > b.length) size = b.length - off - 8; // streamed/truncated
    if (id === 'fmt ') {
      let format = b.readUInt16LE(off + 8);
      if (format === 0xfffe) format = b.readUInt16LE(off + 8 + 24); // EXTENSIBLE subformat
      fmt = {
        format,
        channels: b.readUInt16LE(off + 10),
        rate: b.readUInt32LE(off + 12),
        bits: b.readUInt16LE(off + 22),
      };
    } else if (id === 'data') {
      data = b.subarray(off + 8, off + 8 + size);
    }
    off += 8 + size + (size & 1);
  }
  if (!fmt || !data) throw new Error(`${path}: missing fmt or data chunk`);
  const { format, channels, rate, bits } = fmt;
  const bps = bits / 8;
  const frames = Math.floor(data.length / (bps * channels));
  const read = (o) => {
    if (format === 3) return bits === 64 ? data.readDoubleLE(o) : data.readFloatLE(o);
    if (format !== 1) throw new Error(`${path}: unsupported WAV format ${format}`);
    if (bits === 8) return (data[o] - 128) / 128;
    if (bits === 16) return data.readInt16LE(o) / 32768;
    if (bits === 24) return data.readIntLE(o, 3) / 8388608;
    if (bits === 32) return data.readInt32LE(o) / 2147483648;
    throw new Error(`${path}: unsupported bit depth ${bits}`);
  };
  const mono = new Float32Array(frames); // downmix: average the channels
  for (let i = 0; i < frames; i++) {
    let s = 0;
    for (let c = 0; c < channels; c++) s += read((i * channels + c) * bps);
    mono[i] = s / channels;
  }
  // Linear-interpolation resample to 16 kHz (good enough for a mock).
  let out = mono;
  if (rate !== SR) {
    const n = Math.max(1, Math.floor((frames * SR) / rate));
    out = new Float32Array(n);
    const ratio = rate / SR;
    for (let i = 0; i < n; i++) {
      const p = i * ratio;
      const j = Math.floor(p);
      const f = p - j;
      const a = mono[Math.min(j, frames - 1)];
      const c = mono[Math.min(j + 1, frames - 1)];
      out[i] = a + (c - a) * f;
    }
  }
  console.log(
    `mock: ${path}: ${rate} Hz, ${channels} ch, ${bits}-bit ${format === 3 ? 'float' : 'PCM'}, ` +
      `${(frames / rate).toFixed(1)} s → 16 kHz mono (looped)`,
  );
  return toInt16(out);
}

// No file: 1.5 s of a buzzy 150 Hz "vowel" with a 4 Hz syllable wobble, then
// 1.5 s of faint noise — something for the speech gate and meters to react to.
function synth() {
  const n = SR * 3;
  const out = new Float32Array(n);
  let seed = 1;
  const noise = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff) * 2 - 1;
  for (let i = 0; i < n; i++) {
    const t = i / SR;
    let s = noise() * 0.002;
    if (t < 1.5) {
      const env = 0.5 - 0.5 * Math.cos(2 * Math.PI * 4 * t);
      let v = 0;
      for (let h = 1; h <= 8; h++) v += Math.sin(2 * Math.PI * 150 * h * t) / h;
      s += 0.12 * env * v;
    }
    out[i] = s;
  }
  console.log('mock: no WAV given — streaming a synthetic 3 s buzz/pause loop');
  return toInt16(out);
}

function toInt16(f32) {
  const out = new Int16Array(f32.length);
  for (let i = 0; i < f32.length; i++) {
    const v = Math.max(-1, Math.min(1, f32[i]));
    out[i] = v < 0 ? Math.round(v * 32768) : Math.round(v * 32767);
  }
  return out;
}

const loop = wavPath ? readWav(wavPath) : synth();
const t0 = Date.now();
// A virtual "live" playhead shared by everyone: like the firmware's streamHead(),
// a new client starts at the current position, not at the top of the file.
const playhead = () => Math.floor(((Date.now() - t0) * SR) / 1000);

function levelDb(from, n, gain = 1) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    const v = (loop[(from + i) % loop.length] / 32768) * gain;
    sum += v * v;
  }
  return 10 * Math.log10(sum / n + 1e-12);
}

// ---------------------------------------------------------------- audio
function wavHeader({ channels = 1, bits = 16, dataBytes = 0x7fff0000 } = {}) {
  // dataBytes default: "endless", as the firmware does for /beam.wav
  const h = Buffer.alloc(44);
  const block = (channels * bits) / 8;
  h.write('RIFF', 0, 'ascii');
  h.writeUInt32LE(36 + dataBytes, 4);
  h.write('WAVEfmt ', 8, 'ascii');
  h.writeUInt32LE(16, 16);
  h.writeUInt16LE(1, 20); // PCM
  h.writeUInt16LE(channels, 22);
  h.writeUInt32LE(SR, 24);
  h.writeUInt32LE(SR * block, 28);
  h.writeUInt16LE(block, 32);
  h.writeUInt16LE(bits, 34);
  h.write('data', 36, 'ascii');
  h.writeUInt32LE(dataBytes, 40);
  return h;
}

// Raw mics: same audio, a little quieter per mic so the choice is visible.
const micGain = (mic) => (mic ? 10 ** (-(mic * 1.5) / 20) : 1);

function pcm16(pos, n, gain) {
  const out = Buffer.alloc(n * 2);
  for (let i = 0; i < n; i++) {
    const v = Math.round(loop[(pos + i) % loop.length] * gain);
    out.writeInt16LE(Math.max(-32768, Math.min(32767, v)), i * 2);
  }
  return out;
}

// Real-time pump shared by both servers: calls write(Buffer) with BATCH-sized
// blocks as the virtual playhead advances. canWrite() false → drop the audio
// (like the device's stream ring does for a stuck client). Returns stop().
function pumpAudio(mic, write, canWrite) {
  const gain = micGain(mic);
  let pos = playhead();
  let dropped = 0;
  const timer = setInterval(() => {
    const due = playhead() - pos;
    if (due < BATCH) return;
    const n = due - (due % BATCH);
    if (!canWrite()) {
      pos += n;
      dropped += n;
      return;
    }
    write(pcm16(pos, n, gain));
    pos += n;
  }, 16);
  return () => {
    clearInterval(timer);
    return dropped;
  };
}

const CORS_H = { 'Access-Control-Allow-Origin': '*', 'Cross-Origin-Resource-Policy': 'cross-origin' };
const ISOLATION_H = {
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'require-corp',
};
let clientSeq = 0;
let streams = 0; // active /beam.wav clients on the HTTP server
let rawBusy = false;

function parseMic(url) {
  const m = Number(url.searchParams.get('mic') || 0);
  return m >= 1 && m <= 4 ? m : 0;
}

function textReply(res, code, text) {
  res.writeHead(code, { 'Content-Type': 'text/plain', 'Cache-Control': 'no-store', ...CORS_H, ...ISOLATION_H });
  res.end(`${text}\n`);
}

// GET /beam.wav on the HTTP server: chunked, up to MAX_STREAMS at once.
function serveBeam(req, res, url) {
  const id = ++clientSeq;
  const mic = parseMic(url);
  if (streams >= MAX_STREAMS) {
    console.log(`mock: http client #${id} refused: ${streams} streams already (503)`);
    return textReply(res, 503, 'too many audio streams');
  }
  streams++;
  console.log(
    `mock: http client #${id} streaming ${mic ? `raw mic ${mic}` : 'beam'} ` +
      `(${req.socket.remoteAddress}; ${streams}/${MAX_STREAMS} streams)`,
  );
  res.writeHead(200, { 'Content-Type': 'audio/wav', 'Cache-Control': 'no-store', ...CORS_H, ...ISOLATION_H });
  res.write(wavHeader());
  const stop = pumpAudio(mic, (b) => res.write(b), () => res.writableLength <= SR * 2);
  let done = false;
  const finish = () => {
    if (done) return;
    done = true;
    const dropped = stop();
    streams--;
    console.log(`mock: http client #${id} disconnected${dropped ? ` (dropped ${dropped} samples)` : ''}`);
  };
  res.on('close', finish);
  res.on('error', finish);
  return undefined;
}

// GET /raw.wav?s=N: N seconds of 4-channel 24-bit audio, captured first (the
// response only starts after N seconds, like the firmware), one capture at a time.
function serveRaw(req, res, url) {
  if (rawBusy) return textReply(res, 503, 'capture busy, or the mic source is switching');
  const secs = Math.max(1, Math.min(MAX_RAW_SECONDS, Math.round(Number(url.searchParams.get('s')) || 1)));
  rawBusy = true;
  const start = playhead();
  console.log(`mock: raw capture ${secs} s`);
  const timer = setTimeout(() => {
    rawBusy = false;
    const frames = secs * SR;
    const channels = state.source === 'inbuilt' ? 1 : 4;
    const data = Buffer.alloc(frames * channels * 3);
    for (let i = 0, o = 0; i < frames; i++) {
      for (let c = 1; c <= channels; c++, o += 3) {
        const v = Math.round(loop[(start + i) % loop.length] * micGain(c) * 256);
        data.writeIntLE(Math.max(-8388608, Math.min(8388607, v)), o, 3);
      }
    }
    const head = wavHeader({ channels, bits: 24, dataBytes: data.length });
    res.writeHead(200, {
      'Content-Type': 'audio/wav',
      'Content-Length': head.length + data.length,
      'Content-Disposition': 'attachment; filename="earwing-raw.wav"',
      'X-Earwing-Resyncs-During-Capture': '0',
      'X-Earwing-Source': state.source,
      'Access-Control-Expose-Headers': 'X-Earwing-Resyncs-During-Capture, X-Earwing-Source',
      ...CORS_H,
      ...ISOLATION_H,
    });
    res.end(Buffer.concat([head, data]));
  }, secs * 1000);
  res.on('close', () => {
    if (!res.writableEnded) {
      clearTimeout(timer);
      rawBusy = false;
    }
  });
  return undefined;
}

// ---------------------------------------------------------------- legacy :81
// Only with --audio-port: raw HTTP over net.Socket, one client at a time; extra
// connections wait unanswered (like the firmware's TCP accept backlog).
const CORS = 'Access-Control-Allow-Origin: *\r\nCross-Origin-Resource-Policy: cross-origin\r\n';
const queue = [];
let busy = false;

function readRequest(sock) {
  return new Promise((resolve) => {
    let buf = '';
    const timer = setTimeout(() => done(null), 2000); // firmware: REQUEST_TIMEOUT_MS
    const onData = (d) => {
      buf += d.toString('latin1');
      if (buf.includes('\r\n\r\n') || buf.includes('\n\n')) done(buf.split(/\r?\n/)[0]);
      else if (buf.length > 8192) done(null);
    };
    const onEnd = () => done(null);
    function done(line) {
      clearTimeout(timer);
      sock.off('data', onData);
      sock.off('end', onEnd);
      sock.off('close', onEnd);
      resolve(line);
    }
    sock.on('data', onData);
    sock.on('end', onEnd);
    sock.on('close', onEnd);
    sock.resume();
  });
}

async function handleLegacy(sock) {
  const id = ++clientSeq;
  const line = await readRequest(sock);
  if (!line) {
    sock.destroy();
    return;
  }
  const [, target = '/'] = line.split(' ');
  const url = new URL(target, 'http://mock');
  if (url.pathname === '/beam.wav') {
    const mic = parseMic(url);
    console.log(`mock: :${audioPort} client #${id} streaming ${mic ? `raw mic ${mic}` : 'beam'}`);
    sock.write(
      'HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nCache-Control: no-store\r\n' +
        `${CORS}Connection: close\r\n\r\n`,
    );
    sock.write(wavHeader());
    const stop = pumpAudio(mic, (b) => sock.write(b), () => sock.writableLength <= SR * 2);
    await new Promise((r) => sock.once('close', r));
    const dropped = stop();
    console.log(`mock: :${audioPort} client #${id} disconnected${dropped ? ` (dropped ${dropped})` : ''}`);
  } else {
    sock.end(
      `HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n${CORS}Connection: close\r\n\r\n` +
        'earwing audio (mock):\n  /beam.wav        live beam output (16 kHz, 16-bit mono)\n' +
        '  /beam.wav?mic=N  raw mic N (1-4)\n',
    );
    await new Promise((r) => sock.once('close', r));
  }
}

async function pumpQueue() {
  if (busy) return;
  busy = true;
  while (queue.length) {
    const sock = queue.shift();
    if (sock.destroyed) continue; // gave up while waiting in the "backlog"
    try {
      await handleLegacy(sock);
    } catch (e) {
      console.warn('mock: client error', e.message);
      sock.destroy();
    }
  }
  busy = false;
}

if (audioPort) {
  const audioServer = net.createServer({ pauseOnConnect: true }, (sock) => {
    sock.setNoDelay(true);
    sock.on('error', () => {});
    if (busy) console.log(`mock: :${audioPort} connection waiting — legacy audio slot busy`);
    queue.push(sock);
    pumpQueue();
  });
  audioServer.on('error', (e) => {
    console.error(`mock: can't listen on legacy audio port ${audioPort}: ${e.message}`);
    process.exit(1);
  });
  audioServer.listen(audioPort, () =>
    console.log(`mock: legacy audio http://localhost:${audioPort}/beam.wav (one client)`),
  );
}

// ---------------------------------------------------------------- meeting display
// POST /meeting: the assistant's live summary for the Meeting tab, with the
// firmware's checks (415 unless application/json, 413 over 16384 bytes, 400 for bad
// JSON / v != 1 / no session), its truncation and ASCII sanitizing, and its accept
// policy (code/meeting_types.h decide()). GET /meeting.json: what's stored.
const MEETING = { maxBody: 16384, summary: 560, text: 180, feedText: 360, t: 8, session: 23, items: 12, feed: 12 };
const MEETING_SECS = ['key', 'decisions', 'actions', 'questions'];
const STALE_MS = 150000;
let meeting = null; // the stored snapshot (gen 0 = empty)
let meetingGen = 0;
let meetingSeenMs = 0; // last replaced or touched
const meetingSessions = new Set();

// Like the firmware's sanitize(): non-ASCII → '?', controls → space, spaces
// collapsed, ends trimmed; then cut to the cap.
const mclean = (v, cap) =>
  String(v ?? '')
    .replace(/[^\x00-\x7f]/gu, '?')
    .replace(/[\x00-\x1f\x7f]/g, ' ')
    .replace(/ +/g, ' ')
    .trim()
    .slice(0, cap);

function parseMeeting(body) {
  let j;
  try {
    j = JSON.parse(body);
  } catch {
    return { err: 'bad JSON' };
  }
  if (!j || typeof j !== 'object' || j.v !== 1) return { err: 'v must be 1' };
  const session = mclean(j.session, MEETING.session);
  if (!session) return { err: 'no session' };
  const sections = {};
  for (const k of MEETING_SECS) {
    const list = Array.isArray(j.sections && j.sections[k]) ? j.sections[k] : [];
    sections[k] = list.slice(0, MEETING.items).map((it) => ({
      text: mclean(it && it.text, MEETING.text),
      t: mclean(it && it.t, MEETING.t),
      new: !!(it && it.new),
    }));
  }
  const feed = (Array.isArray(j.feed) ? j.feed : [])
    .filter((e) => e && [...MEETING_SECS, 'summary'].includes(e.sec) && ['new', 'edited', 'summary'].includes(e.kind))
    .slice(0, MEETING.feed)
    .map((e) => ({
      sec: e.sec,
      kind: e.kind,
      text: mclean(e.text, MEETING.feedText),
      t: mclean(e.t, MEETING.t),
      lvl: Math.max(0, Math.min(3, Math.round(Number(e.lvl) || 0))),
      ago: Math.max(0, Math.round(Number(e.ago) || 0)),
    }));
  return {
    session,
    rev: Math.max(0, Math.round(Number(j.rev) || 0)),
    seq: Math.max(0, Math.round(Number(j.seq) || 0)),
    hb: !!j.hb,
    coversTo: mclean(j.coversTo, MEETING.t),
    summary: mclean(j.summary, MEETING.summary),
    sections,
    feed,
  };
}

// The accept policy, as the firmware's decide().
function decideMeeting(cur, lastSeenMs, p, nowMs) {
  if (!cur) return 'replaced';
  if (p.session === cur.session) return p.rev > cur.rev ? 'replaced' : p.rev === cur.rev ? 'touched' : 'old';
  if (!p.hb) return 'replaced';
  return nowMs - lastSeenMs > STALE_MS ? 'replaced' : 'ignored';
}

function serveMeeting(req, res) {
  if (req.method !== 'POST') {
    req.resume();
    return textReply(res, 405, 'POST only');
  }
  const type = String(req.headers['content-type'] || '').split(';')[0].trim().toLowerCase();
  if (type !== 'application/json') {
    req.resume();
    console.log(`mock: POST /meeting 415 (${type || 'no content type'})`);
    return textReply(res, 415, 'Content-Type must be application/json');
  }
  if (Number(req.headers['content-length']) > MEETING.maxBody) {
    req.resume();
    console.log(`mock: POST /meeting 413 (${req.headers['content-length']} B)`);
    return textReply(res, 413, 'body too large');
  }
  const chunks = [];
  let size = 0;
  let over = false;
  req.on('data', (d) => {
    size += d.length;
    if (size > MEETING.maxBody) over = true;
    else chunks.push(d);
  });
  req.on('end', () => {
    if (over) {
      console.log(`mock: POST /meeting 413 (${size} B)`);
      return textReply(res, 413, 'body too large');
    }
    const p = parseMeeting(Buffer.concat(chunks).toString('utf8'));
    if (p.err) {
      console.log(`mock: POST /meeting 400 (${p.err})`);
      return textReply(res, 400, p.err);
    }
    const now = Date.now();
    const result = decideMeeting(meeting, meetingSeenMs, p, now);
    if (result === 'replaced') {
      const firstOfSession = !meetingSessions.has(p.session);
      meetingSessions.add(p.session);
      const { seq, hb, ...snap } = p;
      meeting = { gen: ++meetingGen, ...snap, recvMs: now, firstOfSession };
    }
    if (result === 'replaced' || result === 'touched') meetingSeenMs = now;
    const counts = MEETING_SECS.map((k) => p.sections[k].length).join('/');
    console.log(
      `mock: POST /meeting ${p.session} rev ${p.rev} seq ${p.seq}${p.hb ? ' hb' : ''} → ${result} ` +
        `(items ${counts}, feed ${p.feed.length}, ${size} B)`,
    );
    res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', ...ISOLATION_H });
    return res.end(JSON.stringify({ ok: true, result, session: meeting.session, rev: meeting.rev }));
  });
  return undefined;
}

function meetingJson() {
  if (!meeting) return { gen: 0 };
  return { ...meeting, ageMs: Date.now() - meeting.recvMs, lastSeenAgoMs: Date.now() - meetingSeenMs };
}

// ---------------------------------------------------------------- status server
const state = { source: 'array', mode: 'auto', fixed: 0 };
let tracked = 90;
let hops = 0;

function statusJson() {
  const ph = playhead();
  const win = SR / 10;
  const beamDb = levelDb(Math.max(0, ph - win), win) - 3; // dBFS-ish, like the firmware's meter
  const speech = beamDb > -45;
  const board = state.source === 'inbuilt';
  // DOA drifts slowly while someone is talking.
  if (speech) tracked = (tracked + (Math.random() - 0.5) * 6 + 360) % 360;
  hops = Math.floor(ph / 256);
  const micDb = [1, 2, 3, 4].map((m) => +(beamDb - 2 + Math.random() - m * 0.4).toFixed(2));
  return {
    source: state.source,
    sourceTarget: state.source,
    switching: false,
    sourceSwitches: 0,
    liveChannels: board ? 1 : 4,
    codec: { init: 'ok', powered: true, inits: 1, failures: 0, detail: 'mock' },
    touch: { ready: true, ok: true, taps: 0, errors: 0 },
    mode: state.mode,
    doaAvailable: !board,
    doa: board ? null : +((tracked + (Math.random() - 0.5) * 20 + 360) % 360).toFixed(2),
    confidence: board ? null : +(speech ? 0.6 + Math.random() * 0.3 : Math.random() * 0.2).toFixed(3),
    tracked: board ? null : +tracked.toFixed(2),
    trackedValid: !board && speech,
    steer: board ? null : +(state.mode === 'fixed' ? state.fixed : tracked).toFixed(2),
    fixed: state.fixed,
    speech,
    micDb: board ? [micDb[0], null, null, null] : micDb,
    micPeakDb: board ? [micDb[0] + 9, null, null, null] : micDb.map((d) => +(d + 9).toFixed(2)),
    beamDb: +beamDb.toFixed(2),
    lead: [0, 0.012, -0.004, 0.02],
    leadCoh: [1, 0.91, 0.88, 0.9],
    leadAgeMs: 1200,
    dspMeanUs: 2100.5,
    dspMaxUs: 3900.2,
    dspPeakUs: 5200.8,
    hopUs: 16000,
    hops,
    running: true,
    frames: [hops * 256, hops * 256],
    overflows: [0, 0],
    timeouts: [0, 0],
    lowByteNonzero: [0, 0],
    mismatches: 0,
    resyncs: 0,
    hwFaults: 0,
    ringDepth: 1,
    ringHigh: 3,
    ringDrops: 0,
    streamDrops: 0,
    clipped: 0,
    align: { status: 'ok', offset: 0, bitShift: 0, dropMaster: 0, dropSlave: 0, runs: 1, matched: 1, firstOffset: 0, changes: 0 },
    cal: { map: '1234', gainDb: [0, 0, 0, 0], advanceUs: [0, 0, 0, 0], gainCal: 0 },
    fftMaxErr: 1.2e-6,
    fftEspUs: 410.2,
    fftRefUs: 2300.1,
    heapInternal: 180000,
    heapPsram: 7000000,
  };
}

function statusPage() {
  return `<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width">
<title>earwing (mock) status</title><body style="font:14px system-ui;max-width:640px;margin:24px auto;padding:0 16px">
<h2>earwing — mock device</h2><p><a href="/">← transcription</a></p>
<form action="/control" method="post">source <select name=source><option>array<option>inbuilt</select>
mode <select name=mode><option>auto<option>doa<option>fixed<option>raw</select>
<button>apply</button></form><pre id=j></pre>
<script>setInterval(async()=>{j.textContent=JSON.stringify(await (await fetch('/status.json')).json(),null,1)},1000)</script>`;
}

const httpServer = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://mock');
  const json = (obj, code = 200) => {
    res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', ...ISOLATION_H });
    res.end(JSON.stringify(obj));
  };
  if (url.pathname === '/beam.wav') return serveBeam(req, res, url);
  if (url.pathname === '/raw.wav') return serveRaw(req, res, url);
  if (url.pathname === '/earwing.crt') {
    return textReply(res, 404, 'mock: no certificate here (the real device serves its self-signed cert)');
  }
  if (url.pathname === '/status.json') return json(statusJson());
  if (url.pathname === '/meeting') return serveMeeting(req, res);
  if (url.pathname === '/meeting.json') return json(meetingJson());
  if (url.pathname === '/control') {
    let body = '';
    req.on('data', (d) => (body += d));
    req.on('end', () => {
      const p = new URLSearchParams(url.search);
      for (const [k, v] of new URLSearchParams(body)) p.set(k, v);
      let err = '';
      if (p.has('source')) {
        if (['array', 'inbuilt'].includes(p.get('source'))) state.source = p.get('source');
        else err += 'source: array|inbuilt\n';
      }
      if (p.has('mode')) {
        if (['raw', 'doa', 'fixed', 'auto'].includes(p.get('mode'))) state.mode = p.get('mode');
        else err += 'mode: raw|doa|fixed|auto\n';
      }
      if (p.has('angle')) state.fixed = Number(p.get('angle')) || 0;
      if (err) {
        res.writeHead(400, { 'Content-Type': 'text/plain' });
        return res.end(err);
      }
      if (p.has('json')) return json(statusJson());
      res.writeHead(303, { Location: '/status' });
      return res.end();
    });
    return undefined;
  }
  if (url.pathname === '/status') {
    res.writeHead(200, { 'Content-Type': 'text/html', ...ISOLATION_H });
    return res.end(statusPage());
  }
  if (serveSite(url.pathname, res)) return undefined;
  if (url.pathname === '/') {
    res.writeHead(200, { 'Content-Type': 'text/html', ...ISOLATION_H });
    return res.end(statusPage());
  }
  return textReply(res, 404, 'not found');
});

// Serve the packed site (../code/data, from `npm run build:device`) the way the
// firmware does: /x → /x.gz with Content-Encoding: gzip, content type from the
// un-gzipped extension, /_astro/* immutable, COOP/COEP on everything. Open
// http://<this machine's LAN IP>:<http-port>/ to test the real artifact in an
// insecure (non-localhost) context, like http://earwing.local.
const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.css': 'text/css',
  '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.ico': 'image/x-icon',
  '.txt': 'text/plain', '.webmanifest': 'application/manifest+json' };
function serveSite(pathname, res) {
  if (!siteDir) return false;
  let rel = decodeURIComponent(pathname);
  if (rel.endsWith('/')) rel += 'index.html';
  const file = resolve(siteDir, `.${rel}.gz`);
  if (!file.startsWith(siteDir + sep) || !existsSync(file)) return false;
  const ext = extname(rel);
  res.writeHead(200, {
    'Content-Type': TYPES[ext] || 'application/octet-stream',
    'Content-Encoding': 'gzip',
    'Cache-Control': rel.startsWith('/_astro/') ? 'public, max-age=31536000, immutable' : 'no-cache',
    ...ISOLATION_H,
  });
  res.end(readFileSync(file));
  return true;
}
httpServer.on('error', (e) => {
  console.error(`mock: can't listen on HTTP port ${httpPort}: ${e.message}`);
  process.exit(1);
});
if (!existsSync(resolve(siteDir, 'index.html.gz'))) siteDir = null;
httpServer.listen(httpPort, () => {
  console.log(
    `mock: http   http://localhost:${httpPort}/beam.wav (up to ${MAX_STREAMS} streams), /status.json, /meeting(.json)`,
  );
  if (siteDir) console.log(`mock: serving the packed site from ${siteDir} at http://<LAN IP>:${httpPort}/`);
  console.log(`\nrun the site against it:\n  DEVICE_URL=http://localhost:${httpPort} npm run dev\n`);
});
