// In-browser ISO9660 reader (adapted from celosia nx tools/switch/iso9660.mjs).
// Reads slices of the local file with File.slice: the ISO never leaves the computer.
'use strict';

const ISO_BLOCK = 2048;
const ISO_REQUIRED = ['SYSTEM.CNF', 'SCUS_974.72', 'KERNEL.XFF', 'MANAGER.XFF', 'GAMECORE.XFF', 'NICO.DAT'];
// SHA-256 of SCUS_974.72, Shadow of the Colossus NTSC-U (SCUS-97472), from a copy that runs on the port.
const ISO_SCUS_SHA256 = 'd16ad76ba8cdf87695d94b1316d51c1f248959ec0749814c31bb7a2d55d9a2e1';

async function isoRead(file, offset, length) {
  if (offset < 0 || length < 0 || offset + length > file.size) throw new Error('The ISO is truncated or damaged.');
  return new Uint8Array(await file.slice(offset, offset + length).arrayBuffer());
}
function isoP32(b, o) {
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const le = v.getUint32(o, true);
  if (le !== v.getUint32(o + 4, false)) throw new Error('Inconsistent ISO structure.');
  return le;
}
function isoP16(b, o) {
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const le = v.getUint16(o, true);
  if (le !== v.getUint16(o + 2, false)) throw new Error('Inconsistent ISO structure.');
  return le;
}
function isoAscii(b, from, to) { return String.fromCharCode(...b.subarray(from, to)); }

function isoEntry(r, limit) {
  if (r.length < 34 || r[0] !== r.length) throw new Error('Invalid directory record.');
  const n = r[32];
  if (!n || 33 + n > r.length) throw new Error('Invalid ISO file name.');
  if (r[25] & 0x80) throw new Error('Multi-extent files are not supported.');
  const offset = isoP32(r, 2) * ISO_BLOCK, length = isoP32(r, 10);
  if (offset + length > limit) throw new Error('A file lies outside the volume: incomplete ISO.');
  if (n === 1 && r[33] <= 1) return { special: true, offset, length, directory: !!(r[25] & 2) };
  const raw = r.subarray(33, 33 + n);
  if (raw.some(c => c < 33 || c > 126)) throw new Error('Unsafe ISO file name.');
  const name = isoAscii(raw, 0, raw.length).replace(/;[1-9][0-9]*$/, '').toUpperCase();
  if (!/^[A-Z0-9_.-]+$/.test(name) || name.includes('..')) throw new Error('Unsafe ISO path: ' + name);
  return { name, offset, length, directory: !!(r[25] & 2) };
}

async function isoIndex(file) {
  if (file.size < 18 * ISO_BLOCK) throw new Error('The file is too small to be an ISO.');
  let pvd = null, terminated = false;
  for (let block = 16; block < Math.min(80, Math.floor(file.size / ISO_BLOCK)); block++) {
    const d = await isoRead(file, block * ISO_BLOCK, ISO_BLOCK);
    if (isoAscii(d, 1, 6) !== 'CD001') throw new Error('Not an ISO9660 image (no CD001).');
    if (d[0] === 1 && !pvd) pvd = d;
    if (d[0] === 255) { terminated = true; break; }
  }
  if (!pvd || !terminated) throw new Error('The ISO has no valid volume descriptor.');
  if (isoP16(pvd, 128) !== ISO_BLOCK) throw new Error('Unsupported ISO block size.');
  const volumeBytes = isoP32(pvd, 80) * ISO_BLOCK;
  if (volumeBytes > file.size) throw new Error('The ISO is incomplete (shorter than its volume).');
  const root = isoEntry(pvd.subarray(156, 156 + pvd[156]), volumeBytes);
  const files = new Map(), seen = new Set(), pending = [{ ...root, prefix: '', depth: 0 }];
  while (pending.length) {
    const dir = pending.pop();
    const key = dir.offset + ':' + dir.length;
    if (seen.has(key) || seen.size > 4096 || dir.depth > 32 || dir.length > 16 * 1024 * 1024) throw new Error('Invalid ISO directories.');
    seen.add(key);
    const data = await isoRead(file, dir.offset, dir.length);
    for (let p = 0; p < data.length;) {
      const len = data[p];
      if (!len) { p += ISO_BLOCK - p % ISO_BLOCK; continue; }
      if (len > ISO_BLOCK - p % ISO_BLOCK || len > data.length - p) throw new Error('An ISO record crosses a sector.');
      const e = isoEntry(data.subarray(p, p + len), volumeBytes);
      p += len;
      if (e.special) continue;
      const name = dir.prefix + e.name;
      if (files.has(name)) throw new Error('Duplicate path in the ISO: ' + name);
      files.set(name, { ...e, name });
      if (e.directory) pending.push({ ...e, prefix: name + '/', depth: dir.depth + 1 });
    }
  }
  return { files, volumeBytes, volumeId: isoAscii(pvd, 40, 72).trim() };
}

async function isoSha256(file, entry) {
  // SCUS_974.72 is ~240 KB: read whole for the hash.
  const data = await isoRead(file, entry.offset, entry.length);
  const hash = await crypto.subtle.digest('SHA-256', data);
  return [...new Uint8Array(hash)].map(x => x.toString(16).padStart(2, '0')).join('');
}

// Verifies the copy: Shadow of the Colossus NTSC-U (SCUS-97472). Returns the files to copy.
async function isoVerify(file, onStep) {
  onStep && onStep('Reading the disc structure…');
  const index = await isoIndex(file);
  for (const name of ISO_REQUIRED) {
    const f = index.files.get(name);
    if (!f || f.directory || !f.length) throw new Error(name + ' is missing: not Shadow of the Colossus, or the ISO is incomplete.');
  }
  onStep && onStep('Checking the edition…');
  const cnf = index.files.get('SYSTEM.CNF');
  const text = new TextDecoder('ascii').decode(await isoRead(file, cnf.offset, Math.min(cnf.length, 65536)));
  const boots = [...text.matchAll(/^\s*BOOT2\s*=\s*([^\r\n]+)\s*$/gim)];
  if (boots.length !== 1 || !/^cdrom0:[\\/]SCUS_974\.72;1\s*$/i.test(boots[0][1])) {
    throw new Error('Unsupported edition: Shadow of the Colossus NTSC-U (SCUS-97472) is required.');
  }
  onStep && onStep('Checking the executable…');
  const sha = await isoSha256(file, index.files.get('SCUS_974.72'));
  const exact = sha === ISO_SCUS_SHA256;
  const list = [...index.files.values()].filter(f => !f.directory).sort((a, b) => a.name.localeCompare(b.name));
  const bytes = list.reduce((s, f) => s + f.length, 0);
  return { list, bytes, exact, sha, volumeId: index.volumeId };
}
