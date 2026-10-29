#!/usr/bin/env python3
"""Portable structural tests, not native static-safety certification."""
import importlib.util,json,pathlib,tempfile,unittest
from unittest.mock import patch
HERE=pathlib.Path(__file__).resolve().parent
ROOT=HERE.parents[3]
def module(name):
 spec=importlib.util.spec_from_file_location(name,HERE/(name+'.py'));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
review=module('review-evidence');collector=module('collect-evidence')
class Evidence(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.root=pathlib.Path(self.tmp.name)
 def fixture(self):
  self.root.joinpath('diagnostic-dll-pe.txt').write_text('[Ordinal/Name Pointer] Table\n [ 1] stormtty_path_init_v1\n [ 2] stormtty_path_handler_v1\n\n')
  self.root.joinpath('console-native-disassembly.txt').write_text('00000 <stormtty_path_init_v1>:\n 0: c3 ret\n00010 <stormtty_path_handler_v1>:\n 10: c3 ret\n')
  self.root.joinpath('console-native-symbols.txt').write_text('synthetic fixture only')
 def test_structural_success_never_certifies_safety(self):
  self.fixture();r=review.review(self.root)
  self.assertTrue(r['review_required']);self.assertFalse(r['async_signal_safety_verified']);self.assertFalse(r['fixtures_executed']);self.assertFalse(r['production_qualified'])
 def test_modern_binutils_export_format(self):
  self.fixture();self.root.joinpath('diagnostic-dll-pe.txt').write_text('[Ordinal/Name Pointer] Table -- Ordinal Base 1\n Ordinal Hint Name\n [ 0] +base[ 1] 0000 stormtty_path_init_v1\n [ 1] +base[ 2] 0001 stormtty_path_handler_v1\n\n')
  self.assertEqual(review.review(self.root)['exports_verified'],list(review.EXPORTS))
 def test_import_name_is_not_export_proof(self):
  self.fixture();self.root.joinpath('diagnostic-dll-pe.txt').write_text('DLL Name: other.dll\n stormtty_path_init_v1\n stormtty_path_handler_v1\n')
  with self.assertRaises(ValueError):review.review(self.root)
 def test_symbol_name_is_not_instruction_body(self):
  self.fixture();self.root.joinpath('console-native-disassembly.txt').write_text('00000 <stormtty_path_init_v1>:\n00010 <stormtty_path_handler_v1>:\n')
  with self.assertRaises(ValueError):review.review(self.root)
 def test_caps_and_binary_exclusion(self):
  (self.root/'evidence').mkdir();(self.root/'evidence/commit.txt').write_text('abc');(self.root/'evidence/stock-build.log').write_text('tool log over cap')
  (self.root/'evidence/secret.dll').write_bytes(b'MZ')
  with patch.object(collector,'FILE_LIMIT',8):r=collector.collect(self.root,self.root/'packet')
  self.assertFalse(r['collection_complete']);self.assertEqual(len(r['rejected']),1)
  self.assertFalse((self.root/'packet/evidence/secret.dll').exists());self.assertFalse(r['both_static_reports_present'])
 def test_missing_build_results_are_explicit(self):
  r=collector.collect(self.root,self.root/'packet');self.assertIn('stock-build/provenance/static-review.json',r['missing']);self.assertFalse(r['both_static_reports_present'])
 def test_workflow_is_exact_push_build_only(self):
  s=(ROOT/'.github/workflows/msys-passive-build.yml').read_text()
  self.assertIn("branches: ['experiment/msys-passive-build']",s)
  self.assertIn("paths: ['.github/workflows/msys-passive-build.yml']",s)
  self.assertIn("github.ref == 'refs/heads/experiment/msys-passive-build'",s)
  self.assertIn('ref: ${{ github.sha }}',s);self.assertIn('contents: read',s)
  for forbidden in ('workflow_dispatch','pull_request:', 'shell-bootstrap.py','run.py --','reader.exe','controller.exe','stage.py','cmake --build','download-artifact','actions/cache'):
   self.assertNotIn(forbidden,s)
  self.assertIn('timeout-minutes: 150',s);self.assertIn('gnupg python',s)
  self.assertEqual(s.count('passive-path/build.sh'),2)
  upload=s.split('uses: actions/upload-artifact@',1)[1]
  self.assertIn('/review-packet/',upload);self.assertNotIn('*.dll',upload);self.assertNotIn('*.exe',upload)
 def test_actual_runtime_object_not_only_smoke(self):
  s=(HERE/'build.sh').read_text()
  self.assertIn("-path '*/fhandler/console.o'",s);self.assertIn('${#console_objects[@]} == 1',s)
  self.assertIn('console-native-disassembly.txt',s);self.assertIn('review-evidence.py',s)
  self.assertNotIn('-Wno-error',s);self.assertNotIn('--skipinteg',s.replace('# No --syncdeps, --install, --skipinteg, or --nocheck.',''))
if __name__=='__main__':unittest.main()
