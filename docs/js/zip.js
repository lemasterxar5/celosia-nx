// Uncompressed (stored) ZIP writer with ZIP64, into the file picked with showSaveFilePicker.
// Data is written in chunks; each file's CRC32 is computed on the fly and patched into its local
// header afterwards (positional writes of FileSystemWritableFileStream). Nothing is held in memory.
'use strict';

const CRC_TABLE = (() => {
  const t = new Uint32Array(256 * 8);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    t[i] = c >>> 0;
  }
  for (let i = 0; i < 256; i++) for (let s = 1; s < 8; s++) t[s * 256 + i] = (t[(s - 1) * 256 + i] >>> 8) ^ t[t[(s - 1) * 256 + i] & 255];
  return t;
})();
function crc32Update(crc, d) {
  let c = ~crc >>> 0, i = 0;
  const n8 = d.length & ~7;
  for (; i < n8; i += 8) {   // slice-by-8
    const a = (c ^ (d[i] | d[i + 1] << 8 | d[i + 2] << 16 | d[i + 3] << 24)) >>> 0;
    c = CRC_TABLE[7 * 256 + (a & 255)] ^ CRC_TABLE[6 * 256 + ((a >>> 8) & 255)] ^ CRC_TABLE[5 * 256 + ((a >>> 16) & 255)] ^
        CRC_TABLE[4 * 256 + (a >>> 24)] ^ CRC_TABLE[3 * 256 + d[i + 4]] ^ CRC_TABLE[2 * 256 + d[i + 5]] ^
        CRC_TABLE[256 + d[i + 6]] ^ CRC_TABLE[d[i + 7]];
  }
  for (; i < d.length; i++) c = CRC_TABLE[(c ^ d[i]) & 255] ^ (c >>> 8);
  return ~c >>> 0;
}

class ZipWriter {
  constructor(writable) { this.w = writable; this.pos = 0; this.entries = []; this.names = new Set(); }
  static dosTime(d = new Date()) {
    return { time: (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1),
             date: ((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate() };
  }
  async raw(bytes) { await this.w.write({ type: 'write', position: this.pos, data: bytes }); this.pos += bytes.length; }

  // Adds a file of 'size' bytes; 'chunks' is an async iterator of Uint8Array.
  async add(path, size, chunks, onBytes) {
    if (this.names.has(path)) throw new Error('Duplicate path in the ZIP: ' + path);
    this.names.add(path);
    const name = new TextEncoder().encode(path);
    const zip64 = size >= 0xFFFFFFFF;
    const { time, date } = ZipWriter.dosTime();
    const offset = this.pos;
    const h = new DataView(new ArrayBuffer(30 + name.length + (zip64 ? 20 : 0)));
    h.setUint32(0, 0x04034b50, true); h.setUint16(4, 45, true); h.setUint16(6, 0x0800, true); h.setUint16(8, 0, true);
    h.setUint16(10, time, true); h.setUint16(12, date, true); h.setUint32(14, 0, true);           // CRC: patched later
    h.setUint32(18, zip64 ? 0xFFFFFFFF : size, true); h.setUint32(22, zip64 ? 0xFFFFFFFF : size, true);
    h.setUint16(26, name.length, true); h.setUint16(28, zip64 ? 20 : 0, true);
    new Uint8Array(h.buffer).set(name, 30);
    if (zip64) {
      const e = 30 + name.length;
      h.setUint16(e, 1, true); h.setUint16(e + 2, 16, true);
      h.setBigUint64(e + 4, BigInt(size), true); h.setBigUint64(e + 12, BigInt(size), true);
    }
    await this.raw(new Uint8Array(h.buffer));
    let crc = 0, written = 0;
    for await (const c of chunks) {
      crc = crc32Update(crc, c); await this.raw(c); written += c.length;
      onBytes && onBytes(c.length);
    }
    if (written !== size) throw new Error('Unexpected size while copying ' + path);
    const fix = new DataView(new ArrayBuffer(4)); fix.setUint32(0, crc, true);
    await this.w.write({ type: 'write', position: offset + 14, data: new Uint8Array(fix.buffer) });
    this.entries.push({ name, size, crc, offset, time, date });
  }

  async addBytes(path, bytes, onBytes) {
    await this.add(path, bytes.length, (async function* () { yield bytes; })(), onBytes);
  }

  async addFile(path, file, onBytes, cancelled) {
    const CH = 8 * 1024 * 1024;
    await this.add(path, file.size, (async function* () {
      for (let o = 0; o < file.size; o += CH) {
        if (cancelled && cancelled()) throw new Error('Cancelled.');
        yield new Uint8Array(await file.slice(o, Math.min(file.size, o + CH)).arrayBuffer());
      }
    })(), onBytes);
  }

  async finish() {
    const cdStart = this.pos;
    for (const e of this.entries) {
      const big = e.size >= 0xFFFFFFFF, farOff = e.offset >= 0xFFFFFFFF;
      const extra = (big ? 16 : 0) + (farOff ? 8 : 0);
      const c = new DataView(new ArrayBuffer(46 + e.name.length + (extra ? 4 + extra : 0)));
      c.setUint32(0, 0x02014b50, true); c.setUint16(4, 45, true); c.setUint16(6, 45, true); c.setUint16(8, 0x0800, true);
      c.setUint16(10, 0, true); c.setUint16(12, e.time, true); c.setUint16(14, e.date, true); c.setUint32(16, e.crc, true);
      c.setUint32(20, big ? 0xFFFFFFFF : e.size, true); c.setUint32(24, big ? 0xFFFFFFFF : e.size, true);
      c.setUint16(28, e.name.length, true); c.setUint16(30, extra ? 4 + extra : 0, true);
      c.setUint32(42, farOff ? 0xFFFFFFFF : e.offset, true);
      new Uint8Array(c.buffer).set(e.name, 46);
      if (extra) {
        let p = 46 + e.name.length;
        c.setUint16(p, 1, true); c.setUint16(p + 2, extra, true); p += 4;
        if (big) { c.setBigUint64(p, BigInt(e.size), true); c.setBigUint64(p + 8, BigInt(e.size), true); p += 16; }
        if (farOff) c.setBigUint64(p, BigInt(e.offset), true);
      }
      await this.raw(new Uint8Array(c.buffer));
    }
    const cdSize = this.pos - cdStart, count = this.entries.length;
    const z64 = cdStart >= 0xFFFFFFFF || cdSize >= 0xFFFFFFFF || count >= 0xFFFF;
    if (z64) {
      const eocd64 = this.pos;
      const r = new DataView(new ArrayBuffer(56 + 20));
      r.setUint32(0, 0x06064b50, true); r.setBigUint64(4, 44n, true); r.setUint16(12, 45, true); r.setUint16(14, 45, true);
      r.setBigUint64(24, BigInt(count), true); r.setBigUint64(32, BigInt(count), true);
      r.setBigUint64(40, BigInt(cdSize), true); r.setBigUint64(48, BigInt(cdStart), true);
      r.setUint32(56, 0x07064b50, true); r.setBigUint64(64, BigInt(eocd64), true); r.setUint32(72, 1, true);
      await this.raw(new Uint8Array(r.buffer));
    }
    const e = new DataView(new ArrayBuffer(22));
    e.setUint32(0, 0x06054b50, true);
    e.setUint16(8, z64 ? 0xFFFF : count, true); e.setUint16(10, z64 ? 0xFFFF : count, true);
    e.setUint32(12, z64 ? 0xFFFFFFFF : cdSize, true); e.setUint32(16, z64 ? 0xFFFFFFFF : cdStart, true);
    await this.raw(new Uint8Array(e.buffer));
    await this.w.close();
  }
}
