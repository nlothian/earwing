// Browser audio helpers for the transcriber island.
// All output is 16 kHz mono Float32 PCM — exactly what the Parakeet ASR model expects.
//
// (rigar's startMicCapture is gone: earwing's audio comes from the device's
// port-81 WAV stream — see deviceStream.js. That also sidesteps getUserMedia and
// AudioWorklet, neither of which exists on the insecure http://earwing.local.)

const TARGET_SR = 16000;

function getAudioContextClass() {
  return window.AudioContext || window.webkitAudioContext;
}

// Decode an uploaded file and return a 16 kHz mono Float32Array.
export async function decodeFileTo16kMono(file) {
  const arrayBuffer = await file.arrayBuffer();
  const AudioCtx = getAudioContextClass();
  const tmpCtx = new AudioCtx();
  let decoded;
  try {
    decoded = await tmpCtx.decodeAudioData(arrayBuffer);
  } finally {
    tmpCtx.close();
  }

  if (decoded.sampleRate === TARGET_SR && decoded.numberOfChannels === 1) {
    return decoded.getChannelData(0).slice();
  }

  // OfflineAudioContext resamples to 16 kHz and downmixes to mono in one pass.
  const length = Math.max(1, Math.ceil(decoded.duration * TARGET_SR));
  const offline = new OfflineAudioContext(1, length, TARGET_SR);
  const source = offline.createBufferSource();
  source.buffer = decoded;
  source.connect(offline.destination);
  source.start();
  const rendered = await offline.startRendering();
  return rendered.getChannelData(0).slice();
}

// Extract absolute sample range [start, end) from an array of Float32 chunks
// into one contiguous Float32Array.
export function sliceSamples(chunks, start, end) {
  const out = new Float32Array(Math.max(0, end - start));
  let pos = 0; // absolute index at the start of the current chunk
  let written = 0;
  for (const c of chunks) {
    const cStart = pos;
    const cEnd = pos + c.length;
    pos = cEnd;
    if (cEnd <= start) continue;
    if (cStart >= end) break;
    const from = Math.max(start, cStart) - cStart;
    const to = Math.min(end, cEnd) - cStart;
    out.set(c.subarray(from, to), written);
    written += to - from;
  }
  return out;
}

// Root-mean-square level of a Float32 PCM block (0..~1), for silence detection.
export function rms(chunk) {
  let sum = 0;
  for (let i = 0; i < chunk.length; i++) sum += chunk[i] * chunk[i];
  return Math.sqrt(sum / (chunk.length || 1));
}
