#!/usr/bin/env node
import { writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { inspectDisc, extractRequired, extractAll } from './iso9660.mjs';

const HELP = `Celosia local disc preparation (no code is executed from the disc).
Usage: node prepare-disc.mjs --iso <SCUS-97472.iso> [--inspect]
       [--manifest <new-file.json>] [--extract <new-directory>] [--full-disc]

Inspect validates ISO9660, checks SYSTEM.CNF and hashes the five recompilation
inputs. Extraction copies ONLY SYSTEM.CNF, SCUS_974.72 and the three XFF modules;
it does not prepare the remaining game data needed to play. Existing output
paths are never overwritten. --full-disc with --extract copies all validated
files for local module capture; it can use as much space as the original ISO.
Keep extracted/generated game material private.
`;

async function main() {
  const args = process.argv.slice(2);
  if (args.includes('--help')) { console.log(HELP); return; }
  const options = {};
  for (let i = 0; i < args.length; i++) {
    const key = args[i];
    if (key === '--inspect') continue;
    if (key === '--full-disc') {
      if (options.fullDisc) throw new Error('Duplicate --full-disc');
      options.fullDisc = true;
      continue;
    }
    if (!['--iso', '--manifest', '--extract'].includes(key) || options[key]) throw new Error(`Invalid or duplicate argument: ${key}`);
    const value = args[++i];
    if (!value || value.startsWith('--')) throw new Error(`Missing value for ${key}`);
    options[key] = value;
  }
  if (!options['--iso']) throw new Error('Required --iso argument missing; use --help');
  if (options.fullDisc && !options['--extract']) throw new Error('--full-disc requires --extract');
  const manifest = options['--extract']
    ? await (options.fullDisc ? extractAll : extractRequired)(options['--iso'], options['--extract'])
    : await inspectDisc(options['--iso']);
  const json = JSON.stringify(manifest, null, 2) + '\n';
  if (options['--manifest']) await writeFile(resolve(options['--manifest']), json, { flag: 'wx' });
  console.log(json.trimEnd());
}

main().catch(error => { console.error(`Disc preparation failed: ${error.message}`); process.exitCode = 1; });
