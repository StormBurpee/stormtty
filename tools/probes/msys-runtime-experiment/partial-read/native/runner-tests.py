"""Portable assess() false-success tests; parser is deliberately stubbed positive.

These tests cannot establish native ownership, byte behavior or path coverage.
"""
import copy
import json
import tempfile
import struct
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
                                    self.owned, b'', 'copied')

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
            self.assertEqual(result['coverage']['verdict'], 'PATH_OBSERVED' if reader else 'INCONCLUSIVE')

    def test_zero_copy_control_has_exact_distinct_tail(self):
        self.assertEqual(self.assess('zero-copy')['byte_verdict'], 'FAIL')
        self.reader=[{'result':0,'bytes':1,'synthetic_hex':'62'}]
        self.assertEqual(self.assess('zero-copy')['byte_verdict'], 'PASS')
        self.reader=[{'result':0,'bytes':2,'synthetic_hex':'6262'}]
        self.assertEqual(self.assess('zero-copy')['byte_verdict'], 'FAIL')

    def test_positive_case_has_distinct_expected_bytes(self):
        self.assertEqual(self.assess('positive-prefix')['byte_verdict'], 'FAIL')
        self.reader=[{'result':0,'bytes':3,'synthetic_hex':'616263'}]
        self.assertEqual(self.assess('positive-prefix')['byte_verdict'], 'PASS')


class ComponentTests(unittest.TestCase):
    def test_requires_bytes_and_exact_resize_outcome(self):
        good=dict(byte_verdict='PASS',resize_observation=dict(verdict='RESIZE_OBSERVED',expected_return_match=True))
        self.assertEqual(runner_module.component_verdict(good),'PASS')
        for byte in ('FAIL',None):
            self.assertEqual(runner_module.component_verdict(dict(good,byte_verdict=byte)),'FAIL')
        for observation in ({},{'verdict':'INCONCLUSIVE','expected_return_match':True},{'verdict':'RESIZE_OBSERVED','expected_return_match':False}):
            self.assertEqual(runner_module.component_verdict(dict(good,resize_observation=observation)),'INCONCLUSIVE')
        # Inconclusive legacy mouse/master coverage is never renamed or promoted.
        good['coverage']={'verdict':'INCONCLUSIVE'}
        self.assertEqual(runner_module.component_verdict(good),'PASS')

class FailureEvidenceTests(unittest.TestCase):
    def test_missing_truncated_oversize_logs_preserve_numeric_facts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            (root/'trace.bin').write_bytes(b'bounded-test-trace')
            metadata=runner_module.owned.Result(1,0,False,None,True,0.1,b'',False,'').metadata()
            good_reader=json.dumps(dict(result=0,bytes=2,synthetic_hex='6162'))+'\n'
            good_controller=json.dumps(dict(remaining_native_records=0))+'\n'+json.dumps(dict(cleanup_ok=True,timeout=False,scenario_ok=True,reader_exit=0))+'\n'
            def observed(*args):
                return dict(verdict='PATH_OBSERVED',reason='synthetic-stub',
                            recorded_observations=dict(interrupted_partial_reads=dict(count=1,max_copied=1)))
            for filename in ('reader.jsonl','controller.jsonl'):
                for bad in (None,'{"truncated":','[]\n','x'*65537):
                    (root/'reader.jsonl').write_text(good_reader)
                    (root/'controller.jsonl').write_text(good_controller)
                    path=root/filename
                    if bad is None:path.unlink()
                    else:path.write_text(bad)
                    with patch.object(runner_module.parser,'analyze',side_effect=observed) as analyze:
                        result=runner_module.collect_assessment('partial-zero',root,metadata,'copied')
                    analyze.assert_called_once_with(b'bounded-test-trace','partial-zero','copied')
                    self.assertEqual(result['byte_verdict'],'FAIL')
                    self.assertEqual(result['coverage']['verdict'],'INCONCLUSIVE')
                    self.assertEqual(result['coverage']['recorded_observations']['interrupted_partial_reads'],dict(count=1,max_copied=1))
                    self.assertIn('trace_sha256',result)
                    self.assertIn('fixture_error',result)

    def test_real_valid_trace_observation_survives_missing_logs(self):
        header=struct.pack('<8sIIIIQQIIQQ',b'STPTH001',1,64,128,3,3,0,1,1,9,0)
        rows=[]
        for seq,(event,payload) in enumerate(((1,(2,0,2,0,0)),(2,(0,1,1,0,0,1)),(15,((1<<64)-1,1,4)))):
            rows.append(struct.pack('<IHHQQIHHQQQQ8Q',1,1,event,seq,9,11,1,1,1,0,0,0,
                                    *(payload+(0,)*(8-len(payload)))))
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);(root/'trace.bin').write_bytes(header+b''.join(rows))
            metadata=runner_module.owned.Result(1,124,False,None,True,8.0,b'',False,'').metadata()
            result=runner_module.collect_assessment('partial-zero',root,metadata,'copied')
            self.assertEqual(result['byte_verdict'],'FAIL')
            self.assertEqual(result['coverage']['verdict'],'INCONCLUSIVE')
            self.assertEqual(result['coverage']['recorded_observations']['interrupted_partial_reads'],dict(count=1,max_copied=1))

    def test_valid_but_unfinished_reader_log_cannot_inherit_path(self):
        coverage=dict(verdict='PATH_OBSERVED',recorded_observations=dict(interrupted_partial_reads=dict(count=1,max_copied=1)))
        reader=[dict(attempt=0,count=-1,errno=4)]
        controller=[dict(remaining_native_records=0),dict(cleanup_ok=True,timeout=False,scenario_ok=True,reader_exit=0)]
        metadata=runner_module.owned.Result(1,0,False,None,True,0.1,b'',False,'').metadata()
        result=runner_module.assess('partial-zero',reader,controller,metadata,b'','copied',coverage)
        self.assertEqual(result['byte_verdict'],'FAIL')
        self.assertEqual(result['coverage']['verdict'],'INCONCLUSIVE')
        self.assertEqual(result['coverage']['recorded_observations']['interrupted_partial_reads']['count'],1)


if __name__ == '__main__': unittest.main()
