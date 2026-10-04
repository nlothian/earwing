// Generates an animated (SMIL) version of the EarWing bat logo by bending the
// wing paths around a hinge line at each wing root. Frame 0 is the original art.
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const HERE = fileURLToPath(new URL('.', import.meta.url));
const SRC = HERE + 'EarWing Logo simplified strokes.svg';
const OUT = process.argv[2] ?? HERE + 'EarWing Logo simplified strokes animated.svg';
const FRAMES_DIR = process.argv[3]; // optional: write static frame SVGs here
const N = 30;          // keyframes per cycle
const DUR = 1.1;       // seconds per wingbeat

const src = fs.readFileSync(SRC, 'utf8');
const pathRe = /<path d="([^"]+)"([^>]*)\/>/g;
const paths = [...src.matchAll(pathRe)].map(m => ({ d: m[1], attrs: m[2] }));

// Tokenise a path into commands + numbers so the structure is preserved when morphing.
const tok = d => d.match(/[MCLZ]|-?\d+(?:\.\d+)?/g);

const sub = (a, b) => [a[0] - b[0], a[1] - b[1]];
const hinge = (A, B, wingSide) => {
  const ax = sub(B, A), len = Math.hypot(...ax);
  const u = [ax[0] / len, ax[1] / len];
  return { A, u, n: [-u[1], u[0]], wingSide };
};
// Left wing hinges along the back/flank, right wing at its shoulder.
const LEFT = hinge([640, 600], [560, 960], +1);
const RIGHT = hinge([900, 680], [935, 800], -1);

// Bend: rotate each point about its foot on the hinge, by an angle that grows
// with distance from the hinge, and foreshorten slightly as the wing swings.
function bend(p, h, angle, reach, squash) {
  const v = sub(p, h.A);
  const along = v[0] * h.u[0] + v[1] * h.u[1];
  const s = (v[0] * h.u[1] - v[1] * h.u[0]) * -1; // signed distance along n
  const sd = s * h.wingSide;
  if (sd <= 0) return p;
  const k = Math.min(sd / reach, 1.25);
  const phi = angle * Math.pow(k, 1.15);
  const off = [s * h.n[0] * (1 - squash * k), s * h.n[1] * (1 - squash * k)];
  const c = Math.cos(phi), si = Math.sin(phi);
  const r = [off[0] * c - off[1] * si, off[0] * si + off[1] * c];
  return [h.A[0] + along * h.u[0] + r[0], h.A[1] + along * h.u[1] + r[1]];
}

// Which wing each path belongs to (by index in the source file).
const LEFT_IDX = new Set([0, 1, 2, 3, 4, 5]);
const RIGHT_IDX = new Set([12, 13, 14]);

function frame(phase) {
  // 0 = wings up (as drawn), 1 = bottom of the downstroke. Downstroke a touch quicker.
  const warped = phase + 0.07 * Math.sin(2 * Math.PI * phase);
  const f = (1 - Math.cos(2 * Math.PI * warped)) / 2;
  const bob = -14 * f; // body lifts as the wings push down
  return paths.map((p, i) => {
    const t = tok(p.d);
    const out = [];
    for (let j = 0; j < t.length; j++) {
      if (/[MCLZ]/.test(t[j])) { out.push(t[j]); continue; }
      let pt = [+t[j], +t[j + 1]]; j++;
      if (LEFT_IDX.has(i)) pt = bend(pt, LEFT, -0.52 * f, 600, 0.1 * f);
      if (RIGHT_IDX.has(i)) pt = bend(pt, RIGHT, 0.75 * f, 300, 0.08 * f);
      pt[1] += bob;
      out.push(pt[0].toFixed(1) + ',' + pt[1].toFixed(1));
    }
    return out.join(' ').replace(/([MCL]) /g, '$1');
  });
}

const frames = Array.from({ length: N + 1 }, (_, k) => frame(k / N));

const eye = src.match(/<circle[^>]+\/>/)[0];
const animBob = (attr) => `<animateTransform attributeName="transform" type="translate" dur="${DUR}s" repeatCount="indefinite" values="${frames.map((_, k) => {
  const ph = k / N; const w = ph + 0.07 * Math.sin(2 * Math.PI * ph);
  return '0 ' + (-14 * (1 - Math.cos(2 * Math.PI * w)) / 2).toFixed(1);
}).join(';')}"/>`;

const body = paths.map((p, i) => {
  const animated = frames.some(fr => fr[i] !== frames[0][i]);
  const anim = animated
    ? `<animate attributeName="d" dur="${DUR}s" repeatCount="indefinite" values="${frames.map(fr => fr[i]).join(';')}"/>`
    : '';
  return `<path d="${frames[0][i]}"${p.attrs}>${anim}</path>`;
});

const header = src.match(/<svg[^>]+>\s*<rect[^>]+\/>/)[0];
// The eye isn't a path, so it rides along with the body bob via a group transform.
const svg = `${header}
${body.join('\n')}
<g>${eye}${animBob()}</g>
</svg>
`;
fs.writeFileSync(OUT, svg);

if (FRAMES_DIR) {
  fs.mkdirSync(FRAMES_DIR, { recursive: true });
  frames.slice(0, N).forEach((fr, k) => {
    const ph = k / N; const w = ph + 0.07 * Math.sin(2 * Math.PI * ph);
    const bob = (-14 * (1 - Math.cos(2 * Math.PI * w)) / 2).toFixed(1);
    const s = `${header}\n${fr.map((d, i) => `<path d="${d}"${paths[i].attrs}/>`).join('\n')}\n<g transform="translate(0 ${bob})">${eye}</g>\n</svg>`;
    fs.writeFileSync(`${FRAMES_DIR}/f${String(k).padStart(2, '0')}.svg`, s);
  });
}
console.log('wrote', OUT, (svg.length / 1024).toFixed(0) + 'KB');
