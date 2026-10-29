#!/usr/bin/env python3
"""Exact-source generator regressions; source inputs are explicit, never downloaded."""
import importlib.util,pathlib,sys,unittest,re,tempfile,subprocess,json,hashlib
HERE=pathlib.Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('patch',HERE/'make-patch.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
SOURCES={}
class SourceTests(unittest.TestCase):
 def test_reject_unverified_source(self):
  for variant in ('stock','refined'):
   with self.assertRaisesRegex(ValueError,'unverified'):m.generate('not source',variant)
 def test_fail_closed_anchor(self):
  with self.assertRaises(ValueError):m.replace('duplicate duplicate','duplicate','x')
 def test_exact_generation_and_api_counts(self):
  for variant,source in SOURCES.items():
   new,patch=m.generate(source,variant)
   self.assertEqual(m.generate(source,variant),(new,patch))
   for token in ('ReadConsoleInputW (','WriteConsoleInputW (','PeekConsoleInputW (','kill_pgrp (SIGWINCH)','con.fillin ('):
    self.assertEqual(source.count(token),new.count(token),(variant,token))
   self.assertIn('STORMTTY_PASSIVE_CONSOLE_TRACE',patch)
   self.assertIn('--- /dev/null\n+++ b/winsup/cygwin/local_includes/stormtty-passive-path.h',patch)
   self.assertEqual(new.count('st_path::read_begin ('),1)
   self.assertEqual(new.count('st_path::master_event (0, 0, 0, false);'),1)
 def test_checked_in_patch_applies_exactly(self):
  lock=json.loads((HERE/'identities.json').read_text())
  self.assertEqual(hashlib.sha256((HERE/'recorder.hpp').read_bytes()).hexdigest(),lock['header_sha256'])
  for variant,source in SOURCES.items():
   new,patch=m.generate(source,variant)
   self.assertEqual(patch,(HERE/(variant+'.patch')).read_text())
   self.assertEqual(hashlib.sha256(new.encode()).hexdigest(),lock['variants'][variant]['instrumented_sha256'])
   with tempfile.TemporaryDirectory() as directory:
    root=pathlib.Path(directory);console=root/'winsup/cygwin/fhandler/console.cc'
    console.parent.mkdir(parents=True);console.write_text(source)
    subprocess.run(['git','apply','--check',str(HERE/(variant+'.patch'))],cwd=root,check=True,capture_output=True)
    subprocess.run(['git','apply',str(HERE/(variant+'.patch'))],cwd=root,check=True,capture_output=True)
    self.assertEqual(console.read_text(),new)
    self.assertEqual((root/'winsup/cygwin/local_includes/stormtty-passive-path.h').read_bytes(),(HERE/'recorder.hpp').read_bytes())
 def test_return_values_and_candidate_unchanged(self):
  for variant,source in SOURCES.items():
   new,_=m.generate(source,variant)
   a=source.index('fhandler_console::process_input_message (');b=source.index('\nvoid\nfhandler_console::discard_key_events',a)
   aa=new.index('fhandler_console::process_input_message (');bb=new.index('\nvoid\nfhandler_console::discard_key_events',aa)
   before=re.findall(r'return (input_error|input_ok|input_empty|input_winch|stat);',source[a:b])
   after=re.findall(r'return st_path::process_result \((input_error|input_ok|input_empty|input_winch|stat),',new[aa:bb])
   self.assertEqual(before,after)
   self.assertEqual(source.count('discard_key_events (i + 1)'),new.count('discard_key_events (i + 1)'))
 def test_recorder_bounded_and_private(self):
  s=(HERE/'recorder.hpp').read_text()
  self.assertIn('attempt<32',s);self.assertIn('slot>=capacity',s)
  self.assertIn('result==UINT64_MAX?(uint64_t)error:0',s)
  for token in ('printf(', 'fprintf(', 'malloc(', 'new ', 'CreateThread(', 'getenv(', 'Sleep('):self.assertNotIn(token,s)
if __name__=='__main__':
 if len(sys.argv)!=3:raise SystemExit('usage: source-tests.py VERIFIED_STOCK_CONSOLE VERIFIED_REFINED_CONSOLE')
 SOURCES={'stock':pathlib.Path(sys.argv[1]).read_text(),'refined':pathlib.Path(sys.argv[2]).read_text()}
 sys.argv=sys.argv[:1];unittest.main()
