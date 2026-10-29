#!/usr/bin/env python3
"""Portable experiment contracts; no MSYS/native behavior qualification."""
import importlib.util
import json
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('conditional_compare', HERE/'compare-conditional.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

class ConditionalTests(unittest.TestCase):
    def test_probe_is_exact_pr7_bytes(self):
        self.assertEqual(m.identity.sha(m.REPO/'tests/paste_input_probe.cpp'), m.PROBE_SOURCE_SHA256)

    def test_fixed_order(self):
        cases=list(m.plan()); self.assertEqual(len(cases),12)
        for provider in ('inbox','packaged'):
            self.assertEqual([(r,v) for p,r,v in cases if p==provider],
                [(1,'stock'),(1,'conditional'),(2,'conditional'),(2,'stock'),(3,'stock'),(3,'conditional')])

    def test_summary_keeps_control_failures_strict(self):
        data=b'PASTE_RESULT code=1 provider=inbox vt_off_control_failures=1 raw_vt_reader_failures=0 production_cause_established=0'
        self.assertEqual(m.raw_summary(data)['code'],1)
        for value in (b'',data+b'\n'+data,data.replace(b'code=1',b'code=2'),data.replace(b'failures=1',b'failures=9')):
            self.assertIsNone(m.raw_summary(value))

    def run_matrix(self, workload, failures=False, truncated=False):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); app=root/'app.exe'; app.write_bytes(b'MZfixture')
            args=types.SimpleNamespace(**{name:root/name for name in ('original','stock','conditional','provider')},
                output=root/'evidence',probe=app,app=app,workload=workload)
            calls=[]
            def fake(command, **kwargs):
                calls.append((command,kwargs)); code=int(failures and len(calls)==1)
                if failures and len(calls)==2: raise OSError('owned child fixture')
                provider='packaged' if len(calls)>6 else 'inbox'
                output=(f'PASTE_RESULT code={code} provider={provider} vt_off_control_failures={code} raw_vt_reader_failures=0 production_cause_established=0\n').encode()
                output+=b'PASTE_MISMATCH expected_hex=1b5b3230307e child_hex=5b3230307e\n'
                return types.SimpleNamespace(exit_code=code,output_tail=output,reaped=True,output_truncated=truncated,cleanup_error='',metadata=lambda:dict(reaped=True,timed_out=False))
            with patch.object(m.identity,'identities',return_value={}),patch.object(m.runner,'run_owned',side_effect=fake):
                code=m.run(args)
            self.assertEqual(code,int(failures or truncated)); self.assertEqual(len(calls),12)
            report=json.loads((args.output/'results.json').read_text()); self.assertEqual(len(report['cases']),12)
            self.assertIn('child_hex=5b3230307e',(args.output/'inbox-1-stock.log').read_text())
            for index,(command,options) in enumerate(calls):
                self.assertEqual(options['timeout'],180 if workload=='raw' else 30)
                self.assertEqual(options['reap_timeout'],10)
                if workload=='raw':self.assertEqual(len(command),2 if index<6 else 3)
                else:
                    self.assertIn('--smoke-settings',command);self.assertIn('--capture',command)
                    self.assertEqual('--console-runtime' in command,index>=6)
            with patch.object(m.identity,'identities',return_value={}):
                with self.assertRaisesRegex(ValueError,'must be new'):m.run(args)

    def test_raw_all_cases_survive_failure_and_launch_error(self):self.run_matrix('raw',failures=True)
    def test_settings_fixed_cases(self):self.run_matrix('settings')
    def test_truncated_evidence_fails_without_retry(self):self.run_matrix('raw',truncated=True)

    def test_only_conditional_source_patch(self):
        script=(HERE/'build-conditional.sh').read_text()
        self.assertNotIn('0003-',script);self.assertNotIn('0004-',script);self.assertNotIn('0001-',script)
        self.assertIn('[[ $variant == conditional ]]',script)
        self.assertEqual(m.identity.sha(HERE/'0005-conditional-retirement.patch'),'788f01ace57334125ef99c1358699e4ebd3aa0f1d888d8202a1511c416c21039')
        self.assertIn('201703L',script);self.assertIn('15.3.0',script)
        self.assertNotIn('--skipinteg', '\n'.join(line for line in script.splitlines() if not line.startswith('#')))

    def test_workflow_is_exact_push_and_preserves_failed_raw_followup(self):
        workflow=(m.REPO/'.github/workflows/msys-conditional-retirement.yml').read_text()
        self.assertIn("branches: ['experiment/msys-conditional-retirement']",workflow)
        self.assertIn("paths: ['.github/workflows/msys-conditional-retirement.yml']",workflow)
        self.assertIn("if: github.event_name == 'push' && github.ref == 'refs/heads/experiment/msys-conditional-retirement'",workflow)
        self.assertIn('ref: ${{ github.sha }}',workflow)
        self.assertNotIn('pull_request:',workflow)
        self.assertNotIn('workflow_dispatch:',workflow)
        self.assertIn('Do not push workflow edits to',workflow)
        settings=workflow.split('      - name: Fixed actual settings-close paste matrix')[1].split('      - name: Preserve')[0]
        self.assertIn("if: always() && steps.staging.outcome == 'success'",settings)
        self.assertNotIn('steps.raw.outcome',settings)
        self.assertIn('timeout-minutes: 42',workflow);self.assertIn('timeout-minutes: 9',settings)

if __name__=='__main__':unittest.main()
