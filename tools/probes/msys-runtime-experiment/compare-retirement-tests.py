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
spec = importlib.util.spec_from_file_location('cursor_compare', HERE / 'compare-retirement.py')
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


class CursorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)

    def summary(self, **updates):
        values = dict(code=0, phase=50, expected_cols=100, expected_rows=30, observed_cols=100, observed_rows=30)
        values.update(substage=0, protocol_step=0, reason=0, write_result=0, expected_prompt_sequence=0,
                      prompt_status_known=0, prompt_status=0, child_exit_known=0, child_exit_code=0, resize_accepted=1)
        values.update(updates)
        return ('RESIZE_CASE_SUMMARY ' + ' '.join(f'{k}={v}' for k,v in values.items()) + '\n').encode()

    def test_summary_success_failure_and_rejection(self):
        self.assertEqual(compare.parse_summary(self.summary(), 0, 'repeated')['code'], 0)
        self.assertEqual(compare.parse_summary(self.summary(code=4, phase=2, observed_cols=111), 1, 'repeated')['code'], 4)
        for data in (b'', self.summary()*2, self.summary(code=9), self.summary(phase=51), self.summary(observed_cols=99999),
                     self.summary()+b'RESIZE_CASE_SUMMARY private text\n', self.summary().replace(b'code=0', b'code=-1')):
            with self.subTest(data=data), self.assertRaises(ValueError):
                compare.parse_summary(data, 0, 'repeated')
        for data in (self.summary(code=4), self.summary(phase=1), self.summary(observed_rows=31)):
            with self.assertRaises(ValueError):
                compare.parse_summary(data, 0, 'repeated')
        self.assertEqual(compare.parse_summary(b'private text ignored\n'+self.summary(), 0, 'repeated')['code'],0)

    def test_failure_grid_evidence_is_coherent(self):
        data = self.summary(code=1, phase=0, expected_cols=0, expected_rows=0, observed_cols=0, observed_rows=0)
        self.assertEqual(compare.parse_summary(data, 1, 'repeated')['phase'], 0)
        data = self.summary(code=4, phase=3, expected_cols=111, expected_rows=35, observed_cols=0, observed_rows=0)
        self.assertEqual(compare.parse_summary(data, 1, 'repeated')['observed_cols'], 0)
        for data in (self.summary(code=1, phase=0), self.summary(code=4, phase=3),
                     self.summary(code=4, phase=2, observed_cols=0),
                     self.summary(code=4, phase=2, observed_rows=0)):
            with self.subTest(data=data), self.assertRaises(ValueError):
                compare.parse_summary(data, 1, 'repeated')

    def test_protocol_fields_are_required_unique_and_bounded(self):
        for field, value in [('substage', 5), ('protocol_step', 5), ('reason', 5), ('write_result', 5),
                             ('expected_prompt_sequence', 4294967296), ('prompt_status_known', 2),
                             ('prompt_status', 256), ('child_exit_known', 2), ('child_exit_code', 4294967296),
                             ('resize_accepted', 2)]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                compare.parse_summary(self.summary(code=3, **{field: value}), 1, 'repeated')
        for data in (self.summary().replace(b' substage=0', b''), self.summary().replace(b' substage=0', b' substage=0 substage=0'),
                     self.summary().replace(b' child_exit_code=0', b' child_exit_code=-1'),
                     self.summary() + b'RESIZE_CASE_SUMMARYmalformed\n'):
            with self.subTest(data=data), self.assertRaises(ValueError):
                compare.parse_summary(data, 0, 'repeated')

    def test_known_unknown_status_and_exit(self):
        for data in (self.summary(code=3, prompt_status=1), self.summary(code=3, child_exit_code=1),
                     self.summary(child_exit_known=1), self.summary(resize_accepted=0)):
            with self.subTest(data=data), self.assertRaises(ValueError):
                compare.parse_summary(data, 1 if b'code=3 ' in data else 0, 'repeated')
        data = self.summary(code=3, child_exit_known=1, child_exit_code=4294967295)
        self.assertEqual(compare.parse_summary(data, 1, 'repeated')['child_exit_code'], 4294967295)

    def test_protocol_step_reason_coherence(self):
        for updates in (dict(protocol_step=1, reason=1, expected_prompt_sequence=1),
                        dict(protocol_step=4, reason=3, expected_prompt_sequence=9, prompt_status_known=1),
                        dict(protocol_step=2, reason=2, expected_prompt_sequence=9, write_result=3),
                        dict(protocol_step=3, reason=4, expected_prompt_sequence=9, write_result=1)):
            self.assertEqual(compare.parse_summary(self.summary(code=3, **updates), 1, 'repeated')['code'], 3)
        for updates in (dict(reason=1), dict(expected_prompt_sequence=1), dict(protocol_step=1, reason=1),
                        dict(protocol_step=1, reason=2, expected_prompt_sequence=1),
                        dict(protocol_step=1, reason=3, expected_prompt_sequence=1, prompt_status_known=1, prompt_status=19),
                        dict(protocol_step=4, reason=3, expected_prompt_sequence=9),
                        dict(protocol_step=2, reason=2, expected_prompt_sequence=9, write_result=1),
                        dict(protocol_step=3, reason=4, expected_prompt_sequence=9, prompt_status_known=1)):
            with self.subTest(updates=updates), self.assertRaises(ValueError):
                compare.parse_summary(self.summary(code=3, **updates), 1, 'repeated')

    def test_prespecified_round_order_excludes_original(self):
        self.assertEqual([compare.variant_order(i) for i in range(3)],
                         [('baseline','combined'), ('combined','baseline'), ('baseline','combined')])
        self.assertNotIn('original', compare.VARIANTS)

    def test_workloads_do_not_mix(self):
        args = types.SimpleNamespace(probe=self.root/'probe', fixture=self.root/'fixture',
                                     original=self.root/'bundle', provider=self.root/'provider', workload='idle')
        for workload, count, one_shot in [('idle','1',True),('repeated','50',False),('builtin-only','50',True)]:
            args.workload=workload
            command=compare.case_command(args,'original','inbox',self.root/'scratch')
            self.assertEqual(command[command.index('--resize-repeats')+1],count)
            self.assertEqual('--resize-one-shot' in command,one_shot)
            self.assertIn('--resize-summary',command)
            self.assertNotIn('--resize-trace-directory',command)

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
                    'variant': name, 'diagnostic_kind': 'cursor-retirement-v1',
                    'behavior_patch_applied': True, 'retirement_patch_applied': name == 'combined', 'trace_enabled': False,
                    'experimental_dll_sha256': compare.sha(dll),
                    'original_dll_sha256': compare.sha(roots['original'] / 'usr/bin/msys-2.0.dll'),
                    'patch_sha256': compare.sha(HERE / '0004-retirement-after-cursor.patch'),
                    'cursor_patch_sha256': compare.sha(HERE / '0003-modern-cursor-fill.patch'),
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
        path = roots['combined'] / 'stormtty-shell-manifest.json'
        manifest = json.loads(path.read_text())
        manifest['experimental_runtime']['trace_enabled'] = True
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'behavior patch'):
            compare.identities(roots)

    def test_compiler_mode_cannot_be_self_attested(self):
        roots = self.make_identities()
        mode = roots['combined'] / 'experimental-runtime-provenance/compiler-mode.txt'
        mode.write_bytes(b'different compiler flags\n')
        path = roots['combined'] / 'stormtty-shell-manifest.json'
        manifest = json.loads(path.read_text())
        manifest['experimental_runtime']['compiler_mode_sha256'] = compare.sha(mode)
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'compiler mode changed'):
            compare.identities(roots)


if __name__ == '__main__':
    unittest.main()
