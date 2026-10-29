#!/usr/bin/env python3
"""Local safety tests; these do not compile or execute the experimental DLL."""
import hashlib
import importlib.util
import json
import pathlib
import shutil
import tempfile
import unittest
import types
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
        (prov / 'compiler-packages.txt').write_bytes(b'identical verified compiler packages\n')
        (prov / 'variant.txt').write_text('refined\n')
        (prov / 'compiler-mode.txt').write_bytes(stage.COMPILER_MODE['refined'])
        for name in ('pacman-before.txt', 'pacman-after.txt'):
            (prov / name).write_text('gcc identical\n')
        for name in ('msys2-runtime-3.6.11-4.src.tar.zst', 'console-before.cc', 'console-after.cc'):
            # Hash fixtures are byte contracts, not platform-native text files.
            (prov / name).write_bytes(b'fixture\n')
        fixture_hash = hashlib.sha256(b'fixture\n').hexdigest()
        for filename in ('console.cc', 'termios.cc', 'fhandler.h'):
            for suffix in ('.before', '.after'):
                (prov / (filename + suffix)).write_bytes(b'fixture\n')
        file_hashes = {v: {f: fixture_hash for f in ('console.cc', 'termios.cc', 'fhandler.h')} for v in ('stock', 'refined')}
        patcher = patch.object(stage, 'SOURCE_FILES_SHA256', file_hashes)
        patcher.start()
        self.addCleanup(patcher.stop)
        patcher = patch.object(stage, 'ORIGINAL_SOURCE_FILES_SHA256', file_hashes['stock'])
        patcher.start()
        self.addCleanup(patcher.stop)
        for name in ('SOURCE_SHA256', 'BEFORE_SHA256', 'STOCK_SHA256', 'AFTER_SHA256'):
            patcher = patch.object(stage, name, fixture_hash)
            patcher.start()
            self.addCleanup(patcher.stop)
        shutil.copy2(HERE.parent / '0006-refined-prefix.patch', prov)
        shutil.copy2(HERE.parent / '0000-utils-const-compat.patch', prov)
        shutil.copy2(HERE / 'identities.json', prov / 'passive-identities.json')
        shutil.copy2(HERE / 'refined.patch', prov / 'passive.patch')
        shutil.copy2(HERE / 'recorder.hpp', prov / 'stormtty-passive-path.h')
        (prov / 'diagnostic-dll-pe.txt').write_text('stormtty_path_init_v1\nstormtty_path_handler_v1\n')
        self.linked = dict(scope='fixed-diagnostic-linked-preflight', variant='refined', linked_code_gate_passed=True,
                           dll_sha256=stage.sha(self.build / 'experimental-msys-2.0.dll'), production_qualified=False, fixtures_executed=False)
        (prov / 'linked-preflight.json').write_text(json.dumps(self.linked))
        (prov / 'hashes.txt').write_text(stage.sha(self.build / 'experimental-msys-2.0.dll') + '  dll\n')

    def test_stage_marks_experimental_and_preserves_original(self):
        stage.stage(self.base, self.build, self.dest)
        self.assertEqual(stage.sha(self.base / 'usr/bin/msys-2.0.dll'), self.base_hash)
        result = json.loads((self.dest / 'stormtty-shell-manifest.json').read_text())
        self.assertFalse(result['default_activation_allowed'])
        self.assertTrue(result['qualification_only'])
        self.assertEqual(result['experimental_runtime']['diagnostic_kind'], 'passive-path-v1')
        self.assertTrue(result['experimental_runtime']['behavior_patch_applied'])
        self.assertTrue(result['experimental_runtime']['retirement_patch_applied'])
        self.assertTrue(result['experimental_runtime']['trace_enabled'])
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

    def test_diagnostic_drift_rejected(self):
        for name in ('passive-identities.json', 'passive.patch', 'stormtty-passive-path.h', 'diagnostic-dll-pe.txt'):
            path = self.build / 'provenance' / name
            original = path.read_bytes()
            path.write_bytes(b'drift')
            with self.assertRaises(ValueError):
                stage.stage(self.base, self.build, self.dest)
            self.assertFalse(self.dest.exists())
            path.write_bytes(original)

    def test_linked_gate_rejects_drift_and_false_success(self):
        for field, value in (('linked_code_gate_passed',False),('dll_sha256','wrong'),('variant','stock'),('production_qualified',True),('fixtures_executed',True)):
            changed = dict(self.linked);changed[field] = value
            (self.build / 'provenance/linked-preflight.json').write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError,'Linked native'):
                stage.stage(self.base,self.build,self.dest)
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

    def test_common_utility_patch_corruption_rejected(self):
        (self.build / 'provenance/0000-utils-const-compat.patch').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'Common utility patch provenance mismatch'):
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

    def test_base_symlink_root_rejected_before_resolution(self):
        alias = self.base.parent / 'base-alias'
        alias.symlink_to(self.base, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'Symlink/reparse'):
            stage.stage(alias, self.build, self.dest)
        self.assertFalse(self.dest.exists())

    def test_provenance_symlink_rejected(self):
        (self.build / 'provenance' / 'escape').symlink_to(self.base, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'Symlink/reparse'):
            stage.stage(self.base, self.build, self.dest)
        self.assertFalse(self.dest.exists())

    def test_mock_windows_junction_attributes_rejected(self):
        target = self.base / 'usr'
        original = pathlib.Path.lstat
        def lstat(path):
            info = original(path)
            if path == target:
                return types.SimpleNamespace(st_mode=info.st_mode, st_file_attributes=0x400)
            return info
        with patch.object(pathlib.Path, 'lstat', lstat):
            with self.assertRaisesRegex(ValueError, 'Symlink/reparse'):
                stage.stage(self.base, self.build, self.dest)
        self.assertFalse(self.dest.exists())

    def test_dangling_destination_alias_rejected(self):
        outside = self.base.parent / 'outside-new'
        self.dest.symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'Destination must be absent'):
            stage.stage(self.base, self.build, self.dest)
        self.assertFalse(outside.exists())

    def test_destination_parent_alias_rejected(self):
        alias = self.base.parent / 'destination-alias'
        alias.symlink_to(self.base.parent, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'Symlink/reparse'):
            stage.stage(self.base, self.build, alias / 'copy')
        self.assertFalse(self.dest.exists())

    def test_failed_behavior_patch_variant_rejected(self):
        (self.build / 'provenance/variant.txt').write_text('patched\n')
        with self.assertRaisesRegex(ValueError, 'Unknown experimental build variant'):
            stage.stage(self.base, self.build, self.dest)
        self.assertFalse(self.dest.exists())

    def test_traced_termios_corruption_rejected(self):
        (self.build / 'provenance/termios.cc.after').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'Unexpected traced source'):
            stage.stage(self.base, self.build, self.dest)
        self.assertFalse(self.dest.exists())

    def test_instrumented_variant_rejected(self):
        (self.build / 'provenance/variant.txt').write_text('instrumented\n')
        with self.assertRaisesRegex(ValueError, 'Unknown experimental build variant'):
            stage.stage(self.base, self.build, self.dest)
        self.assertFalse(self.dest.exists())

    def test_trace_macro_rejected(self):
        (self.build / 'provenance/compiler-mode.txt').write_bytes(
            b'CXX=stormtty-gxx-15 -std=gnu++17 -DSTORMTTY_MSYS_RESIZE_TRACE=1\n__cplusplus=201703L\n')
        with self.assertRaisesRegex(ValueError, 'compiler dialect'):
            stage.stage(self.base, self.build, self.dest)

    def test_stock_has_no_behavior_patch_but_same_trace(self):
        (self.build / 'provenance/variant.txt').write_text('stock\n')
        shutil.copy2(HERE / 'stock.patch', self.build / 'provenance/passive.patch')
        self.linked['variant'] = 'stock'
        (self.build / 'provenance/linked-preflight.json').write_text(json.dumps(self.linked))
        info = stage.stage(self.base, self.build, self.dest)
        self.assertFalse(info['behavior_patch_applied'])
        self.assertFalse(info['retirement_patch_applied'])
        self.assertTrue(info['trace_enabled'])

    def test_wrong_source_rejected(self):
        (self.build / 'provenance/console-after.cc').write_text('wrong source\n')
        with self.assertRaises(ValueError):
            stage.stage(self.base, self.build, self.dest)


if __name__ == '__main__':
    unittest.main()
