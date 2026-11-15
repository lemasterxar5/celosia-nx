import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, readFile, mkdir, symlink, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { inspectDisc, extractRequired } from '../iso9660.mjs';
import * as discTools from '../iso9660.mjs';

const sector = 2048;
test('full-disc extraction includes nested runtime modules in a new directory', async t => {
  const { iso, dir } = await fixture(t, { mutate: b => {
    let p = 20 * sector;
    while (b[p]) p += b[p];
    record('MODULES', 30, Buffer.alloc(sector), 2).copy(b, p);
    let q = 30 * sector;
    for (const item of [record(Buffer.from([0]), 30, Buffer.alloc(sector), 2),
      record(Buffer.from([1]), 20, Buffer.alloc(sector), 2),
      record('DS1O_D.IRX;1', 31, Buffer.from('module fixture'))]) {
      item.copy(b, q); q += item.length;
    }
    b.write('module fixture', 31 * sector);
  } });
  assert.equal(typeof discTools.extractAll, 'function');
  const destination = join(dir, 'full');
  const manifest = await discTools.extractAll(iso, destination);
  assert.equal(await readFile(join(destination, 'MODULES', 'DS1O_D.IRX'), 'utf8'), 'module fixture');
  assert.equal(manifest.extraction, 'full-disc');
  assert.equal(manifest.files.length, 6);
  await assert.rejects(discTools.extractAll(iso, destination), /exist/i);
});

test('full-disc mode rejects aggregate file lengths exceeding the source volume', async t => {
  const { iso, dir } = await fixture(t, { mutate: b => {
    let p = 20 * sector;
    while (b[p]) p += b[p];
    for (let i = 0; i < 12; i++) {
      const item = record(`REPEAT${i}.BIN;1`, 21, Buffer.alloc(10 * sector));
      item.copy(b, p); p += item.length;
    }
  } });
  assert.equal(typeof discTools.extractAll, 'function');
  await assert.rejects(discTools.extractAll(iso, join(dir, 'full')), /budget|volume/i);
});
test('CLI full-disc mode requires an extraction directory', async t => {
  const { iso, dir } = await fixture(t);
  const cli = fileURLToPath(new URL('../prepare-disc.mjs', import.meta.url));
  const valid = spawnSync(process.execPath,
    [cli, '--iso', iso, '--full-disc', '--extract', join(dir, 'all')], { encoding: 'utf8' });
  assert.equal(valid.status, 0, valid.stderr);
  assert.equal(JSON.parse(valid.stdout).extraction, 'full-disc');
  assert.equal(spawnSync(process.execPath, [cli, '--iso', iso, '--full-disc']).status, 1);
});
test('full-disc extraction rejects Windows device paths before creating output', async t => {
  const { iso, dir } = await fixture(t, { mutate: b => {
    let p = 20 * sector;
    while (b[p]) p += b[p];
    record('NUL.TXT;1', 31, Buffer.from('bad')).copy(b, p);
  } });
  const destination = join(dir, 'unsafe');
  await assert.rejects(discTools.extractAll(iso, destination), /device|reserved/i);
  await assert.rejects(readFile(join(destination, 'SYSTEM.CNF')), /ENOENT/);
});
function both32(b, offset, n) { b.writeUInt32LE(n, offset); b.writeUInt32BE(n, offset + 4); }
function both16(b, offset, n) { b.writeUInt16LE(n, offset); b.writeUInt16BE(n, offset + 2); }
function record(name, block, data, flags = 0) {
  const id = Buffer.isBuffer(name) ? name : Buffer.from(name);
  const b = Buffer.alloc(33 + id.length + (id.length % 2 === 0 ? 1 : 0));
  b[0] = b.length; both32(b, 2, block); both32(b, 10, data.length);
  b[25] = flags; both16(b, 28, 1); b[32] = id.length; id.copy(b, 33); return b;
}
function image({ boot = 'SCUS_974.72', name = 'KERNEL.XFF;1', mutate } = {}) {
  const b = Buffer.alloc(40 * sector);
  const files = [
    ['SYSTEM.CNF;1', Buffer.from(`BOOT2 = cdrom0:\\${boot};1\nVER = 1.00\n`)],
    ['SCUS_974.72;1', Buffer.from('ELF test placeholder')],
    [name, Buffer.from('kernel fixture')],
    ['MANAGER.XFF;1', Buffer.from('manager fixture')],
    ['GAMECORE.XFF;1', Buffer.from('game fixture')],
  ];
  const root = record(Buffer.from([0]), 20, Buffer.alloc(sector), 2);
  const pvd = b.subarray(16 * sector, 17 * sector);
  pvd[0] = 1; pvd.write('CD001', 1); pvd[6] = 1; pvd.write('SOTC_TEST', 40);
  both32(pvd, 80, 40); both16(pvd, 128, sector); root.copy(pvd, 156);
  const end = b.subarray(17 * sector, 18 * sector); end[0] = 255; end.write('CD001', 1); end[6] = 1;
  let pos = 20 * sector;
  for (const r of [root, record(Buffer.from([1]), 20, Buffer.alloc(sector), 2)]) { r.copy(b, pos); pos += r.length; }
  files.forEach(([n, data], i) => { const r = record(n, 21 + i, data); r.copy(b, pos); pos += r.length; data.copy(b, (21 + i) * sector); });
  mutate?.(b); return b;
}
async function fixture(t, options) {
  const dir = await mkdtemp(join(tmpdir(), 'celosia-iso-test-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const iso = join(dir, 'fixture.iso'); await writeFile(iso, image(options)); return { dir, iso };
}

test('validates edition and hashes required files without exposing their bytes', async t => {
  const { iso } = await fixture(t);
  const m = await inspectDisc(iso);
  assert.equal(m.serial, 'SCUS-97472'); assert.equal(m.bootFile, 'SCUS_974.72');
  assert.equal(m.logicalBlockSize, 2048); assert.equal(m.files.length, 5);
  const kernel = m.files.find(f => f.name === 'KERNEL.XFF');
  assert.equal(kernel.sha256, createHash('sha256').update('kernel fixture').digest('hex'));
  assert.ok(!JSON.stringify(m).includes('kernel fixture'));
});
test('rejects a truncated volume', async t => {
  const { iso } = await fixture(t); await writeFile(iso, image().subarray(0, 17 * sector));
  await assert.rejects(inspectDisc(iso), /truncated|outside|volume/i);
});
test('rejects inconsistent endian size fields', async t => {
  const { iso } = await fixture(t, { mutate: b => b.writeUInt32BE(39, 16 * sector + 84) });
  await assert.rejects(inspectDisc(iso), /endian/i);
});
test('rejects inconsistent endian 16-bit fields', async t => {
  const { iso } = await fixture(t, { mutate: b => b.writeUInt16BE(1024, 16 * sector + 130) });
  await assert.rejects(inspectDisc(iso), /endian/i);
});
test('rejects an invalid descriptor signature', async t => {
  const { iso } = await fixture(t, { mutate: b => b.write('BAD01', 16 * sector + 1) });
  await assert.rejects(inspectDisc(iso), /CD001/i);
});
test('rejects an absent primary descriptor', async t => {
  const { iso } = await fixture(t, { mutate: b => { b[16 * sector] = 255; } });
  await assert.rejects(inspectDisc(iso), /descriptor/i);
});
test('rejects a duplicate primary descriptor', async t => {
  const { iso } = await fixture(t, { mutate: b => b.copy(b, 17 * sector, 16 * sector, 17 * sector) });
  await assert.rejects(inspectDisc(iso), /duplicate/i);
});
test('rejects unsupported logical block size', async t => {
  const { iso } = await fixture(t, { mutate: b => both16(b, 16 * sector + 128, 1024) });
  await assert.rejects(inspectDisc(iso), /2048|block size/i);
});
test('rejects a file extent outside volume', async t => {
  const { iso } = await fixture(t, { mutate: b => both32(b, 20 * sector + 68 + 2, 100) });
  await assert.rejects(inspectDisc(iso), /extent|outside/i);
});
test('rejects traversal filenames', async t => {
  const { iso } = await fixture(t, { name: '../KERNEL.XFF;1' });
  await assert.rejects(inspectDisc(iso), /name|path/i);
});
test('rejects multi-extent files rather than extracting incomplete content', async t => {
  const { iso } = await fixture(t, { mutate: b => { b[20 * sector + 68 + 25] = 0x80; } });
  await assert.rejects(inspectDisc(iso), /multi-extent/i);
});
test('rejects a malformed root record', async t => {
  const { iso } = await fixture(t, { mutate: b => { b[16 * sector + 156] = 20; } });
  await assert.rejects(inspectDisc(iso), /record/i);
});
test('rejects root extent with missing directory flag', async t => {
  const { iso } = await fixture(t, { mutate: b => { b[16 * sector + 156 + 25] = 0; } });
  await assert.rejects(inspectDisc(iso), /root/i);
});
test('rejects multi-volume entries', async t => {
  const { iso } = await fixture(t, { mutate: b => both16(b, 20 * sector + 68 + 28, 2) });
  await assert.rejects(inspectDisc(iso), /multi-volume/i);
});
test('rejects a cyclic directory without infinite traversal', async t => {
  const { iso } = await fixture(t, { mutate: b => {
    const r = record('LOOP', 20, Buffer.alloc(sector), 2);
    r.copy(b, 20 * sector + 68);
    b.fill(0, 20 * sector + 68 + r.length, 21 * sector);
  } });
  await assert.rejects(inspectDisc(iso), /cyclic|aliased/i);
});
test('rejects unsafe non-ASCII filenames', async t => {
  const { iso } = await fixture(t, { mutate: b => { b[20 * sector + 68 + 33] = 0xff; } });
  await assert.rejects(inspectDisc(iso), /encoding/i);
});
test('rejects Rock Ridge symlink records', async t => {
  const { iso } = await fixture(t, { mutate: b => {
    const base = record('LINK', 30, Buffer.alloc(0));
    const r = Buffer.concat([base, Buffer.from([83, 76, 5, 1, 0])]); r[0] = r.length;
    r.copy(b, 20 * sector + 68); b.fill(0, 20 * sector + 68 + r.length, 21 * sector);
  } });
  await assert.rejects(inspectDisc(iso), /symlink/i);
});
test('extracts and hashes a file larger than its IO chunk', async t => {
  const { iso, dir } = await fixture(t);
  const initial = image(); const length = 1024 * 1024 + 17;
  const blocks = Math.ceil((40 * sector + length) / sector);
  const large = Buffer.alloc(blocks * sector); initial.copy(large);
  both32(large, 16 * sector + 80, blocks);
  const pos = large.indexOf('KERNEL.XFF;1', 20 * sector) - 33;
  both32(large, pos + 2, 40); both32(large, pos + 10, length);
  large.fill(127, 40 * sector, 40 * sector + length); await writeFile(iso, large);
  const m = await extractRequired(iso, join(dir, 'large-disc'));
  const data = await readFile(join(dir, 'large-disc', 'KERNEL.XFF'));
  assert.equal(data.length, length); assert.ok(data.every(c => c === 127));
  assert.equal(m.files.find(f => f.name === 'KERNEL.XFF').sha256, createHash('sha256').update(data).digest('hex'));
});
test('rejects records spanning sector boundaries', async t => {
  const { iso } = await fixture(t, { mutate: b => {
    b.fill(0, 20 * sector + 68, 21 * sector);
    for (let pos = 20 * sector + 68; pos < 21 * sector;) {
      const remain = 21 * sector - pos;
      if (remain < 255) { b[pos] = 255; break; }
      const r = record('A'.repeat(221), 30, Buffer.alloc(0)); r[0] = 254; r.copy(b, pos); pos += 254;
    }
  } });
  await assert.rejects(inspectDisc(iso), /crosses|duplicate|record/i);
});
test('rejects a different game edition', async t => {
  const { iso } = await fixture(t, { boot: 'SCES_533.26' });
  await assert.rejects(inspectDisc(iso), /SCUS|edition|BOOT2/i);
});
test('rejects duplicate normalized paths', async t => {
  const { iso } = await fixture(t, { name: 'SCUS_974.72;2' });
  await assert.rejects(inspectDisc(iso), /duplicate/i);
});
test('requires expected game modules', async t => {
  const { iso } = await fixture(t, { name: 'UNRELATED.XFF;1' });
  await assert.rejects(inspectDisc(iso), /KERNEL.XFF/i);
});
test('extracts only required files and refuses overwriting destination', async t => {
  const { iso, dir } = await fixture(t); const dest = join(dir, 'disc');
  const m = await extractRequired(iso, dest);
  assert.equal((await readFile(join(dest, 'KERNEL.XFF'))).toString(), 'kernel fixture');
  assert.equal(m.files.length, 5);
  await assert.rejects(extractRequired(iso, dest), /exist/i);
});
test('refuses a symlink destination', async t => {
  const { iso, dir } = await fixture(t); const dest = join(dir, 'disc');
  const target = join(dir, 'target'); await mkdir(target);
  try { await symlink(target, dest, 'junction'); }
  catch (e) { if (e.code === 'EPERM') { t.skip('OS does not allow symlinks'); return; } throw e; }
  await assert.rejects(extractRequired(iso, dest), /symlink|exist/i);
});
test('requires an explicit extraction destination', async t => {
  const { iso } = await fixture(t);
  await assert.rejects(extractRequired(iso, ''), /explicit/i);
});
test('refuses a non-directory destination ancestor', async t => {
  const { iso, dir } = await fixture(t); const file = join(dir, 'file'); await writeFile(file, 'unchanged');
  await assert.rejects(extractRequired(iso, join(file, 'disc')), /not a directory|ENOTDIR/i);
  assert.equal(await readFile(file, 'utf8'), 'unchanged');
});
test('CLI inspects and writes metadata, refusing an existing manifest', async t => {
  const { iso, dir } = await fixture(t); const manifest = join(dir, 'manifest.json');
  const cli = fileURLToPath(new URL('../prepare-disc.mjs', import.meta.url));
  const args = [cli, '--iso', iso, '--inspect', '--manifest', manifest];
  const first = spawnSync(process.execPath, args, { encoding: 'utf8' });
  assert.equal(first.status, 0, first.stderr);
  assert.equal(JSON.parse(first.stdout).serial, 'SCUS-97472');
  const initial = await readFile(manifest, 'utf8');
  const second = spawnSync(process.execPath, args, { encoding: 'utf8' });
  assert.equal(second.status, 1); assert.match(second.stderr, /exist/i);
  assert.equal(await readFile(manifest, 'utf8'), initial);
});
test('CLI rejects unknown or incomplete arguments and provides help', () => {
  const cli = fileURLToPath(new URL('../prepare-disc.mjs', import.meta.url));
  for (const args of [[], ['--bad'], ['--iso'], ['--iso', '--inspect'], ['--iso', 'a', '--iso', 'b']]) {
    assert.equal(spawnSync(process.execPath, [cli, ...args]).status, 1);
  }
  const help = spawnSync(process.execPath, [cli, '--help'], { encoding: 'utf8' });
  assert.equal(help.status, 0); assert.match(help.stdout, /Usage:/);
});
