#!/usr/bin/env python3
"""Portable trace-runner contracts only; no native runtime execution."""
import importlib.util
import json
import pathlib
import tempfile
import types
import unittest
from unittest.mock import patch

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('trace_compare', HERE / 'compare-trace.py')
IDENTITY = b'1 123 456 0 11 -1 0 0 0 0 321 321\n'
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


class TraceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)

    def snapshot(self, content):
        path = self.root / 'resize-123.log'
        path.write_bytes(content)
        return compare.inspect_snapshot(self.root)

    def test_empty_control_and_numeric_instrumented(self):
        self.assertEqual(compare.inspect_snapshot(self.root)['records'], 0)
        result = self.snapshot(IDENTITY + b'2 123 456 1191 1 0 100 30 111 35 0 0\n')
        self.assertEqual(result['records'], 2)
        self.assertEqual(result['incomplete_suffix_bytes'], 0)

    def test_thread_order_not_assumed(self):
        result = self.snapshot(b'2 123 456 1191 1 0 100 30 111 35 0 0\n' + IDENTITY)
        self.assertEqual(result['records'], 2)

    def test_partial_suffix_is_explicit(self):
        result = self.snapshot(IDENTITY + b'2 123 456 1191 1 0 100 30 111 35 0 0\n3 123')
        self.assertEqual(result['records'], 2)
        self.assertEqual(result['incomplete_suffix_bytes'], 5)

    def test_bad_records_rejected(self):
        for record in (b'command text\n', b'1 999 1 1 1 1 1 1 1 1 1 1\n',
                       b'1 123 1 1 12 1 1 1 1 1 1 1\n', b'2 123 1 1 1 1 1 1 1 1 1 1\n',
                       b'1 123 1 1 1 1 1 1 1 1 1 1\n' * 2):
            with self.subTest(record=record), self.assertRaises(ValueError):
                self.snapshot(record)

    def test_identity_required_and_validated(self):
        result = self.snapshot(IDENTITY)
        self.assertEqual(result['files'][0]['process_identity']['posix_pid'], 321)
        for data in (b'1 123 456 1 1 0 100 30 111 35 0 0\n',
                     IDENTITY.replace(b'321 321', b'0 321'),
                     IDENTITY + IDENTITY.replace(b'1 123', b'2 123')):
            with self.assertRaisesRegex(ValueError, 'identity'):
                self.snapshot(data)

    def test_unexpected_file_and_budget(self):
        (self.root / 'other').touch()
        with self.assertRaises(ValueError):
            compare.inspect_snapshot(self.root)
        (self.root / 'other').unlink()
        with self.assertRaises(ValueError):
            self.snapshot(b'0' * (2 * 1024 * 1024 + 1))

    def test_file_count_and_symlink_rejected(self):
        for n in range(33):
            (self.root / f'resize-{n}.log').touch()
        with self.assertRaisesRegex(ValueError, 'budget'):
            compare.inspect_snapshot(self.root)
        for path in self.root.iterdir():
            path.unlink()
        target = self.root / 'target'
        target.touch()
        link = self.root / 'resize-123.log'
        try:
            link.symlink_to(target)
        except OSError:
            self.skipTest('symlink creation unavailable')
        with self.assertRaises(ValueError):
            compare.inspect_snapshot(self.root)

    def test_workloads_do_not_mix(self):
        args = types.SimpleNamespace(probe=self.root / 'probe', fixture=self.root / 'fixture',
                                     original=self.root / 'bundle', provider=self.root / 'provider', workload='idle')
        command = compare.case_command(args, 'original', 'inbox', self.root / 'scratch', self.root / 'trace')
        self.assertEqual(command[-1], '--resize-trace-idle')
        self.assertNotIn('--resize-repeats', command)
        args.workload = 'repeated'
        command = compare.case_command(args, 'original', 'packaged', self.root / 'scratch', self.root / 'trace')
        self.assertEqual(command[-2:], ['--resize-repeats', '50'])
        self.assertNotIn('--resize-trace-idle', command)

    def make_identities(self):
        roots = {name: self.root / name for name in compare.VARIANTS}
        source = b'synthetic-source\n'
        source_hash = __import__('hashlib').sha256(source).hexdigest()
        mapping = {name: {file: source_hash for file in ('console.cc', 'termios.cc', 'fhandler.h')}
                   for name in compare.VARIANTS[1:]}
        archive_patcher = patch.object(compare.stage, 'SOURCE_SHA256', source_hash)
        archive_patcher.start()
        self.addCleanup(archive_patcher.stop)
        patcher = patch.object(compare.stage, 'SOURCE_FILES_SHA256', mapping)
        patcher.start()
        self.addCleanup(patcher.stop)
        for name, root in roots.items():
            (root / 'usr/bin').mkdir(parents=True)
            dll, zsh = root / 'usr/bin/msys-2.0.dll', root / 'usr/bin/zsh.exe'
            dll.write_bytes(('MZ' + name).encode())
            zsh.write_bytes(b'MZfixed-zsh')
            manifest = {'files': [{'path': str(path.relative_to(root)).replace('\\', '/'),
                                  'directory': False, 'sha256': compare.sha(path)} for path in (dll, zsh)]}
            if name != 'original':
                prov = root / 'experimental-runtime-provenance'
                prov.mkdir()
                for file in ('pacman-before.txt', 'pacman-after.txt', 'compiler-packages.txt', 'msys2-runtime-3.6.11-4.src.tar.zst'):
                    (prov / file).write_bytes(source)
                (prov / 'compiler-mode.txt').write_bytes(compare.stage.COMPILER_MODE[name])
                for file in mapping[name]:
                    (prov / (file + '.before')).write_bytes(source)
                    (prov / (file + '.after')).write_bytes(source)
                manifest.update(qualification_only=True, default_activation_allowed=False)
                manifest['experimental_runtime'] = {
                    'variant': name, 'diagnostic_kind': 'resize-observation-v1',
                    'behavior_patch_applied': False, 'trace_enabled': name == 'instrumented',
                    'experimental_dll_sha256': compare.sha(dll),
                    'original_dll_sha256': compare.sha(roots['original'] / 'usr/bin/msys-2.0.dll'),
                    'patch_sha256': compare.sha(HERE / '0002-resize-observation-trace.patch'),
                    'common_utility_patch_sha256': compare.sha(HERE / '0000-utils-const-compat.patch'),
                    'compiler_dialect': 'gnu++17', 'compiler_version': '15.3.0',
                    'compiler_mode_sha256': compare.sha(prov / 'compiler-mode.txt'),
                    'source_archive_sha256': compare.stage.SOURCE_SHA256,
                    'source_files_sha256': mapping[name],
                    'build_environment_sha256': source_hash, 'compiler_packages_sha256': source_hash}
            (root / 'stormtty-shell-manifest.json').write_text(json.dumps(manifest))
        return roots

    def test_identity_valid_and_drift_rejected(self):
        roots = self.make_identities()
        self.assertEqual(set(compare.identities(roots)), set(compare.VARIANTS))
        path = roots['instrumented'] / 'stormtty-shell-manifest.json'
        manifest = json.loads(path.read_text())
        manifest['experimental_runtime']['behavior_patch_applied'] = True
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'behavior patch'):
            compare.identities(roots)

    def test_compiler_mode_cannot_be_self_attested(self):
        roots = self.make_identities()
        mode = roots['instrumented'] / 'experimental-runtime-provenance/compiler-mode.txt'
        mode.write_bytes(b'different compiler flags\n')
        path = roots['instrumented'] / 'stormtty-shell-manifest.json'
        manifest = json.loads(path.read_text())
        manifest['experimental_runtime']['compiler_mode_sha256'] = compare.sha(mode)
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'compiler mode changed'):
            compare.identities(roots)


if __name__ == '__main__':
    unittest.main()
