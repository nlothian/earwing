// The product, in three.js: the meshes exported from 3dprint/micarray.scad
// (scripts/export-models.sh), lit like a studio product shot. Everything is a
// pure function of film time so any frame can be rendered in any order.
//
// World units are millimetres, Z up, exactly as in the SCAD file: the four mic
// grills sit on a row along X at y = 0 (x = -45, -15, 15, 45) and face up; the
// display box runs back to y = 73. People face the device from -Y, the mic
// side, where M1 is on the left and the screen reads the right way up.

import * as THREE from "three";
import { STLLoader } from "three/addons/loaders/STLLoader.js";
import { toCreasedNormals } from "three/addons/utils/BufferGeometryUtils.js";
import { RectAreaLightUniformsLib } from "three/addons/lights/RectAreaLightUniformsLib.js";
import { EffectComposer } from "three/addons/postprocessing/EffectComposer.js";
import { RenderPass } from "three/addons/postprocessing/RenderPass.js";
import { UnrealBloomPass } from "three/addons/postprocessing/UnrealBloomPass.js";
import { OutputPass } from "three/addons/postprocessing/OutputPass.js";
import { BokehPass } from "three/addons/postprocessing/BokehPass.js";
import { clamp, ease, lerp, mix, seg, smooth } from "./util.js";

const W = 1920;
const H = 1080;
export const MIC_X = [-45, -15, 15, 45];
const GRILL_Z = 10.8;

// Screen glass top face (measured from assets/models/screen.stl).
const SCREEN = {
  center: new THREE.Vector3(2.25, 43.103, 4.66),
  size: [57.2, 42.8],
  normal: new THREE.Vector3(0, 0.11333, 0.99356),
  // seen from the mic side (-Y): image up runs away from the viewer, right is +x
  up: new THREE.Vector3(0, 0.99356, -0.11333),
  right: new THREE.Vector3(1, 0, 0),
};


// ---------------------------------------------------------------- shots ----

function orbit(target, R, azDeg, elDeg) {
  const az = THREE.MathUtils.degToRad(azDeg);
  const el = THREE.MathUtils.degToRad(elDeg);
  return new THREE.Vector3(
    target.x + R * Math.cos(el) * Math.sin(az),
    target.y + R * Math.cos(el) * Math.cos(az),
    target.z + R * Math.sin(el),
  );
}

const HERO_T = new THREE.Vector3(0, 26, -2);
const UP_Z = new THREE.Vector3(0, 0, 1);
const UP_TOPDOWN = new THREE.Vector3(0, -1, 0);
// Top-down framing that the schematic (S3) picks up: mic row 240 px below the
// frame centre at 5 px/mm.
export const TOPDOWN = { target: new THREE.Vector3(0, -48, GRILL_Z), height: 612, fov: 20 };

// Each shot returns camera pose + light levels for a film time t.
function shotAt(t) {
  if (t < 3.75) {
    // A · reveal: slow orbit in from the right while the light finds it.
    const u = ease.inOutSine(seg(t, 0, 3.75));
    return {
      target: HERO_T,
      pos: orbit(HERO_T, lerp(410, 330, u), lerp(222, 202, u), lerp(15, 23, u)),
      up: UP_Z,
      fov: 26,
      offY: 150,
      key: ease.inOutSine(seg(t, 0.15, 2.4)),
      sweep: seg(t, 0.2, 3.2),
      screen: ease.outCubic(seg(t, 1.5, 2.4)),
    };
  }
  if (t < 8.1) {
    // B · macro down the row of grills, then crane up to top-down.
    const dolly = ease.outCubic(seg(t, 5.625, 6.6));
    const crane = ease.inOutCubic(seg(t, 6.2, 7.5));
    const tgtMacro = new THREE.Vector3(lerp(-36, -14, dolly), 4, 9.5);
    const azMacro = lerp(-152, -140, dolly);
    const elMacro = lerp(17, 22, dolly);
    const posMacro = orbit(tgtMacro, lerp(158, 142, dolly), azMacro, elMacro);
    const posTop = new THREE.Vector3(TOPDOWN.target.x, TOPDOWN.target.y + 0.001, TOPDOWN.target.z + TOPDOWN.height);
    // crane along an orbit around a moving target so the motion arcs
    const tgt = new THREE.Vector3().lerpVectors(tgtMacro, TOPDOWN.target, crane);
    const R = lerp(posMacro.distanceTo(tgtMacro), TOPDOWN.height, crane);
    // swing round to the back while rising, so the top-down view has the
    // mic side (where people are) at the top of frame, like the schematic
    const az = lerp(azMacro, 0, crane);
    const el = lerp(elMacro, 89.9, Math.pow(crane, 1.25));
    const pos = crane >= 1 ? posTop : orbit(tgt, R, az, el);
    const up = new THREE.Vector3().lerpVectors(UP_Z, UP_TOPDOWN, smooth(0.55, 1, crane)).normalize();
    return {
      dof: 1 - smooth(0, 0.35, crane),
      target: tgt,
      pos,
      up,
      fov: lerp(30, TOPDOWN.fov, crane),
      offY: lerp(-70, 0, crane),
      key: 1,
      sweep: -1,
      screen: 1 - 0.85 * crane,
    };
  }
  if (t < 22) {
    // C · the device, left of frame, next to the browser.
    const u = ease.inOutSine(seg(t, 15, 20.8));
    return {
      target: HERO_T,
      pos: orbit(HERO_T, lerp(500, 470, u), lerp(203, 191, u), lerp(30, 25, u)),
      up: UP_Z,
      fov: 26,
      offX: 570,
      offY: 10,
      key: 1,
      sweep: -1,
      screen: 1,
    };
  }
  // D · end card: sweep across the front and pull back.
  const u = ease.outCubic(seg(t, 26.25, 30));
  return {
    target: HERO_T,
    pos: orbit(HERO_T, lerp(262, 318, u), lerp(158, 192, u), lerp(11, 21, u)),
    up: UP_Z,
    fov: 26,
    offY: 150,
    key: ease.outCubic(seg(t, 26.25, 26.7)),
    sweep: seg(t, 26.3, 28.6),
    screen: ease.outCubic(seg(t, 26.4, 27.0)),
  };
}

// DOA (degrees, + toward M4) the device's screen shows at time t.
export function doaAt(t) {
  if (t < 12) return 21 + 6 * Math.sin(t * 0.9);
  if (t < 26) {
    const a = mix(35, -25, ease.inOutCubic(seg(t, 18.4, 19.1)));
    return a + 1.5 * Math.sin(t * 2.1);
  }
  return 12 + 4 * Math.sin(t * 0.8);
}

// ------------------------------------------------------------ the TFT ------

function makeTft() {
  const c = document.createElement("canvas");
  c.width = 640;
  c.height = 480;
  const g = c.getContext("2d");
  const tex = new THREE.CanvasTexture(c);
  tex.colorSpace = THREE.SRGBColorSpace;
  tex.anisotropy = 8;
  const CYAN = "#34e3f0";
  const GREY = "#6f7680";

  function levels(t, ch) {
    // speech-like: syllable bursts, a little different per mic
    const syl = Math.max(0, Math.sin(t * 5.3 + ch * 0.35)) * Math.max(0, Math.sin(t * 1.7 + 0.8));
    return clamp(0.3 + 0.62 * Math.sqrt(syl) + 0.06 * Math.sin(t * 23 + ch * 2.1), 0, 1);
  }

  function draw(t) {
    const s = 2; // canvas is 2x the 320x240 panel
    g.setTransform(1, 0, 0, 1, 0, 0);
    g.fillStyle = "#000";
    g.fillRect(0, 0, 640, 480);
    g.setTransform(s, 0, 0, s, 0, 0);
    g.textBaseline = "top";
    // Wi-Fi block (top left)
    g.fillStyle = "#e9eef2";
    g.font = "600 21px Inter, sans-serif";
    g.fillText("earwing.local", 12, 26);
    g.fillStyle = GREY;
    g.font = "500 12px 'JetBrains Mono', monospace";
    g.fillText("https · 2 listeners", 12, 58);
    g.fillText("192.168.1.42", 12, 74);
    // mode (top right)
    g.textAlign = "right";
    g.fillStyle = CYAN;
    g.font = "600 12px 'JetBrains Mono', monospace";
    g.fillText("AUTO_BEAM", 308, 6);
    g.textAlign = "left";
    // source button
    g.strokeStyle = CYAN;
    g.lineWidth = 2;
    roundRect(g, 214, 30, 100, 54, 6);
    g.stroke();
    g.fillStyle = CYAN;
    g.textAlign = "center";
    g.font = "500 11px 'JetBrains Mono', monospace";
    g.fillText("MIC", 264, 37);
    g.font = "700 19px Inter, sans-serif";
    g.fillText("ARRAY", 264, 54);
    g.textAlign = "left";
    // DOA dial
    const cx = 110, cy = 212, r = 88;
    g.strokeStyle = GREY;
    g.lineWidth = 2;
    g.beginPath();
    g.arc(cx, cy, r - 1, Math.PI, 2 * Math.PI);
    g.stroke();
    for (let a = -90; a <= 90; a += 30) {
      const p0 = dial(cx, cy, a, r - 9), p1 = dial(cx, cy, a, r - 3);
      g.beginPath();
      g.moveTo(p0[0], p0[1]);
      g.lineTo(p1[0], p1[1]);
      g.stroke();
    }
    g.fillStyle = GREY;
    g.font = "500 12px 'JetBrains Mono', monospace";
    g.textBaseline = "bottom";
    g.fillText("M1", cx - r + 10, cy - 4);
    g.textAlign = "right";
    g.fillText("M4", cx + r - 10, cy - 4);
    g.textAlign = "left";
    g.textBaseline = "top";
    const doa = doaAt(t);
    const tip = dial(cx, cy, doa, r - 14);
    g.strokeStyle = CYAN;
    g.lineWidth = 3;
    g.lineCap = "round";
    g.shadowColor = CYAN;
    g.shadowBlur = 8;
    g.beginPath();
    g.moveTo(cx, cy);
    g.lineTo(tip[0], tip[1]);
    g.stroke();
    const a0 = dial(cx, cy, doa, r + 2), a1 = dial(cx, cy, doa - 4, r + 11), a2 = dial(cx, cy, doa + 4, r + 11);
    g.fillStyle = CYAN;
    g.beginPath();
    g.moveTo(a0[0], a0[1]);
    g.lineTo(a1[0], a1[1]);
    g.lineTo(a2[0], a2[1]);
    g.closePath();
    g.fill();
    g.shadowBlur = 0;
    g.beginPath();
    g.arc(cx, cy, 4, 0, Math.PI * 2);
    g.fill();
    // readout line, under the Wi-Fi block
    g.fillStyle = "#e9eef2";
    g.font = "600 13px 'JetBrains Mono', monospace";
    g.fillText(`DOA ${doa >= 0 ? "+" : "−"}${Math.abs(Math.round(doa))}°  beam locked`, 12, 96);
    // level bars
    for (let ch = 0; ch < 4; ch++) {
      const x = 230 + ch * 22, top = 118, bot = 202;
      g.fillStyle = "#15191d";
      g.fillRect(x, top, 14, bot - top);
      const h = Math.round(levels(t, ch) * (bot - top));
      const grd = g.createLinearGradient(0, bot, 0, top);
      grd.addColorStop(0, "#1fa8b4");
      grd.addColorStop(1, CYAN);
      g.fillStyle = grd;
      g.fillRect(x, bot - h, 14, h);
      g.fillStyle = GREY;
      g.textAlign = "center";
      g.font = "500 12px 'JetBrains Mono', monospace";
      g.fillText(String(ch + 1), x + 7, 206);
      g.textAlign = "left";
    }
    tex.needsUpdate = true;
  }
  return { tex, draw };
}

function dial(cx, cy, deg, r) {
  const a = THREE.MathUtils.degToRad(deg);
  return [cx + r * Math.sin(a), cy - r * Math.cos(a)];
}

function roundRect(g, x, y, w, h, r) {
  g.beginPath();
  g.moveTo(x + r, y);
  g.arcTo(x + w, y, x + w, y + h, r);
  g.arcTo(x + w, y + h, x, y + h, r);
  g.arcTo(x, y + h, x, y, r);
  g.arcTo(x, y, x + w, y, r);
  g.closePath();
}

// ------------------------------------------------------------ studio env ---

function studioEnvironment(renderer) {
  const env = new THREE.Scene();
  env.background = new THREE.Color(0x000000);
  const panel = (w, h, level, pos, tint = 0xffffff) => {
    const c = new THREE.Color(tint).multiplyScalar(level);
    const m = new THREE.Mesh(new THREE.PlaneGeometry(w, h), new THREE.MeshBasicMaterial({ color: c, side: THREE.DoubleSide }));
    m.position.copy(pos);
    m.lookAt(0, 0, 0);
    env.add(m);
  };
  // Z-up world: a big soft box overhead, a warm key strip left, a cool rim strip behind.
  panel(9, 6, 2.2, new THREE.Vector3(0, -1.5, 7));
  panel(3, 8, 3.0, new THREE.Vector3(-7, -4, 3), 0xfff1e0);
  panel(10, 1.4, 4.0, new THREE.Vector3(0, 8, 3.5), 0xe8f0ff);
  panel(2, 6, 1.4, new THREE.Vector3(7, -3, 2));
  const pm = new THREE.PMREMGenerator(renderer);
  const rt = pm.fromScene(env, 0.02);
  pm.dispose();
  return rt.texture;
}

// ---------------------------------------------------------------- build ----

export function createScene3d(canvas) {
  RectAreaLightUniformsLib.init();
  const renderer = new THREE.WebGLRenderer({ canvas, antialias: false, preserveDrawingBuffer: true, powerPreference: "high-performance" });
  renderer.setPixelRatio(1);
  renderer.setSize(W, H, false);
  renderer.toneMapping = THREE.NeutralToneMapping;
  renderer.toneMappingExposure = 1.0;
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  renderer.shadowMap.enabled = true;
  renderer.shadowMap.type = THREE.VSMShadowMap;

  const scene = new THREE.Scene();
  scene.background = new THREE.Color("#070708");
  scene.fog = new THREE.Fog("#070708", 420, 1500);
  scene.environment = studioEnvironment(renderer);

  const camera = new THREE.PerspectiveCamera(26, W / H, 5, 5000);

  // lights
  const key = new THREE.SpotLight("#fff2e2", 0, 0, 0.55, 1, 0);
  key.position.set(-230, -260, 430);
  key.target.position.set(0, 25, -12);
  key.castShadow = true;
  key.shadow.mapSize.set(2048, 2048);
  key.shadow.radius = 14;
  key.shadow.blurSamples = 24;
  key.shadow.bias = -0.0004;
  key.shadow.camera.near = 200;
  key.shadow.camera.far = 1200;
  scene.add(key, key.target);

  const rim = new THREE.RectAreaLight("#dfe8ff", 0, 320, 40);
  rim.position.set(0, 300, 140);
  rim.lookAt(0, 25, 0);
  scene.add(rim);

  const top = new THREE.RectAreaLight("#fff8ef", 0, 420, 220);
  top.position.set(20, 10, 380);
  top.lookAt(0, 30, 0);
  scene.add(top);

  const sweep = new THREE.RectAreaLight("#ffffff", 0, 60, 380);
  scene.add(sweep);

  const fill = new THREE.HemisphereLight("#fff6ea", "#1a1612", 0);
  scene.add(fill);

  // floor: catches the key light's pool and the soft shadow
  const floor = new THREE.Mesh(
    new THREE.PlaneGeometry(6000, 6000),
    new THREE.MeshStandardMaterial({ color: "#1a1816", roughness: 0.92, metalness: 0 }),
  );
  floor.position.z = -16.25;
  floor.receiveShadow = true;
  scene.add(floor);

  // materials
  const beige = new THREE.MeshPhysicalMaterial({
    color: "#d6c7ac",
    roughness: 0.55,
    metalness: 0,
    sheen: 0.35,
    sheenRoughness: 0.7,
    sheenColor: new THREE.Color("#fff1dc"),
    clearcoat: 0.06,
    clearcoatRoughness: 0.5,
  });
  const grillMat = () =>
    new THREE.MeshPhysicalMaterial({
      color: "#141414",
      roughness: 0.4,
      metalness: 0.25,
      clearcoat: 0.3,
      clearcoatRoughness: 0.35,
      emissive: new THREE.Color("#ff7a2e"),
      emissiveIntensity: 0,
    });
  const glassBase = new THREE.MeshPhysicalMaterial({ color: "#050506", roughness: 0.2, metalness: 0 });

  const device = new THREE.Group();
  scene.add(device);

  const tft = makeTft();
  const screenMat = new THREE.MeshPhysicalMaterial({
    color: "#000000",
    roughness: 0.06,
    metalness: 0,
    clearcoat: 1,
    clearcoatRoughness: 0.03,
    emissive: new THREE.Color("#ffffff"),
    emissiveMap: tft.tex,
    emissiveIntensity: 1,
  });
  const screen = new THREE.Mesh(new THREE.PlaneGeometry(SCREEN.size[0] - 0.4, SCREEN.size[1] - 0.4), screenMat);
  const basis = new THREE.Matrix4().makeBasis(SCREEN.right, SCREEN.up, SCREEN.normal);
  screen.quaternion.setFromRotationMatrix(basis);
  screen.position.copy(SCREEN.center);
  device.add(screen);

  // mic "ping" rings: glow on the beige plate around each grill
  const rings = MIC_X.map((x) => {
    const m = new THREE.Mesh(
      new THREE.RingGeometry(12.4, 13.1, 128),
      new THREE.MeshBasicMaterial({ color: new THREE.Color(3.2, 1.0, 0.24), transparent: true, opacity: 0, blending: THREE.AdditiveBlending, depthWrite: false }),
    );
    m.position.set(x, 0, 8.86);
    device.add(m);
    return m;
  });
  const grills = [];

  const loader = new STLLoader();
  const load = (url) => new Promise((res, rej) => loader.load(url, res, undefined, rej));
  const prep = (geo, crease = 32) => {
    const g = toCreasedNormals(geo, THREE.MathUtils.degToRad(crease));
    g.computeBoundingSphere();
    return g;
  };

  const ready = Promise.all([
    load("assets/models/body.stl"),
    load("assets/models/floor.stl"),
    load("assets/models/mount.stl"),
    load("assets/models/screen.stl"),
  ]).then(([body, flr, mount, glass]) => {
    const add = (geo, mat) => {
      const m = new THREE.Mesh(geo, mat);
      m.castShadow = true;
      m.receiveShadow = true;
      device.add(m);
      return m;
    };
    add(prep(body), beige);
    add(prep(flr), beige);
    add(prep(glass, 20), glassBase);
    const mg = prep(mount, 40);
    for (const x of MIC_X) {
      const m = add(mg, grillMat());
      m.position.x = x;
      grills.push(m);
    }
    renderer.compile(scene, camera);
  });

  // post: bloom on the screen and the ping rings only (threshold above white)
  const rt = new THREE.WebGLRenderTarget(W, H, { samples: 4, type: THREE.HalfFloatType });
  const composer = new EffectComposer(renderer, rt);
  composer.setPixelRatio(1);
  composer.setSize(W, H);
  composer.addPass(new RenderPass(scene, camera));
  // shallow depth of field for the macro shot only
  const bokeh = new BokehPass(scene, camera, { focus: 150, aperture: 0.00002, maxblur: 0.006 });
  bokeh.enabled = false;
  composer.addPass(bokeh);
  const bloom = new UnrealBloomPass(new THREE.Vector2(W, H), 0.32, 0.45, 1.25);
  composer.addPass(bloom);
  composer.addPass(new OutputPass());

  const tmp = new THREE.Vector3();
  function project(x, y, z) {
    // setViewOffset is baked into the projection matrix, so NDC maps to the canvas
    tmp.set(x, y, z).project(camera);
    return { x: ((tmp.x + 1) / 2) * W, y: ((1 - tmp.y) / 2) * H, z: tmp.z };
  }

  function pose(t) {
    const s = shotAt(t);
    camera.fov = s.fov;
    camera.up.copy(s.up);
    camera.position.copy(s.pos);
    camera.lookAt(s.target);
    if (s.offX || s.offY) camera.setViewOffset(W, H, s.offX || 0, s.offY || 0, W, H);
    else camera.clearViewOffset();
    camera.updateProjectionMatrix();
    return s;
  }

  function render(t) {
    const s = pose(t);

    key.intensity = 1.5 * s.key;
    top.intensity = 0.55 * s.key;
    rim.intensity = 3.2 * Math.max(s.key, 0.35 * s.sweep);
    fill.intensity = 0.12 * s.key;
    scene.environmentIntensity = 0.06 + 0.3 * s.key;

    if (s.sweep >= 0 && s.sweep <= 1) {
      const u = ease.inOutSine(s.sweep);
      sweep.intensity = 9 * Math.sin(Math.PI * u);
      sweep.position.set(lerp(-420, 420, u), -170, 180);
      sweep.lookAt(0, 20, 0);
    } else sweep.intensity = 0;

    tft.draw(t);
    screenMat.emissiveIntensity = 1.15 * s.screen;

    // the four counted mics stay ringed into the macro, then fade on the crane
    const ringOn = t >= 5.625 && t < 8 ? 0.5 * Math.exp(-(t - 5.625) / 0.4) + 0.3 * (1 - seg(t, 6.2, 7.0)) : 0;
    for (const r of rings) r.material.opacity = clamp(ringOn, 0, 1);

    bokeh.enabled = !!s.dof && s.dof > 0.01;
    if (bokeh.enabled) {
      bokeh.uniforms.focus.value = camera.position.distanceTo(s.target);
      bokeh.uniforms.aperture.value = 0.000022 * s.dof;
      bokeh.uniforms.maxblur.value = 0.006 * s.dof;
    }
    composer.render();
  }

  return { ready, render, project, pose };
}
