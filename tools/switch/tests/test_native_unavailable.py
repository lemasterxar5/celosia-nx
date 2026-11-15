import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / 'generate-native-unavailable.py'
spec = importlib.util.spec_from_file_location('native_unavailable', SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class NativeUnavailableTests(unittest.TestCase):
    signature = '(unsigned char*, R5900Context*, PS2Runtime*)'

    def symbol(self, kind, name):
        return f'                 {kind} {name}{self.signature}\n'

    def test_only_missing_native_provider_namespaces(self):
        game = self.symbol('U', 'ps2_stubs::sceRead') + ' U sqrtf\n U std::bad_alloc::what() const\n'
        self.assertEqual(module.missing_providers(game, []), [('ps2_stubs', 'sceRead')])

    def test_core_definition_excluded(self):
        game = self.symbol('U', 'ps2_syscalls::CreateSema')
        core = self.symbol('T', 'ps2_syscalls::CreateSema')
        self.assertEqual(module.missing_providers(game, [core]), [])

    def test_game_definition_excluded(self):
        text = self.symbol('U', 'ps2_stubs::malloc') + self.symbol('T', 'ps2_stubs::malloc')
        self.assertEqual(module.missing_providers(text, []), [])

    def test_deduplicated_sorted(self):
        text = self.symbol('U', 'ps2_stubs::z') * 2 + self.symbol('U', 'ps2_stubs::a')
        self.assertEqual(module.missing_providers(text, []), [('ps2_stubs', 'a'), ('ps2_stubs', 'z')])

    def test_weak_undefined_not_treated_as_definition(self):
        text = self.symbol('U', 'ps2_stubs::read')
        self.assertEqual(module.missing_providers(text, [self.symbol('w', 'ps2_stubs::read')]), [('ps2_stubs', 'read')])

    def test_weak_definition_excluded(self):
        text = self.symbol('U', 'ps2_stubs::read')
        self.assertEqual(module.missing_providers(text, [self.symbol('W', 'ps2_stubs::read')]), [])

    def test_malformed_signature_rejected(self):
        with self.assertRaisesRegex(ValueError, 'signature'):
            module.missing_providers(' U ps2_stubs::read(int)\n', [])

    def test_unsafe_identifier_rejected(self):
        with self.assertRaises(ValueError):
            module.missing_providers(' U ps2_stubs::x;evil()' + self.signature, [])

    def test_nested_namespace_rejected(self):
        with self.assertRaises(ValueError):
            module.missing_providers(self.symbol('U', 'ps2_syscalls::nested::x'), [])

    def test_render_fail_closed(self):
        text = module.render([('ps2_syscalls', 'CreateThread')])
        self.assertIn('throw std::runtime_error', text)
        self.assertIn('Native provider not implemented: ps2_syscalls::CreateThread', text)
        self.assertNotIn('setReturn', text)
        self.assertNotIn('return 0', text)

    def test_render_rejects_injected_names(self):
        with self.assertRaises(ValueError):
            module.render([('other', 'unsafe')])

    def test_diagnostic_marked(self):
        self.assertIn('DIAGNOSTIC ONLY', module.render([]))

    def test_missing_archive_rejected_before_nm(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(ValueError, 'Missing archive'):
                module.read_nm(Path('not-used'), Path(folder) / 'missing.a')

    def test_nm_argv_no_shell(self):
        with tempfile.TemporaryDirectory() as folder:
            archive = Path(folder) / 'name with spaces.a'
            archive.write_bytes(b'archive')
            with patch.object(module.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, 'nm output')) as run:
                self.assertEqual(module.read_nm(Path('nm.exe'), archive), 'nm output')
                self.assertEqual(run.call_args.args[0], ['nm.exe', '-C', str(archive)])
                self.assertTrue(run.call_args.kwargs['check'])
                self.assertNotIn('shell', run.call_args.kwargs)

    def test_nm_failure_propagates(self):
        with tempfile.TemporaryDirectory() as folder:
            archive = Path(folder) / 'input.a'
            archive.write_bytes(b'archive')
            with patch.object(module.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, ['nm'])):
                with self.assertRaises(subprocess.CalledProcessError):
                    module.read_nm(Path('nm.exe'), archive)

    def test_cli_excludes_supplied_core(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            nm = root / 'nm.exe'
            nm.write_bytes(b'placeholder')
            output = root / 'providers.cpp'
            game = self.symbol('U', 'ps2_stubs::read') + self.symbol('U', 'ps2_syscalls::CreateSema')
            core = self.symbol('T', 'ps2_syscalls::CreateSema')
            args = ['generator', '--nm', str(nm), '--game-archive', 'game.a',
                    '--native-archive', 'core.a', '--output', str(output)]
            with patch('sys.argv', args), patch.object(module, 'read_nm', side_effect=[game, core]):
                module.main()
            text = output.read_text(encoding='utf8')
            self.assertIn('ps2_stubs::read', text)
            self.assertNotIn('CreateSema', text)


if __name__ == '__main__':
    unittest.main()
