# earwing launch film

Source for [`docs/earwing.mp4`](../earwing.mp4), a 30-second product film:
1920×1080, 60 fps, with an original score. It's a [HyperFrames](https://github.com/heygen-com/hyperframes)
composition: HTML and a GSAP timeline, rendered frame by frame in headless Chrome.

| Time | Scene | What it shows |
|---|---|---|
| 0–3.75 s | Reveal | The enclosure from `3dprint/micarray.scad`, lit in three.js. The TFT shows the firmware's DOA dial. |
| 3.75–7.5 s | Four microphones | Native OpenSCAD renders, cut on the four plucks. Then a macro shot and a crane to top-down. |
| 7.5–15 s | Beamforming | Wavefronts reach each mic 50 µs apart (30 mm · sin 35° / c). The traces are aligned and summed: the voice adds up, and uncorrelated noise drops by up to 6 dB. Then the real 4-mic delay-and-sum pattern (4 kHz) steers to follow the talker. |
| 15–20.6 s | Private | Beam audio streams over local Wi-Fi to a browser tab running Parakeet TDT 0.6B on WebGPU. No audio is uploaded. |
| 20.6–26.25 s | Who said what | A speaker-labelled transcript and speaker lanes. Renaming Speaker 2 to "Kate" saves that voice print on the device. |
| 26.25–30 s | End card | "Hears everyone. Shares nothing." |

The edit is cut to a 128 BPM grid: 16 bars in exactly 30 s, with scene changes on downbeats.

## Rebuild

```sh
cd docs/video
npm install
npm run models    # OpenSCAD → assets/models/*.stl (needs openscad on PATH)
bash scripts/render-stills.sh   # native OpenSCAD stills → assets/renders/
npm run score     # numpy synth + bundled SFX → assets/audio/score.wav
npm run check     # lint / layout / contrast checks
npm run render    # → ../earwing.mp4
```

`npx hyperframes preview` opens the Studio for scrubbing. The STL exports are gitignored,
because `*.stl` is ignored repo-wide. Run `npm run models` before previewing or rendering.

## Files

```
index.html          composition: markup, styles, and the GSAP timeline for all DOM motion
src/scene3d.js      three.js product scene: STL meshes, studio light, TFT texture, camera shots
src/schematic.js    beamforming explainer (canvas): wavefronts, traces, array-factor beam
src/main.js         wires both to HyperFrames time (hf-seek), plus per-frame 3D-anchored overlays
src/util.js         easing, seeded noise
scripts/            model export, OpenSCAD stills, score synthesis
assets/             fonts (OFL: Inter, Inter Tight, JetBrains Mono), renders, audio, grain
```

Everything visual is a pure function of film time, so frames can be rendered in any order
across parallel workers.
