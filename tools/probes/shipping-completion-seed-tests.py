import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('shipping_results',Path(__file__).with_name('run-shipping-completion-seed.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class ResultTests(unittest.TestCase):
    def data(self,candidate):
        lines=[]
        for i in range(1,7):
            lines.append(f'STARTUP_SAMPLE sample={i} fresh_state={int(i<=3)} state_prepare_ms=2 launch_to_zle_ms=1000')
            if candidate:lines.append(f'SHIPPING_SAMPLE sample={i} adoption={1 if i<=3 else -1} verification_publish_ms=500 native_ack=1')
        return ('\n'.join(lines+[m.RESULT])+'\n').encode()
    def test_verification_not_double_counted(self):
        _,ok,metrics=m.sanitized(self.data(True),True);self.assertTrue(ok)
        self.assertEqual([r['total_ms'] for r in metrics],[1002]*6)
    def test_rejected_seed_not_a_speedup_sample(self):
        _,ok,_=m.sanitized(self.data(True).replace(b'adoption=1',b'adoption=0',1),True);self.assertFalse(ok)
    def test_native_ack_required(self):
        _,ok,_=m.sanitized(self.data(True).replace(b'native_ack=1',b'native_ack=0',1),True);self.assertFalse(ok)
    def test_baseline_and_privacy(self):
        lines,ok,_=m.sanitized(self.data(False)+b'secret/user/path\n',False)
        self.assertTrue(ok);self.assertNotIn('secret','\n'.join(lines))
    def test_numeric_verifier_diagnostic_cannot_validate_rejection(self):
        diagnostic=b'SHIPPING_VERIFY sample=1 stage=4 object=6 win32_error=32\n'
        lines,ok,_=m.sanitized(self.data(True).replace(b'adoption=1',b'adoption=0',1)+diagnostic,True)
        self.assertFalse(ok);self.assertIn(diagnostic.decode().strip(),lines)
        lines,_,_=m.sanitized(self.data(False)+b'SHIPPING_VERIFY sample=1 stage=path object=user win32_error=secret\n',False)
        self.assertNotIn('secret','\n'.join(lines))
if __name__=='__main__':unittest.main()
