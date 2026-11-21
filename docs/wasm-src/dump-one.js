// Writes the wasm preparer's cache to a folder (for byte-level diffs).  node dump-one.js <lsfgprep.js> <dll> <outdir>
'use strict';
const fs = require('fs'), path = require('path');
const [, , modPath, dllPath, outDir] = process.argv;
(async () => {
  const M = await require(path.resolve(modPath))({ print: () => {}, printErr: () => {} });
  M.FS.writeFile('/Lossless.dll', fs.readFileSync(dllPath));
  for (const [label, perf] of [['Performance', 1], ['Quality', 0]]) {
    M.FS.mkdirTree('/out/' + label); fs.mkdirSync(path.join(outDir, label), { recursive: true });
    if (M.ccall('lsfgprep_run', 'number', ['string', 'string', 'number'], ['/Lossless.dll', '/out/' + label, perf])) throw new Error('fail');
    for (const n of M.FS.readdir('/out/' + label)) if (n[0] !== '.') fs.writeFileSync(path.join(outDir, label, n), M.FS.readFile('/out/' + label + '/' + n));
  }
})();
