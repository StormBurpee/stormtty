#!/usr/bin/env python3
"""Portable evidence-driver tests; not native terminal qualification."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import types
import unittest
from unittest import mock
import sys

spec=importlib.util.spec_from_file_location('responsiveness',Path(__file__).with_name('run-shell-responsiveness.py'))
m=importlib.util.module_from_spec(spec);sys.modules[spec.name]=m;spec.loader.exec_module(m)
CONTROL='MANAGED_TYPING_RESULT code=0 characters=25 frames=30 retreats=0 echo_total_us=100 echo_max_us=10\nMANAGED_STATE_RESULT code=0 phase=11 cold_samples=1 warm_samples=1 checks=200 cold_ms=4000 warm_ms=500\n'
REPLAY='TYPING_REPLAY code=0 input_bytes=100 input_hash=55 output_bytes=200 publications=30 retreats=0 queued_retreats=0 drainable_retreats=0\n'
EVENT='TYPING_EVENT kind=publication '+ ' '.join(k+'=0' for k in m.TRACE_FIELDS)+' hex=\n'
TRACE='TYPING_TRACE version=1 overflow=0 records=1 bytes=0\n'+EVENT+'END_TYPING_TRACE\n'+CONTROL

def result(output,**kw):
    values=dict(exit_code=0,reaped=True,timed_out=False,output_truncated=False,cleanup_error='',elapsed_seconds=1.0,output_tail=output.encode())
    values.update(kw);return types.SimpleNamespace(**values)

class EvidenceTests(unittest.TestCase):
    def test_allowlist_drops_unexpected_text_and_paths(self):
        lines=m.sanitize(b'private path secret\n'+TRACE.encode()+b'password=not-allowed\n',traced=True)
        self.assertTrue(m.valid(lines,traced=True));self.assertNotIn('secret','\n'.join(lines));self.assertNotIn('password','\n'.join(lines))
    def test_control_requires_exact_count_and_terminal_success(self):
        self.assertTrue(m.valid(m.sanitize(CONTROL.encode())))
        self.assertFalse(m.valid(m.sanitize(CONTROL.replace('characters=25','characters=24').encode())))
        self.assertFalse(m.valid(m.sanitize((CONTROL+CONTROL).encode())))
    def test_trace_overflow_truncation_and_missing_records_fail(self):
        for bad in [TRACE.replace('overflow=0','overflow=1'),TRACE.replace('records=1','records=2'),TRACE.replace('END_TYPING_TRACE\n','')]:
            self.assertFalse(m.valid(m.sanitize(bad.encode(),traced=True),traced=True))
    def test_replay_signature_is_numeric_and_unique(self):
        self.assertTrue(m.valid(m.sanitize(REPLAY.encode(),replay=True),replay=True))
        self.assertFalse(m.valid(m.sanitize((REPLAY+REPLAY).encode(),replay=True),replay=True))
    def test_runner_completion_must_be_reaped_untruncated_and_on_time(self):
        with tempfile.TemporaryDirectory() as tmp:
            for failure in [dict(exit_code=2),dict(reaped=False),dict(timed_out=True),dict(output_truncated=True),dict(cleanup_error='private-error')]:
                with contextlib.redirect_stdout(io.StringIO()):
                    passed,lines=m.one(['fake'],Path(tmp)/'out.log',invoke=lambda *a,**k:result(CONTROL,**failure))
                self.assertFalse(passed);self.assertNotIn('private-error','\n'.join(lines))
    def test_independent_failures_no_retry_and_comparison_preserved(self):
        calls=[]
        def invoke(args,**kw):
            calls.append(args)
            if args[0].endswith('managed_shell_state_probe.exe'):return result(CONTROL,exit_code=1 if len(calls)==1 else 0)
            if args[0].endswith('managed_shell_typing_trace.exe'):return result(TRACE)
            return result(REPLAY)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            argv=['driver','--build',str(root),'--runtime',str(root),'--provider',str(root),'--output',str(root/'out')]
            with mock.patch.object(sys,'argv',argv),mock.patch.object(m.runner,'run_owned',invoke),contextlib.redirect_stdout(io.StringIO()):code=m.main()
            self.assertEqual(code,1);self.assertEqual(len(calls),6)
            self.assertEqual(len(list((root/'out').glob('*.log'))),6)
            self.assertIn('inbox-control',(root/'out/comparison.json').read_text())
    def test_workflow_is_exact_push_and_preserves_independent_results(self):
        text=(Path(__file__).resolve().parents[2]/'.github/workflows/shell-responsiveness.yml').read_text()
        self.assertIn('branches: [diagnostic/shell-responsiveness]',text)
        self.assertNotIn('pull_request:',text);self.assertNotIn('workflow_run:',text)
        self.assertIn('cancel-in-progress: false',text);self.assertIn('if: always()',text)
        self.assertNotIn("steps.typing.outcome == 'success'",text)

if __name__=='__main__':unittest.main(verbosity=2)
