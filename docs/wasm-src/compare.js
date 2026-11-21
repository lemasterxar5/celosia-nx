// Byte comparison: the WebAssembly preparer (node build, same calls as js/lsfg-worker.js) against the cache
// lsfgprep.exe made (the one the game uses).  node compare.js <build>/lsfgprep.js <Lossless.dll> <reference cache dir>
'use strict';
const fs = require('fs'), path = require('path'), crypto = require('crypto');
const [, , modPath, dllPath, refDir] = process.argv;
(async () => {
  const createLsfgPrep = require(path.resolve(modPath));
  const M = await createLsfgPrep({ print: () => {}, printErr: m => process.stderr.write(m + '\n') });
  M.FS.writeFile('/Lossless.dll', fs.readFileSync(dllPath));
  let same = 0, diff = 0;
  for (const [label, perf] of [['Performance', 1], ['Quality', 0]]) {
    const out = '/out/' + label;
    M.FS.mkdirTree(out);
    const t0 = Date.now();
    const rc = M.ccall('lsfgprep_run', 'number', ['string', 'string', 'number'], ['/Lossless.dll', out, perf]);
    if (rc) { console.log(label, 'FAILED', rc, M.UTF8ToString(M.ccall('lsfgprep_error', 'number', [], []))); process.exit(1); }
    const mine = M.FS.readdir(out).filter(n => n !== '.' && n !== '..').sort();
    const ref = fs.readdirSync(path.join(refDir, label)).filter(n => !n.endsWith('.txt')).sort();
    console.log(label, 'files wasm', mine.length, 'ref', ref.length, 'in', ((Date.now() - t0) / 1000).toFixed(1), 's');
    for (const n of new Set([...mine, ...ref])) {
      const a = mine.includes(n) ? Buffer.from(M.FS.readFile(out + '/' + n)) : null;
      const b = ref.includes(n) ? fs.readFileSync(path.join(refDir, label, n)) : null;
      if (a && b && a.equals(b)) same++;
      else { diff++; console.log('  DIFF', label + '/' + n, a ? a.length : '-', b ? b.length : '-'); }
    }
  }
  console.log('identical', same, 'different', diff);
  console.log(diff ? 'RESULT: NOT THE SAME' : 'RESULT: BYTE-IDENTICAL');
})();
