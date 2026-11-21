// Runs the page's own worker code (cuerpoTrabajador from js/app.js) on wasm/lsfgprep-bundle.js in a worker-like
// context, with a Lossless.dll, and compares the result with a reference cache (padding/CRC-aware for manifest.bin).
//   node test-bundle.js <instalador dir> <Lossless.dll> <reference cache dir>
'use strict';
const fs = require('fs'), path = require('path'), vm = require('vm');
const [, , dir, dllPath, refDir] = process.argv;
const app = fs.readFileSync(path.join(dir, 'js/app.js'), 'utf8');
const body = app.slice(app.indexOf('function cuerpoTrabajador()'), app.indexOf('function crearTrabajador()'));
const bundleCtx = { self: {} };
vm.runInNewContext(fs.readFileSync(path.join(dir, 'wasm/lsfgprep-bundle.js'), 'utf8'), bundleCtx);
const { js, wasm } = bundleCtx.self.LSFGPREP;
let resolveDone;
const done = new Promise(r => { resolveDone = r; });
const ctx = {
  console, WebAssembly, TextDecoder, TextEncoder, URL, setTimeout, clearTimeout, performance,
  importScripts() {}, location: { href: 'blob:null/x' },
  postMessage(m) { if (m.type === 'step') console.log(' ', m.text); if (m.type === 'done' || m.type === 'error') resolveDone(m); },
};
ctx.self = ctx; ctx.globalThis = ctx;
vm.createContext(ctx);
vm.runInContext(js + '\n;' + body + '\n;cuerpoTrabajador();', ctx);
const dll = fs.readFileSync(dllPath);
const wasmBytes = Buffer.from(wasm, 'base64');
const t0 = Date.now();
ctx.onmessage({ data: { dll: dll.buffer.slice(dll.byteOffset, dll.byteOffset + dll.length), wasm: wasmBytes.buffer.slice(wasmBytes.byteOffset, wasmBytes.byteOffset + wasmBytes.length) } });
done.then(m => {
  if (m.type === 'error') { console.log('ERROR', m.text); process.exit(1); }
  let same = 0, other = [];
  for (const label of ['Performance', 'Quality']) for (const f of m.result[label]) {
    const ref = fs.readFileSync(path.join(refDir, label, f.name)), a = Buffer.from(f.data);
    if (a.equals(ref)) { same++; continue; }
    if (f.name === 'manifest.bin' && a.length === ref.length) {
      const bad = [];
      for (let i = 0; i < a.length; i++) if (a[i] !== ref[i] && !(i >= 160 && i < 164) && !(i >= 168 && (i - 168) % 96 >= 92)) bad.push(i);
      if (!bad.length) { same++; console.log('  ' + label + '/manifest.bin: same except PassEntry padding + its CRC'); continue; }
    }
    other.push(label + '/' + f.name);
  }
  console.log('files', same + other.length, 'same', same, 'different', other.length, other.join(' '), 'in', ((Date.now() - t0) / 1000).toFixed(1) + 's');
  console.log(other.length ? 'RESULT: DIFFERENT' : 'RESULT: SAME AS THE GAME CACHE');
});
