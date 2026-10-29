import importlib.util,pathlib,unittest,sys
spec=importlib.util.spec_from_file_location('shipping',pathlib.Path(__file__).with_name('run-shipping-deadline.py'));m=importlib.util.module_from_spec(spec);sys.modules[spec.name]=m;spec.loader.exec_module(m)
class Tests(unittest.TestCase):
 def test_modes_and_strict_counters(self):
  base=['MANAGED_TYPING_RESULT code=0 characters=25 frames=28 retreats=0 echo_total_us=3 echo_max_us=2','MANAGED_STATE_RESULT code=0 phase=11 cold_samples=1 warm_samples=1 checks=20 cold_ms=20 warm_ms=10']
  off='SHIPPING_DEADLINE '+' '.join(k+'=0' for k in m.FIELDS)
  on='SHIPPING_DEADLINE requested=1 available=1 waits=30 timers=25 notified=5 spurious=0 failures=0 overshoot_total_us=200 overshoot_max_us=12'
  self.assertTrue(m.validate(base+[off],False));self.assertTrue(m.validate(base+[on],True))
  for bad in [on.replace('available=1','available=0'),on.replace('spurious=0','spurious=1'),on.replace('failures=0','failures=1'),on.replace('timers=25','timers=0')]:self.assertFalse(m.validate(base+[bad],True))
  self.assertFalse(m.validate(base+[off],True));self.assertFalse(m.validate(base+[on],False));self.assertFalse(m.validate(base+[on,on],True))
 def test_isolated_workflow(self):
  s=(pathlib.Path(__file__).resolve().parents[2]/'.github/workflows/shipping-deadline.yml').read_text()
  self.assertIn('branches: [diagnostic/shipping-deadline]',s);self.assertNotIn('pull_request:',s);self.assertIn('session_deadline_failure_tests',s);self.assertIn('session_deadline_unsupported_tests',s)
if __name__=='__main__':unittest.main()
