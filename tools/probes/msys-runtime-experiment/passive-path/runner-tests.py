"""Portable assess() false-success tests; parser is deliberately stubbed positive.

These tests cannot establish native ownership, byte behavior or path coverage.
"""
import copy
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('passive_runner_tests_target', Path(__file__).with_name('run.py'))
runner_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner_module)


class OutcomeTests(unittest.TestCase):
    def setUp(self):
        self.reader = [{'result': 0, 'bytes': 2, 'synthetic_hex': '6162'}]
        self.controller = [{'remaining_native_records': 0},
                           {'cleanup_ok': True, 'timeout': False,
                            'scenario_ok': True, 'reader_exit': 0}]
        self.owned = runner_module.owned.Result(
            pid=123, returncode=0, timed_out=False, alive_at_timeout=None,
            reaped=True, elapsed_seconds=0.1, output_tail=b'',
            output_truncated=False, cleanup_error='').metadata()
        self.stub = patch.object(runner_module.parser, 'analyze',
                                 side_effect=lambda *a: dict(verdict='PATH_OBSERVED', reason='synthetic-stub'))
        self.stub.start(); self.addCleanup(self.stub.stop)

    def assess(self, case='partial-zero'):
        return runner_module.assess(case, self.reader, self.controller,
                                    self.owned, b'', 'refined')

    def test_clean_owned_exact_bytes_can_pass_independent_axes(self):
        result = self.assess()
        self.assertEqual(result['byte_verdict'], 'PASS')
        self.assertEqual(result['coverage']['verdict'], 'PATH_OBSERVED')
        self.assertIs(result['native_promotion_allowed'], False)

    def test_owned_failure_cannot_inherit_positive_parser(self):
        for field, value in (('reaped', False), ('timed_out', True),
                             ('output_truncated', True), ('cleanup_error', 'failed'),
                             ('returncode', 1), ('returncode', None)):
            original=copy.deepcopy(self.owned); self.owned[field]=value
            result=self.assess(); self.owned=original
            self.assertEqual(result['byte_verdict'], 'FAIL', field)
            self.assertEqual(result['coverage']['verdict'], 'INCONCLUSIVE', field)

    def test_missing_required_owned_metadata_cannot_pass(self):
        for field in ('reaped', 'timed_out', 'output_truncated', 'returncode', 'cleanup_error'):
            original=self.owned.copy(); del self.owned[field]
            result=self.assess(); self.owned=original
            self.assertEqual(result['byte_verdict'], 'FAIL', field)
            self.assertEqual(result['coverage']['verdict'], 'INCONCLUSIVE', field)

    def test_controller_incomplete_cannot_inherit_positive_parser(self):
        for field, value in (('cleanup_ok', False), ('timeout', True),
                             ('scenario_ok', False), ('reader_exit', 124)):
            original=copy.deepcopy(self.controller); self.controller[-1][field]=value
            result=self.assess(); self.controller=original
            self.assertEqual(result['byte_verdict'], 'FAIL', field)
            self.assertEqual(result['coverage']['verdict'], 'INCONCLUSIVE', field)

    def test_missing_controller_completion_cannot_pass(self):
        self.controller=self.controller[:1]
        result=self.assess()
        self.assertEqual(result['byte_verdict'], 'FAIL')
        self.assertEqual(result['coverage']['verdict'], 'INCONCLUSIVE')

    def test_remaining_native_records_or_missing_queue_fails_bytes(self):
        for queue in ([{'remaining_native_records': 1}], []):
            self.controller=queue+[self.controller[-1]]
            self.assertEqual(self.assess()['byte_verdict'], 'FAIL')

    def test_wrong_or_missing_reader_bytes_independently_fail(self):
        for reader in ([], [{'result':0,'synthetic_hex':'616263'}],
                       [{'result':1,'synthetic_hex':'6162'}],
                       [{'result':0,'synthetic_hex':'61626162'}]):
            self.reader=reader
            result=self.assess()
            self.assertEqual(result['byte_verdict'], 'FAIL')
            # A complete observed path is not a proof of byte conservation.
            self.assertEqual(result['coverage']['verdict'], 'PATH_OBSERVED')

    def test_positive_case_has_distinct_expected_bytes(self):
        self.assertEqual(self.assess('positive-prefix')['byte_verdict'], 'FAIL')
        self.reader=[{'result':0,'bytes':3,'synthetic_hex':'616263'}]
        self.assertEqual(self.assess('positive-prefix')['byte_verdict'], 'PASS')


if __name__ == '__main__': unittest.main()
