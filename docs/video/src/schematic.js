// S3: how beamforming works, as a top-down technical drawing drawn from time.
//
// Local frame: one unit = the 30 mm mic spacing, origin at the array centre,
// +x to screen right, +y to screen down. The top-down camera in scene3d.js
// looks down with world -y as screen up, so world +x (toward M4) is screen
// LEFT: M1 sits at u = +1.5 and M4 at u = -1.5. Angles follow the firmware:
// theta from broadside, positive toward M4.

import { MIC_X } from "./scene3d.js";
import { clamp, ease, lerp, noiseArray, rng, seg, smooth } from "./util.js";

const W = 1920;
const H = 1080;
const SIGNAL = "#ff7a2e";
const PAPER = "#f3eee6";
const BEIGE = "#d6c7ac";
const NOISE = "#6b6e73";
const BEAT = 0.46875;

const MICS = MIC_X.map((x) => ({ u: -x / 30, v: 0 }));

// ---- layout over time: centre (px) and scale (px per 30 mm) ----
const LAYOUTS = {
  main: { cx: 960, cy: 780, s: 150 }, // == scene3d TOPDOWN framing
  left: { cx: 540, cy: 690, s: 108 },
  beam: { cx: 960, cy: 830, s: 146 },
};
function layout(t) {
  const a = ease.inOutCubic(seg(t, 9.375, 10.05));
  const b = ease.inOutCubic(seg(t, 13.1, 13.75));
  const L = (k) => lerp(lerp(LAYOUTS.main[k], LAYOUTS.left[k], a), LAYOUTS.beam[k], b);
  const exit = ease.inCubic(seg(t, 14.8, 15.08));
  return { cx: L("cx"), cy: L("cy") - 40 * exit, s: L("s") * (1 + 0.16 * exit), alpha: 1 - exit };
}

// ---- sources ----
const deg = (d) => (d * Math.PI) / 180;
const polar = (thetaDeg, r) => ({ u: -r * Math.sin(deg(thetaDeg)), v: -r * Math.cos(deg(thetaDeg)) });
export const voiceTheta = (t) => lerp(35, -25, ease.inOutCubic(seg(t, 13.95, 14.65)));
const steerTheta = (t) => lerp(35, -25, ease.inOutCubic(seg(t, 14.05, 14.85)));
const VOICE_R = 3.8;
const NOISES = [
  { theta: -63, r: 4.25, seed: 11 },
  { theta: 76, r: 4.5, seed: 23 },
];
const SPEED = 4.3; // units per second

const voiceEmits = [];
for (let te = 7.5 + BEAT; te < 15.2; te += BEAT) voiceEmits.push(te);
for (const n of NOISES) {
  const r = rng(n.seed);
  n.emits = [];
  for (let te = 7.62 + r() * 0.3; te < 15.2; te += 0.22 + r() * 0.42) n.emits.push(te);
}

// ---- array factor of the 4-mic delay-and-sum beam (4 kHz, c = 343 m/s) ----
const K = (2 * Math.PI * 4000) / 343000; // rad per mm
function beamPower(phiDeg, steerDeg) {
  const d = Math.sin(deg(phiDeg)) - Math.sin(deg(steerDeg));
  let re = 0, im = 0;
  for (const x of MIC_X) {
    re += Math.cos(K * x * d);
    im += Math.sin(K * x * d);
  }
  return (re * re + im * im) / 16;
}

// ---- waveform panel signals ----
const PANEL = { x0: 1010, w: 780, rows: [330, 440, 550, 660], amp: 36, beamRow: 640, oneRow: 440 };
const N = PANEL.w;
const noises = [0, 1, 2, 3].map((i) => noiseArray(N, 101 + i * 17, 0.35));
const VT = new Float32Array(901); // voice template, index 450 = burst centre
{
  let max = 0;
  for (let i = 0; i < VT.length; i++) {
    const x = i - 450;
    const env = Math.exp(-Math.pow(x / 120, 2)) * (0.45 + 0.55 * Math.pow(Math.sin(x * 0.03 + 0.6), 2));
    const car = Math.sin(x * 0.36) + 0.55 * Math.sin(x * 0.83 + 1.1) + 0.3 * Math.sin(x * 1.61 + 2.3);
    VT[i] = env * car;
    max = Math.max(max, Math.abs(VT[i]));
  }
  for (let i = 0; i < VT.length; i++) VT[i] /= max;
}
const voiceAt = (x) => {
  const i = Math.round(x + 450);
  return i < 0 || i >= VT.length ? 0 : VT[i];
};
const NOISE_AMP = 0.44;
const D_PX = 46; // exaggerated inter-mic delay in the plot

export function createSchematic(canvas) {
  const g = canvas.getContext("2d");

  const P = (L, p) => ({ x: L.cx + p.u * L.s, y: L.cy + p.v * L.s });

  function roundRect(x, y, w, h, r) {
    g.beginPath();
    g.moveTo(x + r, y);
    g.arcTo(x + w, y, x + w, y + h, r);
    g.arcTo(x + w, y + h, x, y + h, r);
    g.arcTo(x, y + h, x, y, r);
    g.arcTo(x, y, x + w, y, r);
    g.closePath();
  }

  function grid(t, L) {
    const a = 0.055 * smooth(7.5, 8.1, t) * L.alpha;
    if (a <= 0) return;
    g.fillStyle = `rgba(243,238,230,${a})`;
    for (let x = 40; x < W; x += 40) for (let y = 40; y < H; y += 40) g.fillRect(x - 1, y - 1, 2, 2);
  }

  function deviceOutline(t, L) {
    const a = smooth(7.5, 7.95, t) * L.alpha;
    if (a <= 0) return;
    const s = L.s;
    // display box (behind the mic row: screen-down)
    g.lineWidth = 1.5;
    g.strokeStyle = `rgba(214,199,172,${0.55 * a})`;
    g.fillStyle = `rgba(20,19,18,${0.9 * a})`;
    roundRect(L.cx - (97 / 60) * s, L.cy + (13 / 30) * s, (97 / 30) * s, 2 * s, (5 / 30) * s);
    g.fill();
    g.stroke();
    // screen
    g.strokeStyle = `rgba(214,199,172,${0.25 * a})`;
    roundRect(L.cx - (2.25 + 28.6) / 30 * s, L.cy + (43.1 - 21.4) / 30 * s, (57.2 / 30) * s, (42.8 / 30) * s, 0.03 * s);
    g.stroke();
    // mic plate
    g.strokeStyle = `rgba(214,199,172,${0.95 * a})`;
    g.fillStyle = `rgba(24,23,21,${a})`;
    roundRect(L.cx - 2 * s, L.cy - 0.5 * s, 4 * s, s, (5 / 30) * s);
    g.fill();
    g.stroke();
  }

  function micFlash(i, t) {
    let v = 0;
    const m = MICS[i];
    for (const te of voiceEmits) {
      if (te > t) break;
      const src = polar(voiceTheta(te), VOICE_R);
      const hit = te + Math.hypot(m.u - src.u, m.v - src.v) / SPEED;
      if (hit <= t) v += Math.exp(-(t - hit) / 0.2);
    }
    return clamp(v, 0, 1);
  }

  function mics(t, L) {
    const a = smooth(7.5, 7.95, t) * L.alpha;
    if (a <= 0) return;
    const r = (9.5 / 30) * L.s;
    MICS.forEach((m, i) => {
      const p = P(L, m);
      const f = micFlash(i, t) * smooth(7.9, 8.3, t);
      g.save();
      if (f > 0.02) {
        g.shadowColor = SIGNAL;
        g.shadowBlur = 28 * f;
      }
      g.beginPath();
      g.arc(p.x, p.y, r, 0, Math.PI * 2);
      g.fillStyle = `rgba(20,20,20,${a})`;
      g.fill();
      if (f > 0.02) {
        g.fillStyle = `rgba(255,122,46,${0.42 * f * a})`;
        g.fill();
      }
      g.restore();
      g.lineWidth = 1.5 + 1.5 * f;
      g.strokeStyle = f > 0.02 ? `rgba(255,${Math.round(199 - 77 * f)},${Math.round(172 - 126 * f)},${(0.7 + 0.3 * f) * a})` : `rgba(214,199,172,${0.7 * a})`;
      g.beginPath();
      g.arc(p.x, p.y, r, 0, Math.PI * 2);
      g.stroke();
      g.fillStyle = `rgba(142,137,128,${a * smooth(8.0, 8.4, t)})`;
      g.font = "600 15px 'JetBrains Mono', monospace";
      g.textAlign = "center";
      g.textBaseline = "top";
      g.fillText(`M${i + 1}`, p.x, p.y + r + 12);
    });
  }

  function rings(t, L, emits, center, color, base, noiseDim) {
    for (const te of emits) {
      if (te > t) break;
      const rr = SPEED * (t - te);
      if (rr > 9.5) continue;
      const c = P(L, center(te));
      const a = base * Math.pow(1 - rr / 9.5, 1.4) * smooth(0, 0.25, rr) * L.alpha * noiseDim;
      if (a <= 0.003) continue;
      g.strokeStyle = color.replace("A", a.toFixed(3));
      g.beginPath();
      g.arc(c.x, c.y, rr * L.s, 0, Math.PI * 2);
      g.stroke();
    }
  }

  function source(t, L, pos, color, label, a, pulse) {
    if (a <= 0) return;
    const p = P(L, pos);
    g.save();
    g.shadowColor = color;
    g.shadowBlur = 24 * pulse;
    g.fillStyle = color;
    g.globalAlpha = a;
    g.beginPath();
    g.arc(p.x, p.y, 9 + 3 * pulse, 0, Math.PI * 2);
    g.fill();
    g.restore();
    g.globalAlpha = a;
    g.fillStyle = color === SIGNAL ? PAPER : "#8e8980";
    g.font = "600 16px 'JetBrains Mono', monospace";
    g.textAlign = "center";
    g.textBaseline = "bottom";
    g.fillText(label, p.x, p.y - 22);
    g.globalAlpha = 1;
    return p;
  }

  function beam(t, L) {
    const gr = ease.outExpo(seg(t, 13.125, 13.8)) * L.alpha;
    if (gr <= 0) return null;
    const st = steerTheta(t);
    const R = 4.25 * L.s;
    const c = { x: L.cx, y: L.cy };
    // polar grid
    g.save();
    g.setLineDash([3, 7]);
    g.lineWidth = 1;
    g.strokeStyle = `rgba(243,238,230,${0.13 * gr})`;
    for (const k of [1 / 3, 2 / 3, 1]) {
      g.beginPath();
      g.arc(c.x, c.y, R * k, Math.PI, 2 * Math.PI);
      g.stroke();
    }
    for (let a = -90; a <= 90; a += 30) {
      g.beginPath();
      g.moveTo(c.x, c.y);
      g.lineTo(c.x - R * Math.sin(deg(a)), c.y - R * Math.cos(deg(a)));
      g.stroke();
    }
    g.restore();
    // lobe
    g.beginPath();
    for (let phi = -90; phi <= 90; phi += 0.5) {
      const r = R * gr * beamPower(phi, st);
      const x = c.x - r * Math.sin(deg(phi));
      const y = c.y - r * Math.cos(deg(phi));
      if (phi === -90) g.moveTo(x, y);
      else g.lineTo(x, y);
    }
    g.closePath();
    const fill = g.createRadialGradient(c.x, c.y, 0, c.x, c.y, R);
    fill.addColorStop(0, `rgba(255,122,46,${0.5 * gr})`);
    fill.addColorStop(1, `rgba(255,122,46,${0.06 * gr})`);
    g.fillStyle = fill;
    g.fill();
    g.save();
    g.shadowColor = SIGNAL;
    g.shadowBlur = 18;
    g.lineWidth = 2;
    g.strokeStyle = `rgba(255,150,90,${0.95 * gr})`;
    g.stroke();
    g.restore();
    return { gr, st };
  }

  // steering readout beside the voice
  function readout(p, b) {
    if (!b || b.gr <= 0 || !p) return;
    const v = Math.round(b.st);
    g.fillStyle = `rgba(243,238,230,${b.gr})`;
    g.font = "600 22px 'JetBrains Mono', monospace";
    g.textAlign = "left";
    g.textBaseline = "middle";
    g.fillText(`θ ${v >= 0 ? "+" : "−"}${Math.abs(v)}°`, p.x + 26, p.y + 1);
  }

  function trace(ys, y0, amp, alpha, burstX, colorBurst, width) {
    if (alpha <= 0.003) return;
    const x0 = PANEL.x0;
    const gradient = g.createLinearGradient(x0, 0, x0 + PANEL.w, 0);
    const a = alpha.toFixed(3);
    const bx = clamp(burstX / PANEL.w, 0.001, 0.999);
    const hw = 175 / PANEL.w;
    gradient.addColorStop(0, `rgba(107,110,115,${a})`);
    gradient.addColorStop(clamp(bx - hw, 0, 1), `rgba(107,110,115,${a})`);
    gradient.addColorStop(clamp(bx - hw * 0.55, 0, 1), colorBurst.replace("A", a));
    gradient.addColorStop(clamp(bx + hw * 0.55, 0, 1), colorBurst.replace("A", a));
    gradient.addColorStop(clamp(bx + hw, 0, 1), `rgba(107,110,115,${a})`);
    gradient.addColorStop(1, `rgba(107,110,115,${a})`);
    g.strokeStyle = gradient;
    g.lineWidth = width;
    g.lineJoin = "round";
    g.beginPath();
    for (let i = 0; i < ys.length; i++) {
      const x = x0 + i, y = y0 - ys[i] * amp;
      if (i === 0) g.moveTo(x, y);
      else g.lineTo(x, y);
    }
    g.stroke();
  }

  function panel(t) {
    const appear = ease.outCubic(seg(t, 9.45, 10.15));
    const out = 1 - ease.inCubic(seg(t, 13.35, 13.6));
    const A = appear * out;
    if (A <= 0) return;
    const align = ease.inOutCubic(seg(t, 11.25, 11.7));
    const merge = ease.inOutCubic(seg(t, 11.75, 12.25));
    const reveal = PANEL.x0 + PANEL.w * appear;
    g.save();
    g.beginPath();
    g.rect(PANEL.x0 - 220, 0, reveal - PANEL.x0 + 220, H);
    g.clip();

    const centre = 395;
    const burst = MICS.map((_, i) => centre + (1.5 - i) * D_PX * (1 - align));
    // per-mic traces
    for (let i = 0; i < 4; i++) {
      const ys = new Float32Array(N);
      for (let x = 0; x < N; x++) ys[x] = voiceAt(x - burst[i]) + NOISE_AMP * noises[i][x];
      const y0 = lerp(PANEL.rows[i], PANEL.beamRow, merge);
      const a = A * (1 - merge);
      trace(ys, y0, PANEL.amp, a, burst[i], "rgba(255,122,46,A)", 2);
      g.fillStyle = `rgba(142,137,128,${a})`;
      g.font = "600 17px 'JetBrains Mono', monospace";
      g.textAlign = "right";
      g.textBaseline = "middle";
      g.fillText(`M${i + 1}`, PANEL.x0 - 26, y0);
    }
    // delay bracket between M3 and M4
    const br = A * (1 - smooth(11.15, 11.3, t)) * smooth(9.9, 10.15, t);
    if (br > 0) {
      const y = (PANEL.rows[2] + PANEL.rows[3]) / 2 + 4;
      const xa = PANEL.x0 + burst[3], xb = PANEL.x0 + burst[2];
      g.strokeStyle = `rgba(243,238,230,${br})`;
      g.lineWidth = 1.5;
      g.beginPath();
      g.moveTo(xa, y - 7); g.lineTo(xa, y + 7);
      g.moveTo(xb, y - 7); g.lineTo(xb, y + 7);
      g.moveTo(xa, y); g.lineTo(xb, y);
      g.stroke();
      g.fillStyle = `rgba(243,238,230,${br})`;
      g.font = "600 20px 'JetBrains Mono', monospace";
      g.textAlign = "left";
      g.fillText("50 µs", xb + 14, y);
    }
    // alignment guide
    const guide = A * smooth(11.6, 11.8, t) * (1 - merge);
    if (guide > 0) {
      g.save();
      g.setLineDash([4, 6]);
      g.strokeStyle = `rgba(255,122,46,${0.6 * guide})`;
      g.lineWidth = 1.5;
      g.beginPath();
      g.moveTo(PANEL.x0 + centre, PANEL.rows[0] - 60);
      g.lineTo(PANEL.x0 + centre, PANEL.rows[3] + 60);
      g.stroke();
      g.restore();
    }
    // result: one mic vs the beam
    if (merge > 0) {
      const a = A * merge;
      const one = new Float32Array(N), bm = new Float32Array(N);
      for (let x = 0; x < N; x++) {
        const v = voiceAt(x - centre);
        one[x] = v + NOISE_AMP * noises[0][x];
        bm[x] = v + (NOISE_AMP * (noises[0][x] + noises[1][x] + noises[2][x] + noises[3][x])) / 4;
      }
      trace(one, PANEL.oneRow, PANEL.amp * 1.25, a * 0.9, centre, "rgba(243,238,230,A)", 2);
      trace(bm, PANEL.beamRow, PANEL.amp * 1.25, a, centre, "rgba(255,122,46,A)", 3);
      g.font = "600 17px 'JetBrains Mono', monospace";
      g.textAlign = "left";
      g.textBaseline = "bottom";
      g.fillStyle = `rgba(142,137,128,${a})`;
      g.fillText("ONE MIC", PANEL.x0, PANEL.oneRow - 74);
      g.fillStyle = `rgba(255,122,46,${a})`;
      g.fillText("EARWING", PANEL.x0, PANEL.beamRow - 74);
    }
    g.restore();
  }

  function render(t) {
    g.clearRect(0, 0, W, H);
    const L = layout(t);
    if (L.alpha <= 0) return;
    grid(t, L);
    const beamOn = ease.outExpo(seg(t, 13.125, 13.8));
    const noiseDim = 1 - 0.7 * beamOn;
    g.lineWidth = 1.5;
    for (const n of NOISES) rings(t, L, n.emits, () => polar(n.theta, n.r), "rgba(107,110,115,A)", 0.55, noiseDim);
    g.lineWidth = 2.2;
    rings(t, L, voiceEmits, (te) => polar(voiceTheta(te), VOICE_R), "rgba(255,122,46,A)", 0.95, 1);
    const b = beam(t, L);
    deviceOutline(t, L);
    mics(t, L);
    const srcIn = ease.outBack(seg(t, 7.62, 7.95));
    const lastEmit = voiceEmits.filter((te) => te <= t).pop() ?? -9;
    const pulse = Math.exp(-(t - lastEmit) / 0.25);
    const vp = source(t, L, polar(voiceTheta(t), VOICE_R), SIGNAL, "VOICE", srcIn * L.alpha, pulse);
    readout(vp, b);
    NOISES.forEach((n, i) => {
      const a = ease.outCubic(seg(t, 7.75 + i * 0.12, 8.1 + i * 0.12)) * L.alpha * (1 - 0.55 * beamOn);
      source(t, L, polar(n.theta, n.r), "#6b6e73", "NOISE", a, 0.3);
    });
    panel(t);
  }

  return { render };
}
