import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'astro/config';
import react from '@astrojs/react';
import { loadEnv } from 'vite';

// .env files aren't in process.env when this file runs; load them the Vite way.
const env = { ...loadEnv(process.env.NODE_ENV || 'development', process.cwd(), ''), ...process.env };

// Dev proxy target: everything the page fetches same-origin from the device (audio,
// status, controls, the meeting summary, the cert). Default: the real device over
// HTTPS. Its cert is self-signed, so the proxy doesn't verify it (`secure: false`);
// plain http would get redirected to https when the device is online, which a proxy
// can't pass through.
// DEVICE_URL=http://localhost:8080 points it at scripts/mock-device.mjs.
const deviceUrl = (env.DEVICE_URL || 'https://earwing.local').replace(/\/+$/, '');
const proxyTo = { target: deviceUrl, changeOrigin: true, secure: false };

// onnxruntime-web: exactly one copy (package.json "overrides" dedupes the copies pinned by
// parakeet.js and @huggingface/transformers onto ours), and for Parakeet we alias the bare
// import to its extern-wasm build with the WebGPU
// (JSEP) and WASM backends: dist/ort.min.mjs, ~370 KB raw, ~90 KB gzipped. The default
// entry (ort.bundle.min.mjs) contains `new URL('ort-wasm-simd-threaded.jsep.wasm',
// import.meta.url)`, which makes Vite emit a 28 MB .wasm into dist/, far too big for the
// device's 3.4 MB LittleFS partition. The extern build instead loads its runtime
// (ort-wasm-simd-threaded.jsep.mjs + .jsep.wasm) from `ort.env.wasm.wasmPaths`, which the
// worker points at jsDelivr, pinned to this exact installed version (__ORT_VERSION__).
// JSEP rather than ort.webgpu.min.mjs (the newer native WebGPU EP, asyncify build): JSEP
// is what parakeet.js and Keet run fp16 Parakeet on, and its CPU path is the plain
// (non-asyncify) WASM, which matters for the int8 fallback.
//
// The assistant (llm.worker.js) runs on Transformers.js, which imports
// 'onnxruntime-web/webgpu': the native WebGPU EP (C++ kernels inside the asyncify .wasm,
// so its JS is only ~66 KB). The package's default for that subpath is
// ort.webgpu.bundle.min.mjs, which embeds `new URL('ort-wasm-simd-threaded.asyncify.wasm',
// import.meta.url)` (the same 20+ MB problem), so it's aliased to the extern-wasm
// dist/ort.webgpu.min.mjs, and the worker points wasmPaths at the pinned jsDelivr copies
// of ort-wasm-simd-threaded.asyncify.{mjs,wasm}. The two workers each get their own ORT
// build, but both from this one installed version.
// (Its "exports" hide package.json, so read it by path; there is only one copy.)
const ortDir = new URL('./node_modules/onnxruntime-web/', import.meta.url);
const ortVersion = JSON.parse(readFileSync(new URL('package.json', ortDir), 'utf8')).version;
const ortEntry = fileURLToPath(new URL('dist/ort.min.mjs', ortDir));
const ortWebgpuEntry = fileURLToPath(new URL('dist/ort.webgpu.min.mjs', ortDir));
const ortAlias = [
  { find: /^onnxruntime-web$/, replacement: ortEntry },
  { find: /^onnxruntime-web\/webgpu$/, replacement: ortWebgpuEntry },
];

// Short, flat asset names: LittleFS stores each as "<name>.gz", and every file is one
// request to the device's single-threaded web server, so fewer + shorter is better.
const output = {
  entryFileNames: '_astro/[hash].js',
  chunkFileNames: '_astro/[hash].js',
  assetFileNames: '_astro/[hash][extname]',
};

export default defineConfig({
  integrations: [react()],
  build: {
    // Inline the CSS into index.html: one less request to the device.
    inlineStylesheets: 'always',
  },
  vite: {
    define: {
      __ORT_VERSION__: JSON.stringify(ortVersion),
    },
    resolve: { alias: ortAlias },
    // parakeet.js, Transformers.js + onnxruntime-web ship wasm assets — don't pre-bundle
    // with esbuild (that would also bypass the aliases above).
    optimizeDeps: { exclude: ['parakeet.js', 'onnxruntime-web', '@huggingface/transformers'] },
    // Bundle the inference Web Worker as an ES module so it can `import`.
    worker: {
      format: 'es',
      rollupOptions: { output },
    },
    build: {
      // The workers legitimately bundle ORT's JS glue + parakeet.js / Transformers.js
      // (~0.5 MB minified); don't nag about it. pack-device.mjs enforces < 1 MB per file.
      chunkSizeWarningLimit: 600,
    },
    server: {
      // http-proxy pipes responses through as they arrive, so the endless /beam.wav
      // streams unbuffered (and closing the tab's fetch closes the upstream request).
      proxy: {
        '^/(beam|raw)\\.wav': proxyTo,
        '^/status(\\.json)?$': proxyTo,
        '^/control': proxyTo,
        '^/meeting(\\.json)?$': proxyTo, // the live summary for the device's Meeting tab
        '^/earwing\\.crt$': proxyTo,
      },
    },
    // Cross-origin isolation so onnxruntime-web can use threaded WASM (SharedArrayBuffer),
    // which makes CPU transcription ~3-4x faster. Astro's dev server serves HTML through its
    // own middleware (bypassing vite `server.headers`), so set the headers via middleware.
    // The firmware sends the same headers; browsers honour them on https://earwing.local
    // (a secure context) but not on plain http (see README).
    plugins: [
      {
        // Client build only: Astro's SSR/prerender pass must keep its own chunk names,
        // or its server chunks land in dist/_astro and are never cleaned up.
        name: 'short-client-asset-names',
        config(_config, { isSsrBuild }) {
          if (!isSsrBuild) return { build: { rollupOptions: { output } } };
          return undefined;
        },
      },
      {
        name: 'cross-origin-isolation-dev',
        configureServer(server) {
          server.middlewares.use((_req, res, next) => {
            res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
            res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
            next();
          });
        },
      },
    ],
  },
});
