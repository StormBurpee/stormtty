import importlib.util
from pathlib import Path
import unittest
import types
HERE=Path(__file__).parent
spec=importlib.util.spec_from_file_location('runner',HERE/'run.py');runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
class Contract(unittest.TestCase):
 def test_owned_completion_strict(self):
  good=dict(reaped=True,timed_out=False,output_truncated=False,cleanup_error='')
  self.assertTrue(runner.complete(types.SimpleNamespace(**good)))
  for key,value in [('reaped',False),('timed_out',True),('output_truncated',True),('cleanup_error','failed')]:
   self.assertFalse(runner.complete(types.SimpleNamespace(**(good|{key:value}))))
 def test_bounded_runner_not_subprocess_run(self):
  source=(HERE/'run.py').read_text()
  self.assertIn('reap_timeout=5',source);self.assertIn('timeout=20',source)
  self.assertNotIn('subprocess.run',source)
 def test_windows_environment_copy_casing(self):
  for key in ('SYSTEMROOT','SystemRoot','systemroot'):
   inherited={key:'C:/Windows','Path':'untrusted-search','OTHER':'keep'}
   original=dict(inherited)
   env=runner.fixture_environment(inherited,Path('private-fixture'))
   self.assertEqual(env['PATH'],'private-fixture'+runner.os.pathsep+str(Path('C:/Windows')/'System32'))
   self.assertNotIn('Path',env);self.assertEqual(env['OTHER'],'keep')
   self.assertEqual(inherited,original)
 def test_missing_or_conflicting_system_root_fails_closed(self):
  for inherited in ({},{'SYSTEMROOT':''},{'SYSTEMROOT':'C:/Windows','SystemRoot':'D:/Other'}):
   with self.assertRaises(ValueError):runner.fixture_environment(inherited,Path('fixture'))
 def good(self,scenario='raw-vmin2'):
  return ([{'final_hex':runner.EXPECTED[scenario].hex(),'signals':1}],
          [{'width_before':120,'width_after':119},{'reader_exit':0,'timeout':False,'remaining_native_records':0}])
 def test_all_scenarios_still_inconclusive(self):
  for scenario in runner.SCENARIOS:
   r,i=self.good(scenario); outcome=runner.assess(scenario,r,i,0)
   self.assertEqual(outcome['scenario_verdict'],'pass');self.assertEqual(outcome['exact_path_verdict'],'inconclusive')
 def test_missing_prefix_fails(self):
  r,i=self.good();r[0]['final_hex']='62';self.assertEqual(runner.assess('raw-vmin2',r,i,0)['scenario_verdict'],'fail')
 def test_duplicate_prefix_fails(self):
  r,i=self.good();r[0]['final_hex']='616162';self.assertEqual(runner.assess('raw-vmin2',r,i,0)['scenario_verdict'],'fail')
 def test_timeout_fails(self):
  r,i=self.good();i[-1]['timeout']=True;self.assertEqual(runner.assess('raw-vmin2',r,i,0)['scenario_verdict'],'fail')
 def test_no_geometry_change_fails(self):
  r,i=self.good();i[0]['width_after']=120;self.assertEqual(runner.assess('raw-vmin2',r,i,0)['scenario_verdict'],'fail')
 def test_residual_queue_fails(self):
  r,i=self.good();i[-1]['remaining_native_records']=1;self.assertEqual(runner.assess('raw-vmin2',r,i,0)['scenario_verdict'],'fail')
 def test_zero_signals_not_path_proof(self):
  r,i=self.good();r[0]['signals']=0;self.assertEqual(runner.assess('raw-vmin2',r,i,0)['exact_path_verdict'],'inconclusive')
 def test_fixed_matrix(self):
  self.assertEqual(len(runner.SCENARIOS)*sum(map(len,runner.ORDERS)),24)
 def test_mouse_expected_length(self):
  self.assertEqual(len(runner.EXPECTED['mouse-on']),12)
if __name__=='__main__': unittest.main()
