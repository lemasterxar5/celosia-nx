"""Generate fail-closed typed providers for a private native boot diagnostic.

Never include this output in a release. It identifies the next unported API;
it does not supply successful service results or emulate devices.
"""
import argparse
from pathlib import Path
import re
import subprocess


SYMBOL = re.compile(r'^\s*(?:[0-9a-fA-F]+\s+)?([A-Za-z])\s+(.+)$')
PROVIDER = re.compile(r'^(ps2_stubs|ps2_syscalls)::([A-Za-z_][A-Za-z0-9_]*)'
                      r'\(unsigned char\*, R5900Context\*, PS2Runtime\*\)$')
IDENTIFIER = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*$')


def symbols(text):
    defined, undefined = set(), set()
    for line in text.splitlines():
        match = SYMBOL.fullmatch(line)
        if match:
            kind, name = match.groups()
            (undefined if kind in 'Uwv' else defined).add(name)
    return defined, undefined


def missing_providers(game_text, native_texts):
    defined, undefined = symbols(game_text)
    for text in native_texts:
        defined.update(symbols(text)[0])
    providers = []
    for name in sorted(undefined - defined):
        if not name.startswith(('ps2_stubs::', 'ps2_syscalls::')):
            continue
        match = PROVIDER.fullmatch(name)
        if not match:
            raise ValueError('Unsupported native provider signature: ' + name)
        providers.append(match.groups())
    return providers


def render(providers):
    lines = ['// DIAGNOSTIC ONLY: unavailable providers always stop, never fake success.',
             '#include "native_aot_runtime.hpp"', '#include <stdexcept>', '']
    for namespace, function in providers:
        if namespace not in ('ps2_stubs', 'ps2_syscalls') or not IDENTIFIER.fullmatch(function):
            raise ValueError('Invalid provider name')
        lines.extend([f'namespace {namespace} {{',
                      f'void {function}(uint8_t *, R5900Context *, PS2Runtime *)',
                      '{ throw std::runtime_error("Native provider not implemented: '
                      f'{namespace}::{function}"); }}', '}'])
    return '\n'.join(lines) + '\n'


def read_nm(executable, archive):
    if not archive.is_file():
        raise ValueError('Missing archive: ' + str(archive))
    return subprocess.run([str(executable), '-C', str(archive)], check=True,
                          capture_output=True, text=True, encoding='utf-8').stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nm', required=True, type=Path)
    parser.add_argument('--game-archive', required=True, type=Path)
    parser.add_argument('--native-archive', action='append', default=[], type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if not args.nm.is_file():
        parser.error('Missing nm executable')
    providers = missing_providers(read_nm(args.nm, args.game_archive),
                                  [read_nm(args.nm, p) for p in args.native_archive])
    args.output.write_text(render(providers), encoding='utf-8')
    print(f'Generated {len(providers)} fail-closed diagnostic providers')


if __name__ == '__main__':
    main()
