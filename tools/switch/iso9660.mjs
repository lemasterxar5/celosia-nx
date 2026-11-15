import { open, mkdir, lstat } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { basename, dirname, join, resolve, parse } from 'node:path';

const BLOCK = 2048;
const REQUIRED = ['SYSTEM.CNF', 'SCUS_974.72', 'KERNEL.XFF', 'MANAGER.XFF', 'GAMECORE.XFF'];
const MAX_DIRECTORY = 16 * 1024 * 1024;

function paired32(b, offset) {
  const value = b.readUInt32LE(offset);
  if (value !== b.readUInt32BE(offset + 4)) throw new Error('Inconsistent endian 32-bit field');
  return value;
}
function paired16(b, offset) {
  const value = b.readUInt16LE(offset);
  if (value !== b.readUInt16BE(offset + 2)) throw new Error('Inconsistent endian 16-bit field');
  return value;
}
function bounds(offset, length, limit) {
  if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(length) || offset < 0 || length < 0 ||
      offset > limit || length > limit - offset) throw new Error('Extent outside or truncated volume');
}
async function exactRead(handle, offset, length) {
  const b = Buffer.alloc(length);
  let done = 0;
  while (done < length) {
    const { bytesRead } = await handle.read(b, done, length - done, offset + done);
    if (!bytesRead) throw new Error('Truncated ISO read');
    done += bytesRead;
  }
  return b;
}
function entry(record, limit) {
  if (record.length < 34 || record[0] !== record.length) throw new Error('Invalid directory record length');
  const n = record[32];
  if (!n || 33 + n > record.length) throw new Error('Invalid ISO filename length');
  if (record[1] || record[26] || record[27]) throw new Error('Unsupported extended or interleaved extent');
  if (record[25] & 0x80) throw new Error('Unsupported multi-extent file');
  if (paired16(record, 28) !== 1) throw new Error('Unsupported multi-volume ISO');
  const offset = paired32(record, 2) * BLOCK;
  const length = paired32(record, 10);
  bounds(offset, length, limit);
  // Rock Ridge links are not files to extract, even if their ISO name is safe.
  for (let p = 33 + n + (n % 2 === 0 ? 1 : 0); p + 4 <= record.length;) {
    const len = record[p + 2];
    if (len < 4 || p + len > record.length) break;
    if (record.toString('ascii', p, p + 2) === 'SL') throw new Error('ISO symlinks are not supported');
    p += len;
  }
  if (n === 1 && record[33] <= 1) return { special: true, offset, length, directory: !!(record[25] & 2) };
  const raw = record.subarray(33, 33 + n);
  if (raw.some(c => c < 33 || c > 126)) throw new Error('Unsafe ISO filename encoding');
  const name = raw.toString('ascii').replace(/;[1-9][0-9]*$/, '').toUpperCase();
  if (!/^[A-Z0-9_.-]+$/.test(name) || name === '.' || name === '..' || name.includes('..')) {
    throw new Error(`Unsafe ISO filename/path: ${JSON.stringify(name)}`);
  }
  if (/^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)/i.test(name)) {
    throw new Error('Reserved Windows device filename');
  }
  return { name, offset, length, directory: !!(record[25] & 2) };
}
async function readIndex(handle, size) {
  if (size < 18 * BLOCK) throw new Error('Truncated ISO volume');
  let pvd;
  let terminated = false;
  for (let block = 16; block < Math.min(80, Math.floor(size / BLOCK)); block++) {
    const d = await exactRead(handle, block * BLOCK, BLOCK);
    if (d.toString('ascii', 1, 6) !== 'CD001' || d[6] !== 1) throw new Error('Invalid ISO9660 CD001 descriptor');
    if (d[0] === 1) { if (pvd) throw new Error('Duplicate primary volume descriptor'); pvd = d; }
    if (d[0] === 255) { terminated = true; break; }
  }
  if (!pvd || !terminated) throw new Error('Missing primary volume descriptor or terminator');
  if (paired16(pvd, 128) !== BLOCK) throw new Error('Logical block size must be 2048');
  const volumeBytes = paired32(pvd, 80) * BLOCK;
  bounds(0, volumeBytes, size);
  if (volumeBytes < 18 * BLOCK) throw new Error('Invalid volume size');
  const rootLength = pvd[156];
  const root = entry(pvd.subarray(156, 156 + rootLength), volumeBytes);
  if (!root.special || !root.directory || !root.length) throw new Error('Invalid root directory');
  const files = new Map();
  const seenDirectories = new Set();
  const pending = [{ ...root, prefix: '', depth: 0 }];
  while (pending.length) {
    const dir = pending.pop();
    const key = `${dir.offset}:${dir.length}`;
    if (seenDirectories.has(key)) throw new Error('Cyclic or aliased directory extent');
    seenDirectories.add(key);
    if (seenDirectories.size > 4096 || dir.depth > 32 || dir.length > MAX_DIRECTORY) throw new Error('Directory budget exceeded');
    const data = await exactRead(handle, dir.offset, dir.length);
    for (let p = 0; p < data.length;) {
      const len = data[p];
      if (!len) { p += BLOCK - p % BLOCK; continue; }
      if (len > BLOCK - p % BLOCK || len > data.length - p) throw new Error('Directory record crosses extent or sector');
      const e = entry(data.subarray(p, p + len), volumeBytes);
      p += len;
      if (e.special) continue;
      const name = dir.prefix + e.name;
      if (files.has(name)) throw new Error(`Duplicate normalized path ${name}`);
      files.set(name, { ...e, name });
      if (files.size > 250000) throw new Error('File count budget exceeded');
      if (e.directory) pending.push({ ...e, prefix: `${name}/`, depth: dir.depth + 1 });
    }
  }
  for (const name of REQUIRED) {
    const f = files.get(name);
    if (!f || f.directory || !f.length) throw new Error(`Required file missing or empty: ${name}`);
  }
  const cnf = files.get('SYSTEM.CNF');
  if (cnf.length > 65536) throw new Error('SYSTEM.CNF exceeds size budget');
  const text = (await exactRead(handle, cnf.offset, cnf.length)).toString('ascii');
  const boots = [...text.matchAll(/^\s*BOOT2\s*=\s*([^\r\n]+)\s*$/gim)];
  if (boots.length !== 1 || !/^cdrom0:[\\/]SCUS_974\.72;1\s*$/i.test(boots[0][1])) {
    throw new Error('Wrong game edition: expected BOOT2 cdrom0:\\SCUS_974.72;1');
  }
  return { files, volumeBytes, volumeId: pvd.toString('ascii', 40, 72).replace(/[\s\0]+$/, '') };
}
async function transfer(handle, file, output) {
  const hash = createHash('sha256');
  const chunkSize = 1024 * 1024;
  for (let done = 0; done < file.length;) {
    const data = await exactRead(handle, file.offset + done, Math.min(chunkSize, file.length - done));
    hash.update(data);
    if (output) await output.writeFile(data);
    done += data.length;
  }
  return hash.digest('hex');
}
async function noSymlinkAncestors(destination) {
  let current = destination;
  while (true) {
    try {
      const stat = await lstat(current);
      if (stat.isSymbolicLink()) throw new Error(`Symlink destination or ancestor: ${current}`);
      if (!stat.isDirectory()) throw new Error(`Destination ancestor is not a directory: ${current}`);
    } catch (e) { if (e.code !== 'ENOENT') throw e; }
    const parent = dirname(current);
    if (parent === current) return;
    current = parent;
  }
}
async function processDisc(isoPath, destination, fullDisc = false) {
  const source = resolve(isoPath);
  const handle = await open(source, 'r');
  try {
    const stat = await handle.stat();
    if (!stat.isFile() || !Number.isSafeInteger(stat.size)) throw new Error('ISO must be a regular file');
    const index = await readIndex(handle, stat.size);
    const selected = fullDisc
      ? [...index.files.values()].filter(f => !f.directory)
      : REQUIRED.map(name => index.files.get(name));
    const totalBytes = selected.reduce((sum, f) => sum + f.length, 0);
    if (!Number.isSafeInteger(totalBytes) || totalBytes > index.volumeBytes) {
      throw new Error('Extracted file size budget exceeds source volume');
    }
    let dest;
    if (destination !== undefined) {
      dest = resolve(destination);
      if (dest === parse(dest).root) throw new Error('Cannot extract into filesystem root');
      await noSymlinkAncestors(dest);
      await mkdir(dirname(dest), { recursive: true });
      await mkdir(dest); // Exclusive: never overwrite any existing destination.
      if (fullDisc) {
        for (const f of index.files.values()) {
          if (f.directory) await mkdir(join(dest, f.name), { recursive: true });
        }
      }
    }
    const files = [];
    for (const f of selected) {
      const name = f.name;
      if (dest && fullDisc) await mkdir(dirname(join(dest, name)), { recursive: true });
      const output = dest ? await open(join(dest, name), 'wx') : undefined;
      let sha256;
      try { sha256 = await transfer(handle, f, output); }
      finally { await output?.close(); }
      files.push({ name, bytes: f.length, offset: f.offset, sha256 });
    }
    // Detect common accidental changes to the ISO during inspection/extraction.
    const after = await handle.stat();
    if (after.size !== stat.size || after.mtimeMs !== stat.mtimeMs) throw new Error('Source ISO changed during operation');
    return {
      schema: 'celosia-disc-v1', isoFile: basename(source), isoBytes: stat.size,
      volumeBytes: index.volumeBytes, volumeId: index.volumeId, logicalBlockSize: BLOCK,
      serial: 'SCUS-97472', bootFile: 'SCUS_974.72', files,
      extraction: fullDisc ? 'full-disc' : 'recompilation-inputs',
    };
  } finally { await handle.close(); }
}

/** Inspect the ISO and hash only the five inputs required by the recompilation pipeline. */
export async function inspectDisc(isoPath) { return processDisc(isoPath); }

/** Extract the five required files into a NEW destination; never replace existing files. */
export async function extractRequired(isoPath, destination) {
  if (typeof destination !== 'string' || !destination.trim()) throw new Error('Explicit extraction destination required');
  return processDisc(isoPath, destination);
}

/** Extract all validated files for local reference capture; includes private game data. */
export async function extractAll(isoPath, destination) {
  if (typeof destination !== 'string' || !destination.trim()) throw new Error('Explicit extraction destination required');
  return processDisc(isoPath, destination, true);
}
