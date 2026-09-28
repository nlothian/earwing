// Small, pure helpers shared by the 3D scene, the schematic and the timeline.

export const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
export const lerp = (a, b, u) => a + (b - a) * u;
export const mix = lerp;

// 0..1 progress of t through [t0, t1]
export const seg = (t, t0, t1) => clamp((t - t0) / (t1 - t0), 0, 1);

// smoothstep of u between edges a and b
export const smooth = (a, b, u) => {
  const x = clamp((u - a) / (b - a), 0, 1);
  return x * x * (3 - 2 * x);
};

export const ease = {
  inOutSine: (u) => -(Math.cos(Math.PI * u) - 1) / 2,
  outCubic: (u) => 1 - Math.pow(1 - u, 3),
  inCubic: (u) => u * u * u,
  inOutCubic: (u) => (u < 0.5 ? 4 * u * u * u : 1 - Math.pow(-2 * u + 2, 3) / 2),
  outExpo: (u) => (u >= 1 ? 1 : 1 - Math.pow(2, -10 * u)),
  inOutExpo: (u) =>
    u <= 0 ? 0 : u >= 1 ? 1 : u < 0.5 ? Math.pow(2, 20 * u - 10) / 2 : (2 - Math.pow(2, -20 * u + 10)) / 2,
  outBack: (u) => {
    const c1 = 1.4, c3 = c1 + 1;
    return 1 + c3 * Math.pow(u - 1, 3) + c1 * Math.pow(u - 1, 2);
  },
};

// Deterministic PRNG (mulberry32) for noise that must be identical every render.
export function rng(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// Gaussian-ish noise array, gently low-passed so traces read as audio, not static.
export function noiseArray(n, seed, smoothing = 0.55) {
  const r = rng(seed);
  const out = new Float32Array(n);
  let y = 0;
  for (let i = 0; i < n; i++) {
    const g = (r() + r() + r() + r() - 2) * 1.2;
    y = smoothing * y + (1 - smoothing) * g;
    out[i] = y;
  }
  let max = 0;
  for (let i = 0; i < n; i++) max = Math.max(max, Math.abs(out[i]));
  for (let i = 0; i < n; i++) out[i] /= max || 1;
  return out;
}
