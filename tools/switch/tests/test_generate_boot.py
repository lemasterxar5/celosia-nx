import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import tomllib
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / 'generate_boot.py'


class BootGenerationTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('generate_boot', SCRIPT)
        self.mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.mod)
        self.temp = tempfile.TemporaryDirectory(dir=SCRIPT.parent / 'tests')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.disc = self.root / 'disc'
        self.disc.mkdir()
        (self.disc / 'SYSTEM.CNF').write_text('BOOT2 = cdrom0:\\SCUS_974.72;1\n')
        elf = bytearray(56)
        elf[:7] = b'\x7fELF\x01\x01\x01'
        struct.pack_into('<HHI', elf, 16, 2, 8, 1)
        struct.pack_into('<I', elf, 24, 0x100000)
        struct.pack_into('<H', elf, 40, 52)
        (self.disc / 'SCUS_974.72').write_bytes(elf)
        xff = bytearray(256)
        xff[:4] = b'xff2'
        struct.pack_into('<I', xff, 0x14, len(xff))
        struct.pack_into('<I', xff, 0x24, 1)
        struct.pack_into('<I', xff, 0x54, 0x80)
        struct.pack_into('<I', xff, 0x58, 0xA0)
        struct.pack_into('<IIIBBH', xff, 0x80, 1, 0x100000, 4, 0x12, 0, 0xFFF1)
        xff[0xA0:0xA6] = b'\0boot\0'
        (self.disc / 'KERNEL.XFF').write_bytes(xff)
        self.out = self.root / 'new output'

    def mapper(self, boot, symbols, out):
        self.assertEqual(Path(symbols).read_text(), '00100000 boot\n')
        Path(out).write_text('Name,Start,End,Size\nboot,0x100000,0x100004,4\n')

    def test_boot_manifest_and_toml_roundtrip(self):
        with patch.object(self.mod.funcmap_elf, 'main', side_effect=self.mapper):
            result = self.mod.generate(self.disc, self.out)
        config = tomllib.loads((self.out / 'config.toml').read_text())
        self.assertEqual(config['general']['input'], (self.disc / 'SCUS_974.72').resolve().as_posix())
        self.assertEqual(result['scope'], 'boot-only')
        self.assertFalse(result['full_game_ready'])
        self.assertEqual(json.loads((self.out / 'manifest.json').read_text()), result)
        self.assertEqual(result['required_captures'], ['kernel.bin', 'manager.bin', 'gamecore.bin'])

    def test_existing_output_is_never_overwritten(self):
        self.out.mkdir()
        marker = self.out / 'keep'
        marker.write_text('original')
        with self.assertRaises(FileExistsError):
            self.mod.generate(self.disc, self.out)
        self.assertEqual(marker.read_text(), 'original')

    def test_rejects_wrong_elf_and_wrong_edition_before_execution(self):
        for name, contents in [('SCUS_974.72', b'not ELF'), ('SYSTEM.CNF', b'BOOT2 = cdrom0:\\SLES_000.00;1')]:
            path = self.disc / name
            original = path.read_bytes()
            path.write_bytes(contents)
            with patch.object(self.mod.subprocess, 'run') as run:
                with self.assertRaises(ValueError):
                    self.mod.generate(self.disc, self.out, recompiler=sys.executable)
                run.assert_not_called()
            self.assertFalse(self.out.exists())
            path.write_bytes(original)

    def test_rejects_invalid_xff(self):
        (self.disc / 'KERNEL.XFF').write_bytes(b'bad')
        with self.assertRaises(ValueError):
            self.mod.generate(self.disc, self.out)
        self.assertFalse(self.out.exists())

    def test_rejects_duplicate_boot_directives_before_output(self):
        (self.disc / 'SYSTEM.CNF').write_text(
            'BOOT2 = cdrom0:\\SCUS_974.72;1\nBOOT2 = cdrom0:\\SLES_000.00;1\n')
        with patch.object(self.mod.subprocess, 'run') as run, patch.object(self.mod.funcmap_elf, 'main', side_effect=self.mapper):
            with self.assertRaises(ValueError):
                self.mod.generate(self.disc, self.out, recompiler=sys.executable)
            run.assert_not_called()
        self.assertFalse(self.out.exists())

    def test_rejects_wrong_architecture_endian_and_truncated_tables(self):
        path = self.disc / 'SCUS_974.72'
        original = path.read_bytes()
        mutations = [(5, b'\x02'), (18, b'\x3e\x00'), (28, struct.pack('<I', 0x1000))]
        for offset, replacement in mutations:
            data = bytearray(original)
            data[offset:offset + len(replacement)] = replacement
            if offset == 28:
                struct.pack_into('<HH', data, 42, 32, 1)
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                self.mod.generate(self.disc, self.out)
            self.assertFalse(self.out.exists())

    def test_full_game_request_requires_real_captures(self):
        with self.assertRaisesRegex(ValueError, 'captur'):
            self.mod.generate(self.disc, self.out, full_game=True)
        self.assertFalse(self.out.exists())

    def test_rejects_unbacked_and_out_of_ram_executable_ranges(self):
        original = (self.disc / 'SCUS_974.72').read_bytes()
        # Validation only: never invoke funcmap_elf on a malicious range.
        for section_type, address, size in [(8, 0x100000, 0xFFFFFFFC),
                                             (1, 0x2000000, 4),
                                             (1, 0x100001, 4)]:
            elf = bytearray(original) + bytearray(40)
            struct.pack_into('<I', elf, 32, 56)
            struct.pack_into('<HHH', elf, 46, 40, 1, 0)
            struct.pack_into('<10I', elf, 56, 0, section_type, 4, address, 52, size, 0, 0, 4, 0)
            with self.subTest(section_type=section_type, address=address):
                with self.assertRaises(ValueError):
                    self.mod.validate_elf_tables(elf)

    def test_rejects_repeated_executable_segment_budget(self):
        elf = bytearray(17 * 1024 * 1024)
        struct.pack_into('<I', elf, 28, 52)
        struct.pack_into('<HH', elf, 42, 32, 2)
        for index in range(2):
            struct.pack_into('<8I', elf, 52 + 32 * index, 1, 0, 0, 0, len(elf), len(elf), 1, 4)
        # Calls validation alone, never the function mapper.
        with self.assertRaisesRegex(ValueError, 'budget'):
            self.mod.validate_elf_tables(elf)

    def test_recompiler_uses_config_cwd_and_no_shell(self):
        with patch.object(self.mod.funcmap_elf, 'main', side_effect=self.mapper), patch.object(self.mod.subprocess, 'run') as run:
            self.mod.generate(self.disc, self.out, recompiler=sys.executable)
        args, kwargs = run.call_args
        self.assertEqual(args[0], [str(Path(sys.executable).resolve()), 'config.toml'])
        self.assertEqual(kwargs['cwd'], self.out.resolve())
        self.assertTrue(kwargs['check'])
        self.assertNotIn('shell', kwargs)

    def test_toml_quote_escape(self):
        value = 'C:/strange "folder"/SCUS_974.72'
        self.assertEqual(tomllib.loads('input = ' + self.mod.toml_string(value))['input'], value)

    def test_rejects_oversized_inputs_before_reading(self):
        for name, limit in [('SYSTEM.CNF', 64 * 1024),
                            ('SCUS_974.72', 64 * 1024 * 1024),
                            ('KERNEL.XFF', 64 * 1024 * 1024)]:
            path = self.disc / name
            original = path.read_bytes()
            with path.open('wb') as sparse:
                sparse.truncate(limit + 1)
            try:
                # AssertionError prevents accidental large read even during RED.
                with patch.object(Path, 'read_bytes', side_effect=AssertionError('must reject before read')):
                    with self.assertRaisesRegex(ValueError, 'size budget'):
                        self.mod.generate(self.disc, self.out)
                self.assertFalse(self.out.exists())
            finally:
                path.write_bytes(original)


if __name__ == '__main__':
    unittest.main()
