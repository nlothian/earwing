// Live audio from the gulltoppr device.
//
// The firmware serves, on the SAME origin as this page (HTTPS :443 and HTTP :80):
//   GET /beam.wav        endless 16 kHz / 16-bit LE / mono PCM WAV: a canonical
//                        44-byte header, then samples forever (beamformer output)
//   GET /beam.wav?mic=N  the same, but raw mic N (1-4)
// Up to 3 audio streams at once; a 4th gets 503 "too many audio streams". The
// body may come with Transfer-Encoding: chunked or with no length at all: fetch()
// de-chunks transparently and the parser below never looks at Content-Length.
// (Port 81 still serves the same audio as plain HTTP for VLC and scripts, but the
// page must not use it: from an https page that's blocked mixed content.)
// In development the Vite dev server proxies these paths to the device or the mock.
//
// startDeviceStream() has the same shape as rigar's startMicCapture: it calls
// onChunk(Float32Array) with ~100 ms (1600-sample) blocks at 16 kHz, so the
// transcriber's window scheduler works unchanged. No resampling: the device
// already runs at 16 kHz.

export const SAMPLE_RATE = 16000;
export const CHUNK_SAMPLES = 1600; // 100 ms

// source: 0 = beam, 1-4 = raw mic N. Relative (same-origin) by default; `base`
// makes it absolute, for the Node tests (e.g. 'http://localhost:8080').
export function audioUrl(source = 0, base = '') {
  const mic = source >= 1 && source <= 4 ? `?mic=${source}` : '';
  return `${base.replace(/\/+$/, '')}/beam.wav${mic}`;
}

class StreamError extends Error {
  constructor(kind, message) {
    super(message);
    this.kind = kind; // header | http | busy | ended | timeout | network
  }
}

// Incremental WAV → Float32 parser. push() raw bytes in whatever pieces the
// network delivers; the header may be split across pieces, and so may a
// sample (odd byte counts). Emits fixed-size Float32Array blocks via onChunk.
export function createWavParser(onChunk, { chunkSamples = CHUNK_SAMPLES } = {}) {
  const MAX_HEADER = 4096; // give up if no 'data' chunk within this many bytes
  let header = new Uint8Array(0); // accumulated header bytes (until 'data' found)
  let inData = false;
  let carry = -1; // dangling low byte of a sample split across pushes
  let block = new Float32Array(chunkSamples);
  let n = 0;
  let format = null;

  // Returns the byte offset where sample data starts, or -1 if more bytes are
  // needed. Walks the RIFF chunk list, so extra chunks (LIST, fact…) are fine;
  // the firmware's canonical header puts 'data' at offset 36 → samples at 44.
  function parseHeader(b) {
    if (b.length < 12) return -1;
    const tag = (o) => String.fromCharCode(b[o], b[o + 1], b[o + 2], b[o + 3]);
    const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    if (tag(0) !== 'RIFF' || tag(8) !== 'WAVE') {
      throw new StreamError('header', 'The device sent something that is not a WAV stream.');
    }
    let off = 12;
    while (off + 8 <= b.length) {
      const id = tag(off);
      const size = dv.getUint32(off + 4, true);
      if (id === 'data') {
        if (!format) throw new StreamError('header', 'WAV stream has no format chunk.');
        return off + 8;
      }
      if (off + 8 + size > b.length) return -1; // chunk body not here yet
      if (id === 'fmt ') {
        const fmt = dv.getUint16(off + 8, true);
        format = {
          format: fmt,
          channels: dv.getUint16(off + 10, true),
          sampleRate: dv.getUint32(off + 12, true),
          bits: dv.getUint16(off + 22, true),
        };
        // 1 = PCM, 0xFFFE = WAVE_FORMAT_EXTENSIBLE (assume PCM subformat)
        const ok =
          (fmt === 1 || fmt === 0xfffe) &&
          format.channels === 1 &&
          format.sampleRate === SAMPLE_RATE &&
          format.bits === 16;
        if (!ok) {
          throw new StreamError(
            'header',
            `Unexpected audio format from the device (${format.sampleRate} Hz, ` +
              `${format.channels} ch, ${format.bits}-bit); expected 16000 Hz mono 16-bit.`,
          );
        }
      }
      off += 8 + size + (size & 1); // RIFF chunks are word-aligned
    }
    if (b.length > MAX_HEADER) throw new StreamError('header', 'WAV header too long.');
    return -1;
  }

  function sample(s) {
    block[n++] = s / 32768;
    if (n === chunkSamples) {
      onChunk(block);
      block = new Float32Array(chunkSamples);
      n = 0;
    }
  }

  function pushData(bytes) {
    let i = 0;
    if (carry >= 0 && bytes.length > 0) {
      const v = carry | (bytes[0] << 8);
      sample(v >= 0x8000 ? v - 0x10000 : v);
      carry = -1;
      i = 1;
    }
    const end = bytes.length - 1;
    for (; i < end; i += 2) {
      const v = bytes[i] | (bytes[i + 1] << 8);
      sample(v >= 0x8000 ? v - 0x10000 : v);
    }
    if (i < bytes.length) carry = bytes[i];
  }

  return {
    get format() {
      return format;
    },
    get headerDone() {
      return inData;
    },
    push(bytes) {
      if (inData) {
        pushData(bytes);
        return;
      }
      const joined = new Uint8Array(header.length + bytes.length);
      joined.set(header);
      joined.set(bytes, header.length);
      const start = parseHeader(joined);
      if (start < 0) {
        header = joined;
        return;
      }
      inData = true;
      header = null;
      pushData(joined.subarray(start));
    },
  };
}

function friendlyMessage(err, url) {
  if (err instanceof StreamError) {
    switch (err.kind) {
      case 'timeout':
        return (
          `The device didn't start sending audio within a few seconds (${url}). It may be ` +
          'busy or restarting; try again in a moment.'
        );
      case 'ended':
        return 'The device closed the audio stream.';
      default:
        return err.message;
    }
  }
  // fetch() rejects with a bare TypeError for refused/unreachable failures.
  return `Couldn't reach the device's audio stream at ${url}. Is gulltoppr powered on and on this network?`;
}

// Read a short error body (the firmware sends one line of text) without hanging on it.
async function shortBody(res, ms = 1500) {
  try {
    const text = await Promise.race([res.text(), new Promise((r) => setTimeout(() => r(''), ms))]);
    return String(text || '').trim().slice(0, 200);
  } catch {
    return '';
  }
}

function httpError(status, body) {
  if (status === 503) {
    const msg = /too many/i.test(body)
      ? 'The device is already streaming audio to 3 clients, the most it allows. Close another ' +
        'tab, VLC or script that is playing beam.wav and try again.'
      : /capture|busy|switch/i.test(body)
        ? 'The device is busy: a raw capture (/raw.wav) is running or the mic source is ' +
          'switching. Try again in a few seconds.'
        : `The device refused the audio stream (503${body ? `: ${body}` : ''}). Try again shortly.`;
    return new StreamError('busy', msg);
  }
  if (status === 404) {
    return new StreamError(
      'http',
      'The device has no /beam.wav on this address. Its firmware may be older than this page; ' +
        'update it (the audio used to be on port 81).',
    );
  }
  return new StreamError(
    'http',
    `The device's audio server answered ${status}${body ? `: ${body}` : ''}.`,
  );
}

// Stream live audio from the device.
//   onChunk(Float32Array)  ~100 ms blocks of 16 kHz mono audio
//   onState(state, info)   'connecting' | 'streaming' | 'reconnecting' (info.attempt)
//   onError(message)       terminal failure — the stream has stopped
// Returns { url, stop() }; stop() aborts the fetch, and closing the connection frees
// one of the device's 3 audio-stream slots.
//
// If the stream drops mid-session (WiFi hiccup, device reboot) it reconnects with
// backoff, up to maxReconnects consecutive failures (a 503 then usually means the
// device hasn't noticed our old connection is gone yet, so it is retried too). A
// failure on the very first connection is reported straight away instead: it's
// almost always an unreachable device or all stream slots busy, and retrying would
// only hide that.
export function startDeviceStream(
  onChunk,
  {
    base = '', // '' = same origin (the page is served by the device / dev proxy)
    source = 0,
    onState = () => {},
    onError = () => {},
    connectTimeoutMs = 5000, // no header/data within this → device stuck or unreachable
    stallTimeoutMs = 5000, // no bytes for this long mid-stream → treat as dropped
    maxReconnects = 8, // ≈25 s of retries (backoff 0.5→4 s): enough for a device reboot + WiFi rejoin
  } = {},
) {
  const url = audioUrl(source, base);
  let stopped = false;
  let controller = null;
  let everStreamed = false;
  let failures = 0;

  async function connectOnce() {
    controller = new AbortController();
    const ctl = controller;
    let timer = null;
    let timedOut = false;
    const arm = (ms) => {
      clearTimeout(timer);
      timer = setTimeout(() => {
        timedOut = true;
        ctl.abort();
      }, ms);
    };
    // Parser emits into onChunk; the first block marks the session healthy.
    const parser = createWavParser((block) => {
      if (!everStreamed || failures) onState('streaming');
      everStreamed = true;
      failures = 0;
      onChunk(block);
    });

    arm(connectTimeoutMs);
    try {
      const res = await fetch(url, { signal: ctl.signal, cache: 'no-store' });
      if (!res.ok) {
        const body = await shortBody(res);
        ctl.abort(); // don't leave the connection open if the body never ended
        throw httpError(res.status, body);
      }
      if (!res.body) throw new StreamError('http', 'This browser cannot stream fetch bodies.');
      const reader = res.body.getReader();
      for (;;) {
        const { done, value } = await reader.read();
        if (done) throw new StreamError('ended', 'stream ended');
        // Header not in yet → still on the connect timeout; afterwards, stall timeout.
        arm(parser.headerDone ? stallTimeoutMs : connectTimeoutMs);
        if (value && value.length) parser.push(value);
      }
    } catch (err) {
      if (timedOut) throw new StreamError('timeout', 'timeout');
      throw err;
    } finally {
      clearTimeout(timer);
    }
  }

  (async () => {
    onState('connecting');
    while (!stopped) {
      try {
        await connectOnce();
      } catch (err) {
        if (stopped) return; // our own abort
        const fatal = err instanceof StreamError && err.kind === 'header';
        if (!everStreamed || fatal || failures >= maxReconnects) {
          console.warn('[deviceStream]', err);
          stopped = true;
          onError(
            everStreamed && !fatal
              ? `Lost the device's audio stream and couldn't reconnect. ${friendlyMessage(err, url)}`
              : friendlyMessage(err, url),
          );
          return;
        }
        failures += 1;
        onState('reconnecting', { attempt: failures, max: maxReconnects });
        const delay = Math.min(4000, 500 * 2 ** (failures - 1));
        await new Promise((r) => setTimeout(r, delay));
      }
    }
  })();

  return {
    url,
    stop() {
      stopped = true;
      if (controller) controller.abort();
    },
  };
}
