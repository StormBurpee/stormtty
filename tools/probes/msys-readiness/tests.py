#!/usr/bin/env python3
"""Portable harness contracts; local-Zsh script checks are not MSYS qualification."""
import collections
import contextlib
import importlib.util
import io
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[2]
spec = importlib.util.spec_from_file_location('readiness_matrix_test', HERE / 'compare.py')
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)
META = dict(returncode=0, reaped=True, timed_out=False, output_truncated=False, cleanup_error='')


def write_record(seq, data):
    return (f'PTY_WRITE seq={seq} message={seq} offset=0 requested={len(data)} success=1 '
            f'reported={len(data)} accepted={len(data)} error=0 requested_hex_truncated=0 '
            f'accepted_hex_truncated=0 requested_hex={data.hex()} accepted_hex={data.hex()}\n').encode()


def output(row, *, failed=False, cursor=False):
    mode = 2 if row['control'] == 'resize' else 0
    timed = int(row['reader'] == 'timed')
    result = bytearray()
    after = 0
    for index in range(1, 2 if failed else 21):
        child = matrix.EXPECTED[1:] if failed else matrix.EXPECTED
        result.extend((f'READINESS_ROUND round={index} timed={timed} mode={mode} consumed=1 '
                       f'writer_published=1 complete=1 equal={int(not failed)} statuses_ok=1 write_shape_ok=1 '
                       f'expected_hex={matrix.EXPECTED.hex()} child_hex={child.hex()} '
                       f'sysread_status_count={"0,1;" * len(child)} '
                       'retried_eintr=unobserved sigwinch_count=unobserved\n').encode())
        result.extend((f'PASTE_GAPS kind=2 mode={mode} round={index} max_and_after_escape_us=50:10\n'
                       f'PASTE_RESIZE kind=2 mode={mode} round={index} requested={index*4 if mode else 0} '
                       f'applied={index*4 if mode else 0} calls={index if mode else 0} failures=0\n').encode())
        records = [matrix.EXPECTED[:-1], matrix.EXPECTED[-1:]]
        if cursor:
            records.insert(0, b'\x1b[2;1R')
        result.extend((f'PTY_INPUT_EVIDENCE label=authored-paste-only completed={after+len(records)} '
                       'overwritten=0 byte_truncated_records=0 '
                       f'after={after} missing_since_after=0 retained={len(records)}\n').encode())
        for offset, data in enumerate(records, 1):
            result.extend(write_record(after + offset, data))
        after += len(records)
        result.extend(b'END_PTY_INPUT_EVIDENCE\n')
    result.extend(b'READINESS_CLEANUP closed=1 exit_present=1 exit_code=1 error_present=0 requested_close=1\n')
    result.extend(b'READINESS_SCRATCH removed=1\n')
    result.extend((f'PASTE_CASE kind=2 mode={mode} planned=20 completed={0 if failed else 20} code={int(failed)}\n'
                   f'READINESS_RESULT code={int(failed)} reader={row["reader"]} control={row["control"]} '
                   'provider=inbox runtime=official production_cause_established=0\n').encode())
    return bytes(result)


class Evidence(unittest.TestCase):
    def setUp(self):
        self.row = list(matrix.plan())[0]
        self.good = output(self.row)

    def rejects(self, data, metadata=META):
        self.assertFalse(matrix.assess(self.row, metadata, data)['passed'])

    def test_exact_balanced_opposing_fixed_orders(self):
        rows = list(matrix.plan())
        self.assertEqual(len(rows), 8)
        first = [(row['reader'], row['control']) for row in rows[:4]]
        second = [(row['reader'], row['control']) for row in rows[4:]]
        self.assertEqual(first, [('timed', 'plain'), ('direct', 'plain'), ('timed', 'resize'), ('direct', 'resize')])
        self.assertEqual(second, first[::-1])
        self.assertEqual(collections.Counter((r['reader'], r['control']) for r in rows), dict.fromkeys(matrix.CELLS, 2))
        self.assertEqual([r['trial'] for r in rows], [1]*4 + [2]*4)

    def test_all_cells_exact_success_with_optional_standalone_reply(self):
        for row in matrix.plan():
            for cursor in (False, True):
                result = matrix.assess(row, META, output(row, cursor=cursor))
                self.assertTrue(result['passed'], (row, result))
                self.assertTrue(result['exact_round_evidence_valid'])
                self.assertEqual(result['rounds_observed'], 20)
                self.assertEqual(result['retried_eintr'], 'unobserved')
                self.assertEqual(result['sigwinch_count'], 'unobserved')
                self.assertFalse(result['production_qualified'])
                self.assertFalse(result['causal_proof'])

    def test_leading_escape_loss_is_complete_failure_not_suppressed(self):
        result = matrix.assess(self.row, dict(META, returncode=1), output(self.row, failed=True))
        self.assertFalse(result['passed'])
        self.assertTrue(result['complete'])
        self.assertEqual(result['child_byte_mismatches'], 1)
        self.assertEqual(result['rounds_observed'], 1)

    def test_missing_wrong_duplicate_result_identity(self):
        final = self.good.splitlines()[-1] + b'\n'
        for data in (b'', self.good+final, self.good.replace(final, b''),
                     self.good.replace(b'reader=timed', b'reader=direct'),
                     self.good.replace(b'provider=inbox', b'provider=packaged'),
                     self.good.replace(b'runtime=official', b'runtime=copied'),
                     self.good.replace(b'cause_established=0', b'cause_established=1')):
            self.rejects(data)

    def test_each_outer_runner_field_is_required(self):
        for key, bad in [('returncode', 1), ('reaped', False), ('timed_out', True),
                         ('output_truncated', True), ('cleanup_error', 'failure')]:
            self.rejects(self.good, dict(META, **{key: bad}))
            missing = dict(META)
            del missing[key]
            self.rejects(self.good, missing)

    def test_cleanup_and_scratch_are_required_unique_exact(self):
        for marker in (b'READINESS_CLEANUP ', b'READINESS_SCRATCH '):
            line = next(line for line in self.good.splitlines(keepends=True) if line.startswith(marker))
            self.rejects(self.good.replace(line, b''))
            self.rejects(self.good+line)
        for old, new in [(b'closed=1', b'closed=0'), (b'exit_present=1', b'exit_present=0'),
                         (b'error_present=0', b'error_present=1'), (b'removed=1', b'removed=0')]:
            self.rejects(self.good.replace(old, new))

    def test_per_byte_reader_status_and_unobserved_signals_required(self):
        for old, new in [(b'statuses_ok=1', b'statuses_ok=0'), (b'write_shape_ok=1', b'write_shape_ok=0'), (b'0,1;', b'0,0;'),
                         (b'0,1;', b'1,-999;'), (b'retried_eintr=unobserved', b'retried_eintr=0'),
                         (b'sigwinch_count=unobserved', b'sigwinch_count=0'),
                         (b'consumed=1', b'consumed=0'), (b'equal=1', b'equal=0')]:
            self.rejects(self.good.replace(old, new, 1))

    def test_exact_bytes_required_not_just_flags(self):
        for key in (b'child_hex=', b'expected_hex=', b'accepted_hex=', b'requested_hex='):
            self.rejects(self.good.replace(key + b'1b', key + b'00', 1))
        self.rejects(self.good.replace(b'child_hex=' + matrix.EXPECTED.hex().encode(),
                                     b'child_hex=' + matrix.EXPECTED[1:].hex().encode(), 1))

    def test_write_evidence_loss_failure_sequence_offset_truncation_rejected(self):
        for old, new in [(b'overwritten=0', b'overwritten=1'), (b'byte_truncated_records=0', b'byte_truncated_records=1'),
                         (b'missing_since_after=0', b'missing_since_after=1'), (b'retained=2', b'retained=1'),
                         (b'success=1', b'success=0'), (b'error=0', b'error=5'), (b'offset=0', b'offset=1'),
                         (b'seq=1 ', b'seq=2 '), (b'reported=41', b'reported=40'),
                         (b'requested_hex_truncated=0', b'requested_hex_truncated=1'),
                         (b'accepted_hex_truncated=0', b'accepted_hex_truncated=1'),
                         (b'label=authored-paste-only', b'label=failure-tail')]:
            self.rejects(self.good.replace(old, new, 1))
        line = next(line for line in self.good.splitlines(keepends=True) if line.startswith(b'PTY_WRITE '))
        self.rejects(self.good.replace(line, b'', 1))
        self.rejects(self.good.replace(b'END_PTY_INPUT_EVIDENCE\n', b'', 1))

    def test_split_paste_is_not_two_intact_authored_writes(self):
        segment = self.good.split(b'READINESS_ROUND ')[1]
        old = write_record(1, matrix.EXPECTED[:-1]) + write_record(2, matrix.EXPECTED[-1:])
        replacement = write_record(1, matrix.EXPECTED[:1]) + write_record(2, matrix.EXPECTED[1:])
        self.assertIn(old, segment)
        self.rejects(self.good.replace(old, replacement, 1))

    def test_cpr_only_exclusion_never_filters_child_or_invalid_protocol(self):
        data = output(self.row, cursor=True)
        for wrong in (b'\x1b[0;1R', b'\x1b[2;0R', b'\x1b[2;1Rpayload'):
            self.rejects(data.replace(write_record(1, b'\x1b[2;1R'), write_record(1, wrong), 1))
        self.rejects(data.replace(b'child_hex=', b'child_hex=1b5b323b3152', 1))

    def test_missing_duplicate_malformed_round_and_gap_evidence(self):
        for old, new in [(b'round=1 ', b'round=2 '), (b'timed=1 ', b'timed=0 '),
                         (b'mode=0 ', b'mode=2 '), (b'completed=20 code=0', b'completed=19 code=0'),
                         (b'max_and_after_escape_us=50:10', b'max_and_after_escape_us='),
                         (b'max_and_after_escape_us=50:10', b'max_and_after_escape_us=-1:10')]:
            self.rejects(self.good.replace(old, new, 1))
        for marker in (b'PASTE_GAPS ', b'PASTE_RESIZE '):
            line = next(line for line in self.good.splitlines(keepends=True) if line.startswith(marker))
            self.rejects(self.good.replace(line, b'', 1))
            self.rejects(self.good+line)
        self.rejects(self.good.replace(b'round=1 ', b'round=1 round=1 ', 1))
        self.rejects(self.good+self.good)


class ResizeCoverage(unittest.TestCase):
    def setUp(self):
        self.row = dict(reader='timed', control='resize')
        self.good = output(self.row)

    def rejects(self, old, new):
        self.assertIn(old, self.good)
        result = matrix.assess(self.row, META, self.good.replace(old, new, 1))
        self.assertFalse(result['passed'])
        self.assertFalse(result['observed_resize_coverage_valid'])
        self.assertFalse(result['resize_coverage_complete'])

    def test_coalescing_and_one_in_flight_call_allowed_without_waiting(self):
        partial = self.good.replace(b'requested=4 applied=4 calls=1', b'requested=4 applied=1 calls=1')
        in_flight = self.good
        for index in range(1, 21):
            applied = 1 if index == 1 else index*4-1
            in_flight = in_flight.replace(
                f'round={index} requested={index*4} applied={index*4} calls={index} failures=0'.encode(),
                f'round={index} requested={index*4} applied={applied} calls={index+1} failures=0'.encode())
        for data in (self.good, partial, in_flight):
            self.assertTrue(matrix.assess(self.row, META, data)['passed'])

    def test_resize_all_zero_is_not_coverage(self):
        data = re.sub(rb'requested=[0-9]+ applied=[0-9]+ calls=[0-9]+ failures=0',
                      b'requested=0 applied=0 calls=0 failures=0', self.good)
        result = matrix.assess(self.row, META, data)
        self.assertFalse(result['passed'])
        self.assertFalse(result['resize_coverage_complete'])
        self.assertTrue(result['exact_round_evidence_valid'])
        self.assertTrue(result['complete'])
        self.assertEqual(result['child_byte_mismatches'], 0)

    def test_requested_must_be_exactly_four_per_round(self):
        for value in (0, 1, 3, 5, 8):
            self.rejects(b'round=1 requested=4 ', f'round=1 requested={value} '.encode())
        self.rejects(b'round=2 requested=8 ', b'round=2 requested=4 ')

    def test_stale_regressing_future_applied_and_failed_calls_rejected(self):
        for value in (0, 3, 4, 9):
            self.rejects(b'round=2 requested=8 applied=8 ', f'round=2 requested=8 applied={value} '.encode())
        self.rejects(b'failures=0', b'failures=1')
        self.rejects(b'applied=4', b'applied=0')

    def test_calls_must_progress_and_agree_with_generations(self):
        for value in (0, 1, 9):
            self.rejects(b'round=2 requested=8 applied=8 calls=2 ', f'round=2 requested=8 applied=8 calls={value} '.encode())
        self.rejects(b'round=1 requested=4 applied=4 calls=1 ', b'round=1 requested=4 applied=1 calls=3 ')
        self.rejects(b'round=2 requested=8 applied=8 calls=2 ', b'round=2 requested=8 applied=5 calls=5 ')

    def test_plain_controls_cannot_have_authored_resize_activity(self):
        row = dict(reader='direct', control='plain')
        data = output(row)
        self.assertTrue(matrix.assess(row, META, data)['resize_coverage_complete'])
        for field in ('requested', 'applied', 'calls', 'failures'):
            result = matrix.assess(row, META, data.replace((field+'=0').encode(), (field+'=1').encode(), 1))
            self.assertFalse(result['passed'])
            self.assertFalse(result['resize_coverage_complete'])


class Execution(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = pathlib.Path(self.temp.name)
        runtime = root / 'original'
        runtime.mkdir()
        binary = root / 'probe.exe'
        binary.write_bytes(b'MZsynthetic')
        (root/'ghostty-vt.dll').write_bytes(b'MZsynthetic-engine')
        self.args = type('Args', (), dict(runtime=runtime, probe=binary, output=root/'results'))()
        self.files = {binary: matrix.sha(binary)}

    def fake_run(self, command, **kwargs):
        self.assertEqual(kwargs, dict(timeout=180, reap_timeout=10, tail_bytes=1048576, output_dir=self.args.output))
        row = dict(reader=command[-2], control=command[-1])
        return matrix.owned.Result(10, 1, False, None, True, 1.0, output(row, failed=True), False)

    @contextlib.contextmanager
    def mocks(self, runner=None):
        with patch.object(matrix, 'verify_runtime', return_value=(dict(self.files), {'dll_sha256': matrix.OFFICIAL_DLL})), \
             patch.object(matrix.owned, 'run_owned', side_effect=runner or self.fake_run) as launch, \
             contextlib.redirect_stdout(io.StringIO()):
            yield launch

    def test_all_eight_failed_cells_run_once_and_persist(self):
        with self.mocks() as launch:
            self.assertEqual(matrix.run(self.args), 1)
        report = json.loads((self.args.output/'results.json').read_text())
        self.assertEqual(launch.call_count, 8)
        self.assertEqual(report['ordered_plan'], list(matrix.plan()))
        self.assertEqual(len(report['cases']), 8)
        self.assertTrue(report['final_identity_verified'])
        self.assertTrue(all(row['complete'] and not row['passed'] for row in report['cases']))
        self.assertEqual(len(list(self.args.output.glob('*.log'))), 8)

    def test_eight_successes_still_do_not_claim_causality_or_qualification(self):
        def success(command, **kwargs):
            row = dict(reader=command[-2], control=command[-1])
            return matrix.owned.Result(10, 0, False, None, True, 1.0, output(row), False)
        with self.mocks(success) as launch:
            self.assertEqual(matrix.run(self.args), 0)
        report = json.loads((self.args.output/'results.json').read_text())
        self.assertEqual(launch.call_count, 8)
        self.assertEqual(report['executed_launches'], 8)
        self.assertEqual(report['passed_launches'], 8)
        self.assertEqual(report['lifecycle_complete_launches'], 8)
        self.assertFalse(report['production_qualified'])
        self.assertFalse(report['causal_proof'])
        for row in report['cases']:
            path = self.args.output/f"{row['trial']}-{row['reader']}-{row['control']}.log"
            self.assertEqual(row['log_bytes'], path.stat().st_size)
            self.assertEqual(row['log_sha256'], matrix.sha(path))

    def test_outer_timeout_never_stops_or_retries_the_fixed_plan(self):
        def timeout(command, **kwargs):
            row = dict(reader=command[-2], control=command[-1])
            return matrix.owned.Result(10, 0, True, True, True, 180.1, output(row), False)
        with self.mocks(timeout) as launch:
            self.assertEqual(matrix.run(self.args), 1)
        report = json.loads((self.args.output/'results.json').read_text())
        self.assertEqual(launch.call_count, 8)
        self.assertTrue(all(not r['passed'] and not r['complete'] for r in report['cases']))

    def test_launch_error_does_not_drop_later_cells(self):
        with self.mocks(runner=lambda *a, **k: (_ for _ in ()).throw(OSError('synthetic'))) as launch:
            self.assertEqual(matrix.run(self.args), 1)
        report = json.loads((self.args.output/'results.json').read_text())
        self.assertEqual(launch.call_count, 8)
        self.assertTrue(all(row['launch_error'] == 'OSError' and not row['complete'] for row in report['cases']))

    def test_existing_destination_is_never_overwritten_or_retried(self):
        self.args.output.mkdir()
        sentinel = self.args.output/'keep.txt'
        sentinel.write_text('original')
        with self.mocks() as launch, self.assertRaisesRegex(ValueError, 'no overwrite or retry'):
            matrix.run(self.args)
        launch.assert_not_called()
        self.assertEqual(sentinel.read_text(), 'original')

    def test_runtime_evidence_overlap_rejected_without_mutation(self):
        self.args.output = self.args.runtime/'evidence'
        with self.mocks() as launch, self.assertRaisesRegex(ValueError, 'must not overlap'):
            matrix.run(self.args)
        launch.assert_not_called()
        self.assertFalse(self.args.output.exists())

    def test_preflight_failure_persists_empty_fixed_plan(self):
        with patch.object(matrix, 'verify_runtime', side_effect=ValueError('wrong official DLL')), \
             self.assertRaisesRegex(ValueError, 'wrong official DLL'):
            matrix.run(self.args)
        report = json.loads((self.args.output/'results.json').read_text())
        self.assertEqual(report['cases'], [])
        self.assertEqual(report['ordered_plan'], list(matrix.plan()))
        self.assertIn('preflight_or_identity_error', report)

    def test_identity_drift_stops_without_retry_and_preserves_first_result(self):
        def change(*args, **kwargs):
            result = self.fake_run(*args, **kwargs)
            self.args.probe.write_bytes(b'changed')
            return result
        with self.mocks(change) as launch, self.assertRaisesRegex(ValueError, 'changed during diagnostic'):
            matrix.run(self.args)
        self.assertEqual(launch.call_count, 1)
        report = json.loads((self.args.output/'results.json').read_text())
        self.assertEqual(len(report['cases']), 1)
        self.assertNotIn('final_identity_verified', report)

    def test_added_runtime_file_is_identity_drift_not_a_new_baseline(self):
        def change(*args, **kwargs):
            result = self.fake_run(*args, **kwargs)
            (self.args.runtime/'unexpected.txt').write_text('changed')
            return result
        with self.mocks(change) as launch, self.assertRaisesRegex(ValueError, 'inventory changed during diagnostic'):
            matrix.run(self.args)
        self.assertEqual(launch.call_count, 1)
        report = json.loads((self.args.output/'results.json').read_text())
        self.assertEqual(len(report['cases']), 1)

    def test_identity_drift_after_last_launch_still_fails_final_gate(self):
        count = 0
        def change(*args, **kwargs):
            nonlocal count
            count += 1
            result = self.fake_run(*args, **kwargs)
            if count == 8:
                self.args.probe.write_bytes(b'changed after last')
            return result
        with self.mocks(change) as launch, self.assertRaisesRegex(ValueError, 'changed after diagnostic'):
            matrix.run(self.args)
        self.assertEqual(launch.call_count, 8)
        report = json.loads((self.args.output/'results.json').read_text())
        self.assertEqual(len(report['cases']), 8)
        self.assertNotIn('final_identity_verified', report)

    def test_official_identity_and_experiment_marker_rejection(self):
        for manifest, digest in [({'experimental_runtime': {}}, matrix.OFFICIAL_DLL), ({}, 'wrong')]:
            with patch.object(matrix.stager, 'load', return_value=manifest), patch.object(matrix, 'sha', return_value=digest), \
                 patch.object(matrix.stager, 'validate') as validate, self.assertRaisesRegex(ValueError, 'untouched pinned official'):
                matrix.verify_runtime(self.args.runtime)
            validate.assert_not_called()


class Contracts(unittest.TestCase):
    def test_original_qualification_probe_is_untouched(self):
        self.assertEqual(matrix.sha(REPO/'tests/paste_input_probe.cpp'), matrix.ORIGINAL_PROBE)
        self.assertEqual(len(matrix.EXPECTED), 42)
        self.assertEqual(len(matrix.EXPECTED[:-1]), 41)

    def test_probe_has_no_focus_retry_winch_trap_or_runtime_hooks(self):
        text = (HERE/'probe.cpp').read_text()
        self.assertNotIn('s->focus(', text)
        self.assertNotIn('TRAPWINCH', text)
        self.assertEqual(re.findall(r'^trap .*$', text, re.M), ["trap 'stty sane' EXIT"])
        self.assertIn('round<=20', text)
        self.assertIn('i<4', text)
        self.assertIn('i==3?(round%2?170:180):(160+(i%2)*10)', text)
        self.assertIn('std::chrono::milliseconds timeout=10s', text)
        self.assertIn('},250ms)', text)
        self.assertIn('p.provider=ProcessProvider::windows_inbox_basic;', text)
        self.assertIn('p.arguments={"-df",utf8(script)}', text)
        self.assertIn('sysread -i 0 -s 1 -t 30 -c read_count ch', text)
        self.assertIn('sysread -i 0 -s 1 -c read_count ch', text)
        self.assertIn('retried_eintr=unobserved sigwinch_count=unobserved', text)
        self.assertIn('write_shape_ok=readiness::intact_authored_writes(snap,expected)', text)
        self.assertIn('||!statuses_ok||!write_shape_ok){', text)
        self.assertIn('resize_failures', text)

    def test_option_off_isolated_and_never_installed(self):
        text = (REPO/'CMakeLists.txt').read_text()
        self.assertIn('option(STORMTTY_TEST_MSYS_READINESS "Build isolated stock-only MSYS readiness diagnostic" OFF)', text)
        self.assertIn('STORMTTY_TEST_MSYS_READINESS AND (NOT WIN32 OR NOT BUILD_TESTING OR NOT STORMTTY_TEST_SHELL_PROBE)', text)
        self.assertIn('add_executable(msys_readiness_probe EXCLUDE_FROM_ALL', text)
        self.assertIn('target_link_libraries(msys_readiness_probe PRIVATE stormtty_qualification_session shell32 ole32)', text)
        self.assertNotRegex(text, r'install\([^)]*msys_readiness_probe')

    def test_workflow_scope_pins_failure_retention_and_no_runtime_rebuild(self):
        text = (REPO/'.github/workflows/msys-stock-readiness.yml').read_text()
        for marker in ('branches: [\'experiment/msys-stock-readiness\']', 'ref: ${{ github.sha }}',
                       'contents: read', 'cancel-in-progress: false', 'runs-on: windows-2025',
                       '-DSTORMTTY_BUILD_APP=OFF', '-DSTORMTTY_BUNDLE_ZSH=OFF', '-DSTORMTTY_BUNDLE_CONPTY=OFF',
                       '-DSTORMTTY_TEST_MSYS_READINESS=ON', 'if: always()', '--no-tests=error'):
            self.assertIn(marker, text)
        for forbidden in ('workflow_dispatch:', 'schedule:', 'continue-on-error:', 'setup-msys2@',
                          '.src.tar', 'partial-app/build.sh', '--provider ', 'prepare-conpty-probe', 'copied-build'):
            self.assertNotIn(forbidden, text)
        for action in re.findall(r'uses: (\S+)', text):
            self.assertRegex(action, r'^[^@]+@[a-f0-9]{40}$')
        artifacts = text.split('          path: |')[-1]
        self.assertNotRegex(artifacts, r'(?i)\.(dll|exe)|HOME|\*')
        self.assertEqual(len(re.findall(r'/matrix/[12]-.+\.log', artifacts)), 8)
        self.assertEqual(text.count('compare.py --runtime'), 1)


@unittest.skipUnless(os.name == 'posix' and shutil.which('zsh'), 'Local POSIX Zsh optional; no Windows inference')
class LocalZshScript(unittest.TestCase):
    def test_both_reader_templates_return_exact_bytes_and_statuses(self):
        import select
        source = (HERE/'probe.cpp').read_text()
        script = source.split('R"ZSH(', 1)[1].split(')ZSH";', 1)[0]
        for timed in (False, True):
            with self.subTest(timed=timed), tempfile.TemporaryDirectory() as directory:
                path = pathlib.Path(directory)/'reader.zsh'
                call = 'sysread -i 0 -s 1 ' + ('-t 30 ' if timed else '') + '-c read_count ch'
                path.write_text(script.replace('@SYSREAD@', call))
                syntax = subprocess.run(['zsh', '-dfn', str(path)], capture_output=True, timeout=5)
                self.assertEqual(syntax.returncode, 0, syntax.stderr)
                master, slave = os.openpty()
                env = dict(os.environ, HOME=directory, ZDOTDIR=directory, LC_ALL='C')
                child = subprocess.Popen(['zsh', '-df', str(path)], stdin=slave, stdout=slave, stderr=slave,
                                         env=env, cwd=directory, close_fds=True)
                os.close(slave)
                data = bytearray()
                def wait(marker):
                    end = time.monotonic()+5
                    while marker not in data and time.monotonic() < end:
                        if select.select([master], [], [], .1)[0]:
                            data.extend(os.read(master, 8192))
                    self.assertIn(marker, data)
                try:
                    wait(b'RAW_READY')
                    for index in (1, 2):
                        self.assertEqual(os.write(master, matrix.EXPECTED), len(matrix.EXPECTED))
                        wait(f'STATUS:{index}:'.encode() + b'0,1;'*42 + b':END')
                        self.assertIn(f'INPUT:{index}:{matrix.EXPECTED.hex()}:END'.encode(), data)
                        self.assertRegex(bytes(data), f'GAPS:{index}:[0-9]+:[0-9]+:END'.encode())
                finally:
                    child.terminate()
                    try:
                        child.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait(timeout=3)
                    os.close(master)


if __name__ == '__main__':
    unittest.main()
