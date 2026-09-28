// Orchestrates the film: one paused GSAP timeline for the DOM, plus the 3D
// scene and the beamforming schematic, both rendered from HyperFrames time.

import { createScene3d, doaAt } from "./scene3d.js";
import { createSchematic } from "./schematic.js";
import { clamp, seg } from "./util.js";

const $ = (s) => document.querySelector(s);

const gl = createScene3d($("#gl"));
const fx = createSchematic($("#fx"));

// ------------------------------------------------ per-frame DOM overlays ---

const dim = { a: $("#dim-a"), b: $("#dim-b"), line: $("#dim-line"), text: $("#dim-text") };
const setLine = (el, p, q) => {
  el.setAttribute("x1", p.x.toFixed(1));
  el.setAttribute("y1", p.y.toFixed(1));
  el.setAttribute("x2", q.x.toFixed(1));
  el.setAttribute("y2", q.y.toFixed(1));
};
const streamPath = $("#stream-path");
const packets = $("#packets");
const lan = $("#s4-lan");
const stripDoa = $("#strip-doa");
const grain = $("#grain");
const PACKETS = 7;
for (let i = 0; i < PACKETS; i++) {
  const c = document.createElementNS("http://www.w3.org/2000/svg", "circle");
  c.setAttribute("r", "5.5");
  c.setAttribute("fill", "#ff7a2e");
  packets.appendChild(c);
}

function bez(p0, p1, p2, p3, u) {
  const v = 1 - u;
  return {
    x: v * v * v * p0.x + 3 * v * v * u * p1.x + 3 * v * u * u * p2.x + u * u * u * p3.x,
    y: v * v * v * p0.y + 3 * v * v * u * p1.y + 3 * v * u * u * p2.y + u * u * u * p3.y,
  };
}

function overlays(t) {
  // film grain: a new offset every frame, derived from the frame number
  const f = Math.round(t * 60);
  const gx = ((f * 73) % 64) - 32, gy = ((f * 151) % 64) - 32;
  grain.style.transform = `translate(${gx}px, ${gy}px)`;

  if (t >= 5.6 && t <= 7.9) {
    // S2: a 30 mm dimension between M1 and M2
    // a dimension on the table plane just in front of the plate, M1 to M2
    const a0 = gl.project(-45, -19, 8.8), a1 = gl.project(-45, -25, 8.8);
    const b0 = gl.project(-15, -19, 8.8), b1 = gl.project(-15, -25, 8.8);
    const m0 = gl.project(-45, -22, 8.8), m1 = gl.project(-15, -22, 8.8);
    setLine(dim.a, a0, a1);
    setLine(dim.b, b0, b1);
    setLine(dim.line, m0, m1);
    const mid = gl.project(-30, -22, 8.8);
    dim.text.setAttribute("x", mid.x.toFixed(1));
    dim.text.setAttribute("y", (mid.y + 34).toFixed(1));
  }

  if (t >= 14.9 && t <= 21) {
    // S4: audio streaming from the device to the browser over the LAN
    const p0 = gl.project(64, 12, -4);
    const p3 = { x: 898, y: 560 };
    const p1 = { x: p0.x + 120, y: p0.y + 40 };
    const p2 = { x: p3.x - 120, y: p3.y };
    streamPath.setAttribute(
      "d",
      `M ${p0.x.toFixed(1)} ${p0.y.toFixed(1)} C ${p1.x.toFixed(1)} ${p1.y.toFixed(1)}, ${p2.x.toFixed(1)} ${p2.y.toFixed(1)}, ${p3.x} ${p3.y}`,
    );
    streamPath.setAttribute("stroke-dashoffset", (-t * 36).toFixed(1));
    const flow = seg(t, 15.45, 15.9);
    Array.from(packets.children).forEach((c, i) => {
      const u = (t * 0.75 + i / PACKETS) % 1;
      const p = bez(p0, p1, p2, p3, u);
      c.setAttribute("cx", p.x.toFixed(1));
      c.setAttribute("cy", p.y.toFixed(1));
      c.setAttribute("opacity", (flow * Math.sin(Math.PI * u) * 0.95).toFixed(3));
    });
    const mid = bez(p0, p1, p2, p3, 0.5);
    lan.style.left = `${(mid.x - 120).toFixed(1)}px`;
    lan.style.top = `${(mid.y + 26).toFixed(1)}px`;
    const d = Math.round(doaAt(t));
    stripDoa.textContent = `θ ${d >= 0 ? "+" : "−"}${Math.abs(d)}°`;
  }
}

// --------------------------------------------------------------- render ----

const GL_ON = [
  [0, 3.8],
  [5.55, 7.95],
  [14.9, 20.95],
  [26.2, 30.01],
];
let built = false;
function renderAt(t) {
  if (built && GL_ON.some(([a, b]) => t >= a && t <= b)) gl.render(t);
  if (t >= 7.45 && t <= 15.3) fx.render(t);
  overlays(t);
}
window.addEventListener("hf-seek", (e) => renderAt(e.detail.time));

window.__hf = window.__hf || {};
window.__hf.buildReady = window.__hf.buildReady || {};
window.__hf.buildReady.earwing = Promise.all([gl.ready, document.fonts.ready]).then(() => {
  built = true;
  renderAt(window.__hfThreeTime || 0);
});
