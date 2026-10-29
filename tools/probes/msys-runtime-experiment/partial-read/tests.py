#!/usr/bin/env python3
"""Portable decision model plus exact pinned patch regression, not native proof."""
import hashlib,json,os,pathlib,subprocess,tempfile,unittest
HERE=pathlib.Path(__file__).resolve().parent
IDS=json.loads((HERE/'identities.json').read_text())
def signal_exit(copied,errno_before):
 return (copied,errno_before) if copied else (-1,4)
def read_transition(path,copied,errno_before=71,restart=False):
 # Models only the reviewed exits; ownership is independently source-audited.
 if path=='input-signal' and restart:return ('continue',errno_before)
 if path in ('wait-signal','input-signal'):return signal_exit(copied,errno_before)
 if path in ('timeout','nonblocking-empty'):return (copied,errno_before) if copied else (-1,11)
 if path=='error':return (-1,errno_before)
 if path=='cancel':return ('cancel',errno_before)
 if path=='success':return (copied,errno_before)
 raise ValueError(path)
def needs_more(requested,copied,vmin,canonical=False):
 return requested>copied and not canonical and copied<vmin
class Model(unittest.TestCase):
 def test_positive_count_preserved_both_signal_ingresses(self):
  for path in ('wait-signal','input-signal'):
   for n in (1,2,63,64,256):self.assertEqual(read_transition(path,n),(n,71))
 def test_zero_copy_eintr_both_ingresses(self):
  for path in ('wait-signal','input-signal'):self.assertEqual(read_transition(path,0),(-1,4))
 def test_errno_not_cleared_or_set_on_positive_return(self):
  for errno in (0,4,5,71):self.assertEqual(signal_exit(1,errno),(1,errno))
 def test_restart_remains_before_signal_exit(self):
  for n in (0,1,64):self.assertEqual(read_transition('input-signal',n,restart=True),('continue',71))
 def test_unrelated_errors_remain_errors_even_after_copy(self):
  for n in (0,1,64):self.assertEqual(read_transition('error',n),(-1,71))
 def test_cancel_is_not_short_read(self):
  self.assertEqual(read_transition('cancel',1),('cancel',71))
 def test_normal_success_unchanged(self):
  for n in (0,1,64):self.assertEqual(read_transition('success',n),(n,71))
 def test_observed_loss_counterexample_and_candidate_partition(self):
  for payload in (b'ab',b'abc'):
   first=payload[:1];tail=payload[1:]
   self.assertNotEqual(b''+tail,payload) # old -1 hides first copied byte
   n,_=signal_exit(len(first),0)
   self.assertEqual(first[:n]+tail,payload) # no replay, retry, or extra input
 def test_existing_eagain_exits_unchanged(self):
  for path in ('timeout','nonblocking-empty'):
   self.assertEqual(read_transition(path,0),(-1,11))
   for copied in (1,2,64):self.assertEqual(read_transition(path,copied),(copied,71))
 def test_read_more_eligibility_boundaries(self):
  self.assertTrue(needs_more(2,1,2))
  self.assertTrue(needs_more(64,1,2))
  self.assertFalse(needs_more(2,2,3)) # capacity reached, even if VMIN larger
  self.assertFalse(needs_more(64,2,2)) # VMIN reached
  self.assertFalse(needs_more(64,1,2,canonical=True))
  self.assertFalse(needs_more(64,1,0))
 def test_patch_identity_and_single_file_hunk(self):
  p=(HERE/'0007-preserve-copied-on-signal.patch').read_bytes()
  self.assertEqual(hashlib.sha256(p).hexdigest(),IDS['patch_sha256'])
  self.assertEqual(p.count(b'@@ -'),1);self.assertEqual(p.count(b'--- a/'),1)
  self.assertNotIn(b'-  set_sig_errno',p);self.assertNotIn(b'-  buflen',p)
 @unittest.skipUnless(os.environ.get('PARTIAL_READ_SOURCE'),'set PARTIAL_READ_SOURCE to exact console.cc.before for patch gate')
 def test_exact_source_patch_and_unchanged_other_paths(self):
  source=pathlib.Path(os.environ['PARTIAL_READ_SOURCE']).read_bytes()
  self.assertEqual(hashlib.sha256(source).hexdigest(),IDS['stock_console_sha256'])
  with tempfile.TemporaryDirectory() as tmp:
   root=pathlib.Path(tmp);f=root/'winsup/cygwin/fhandler/console.cc';f.parent.mkdir(parents=True);f.write_bytes(source)
   subprocess.run(['git','apply','--check',str(HERE/'0007-preserve-copied-on-signal.patch')],cwd=root,check=True,capture_output=True)
   subprocess.run(['git','apply',str(HERE/'0007-preserve-copied-on-signal.patch')],cwd=root,check=True,capture_output=True)
   after=f.read_bytes();self.assertEqual(hashlib.sha256(after).hexdigest(),IDS['candidate_console_sha256'])
   before_head,before_tail=source.split(b'sig_exit:\n');after_head,after_tail=after.split(b'sig_exit:\n')
   self.assertEqual(before_head,after_head)
   self.assertEqual(before_tail,after_tail[after_tail.index(b'  set_sig_errno (EINTR);'):])
   read=before_head.split(b'fhandler_console::read (void *pv, size_t& buflen)')[1]
   self.assertEqual(read.count(b'goto sig_exit;'),2)
   self.assertIn(b'err:\n  buflen = (size_t) -1;\n  return;',read)
if __name__=='__main__':unittest.main()
