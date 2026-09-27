// The two Parakeet TDT 0.6B v3 builds the app can run, and their exact files.
// Shared by the inference worker, the local-folder loader (localModels.js) and
// scripts/download-models.mjs, so it must stay plain data (no Vite/browser APIs).
//
//   webgpu  fp16 encoder on WebGPU + int8 decoder on WASM   ≈ 1.26 GB
//   wasm    int8 encoder + int8 decoder on WASM (CPU)       ≈ 933 MB
//
// `dir` is the folder name for that repo inside a local models folder
// (<repo-root>/models/<dir>/<file>), as written by `npm run models`.

export const VARIANTS = {
  webgpu: {
    backend: 'webgpu',
    quant: 'fp16',
    repo: 'ysdede/parakeet-tdt-0.6b-v3-onnx',
    dir: 'ysdede--parakeet-tdt-0.6b-v3-onnx',
    // role → { name, size }
    files: {
      encoder: { name: 'encoder-model.fp16.onnx', size: 1_238_960_452 },
      decoder: { name: 'decoder_joint-model.int8.onnx', size: 18_202_004 },
      vocab: { name: 'vocab.txt', size: 102_132 },
    },
  },
  wasm: {
    backend: 'wasm',
    quant: 'int8',
    repo: 'nasedkinpv/parakeet-tdt-0.6b-v3-onnx-int8',
    dir: 'nasedkinpv--parakeet-tdt-0.6b-v3-onnx-int8',
    files: {
      encoder: { name: 'encoder-int8.onnx', size: 1_443_183 },
      encoderData: { name: 'encoder-int8.onnx.data', size: 878_346_240 },
      decoder: { name: 'decoder_joint-int8.onnx', size: 53_592_358 },
      vocab: { name: 'vocab.txt', size: 93_939 },
    },
  },
};

export const variantBytes = (v) => Object.values(v.files).reduce((n, f) => n + f.size, 0);

// Speaker diarization (diarizer.js): pyannote segmentation-3.0 and the WeSpeaker
// ResNet34-LM embedding pyannote 3.1 pairs with it. Small enough to download
// every time the IndexedDB cache is empty. `name` may include a folder in the repo.
export const DIARIZATION = {
  segmentation: {
    repo: 'onnx-community/pyannote-segmentation-3.0',
    dir: 'onnx-community--pyannote-segmentation-3.0',
    name: 'onnx/model.onnx',
    size: 5_986_908,
  },
  embedding: {
    repo: 'Wespeaker/wespeaker-voxceleb-resnet34-LM',
    dir: 'Wespeaker--wespeaker-voxceleb-resnet34-LM',
    name: 'voxceleb_resnet34_LM.onnx',
    size: 26_530_309,
  },
};
