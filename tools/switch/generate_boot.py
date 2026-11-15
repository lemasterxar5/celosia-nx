#!/usr/bin/env python3
"""Generate Celosia's boot-only recompilation inputs from a local USA disc.

This prepares stage_boot, not a playable game or a Switch executable. Loaded
KERNEL/MANAGER/GAMECORE captures are required by the upstream full-game linker.
No guest code or emulator is executed by this preparation tool.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tomllib

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import funcmap_elf
from xff import Xff, SHN_ABS

CAPTURES = ['kernel.bin', 'manager.bin', 'gamecore.bin']
EE_RAM_BYTES = 32 * 1024 * 1024


def toml_string(value):
    """JSON basic-string escapes are compatible with TOML basic strings."""
    return json.dumps(str(value), ensure_ascii=False)


def validate_elf_tables(data):
    """The upstream ELF reader trusts offsets; reject truncated tables first."""
    phoff, shoff = struct.unpack_from('<II', data, 28)
    phsize, phcount, shsize, shcount, string_index = struct.unpack_from('<HHHHH', data, 42)
    for offset, size, count, expected in [(phoff, phsize, phcount, 32),
                                           (shoff, shsize, shcount, 40)]:
        if count and (size != expected or offset < 52 or offset + count * size > len(data)):
            raise ValueError('ELF table is truncated or has an invalid entry size')
    if shcount and string_index >= shcount:
        raise ValueError('ELF section string table index is invalid')
    segments = []
    executable_bytes = 0
    executable_segment_bytes = 0
    for index in range(phcount):
        fields = struct.unpack_from('<8I', data, phoff + index * phsize)
        if fields[0] == 1 and fields[1] + fields[4] > len(data):
            raise ValueError('ELF load segment extends beyond the file')
        if fields[0] == 1:
            segments.append(fields)
            if fields[6] & 1 and fields[4]:
                validate_exec_range(fields[2], fields[4])
                executable_segment_bytes += fields[4]
                if executable_segment_bytes > EE_RAM_BYTES:
                    raise ValueError('Executable segment scan exceeds EE RAM budget')
    for index in range(shcount):
        fields = struct.unpack_from('<10I', data, shoff + index * shsize)
        if fields[1] != 8 and fields[4] + fields[5] > len(data):
            raise ValueError('ELF section extends beyond the file')
        if fields[2] & 4:
            if fields[1] != 1:
                raise ValueError('Executable ELF sections must contain real PROGBITS, not NOBITS')
            if fields[5]:
                validate_exec_range(fields[3], fields[5])
                executable_bytes += fields[5]
                if executable_bytes > EE_RAM_BYTES:
                    raise ValueError('Executable section scan exceeds EE RAM budget')
                if not any(p[2] <= fields[3] and fields[3] + fields[5] <= p[2] + p[4]
                           and fields[4] == p[1] + fields[3] - p[2] for p in segments):
                    raise ValueError('Executable section is not backed by a load segment')


def validate_exec_range(address, size):
    if address % 4 or size % 4 or address + size > EE_RAM_BYTES:
        raise ValueError('Executable range must be word-aligned within the 32 MiB EE RAM')


def validate_disc(disc):
    boot = disc / 'SCUS_974.72'
    module = disc / 'KERNEL.XFF'
    # Tool input budgets, not assumptions about a general-purpose ELF format.
    # Check every input before reading any: upstream Xff reads the whole file.
    for path, limit in [(disc / 'SYSTEM.CNF', 64 * 1024),
                        (boot, 64 * 1024 * 1024), (module, 64 * 1024 * 1024)]:
        if path.stat().st_size > limit:
            raise ValueError(f'{path.name} exceeds input size budget ({limit} bytes)')
    cnf = (disc / 'SYSTEM.CNF').read_text(encoding='ascii')
    boot_lines = re.findall(r'^[ \t]*BOOT2[ \t]*=[ \t]*(.*)$', cnf, re.M | re.I)
    if len(boot_lines) != 1 or not re.fullmatch(r'cdrom0:[\\/]SCUS_974\.72;1\s*', boot_lines[0], re.I):
        raise ValueError('Expected USA SCUS_974.72 edition in SYSTEM.CNF')
    data = boot.read_bytes()
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('Boot must be ELF32 little-endian version 1')
    kind, machine, version = struct.unpack_from('<HHI', data, 16)
    if (kind, machine, version) != (2, 8, 1):
        raise ValueError('Boot must be an executable MIPS ELF')
    if struct.unpack_from('<H', data, 40)[0] != 52:
        raise ValueError('Invalid ELF32 header size')
    validate_elf_tables(data)
    try:
        xff = Xff(str(module))
        symbols = sorted({(s.value, s.name) for s in xff.symbols
                          if s.shndx == SHN_ABS and s.name})
    except (ValueError, IndexError, struct.error) as error:
        raise ValueError(f'Invalid KERNEL.XFF: {error}') from error
    if not symbols or any(re.search(r'\s', name) for _, name in symbols):
        raise ValueError('KERNEL.XFF must supply valid absolute boot symbols')
    return boot, module, symbols


def generate(disc, output, *, recompiler=None, full_game=False):
    """Validate before creating a fresh directory or invoking a host executable."""
    if full_game:
        raise ValueError('Full game requires real loaded-module captures: ' + ', '.join(CAPTURES)
                         + '; use upstream build_elf.py, not boot-only generation')
    disc, output = Path(disc).resolve(), Path(output).resolve()
    if output.exists():
        raise FileExistsError(f'Refusing to overwrite output: {output}')
    boot, module, symbols = validate_disc(disc)
    executable = Path(recompiler).resolve() if recompiler else None
    if executable is not None and not executable.is_file():
        raise FileNotFoundError(f'Recompiler executable not found: {executable}')
    template = (TOOLS.parent / 'game/scus.toml').read_text(encoding='utf-8')
    config, count = re.subn(r'^input\s*=.*$', lambda _: 'input = ' + toml_string(boot.as_posix()),
                            template, flags=re.M)
    if count != 1:
        raise ValueError('Expected exactly one boot input in game/scus.toml')
    tomllib.loads(config)
    output.mkdir(parents=True, exist_ok=False)
    symbol_file = output / 'scus_symbols.txt'
    symbol_file.write_text(''.join(f'{value:08x} {name}\n' for value, name in symbols), encoding='utf-8')
    funcmap_elf.main(str(boot), str(symbol_file), str(output / 'functions.csv'))
    (output / 'config.toml').write_text(config, encoding='utf-8')
    result = {
        'schema_version': 1, 'scope': 'boot-only', 'edition': 'SCUS_974.72',
        'full_game_ready': False, 'required_captures': CAPTURES.copy(),
        'inputs': {p.name: {'path': p.as_posix(), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                   for p in [boot, module]},
        'recompiled': False,
    }
    manifest = output / 'manifest.json'
    manifest.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    if executable:
        with (output / 'recompile.log').open('w', encoding='utf-8') as log:
            subprocess.run([str(executable), 'config.toml'], cwd=output, check=True,
                           stdout=log, stderr=subprocess.STDOUT)
        result = {**result, 'recompiled': True}
        manifest.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disc', type=Path, required=True, help='Prepared local recompilation inputs')
    parser.add_argument('--out', type=Path, required=True, help='New output directory; never overwritten')
    parser.add_argument('--recompiler', type=Path, help='Optional trusted host ps2_recomp executable')
    parser.add_argument('--full-game', action='store_true', help='Reject with missing-capture explanation')
    args = parser.parse_args()
    try:
        result = generate(args.disc, args.out, recompiler=args.recompiler, full_game=args.full_game)
    except (OSError, ValueError, struct.error, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Boot generation failed: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
