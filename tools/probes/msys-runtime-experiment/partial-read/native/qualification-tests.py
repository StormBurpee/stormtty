#!/usr/bin/env python3
"""Exact isolated qualification contracts; no native behavior claimed."""
import hashlib,importlib.util,json,os,pathlib,subprocess,tempfile,unittest
HERE=pathlib.Path(__file__).resolve().parent;EXPERIMENT=HERE.parents[1]
def module(path):
 spec=importlib.util.spec_from_file_location(path.stem,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
stage=module(HERE/'stage.py');verify=module(HERE/'verify.py')
class Qualification(unittest.TestCase):
 def test_exact_observer_and_stock_control_preserved(self):
  old=EXPERIMENT/'passive-path';lock=json.loads((HERE/'identities.json').read_text())
  for name in ('recorder.hpp','recorder-smoke.cpp','stock.patch'):
   self.assertEqual((HERE/name).read_bytes(),(old/name).read_bytes())
  self.assertEqual(lock['variants']['stock'],json.loads((old/'identities.json').read_text())['variants']['stock'])
  for variant in ('stock','copied'):
   self.assertEqual(hashlib.sha256((HERE/(variant+'.patch')).read_bytes()).hexdigest(),lock['variants'][variant]['patch_sha256'])
   self.assertEqual(stage.SOURCE_FILES_SHA256[variant]['console.cc'],lock['variants'][variant]['instrumented_sha256'])
   self.assertEqual(stage.SOURCE_FILES_SHA256[variant]['fhandler.h'],stage.ORIGINAL_SOURCE_FILES_SHA256['fhandler.h'])
 def test_locked_trial_plan_fixture_sources(self):
  plan=json.loads((HERE/'trial-plan.json').read_text())
  self.assertEqual(plan['planned'],12);self.assertFalse(plan['production_qualified'])
  self.assertEqual(plan['cases'],['partial-zero','positive-prefix','zero-copy'])
  self.assertEqual(plan['orders'],[['stock','copied'],['copied','stock']])
  self.assertEqual(plan['expected_hex'],{'partial-zero':'6162','positive-prefix':'616263','zero-copy':'62'})
  for name,digest in plan['source_sha256'].items():self.assertEqual(hashlib.sha256((HERE/name).read_bytes()).hexdigest(),digest)
 def test_only_new_behavior_patch_builds(self):
  text=(HERE/'build.sh').read_text()
  self.assertNotIn('0006-',text);self.assertIn('0007-preserve-copied-on-signal.patch',text)
  self.assertIn('cf4eba2150f2b99dfe7c55c8d73ac0f578d7675e649b8468a364bfb8fe2efdb2',text)
  self.assertIn('if [[ $variant == copied ]]',text)
  self.assertNotIn('-Wno-error',text)
 def test_zero_copy_changes_no_timing_bound(self):
  old=EXPERIMENT/'passive-path'
  reader=(HERE/'reader.c').read_text();controller=(HERE/'controller.c').read_text()
  self.assertIn('size_t expected = zero ? 1 : (positive ? 3 : 2)',reader)
  self.assertIn('zero ? "b" : (positive ? "abc" : "ab")',reader)
  self.assertIn('strcmp(argv[2],"zero-copy") && (!WriteConsoleInputW',controller)
  import re
  for name in ('reader.c','controller.c'):
   for pattern in (r'Sleep\([^)]*\)',r'alarm\([^)]*\)',r'WaitForSingleObject\([^;]*\)',r'GetTickCount64\(\)-start[^;{]*'):
    self.assertEqual(re.findall(pattern,(HERE/name).read_text()),re.findall(pattern,(old/name).read_text()))
 def test_link_gate_uses_separate_source_manifest_and_reviewed_helpers(self):
  text=(HERE/'linked-preflight.py').read_text()
  self.assertIn("['variants']['stock']",text)
  self.assertIn("lock['variants'][variant]['instrumented_sha256']",text)
  self.assertIn("review['observer_helpers']",text)
  self.assertNotIn('native-review-40916378419.json', '\n'.join(p.name for p in HERE.iterdir()))
 def test_fixed_twelve_no_success_mask(self):
  text=(HERE/'run.py').read_text()
  self.assertIn("CASES=('partial-zero','positive-prefix','zero-copy')",text)
  self.assertIn("ORDERS=(('stock','copied'),('copied','stock'))",text)
  self.assertIn('planned=12',text)
  self.assertIn("if any(row['byte_verdict']!='PASS'",text)
  self.assertIn('resize.analyze(trace,case,variant)',text)
 @unittest.skipUnless(os.environ.get('PARTIAL_READ_SOURCE'),'optional local exact prepared-source check; native build always gates hashes')
 def test_apply_behavior_then_observer_exactly(self):
  source=pathlib.Path(os.environ['PARTIAL_READ_SOURCE']).read_bytes()
  for variant in ('stock','copied'):
   with tempfile.TemporaryDirectory() as temporary:
    root=pathlib.Path(temporary);p=root/'winsup/cygwin/fhandler/console.cc';p.parent.mkdir(parents=True);p.write_bytes(source)
    (root/'winsup/cygwin/local_includes').mkdir()
    if variant=='copied':subprocess.run(['git','apply',str(HERE.parent/'0007-preserve-copied-on-signal.patch')],cwd=root,check=True)
    verify.verify(variant,p,'before')
    subprocess.run(['git','apply',str(HERE/(variant+'.patch'))],cwd=root,check=True)
    verify.verify(variant,p,'after')
if __name__=='__main__':unittest.main()
