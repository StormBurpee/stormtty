#!/usr/bin/env python3
import importlib.util,pathlib,types,unittest,tempfile,json
from unittest.mock import patch
HERE=pathlib.Path(__file__).resolve().parent
REPO=HERE.parents[2]
def load(name,file):
 s=importlib.util.spec_from_file_location(name,HERE/file);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
managed=load('managed_compare_tests','compare-managed.py')
imports=load('reader_import_tests','check-reader-imports.py')
class Tests(unittest.TestCase):
 def test_fixed_order(self):
  self.assertEqual(list(managed.plan()),[(p,t,v) for p in ('inbox','packaged') for t,order in enumerate((('stock','refined'),('refined','stock')),1) for v in order])
 def test_strict_owned_pass(self):
  good=dict(exit_code=0,reaped=True,timed_out=False,output_truncated=False,cleanup_error='',output_tail=b'REFINED_MANAGED_RESULT fresh=1 warm=1 result=PASS\n')
  self.assertTrue(managed.passed(types.SimpleNamespace(**good)))
  for key,value in [('exit_code',1),('reaped',False),('timed_out',True),('output_truncated',True),('cleanup_error','error'),('output_tail',b'')]:
   self.assertFalse(managed.passed(types.SimpleNamespace(**(good|{key:value}))))
  self.assertFalse(managed.passed(types.SimpleNamespace(**(good|{'output_tail':good['output_tail']*2}))))
 def test_managed_failures_keep_all_fixed_cases(self):
  with tempfile.TemporaryDirectory() as temp:
   root=pathlib.Path(temp)
   args=types.SimpleNamespace(**{n:root/n for n in ('original','stock','refined','provider','output','host','native','config')})
   for n in ('host','native'):getattr(args,n).write_bytes(b'fixed')
   for n in ('stormtty-console-setup.exe','ghostty-vt.dll'):(root/n).write_bytes(b'fixed')
   calls=[]
   def fake(command,**kwargs):
    calls.append(command)
    if len(calls)==2:raise OSError('fixed launch failure')
    return types.SimpleNamespace(exit_code=0,reaped=True,timed_out=len(calls)==1,output_truncated=False,cleanup_error='',output_tail=b'REFINED_MANAGED_RESULT fresh=1 warm=1 result=PASS\n',metadata=lambda:{'reaped':True})
   with patch.object(managed.identity,'identities',return_value={}),patch.object(managed.runner,'run_owned',side_effect=fake):
    self.assertEqual(managed.run(args),1)
   report=json.loads((args.output/'results.json').read_text())
   self.assertEqual(len(calls),8);self.assertEqual(len(report['cases']),8)
   self.assertFalse(report['cases'][0]['passed']);self.assertFalse(report['cases'][1]['passed'])
   self.assertTrue(all(c['passed'] for c in report['cases'][2:]))
 def test_real_managed_path_and_fixture_limits(self):
  host=(HERE/'managed-native/host.cpp').read_text();config=(HERE/'managed-native/config.zsh').read_text()
  self.assertIn('Session::start',host);self.assertIn('managed_shell_console_environment()',host)
  self.assertIn('warm_dump_unchanged=',host);self.assertIn('ready(s,9,130',host)
  self.assertIn('add-zle-hook-widget line-init',config);self.assertNotIn('PROMPT=',config)
  self.assertIn('static struct termios saved, t;', (HERE/'refined-native/reader.c').read_text())
  self.assertIn("'exact_path_verdict':'inconclusive'",(HERE/'refined-native/run.py').read_text().replace(' ',''))
 def test_import_allowlist(self):
  imports.check('DLL Name: msys-2.0.dll\nDLL Name: KERNEL32.dll\n')
  for s in ('','DLL Name: msys-2.0.dll\nDLL Name: cygwin1.dll','DLL Name: msys-2.0.dll\nDLL Name: msys-gcc_s-seh-1.dll'):
   with self.assertRaises(ValueError):imports.check(s)
 def test_scope_and_independent_failure_preservation(self):
  s=(REPO/'.github/workflows/msys-refined-retirement.yml').read_text()
  self.assertIn("branches: ['experiment/msys-refined-retirement']",s)
  self.assertIn("github.ref == 'refs/heads/experiment/msys-refined-retirement'",s)
  self.assertNotIn('workflow_dispatch',s)
  self.assertNotIn('pull_request:',s)
  self.assertEqual(s.count("if: always() && steps.staging.outcome == 'success'"),4)
  self.assertNotIn('continue-on-error',s)
  self.assertIn('diffutils coreutils gawk grep sed tar binutils curl gnupg',s)
  self.assertIn('STORMTTY_TEST_REFINED_RUNTIME=ON',s)
  self.assertIn('exact paths remain inconclusive',s)
 def test_default_off(self):
  s=(REPO/'CMakeLists.txt').read_text()
  self.assertIn('option(STORMTTY_TEST_REFINED_RUNTIME "Build isolated managed MSYS runtime qualification" OFF)',s)
if __name__=='__main__':unittest.main()
