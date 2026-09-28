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

// The in-browser assistant's language models (llm.worker.js), run by Transformers.js on
// the WebGPU execution provider, both LiquidAI LFM2.5 (hybrid conv + attention) in the
// q4f16 export (q4 weights, fp16 activations and KV cache: WebGPU only):
//
//   large  LFM2.5-2.6B            reasoning model (the worker skips its thinking)  ≈ 1.55 GB
//   small  LFM2.5-1.2B-Instruct   for phones and low-memory machines               ≈ 0.77 GB
//
// `files` are the repo-relative names Transformers.js requests, with exact sizes; the
// weights are the external-data chunks (`use_external_data_format` in config.json).
// generation_config.json is optional to Transformers.js but tiny, so it's required
// here: a local folder then behaves exactly like the Hugging Face copy.
export const LLMS = {
  large: {
    key: 'large',
    label: 'LFM2.5 2.6B',
    repo: 'LiquidAI/LFM2.5-2.6B-ONNX',
    dir: 'LiquidAI--LFM2.5-2.6B-ONNX',
    dtype: 'q4f16',
    thinking: true, // the template opens <think> for every answer
    files: {
      'config.json': 1_728,
      'generation_config.json': 146,
      'tokenizer.json': 17_905_598,
      'tokenizer_config.json': 6_063,
      'onnx/model_q4f16.onnx': 222_160,
      'onnx/model_q4f16.onnx_data': 1_063_972_864,
      'onnx/model_q4f16.onnx_data_1': 469_958_656,
    },
  },
  small: {
    key: 'small',
    label: 'LFM2.5 1.2B',
    repo: 'LiquidAI/LFM2.5-1.2B-Instruct-ONNX',
    dir: 'LiquidAI--LFM2.5-1.2B-Instruct-ONNX',
    dtype: 'q4f16',
    thinking: false,
    files: {
      'config.json': 1_599,
      'generation_config.json': 131,
      'tokenizer.json': 3_297_799,
      'tokenizer_config.json': 2_391,
      'onnx/model_q4f16.onnx': 182_795,
      'onnx/model_q4f16.onnx_data': 760_279_040,
    },
  },
};

export const llmBytes = (m) => Object.values(m.files).reduce((n, s) => n + s, 0);
