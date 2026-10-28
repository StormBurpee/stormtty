#!/usr/bin/env python3
"""Local safety tests; these do not compile or execute the experimental DLL."""
import hashlib
import importlib.util
import json
import pathlib
import shutil
import tempfile
import unittest
from unittest.mock import patch

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('experiment_stage', HERE / 'stage.py')
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)


class StagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = pathlib.Path(self.temp.name)
        self.base, self.build, self.dest = [root / name for name in ('base', 'build', 'copy')]
        dll = self.base / 'usr/bin/msys-2.0.dll'
        dll.parent.mkdir(parents=True)
        dll.write_bytes(b'MZoriginal-fixture')
        self.base_hash = stage.sha(dll)
        manifest = {'bundle_id': 'test', 'files': [{'path': 'usr/bin/msys-2.0.dll',
                    'directory': False, 'sha256': self.base_hash, 'bytes': dll.stat().st_size}]}
        (self.base / 'stormtty-shell-manifest.json').write_text(json.dumps(manifest))
        prov = self.build / 'provenance'
        prov.mkdir(parents=True)
        (self.build / 'experimental-msys-2.0.dll').write_bytes(b'MZcandidate-fixture')
        (prov / 'variant.txt').write_text('patched\n')
        (prov / 'compiler-mode.txt').write_bytes(b'CXX=g++ -std=gnu++17\n__cplusplus=201703L\n')
        for name in ('pacman-before.txt', 'pacman-after.txt'):
            (prov / name).write_text('gcc identical\n')
        for name in ('msys2-runtime-3.6.11-4.src.tar.zst', 'console-before.cc', 'console-after.cc'):
            # Hash fixtures are byte contracts, not platform-native text files.
            (prov / name).write_bytes(b'fixture\n')
        fixture_hash = hashlib.sha256(b'fixture\n').hexdigest()
        for name in ('SOURCE_SHA256', 'BEFORE_SHA256', 'AFTER_SHA256'):
            patcher = patch.object(stage, name, fixture_hash)
            patcher.start()
            self.addCleanup(patcher.stop)
        shutil.copy2(HERE / '0001-retire-resize-before-signal.patch', prov)
        (prov / 'hashes.txt').write_text(stage.sha(self.build / 'experimental-msys-2.0.dll') + '  dll\n')

    def test_stage_marks_experimental_and_preserves_original(self):
        stage.stage(self.base, self.build, self.dest)
        self.assertEqual(stage.sha(self.base / 'usr/bin/msys-2.0.dll'), self.base_hash)
        result = json.loads((self.dest / 'stormtty-shell-manifest.json').read_text())
        self.assertFalse(result['default_activation_allowed'])
        self.assertTrue(result['qualification_only'])
        self.assertEqual(result['native_qualification']['status'], 'unverified-experimental')
        self.assertTrue((self.dest / 'experimental-runtime-provenance/original-bundle-manifest.json').is_file())

    def test_hash_controlled_fixture_bytes_are_platform_independent(self):
        for name in ('msys2-runtime-3.6.11-4.src.tar.zst', 'console-before.cc', 'console-after.cc'):
            path = self.build / 'provenance' / name
            self.assertEqual(path.read_bytes(), b'fixture\n')
            self.assertEqual(stage.sha(path), hashlib.sha256(b'fixture\n').hexdigest())

    def test_newline_translation_is_not_accepted_as_same_source(self):
        path = self.build / 'provenance/msys2-runtime-3.6.11-4.src.tar.zst'
        path.write_bytes(b'fixture\r\n')
        with self.assertRaisesRegex(ValueError, 'source archive provenance mismatch'):
            stage.stage(self.base, self.build, self.dest)
        self.assertFalse(self.dest.exists())

    def test_existing_destination_rejected(self):
        self.dest.mkdir()
        with self.assertRaises(ValueError):
            stage.stage(self.base, self.build, self.dest)

    def test_base_corruption_rejected(self):
        (self.base / 'usr/bin/msys-2.0.dll').write_bytes(b'MZchanged')
        with self.assertRaises(ValueError):
            stage.stage(self.base, self.build, self.dest)
        self.assertFalse(self.dest.exists())

    def test_candidate_corruption_rejected(self):
        (self.build / 'experimental-msys-2.0.dll').write_bytes(b'MZchanged')
        with self.assertRaises(ValueError):
            stage.stage(self.base, self.build, self.dest)

    def test_toolchain_drift_rejected(self):
        (self.build / 'provenance/pacman-after.txt').write_text('gcc changed\n')
        with self.assertRaises(ValueError):
            stage.stage(self.base, self.build, self.dest)

    def test_wrong_compiler_dialect_rejected(self):
        (self.build / 'provenance/compiler-mode.txt').write_bytes(b'CXX=g++\n__cplusplus=202002L\n')
        with self.assertRaisesRegex(ValueError, 'compiler dialect'):
            stage.stage(self.base, self.build, self.dest)

    def test_wrong_source_rejected(self):
        (self.build / 'provenance/console-after.cc').write_text('wrong source\n')
        with self.assertRaises(ValueError):
            stage.stage(self.base, self.build, self.dest)


if __name__ == '__main__':
    unittest.main()
