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
spec = importlib.util.spec_from_file_location('refined_compare', HERE/'compare-refined.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

class RefinedTests(unittest.TestCase):
    def test_probe_is_exact_pr7_bytes(self):
        self.assertEqual(m.identity.sha(m.REPO/'tests/paste_input_probe.cpp'), m.PROBE_SOURCE_SHA256)

    def test_fixed_order(self):
        cases=list(m.plan()); self.assertEqual(len(cases),12)
        for provider in ('inbox','packaged'):
            self.assertEqual([(r,v) for p,r,v in cases if p==provider],
                [(1,'stock'),(1,'refined'),(2,'refined'),(2,'stock'),(3,'stock'),(3,'refined')])

    def test_summary_keeps_control_failures_strict(self):
        data=b'PASTE_RESULT code=1 provider=inbox vt_off_control_failures=1 raw_vt_reader_failures=0 production_cause_established=0'
        self.assertEqual(m.raw_summary(data)['code'],1)
        for value in (b'',data+b'\n'+data,data.replace(b'code=1',b'code=2'),data.replace(b'failures=1',b'failures=9')):
            self.assertIsNone(m.raw_summary(value))

    def run_matrix(self, workload, failures=False, truncated=False, timed_out=False):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); app=root/'app.exe'; app.write_bytes(b'MZfixture')
            args=types.SimpleNamespace(**{name:root/name for name in ('original','stock','refined','provider')},
                output=root/'evidence',probe=app,app=app,workload=workload)
            calls=[]
            def fake(command, **kwargs):
                calls.append((command,kwargs)); code=int(failures and len(calls)==1)
                if failures and len(calls)==2: raise OSError('owned child fixture')
                provider='packaged' if len(calls)>6 else 'inbox'
                output=(f'PASTE_RESULT code={code} provider={provider} vt_off_control_failures={code} raw_vt_reader_failures=0 production_cause_established=0\n').encode()
                output+=b'PASTE_MISMATCH expected_hex=1b5b3230307e child_hex=5b3230307e\n'
                return types.SimpleNamespace(exit_code=code,output_tail=output,reaped=True,output_truncated=truncated,timed_out=timed_out,cleanup_error='',metadata=lambda:dict(reaped=True,timed_out=False))
            with patch.object(m.identity,'identities',return_value={}),patch.object(m.runner,'run_owned',side_effect=fake):
                code=m.run(args)
            self.assertEqual(code,int(failures or truncated or timed_out)); self.assertEqual(len(calls),12)
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
    def test_timeout_fails_without_retry(self):self.run_matrix('raw',timed_out=True)
    def test_settings_fixed_cases(self):self.run_matrix('settings')
    def test_truncated_evidence_fails_without_retry(self):self.run_matrix('raw',truncated=True)

    def test_mouse_return_is_braced_without_status_change(self):
        patch=(HERE/'0006-refined-prefix.patch').read_text()
        added='\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
        self.assertIn('if (send_winch_maybe (i))\n\t    {',added)
        self.assertIn('return stat;\n\t    }',added)
        self.assertEqual(added.count('return input_winch;'),1)
        self.assertNotIn('send_winch_maybe (i + 1)',added)

    def test_only_refined_source_patch(self):
        script=(HERE/'build-refined.sh').read_text()
        self.assertNotIn('0003-',script);self.assertNotIn('0004-',script);self.assertNotIn('0001-',script)
        self.assertIn('[[ $variant == refined ]]',script)
        self.assertEqual(m.identity.sha(HERE/'0006-refined-prefix.patch'),'1afc3483da5dcc8848f336619f858c0ffcfbef608d1446464e6566135b2aff66')
        self.assertIn('201703L',script);self.assertIn('15.3.0',script)
        self.assertNotIn('--skipinteg', '\n'.join(line for line in script.splitlines() if not line.startswith('#')))

    def test_refined_workflow_exact_authorized_branch(self):
        workflow=(m.REPO/'.github/workflows/msys-refined-retirement.yml').read_text()
        self.assertIn("branches: ['experiment/msys-refined-retirement']",workflow)
        self.assertNotIn('pull_request:',workflow)
        self.assertNotIn('workflow_dispatch:',workflow)
        self.assertIn('Root alone',(HERE/'refined-native/PLAN.md').read_text())

if __name__=='__main__':unittest.main()
