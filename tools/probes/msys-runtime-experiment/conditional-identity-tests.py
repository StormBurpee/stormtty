#!/usr/bin/env python3
"""Portable candidate-runner contracts only; no native runtime execution."""
import importlib.util
import json
import pathlib
import tempfile
import types
import unittest
from unittest.mock import patch

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('cursor_compare', HERE / 'conditional-identities.py')
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


class CursorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)

    def make_identities(self):
        roots = {name: self.root / name for name in ('original', *compare.VARIANTS)}
        source = b'synthetic-source\n'
        source_hash = __import__('hashlib').sha256(source).hexdigest()
        mapping = {name: {file: source_hash for file in ('console.cc', 'termios.cc', 'fhandler.h')}
                   for name in compare.VARIANTS}
        archive_patcher = patch.object(compare.stage, 'SOURCE_SHA256', source_hash)
        archive_patcher.start()
        self.addCleanup(archive_patcher.stop)
        original = patch.object(compare.stage, 'ORIGINAL_SOURCE_FILES_SHA256', next(iter(mapping.values())))
        original.start()
        self.addCleanup(original.stop)
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
                    'variant': name, 'diagnostic_kind': 'conditional-retirement-v1',
                    'behavior_patch_applied': name == 'conditional', 'retirement_patch_applied': name == 'conditional', 'trace_enabled': False,
                    'experimental_dll_sha256': compare.sha(dll),
                    'original_dll_sha256': compare.sha(roots['original'] / 'usr/bin/msys-2.0.dll'),
                    'patch_sha256': compare.sha(HERE / '0005-conditional-retirement.patch'),
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
        self.assertEqual(set(compare.identities(roots)), set(('original', *compare.VARIANTS)))
        path = roots['conditional'] / 'stormtty-shell-manifest.json'
        manifest = json.loads(path.read_text())
        manifest['experimental_runtime']['trace_enabled'] = True
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'behavior patch'):
            compare.identities(roots)

    def test_compiler_mode_cannot_be_self_attested(self):
        roots = self.make_identities()
        mode = roots['conditional'] / 'experimental-runtime-provenance/compiler-mode.txt'
        mode.write_bytes(b'different compiler flags\n')
        path = roots['conditional'] / 'stormtty-shell-manifest.json'
        manifest = json.loads(path.read_text())
        manifest['experimental_runtime']['compiler_mode_sha256'] = compare.sha(mode)
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'compiler mode changed'):
            compare.identities(roots)


if __name__ == '__main__':
    unittest.main()
