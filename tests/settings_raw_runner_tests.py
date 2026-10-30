#!/usr/bin/env python3
"""Synthetic runner assessment only; never launches a native process."""
import importlib.util
from pathlib import Path
import unittest
from unittest import mock
import contextlib
import io
import json
import tempfile
import types

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('raw_runner', ROOT / 'tools/probes/run_settings_raw_boundary.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
HEX = (b'\x1b[200~PASTE_QUAL_0123456789_[x]_end\x1b[201~\r').hex()
POST = 'SETTINGS_RAW_POST separate_dispatch=1 closed_scene_presented=1 bracketed=1 resize_requested=3 resize_applied=3 resize_since_raw_ready=1 resize_before=2 client_width_before=960 client_width_after=1040 grid_columns_before=100 grid_columns_after=108 reader_termios=raw-noecho win32_input_mode=unobserved provider=Windows ConPTY (system)'
ROW = f'SETTINGS_RAW_RESULT verdict=PASS writer_complete=1 child_valid=1 child_exit=0 expected_hex={HEX} writer_hex={HEX} child_hex={HEX}'
FINAL = 'SETTINGS_RAW_FINAL verdict=PASS detail=; authored Settings-close boundary only, not ZLE or general qualification.'
CLEAN = 'Native smoke shell state cleanup: actors_closed=1 removed=1\nNative CI teardown: tick_ms=123 thread=456 stage=wWinMain locals destroyed; returning to CRT'
OUTPUT = '\n'.join((POST, ROW, FINAL, CLEAN)) + '\n'


def meta(**changes):
    return dict(timed_out=False, reaped=True, output_truncated=False, cleanup_error='', returncode=0, **changes)


class AssessmentTests(unittest.TestCase):
    def assess(self, output=OUTPUT, **changes):
        metadata = meta(); metadata.update(changes)
        result = runner.assess(metadata, output)
        self.assertIs(result['native_qualified'], False)
        self.assertIs(result['runtime_promotion_allowed'], False)
        return result

    def inconclusive(self, output=OUTPUT, **changes):
        self.assertEqual(self.assess(output, **changes)['boundary_verdict'], 'INCONCLUSIVE')

    def test_exact_single_boundary(self):
        result = self.assess()
        self.assertEqual(result['boundary_verdict'], 'PASS')
        self.assertEqual(result['resize_reproduction'], 'OBSERVED')

    def test_no_resize_does_not_claim_reproduction(self):
        result = self.assess(OUTPUT.replace('resize_since_raw_ready=1', 'resize_since_raw_ready=0'))
        self.assertEqual(result['boundary_verdict'], 'INCONCLUSIVE')
        self.assertEqual(result['resize_reproduction'], 'INCONCLUSIVE')

    def test_real_resize_consistency(self):
        for old, new in (('resize_requested=3', 'resize_requested=4'),
                         ('resize_before=2', 'resize_before=3'),
                         ('resize_before=2', 'resize_before=4'),
                         ('client_width_after=1040', 'client_width_after=960'),
                         ('client_width_after=1040', 'client_width_after=1039'),
                         ('grid_columns_after=108', 'grid_columns_after=100'),
                         ('grid_columns_after=108', 'grid_columns_after=99'),
                         ('resize_since_raw_ready=1', 'resize_since_raw_ready=0')):
            with self.subTest(field=old): self.inconclusive(OUTPUT.replace(old, new))
        self.inconclusive(OUTPUT.replace('resize_before=2 ', ''))

    def test_cleanup_and_runner_fail_closed(self):
        for change in ({'timed_out': True}, {'reaped': False}, {'output_truncated': True}, {'cleanup_error': 'not reaped'}, {'returncode': 2}):
            with self.subTest(change=change): self.inconclusive(**change)
        for token in CLEAN.splitlines():
            self.inconclusive(OUTPUT.replace(token, ''))
            self.inconclusive(OUTPUT.replace(token, 'spoof-prefix ' + token))

    def test_missing_duplicate_and_spoof_markers(self):
        for token in (POST, ROW, FINAL):
            with self.subTest(token=token):
                self.inconclusive(OUTPUT.replace(token, ''))
                self.inconclusive(OUTPUT + token + '\n')
                self.inconclusive(OUTPUT.replace(token, 'spoof-prefix ' + token))
        self.inconclusive(OUTPUT.replace(ROW, 'SETTINGS_RAW_RESULT verdict=PASS'))
        self.inconclusive(OUTPUT.replace('separate_dispatch=1', 'separate_dispatch=0'))

    def test_pass_requires_complete_exact_bytes(self):
        for field in ('writer_complete', 'child_valid'):
            self.inconclusive(OUTPUT.replace(field + '=1', field + '=0'))
        self.inconclusive(OUTPUT.replace('child_exit=0', 'child_exit=93'))
        for field in ('expected_hex', 'writer_hex', 'child_hex'):
            self.inconclusive(OUTPUT.replace(field + '=' + HEX, field + '=' + HEX[2:]))
        self.inconclusive(OUTPUT.replace(FINAL, FINAL.replace('verdict=PASS', 'verdict=INCONCLUSIVE_OR_MISMATCH')))

    def test_real_mismatch_preserved_without_promotion(self):
        output = OUTPUT.replace(ROW, ROW.replace('verdict=PASS', 'verdict=MISMATCH').replace('child_hex=' + HEX, 'child_hex=' + HEX[2:]))
        output = output.replace(FINAL, FINAL.replace('verdict=PASS detail=', 'verdict=INCONCLUSIVE_OR_MISMATCH detail=boundary differs'))
        self.assertEqual(self.assess(output, returncode=2)['boundary_verdict'], 'MISMATCH')
        self.inconclusive(output, returncode=0)

    def test_provider_identity(self):
        packaged = OUTPUT.replace('Windows ConPTY (system)', 'Windows ConPTY (packaged experimental 1.25.260930003; OpenConsole identity verified)')
        self.assertEqual(runner.assess(meta(), OUTPUT, 'inbox')['boundary_verdict'], 'PASS')
        self.assertEqual(runner.assess(meta(), packaged, 'packaged')['boundary_verdict'], 'PASS')
        for text, requested in ((OUTPUT, 'packaged'), (packaged, 'inbox'), (OUTPUT, 'unknown'),
                                (packaged.replace('Windows ConPTY (packaged experimental ', 'spoof '), 'packaged')):
            self.assertEqual(runner.assess(meta(), text, requested)['boundary_verdict'], 'INCONCLUSIVE')

    def test_malformed_mismatch_hex_rejected(self):
        base = OUTPUT.replace('SETTINGS_RAW_RESULT verdict=PASS', 'SETTINGS_RAW_RESULT verdict=MISMATCH')
        base = base.replace(FINAL, FINAL.replace('verdict=PASS detail=', 'verdict=INCONCLUSIVE_OR_MISMATCH detail=boundary differs'))
        for replacement in ('', '0', 'gg', 'AB', '00' * 1025):
            with self.subTest(child=replacement[:12]):
                self.inconclusive(base.replace('child_hex=' + HEX, 'child_hex=' + replacement), returncode=2)
        self.inconclusive(base.replace('writer_hex=' + HEX, 'writer_hex=0'), returncode=2)
        self.inconclusive(base.replace('writer_hex=' + HEX, 'writer_hex=' + '00' * 262145), returncode=2)

    def test_abnormal_child_inconclusive(self):
        output = OUTPUT.replace(ROW, ROW.replace('verdict=PASS', 'verdict=INCONCLUSIVE').replace('child_exit=0', 'child_exit=93'))
        output = output.replace(FINAL, FINAL.replace('verdict=PASS', 'verdict=INCONCLUSIVE_OR_MISMATCH'))
        self.inconclusive(output, returncode=2)


class FreshOutputTests(unittest.TestCase):
    def test_shared_artifacts_coexist_but_diagnostic_directory_is_exclusive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / 'build'; build.mkdir()
            shared = build / 'artifacts'; shared.mkdir()
            sentinel = shared / 'existing.txt'; sentinel.write_bytes(b'keep existing build artifact')
            (build / 'stormtty_settings_raw_probe.exe').write_bytes(b'not executable: synthetic identity only')
            runtime = root / 'runtime'; (runtime / 'usr/bin').mkdir(parents=True)
            for name in ('zsh.exe', 'msys-2.0.dll'):
                (runtime / 'usr/bin' / name).write_bytes(b'synthetic identity only')
            provider = root / 'provider'; provider.mkdir()
            calls = []
            def owned(command, **kwargs):
                calls.append((command, kwargs))
                output = OUTPUT if len(calls) == 1 else OUTPUT.replace('Windows ConPTY (system)', 'Windows ConPTY (packaged experimental 1.25.260930003; OpenConsole identity verified)')
                return types.SimpleNamespace(metadata=meta, output_tail=output.encode())
            module = types.SimpleNamespace()
            loader = types.SimpleNamespace(exec_module=lambda target: setattr(target, 'run_owned', owned))
            spec = types.SimpleNamespace(name='settings_owned_runner', loader=loader)
            argv = ['runner', '--build-dir', str(build), '--shell-runtime', str(runtime), '--console-runtime', str(provider)]
            with mock.patch.object(runner.sys, 'argv', argv), mock.patch.object(runner.sys, 'platform', 'win32'), \
                 mock.patch.object(runner.importlib.util, 'spec_from_file_location', return_value=spec), \
                 mock.patch.object(runner.importlib.util, 'module_from_spec', return_value=module), \
                 mock.patch.dict(runner.sys.modules), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(runner.main(), 0)
                # Production resolves build paths, including Windows 8.3 temp aliases.
                destination = build.resolve(strict=True) / 'settings-raw-artifacts'
                self.assertTrue(destination.is_dir())
                self.assertEqual(len(calls), 2)
                self.assertNotIn('--console-runtime', calls[0][0])
                self.assertIn('--console-runtime', calls[1][0])
                for command, kwargs in calls:
                    self.assertEqual(kwargs, dict(timeout=60, reap_timeout=10, tail_bytes=65536, output_dir=destination))
                    self.assertEqual(command.count('--smoke-settings-raw'), 1)
                self.assertEqual(sentinel.read_bytes(), b'keep existing build artifact')
                self.assertEqual(list(shared.iterdir()), [sentinel])
                before = {p.name: p.read_bytes() for p in destination.iterdir()}
                self.assertEqual(set(before), {'settings-raw-inbox.log', 'settings-raw-packaged.log', 'owned-processes.json'})
                self.assertEqual(len(json.loads(before['owned-processes.json'])['records']), 2)
                with self.assertRaises(FileExistsError): runner.main()
                self.assertEqual(len(calls), 2)  # refusal precedes any additional launch
                self.assertEqual(before, {p.name: p.read_bytes() for p in destination.iterdir()})

    def test_workflow_upload_paths_match_exclusive_directory(self):
        workflow = (ROOT / '.github/workflows/settings-raw-boundary.yml').read_text()
        self.assertIn('build-settings-raw/settings-raw-artifacts/settings-raw-*.log', workflow)
        self.assertIn('build-settings-raw/settings-raw-artifacts/owned-processes.json', workflow)
        self.assertNotIn('build-settings-raw/artifacts/', workflow)


if __name__ == '__main__':
    unittest.main()
