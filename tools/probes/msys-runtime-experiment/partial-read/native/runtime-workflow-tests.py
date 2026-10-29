#!/usr/bin/env python3
import importlib.util,json,pathlib,tempfile,unittest
from unittest.mock import patch
HERE=pathlib.Path(__file__).resolve().parent;ROOT=HERE.parents[4]
spec=importlib.util.spec_from_file_location('runtime_collect',HERE/'collect-runtime-evidence.py');collector=importlib.util.module_from_spec(spec);spec.loader.exec_module(collector)
class Runtime(unittest.TestCase):
 def test_exact_trigger_and_gate_order(self):
  s=(ROOT/'.github/workflows/msys-partial-read.yml').read_text()
  self.assertIn("branches: ['experiment/msys-partial-read']",s);self.assertIn("paths: ['.github/workflows/msys-partial-read.yml']",s)
  self.assertIn("github.ref == 'refs/heads/experiment/msys-partial-read'",s);self.assertIn('ref: ${{ github.sha }}',s)
  self.assertNotIn('workflow_dispatch',s);self.assertNotIn('pull_request:',s);self.assertNotIn('continue-on-error',s)
  self.assertLess(s.index('linked-preflight.py'),s.index('partial-read/native/reader.c'))
  self.assertIn("steps.linked.outcome == 'success'",s);self.assertIn("steps.staging.outcome == 'success' && steps.controller.outcome == 'success'",s)
  self.assertEqual(s.count('partial-read/native/run.py --controller'),1);self.assertIn('exit $LASTEXITCODE',s)
 def test_fixed_bounds_strict_warnings_no_app(self):
  s=(ROOT/'.github/workflows/msys-partial-read.yml').read_text()
  self.assertIn('timeout-minutes: 180',s);self.assertIn('timeout-minutes: 8',s)
  self.assertIn('-Wall -Wextra -Werror',s);self.assertIn('/W4 /WX',s)
  self.assertNotIn('cmake --build',s);self.assertNotIn('compare-managed',s);self.assertNotIn('zsh.exe',s)
  self.assertIn('original mouse/master predicates remain unsupported; stock positive loss stays failure',s)
  self.assertIn('collect-runtime-evidence.py',s);self.assertIn('/review-packet/',s)
 def test_only_declared_numeric_trials_upload_not_private_binaries(self):
  with tempfile.TemporaryDirectory() as directory:
   root=pathlib.Path(directory);run=root/'evidence/native/1-partial-zero-stock';run.mkdir(parents=True)
   (run/'reader.jsonl').write_text('{"synthetic_hex":"6162"}\n');(run/'trace.bin').write_bytes(b'fixed incomplete trace')
   secret=root/'evidence/native/private-fixtures/stock';secret.mkdir(parents=True);(secret/'msys-2.0.dll').write_bytes(b'MZ')
   (run/'reader.exe').write_bytes(b'MZ');(run/'private.txt').write_text('do not upload')
   dest=root/'review';r=collector.collect(root,dest)
   self.assertTrue((dest/'evidence/native/1-partial-zero-stock/trace.bin').is_file())
   self.assertFalse((dest/'evidence/native/private-fixtures').exists())
   self.assertFalse((dest/'evidence/native/1-partial-zero-stock/reader.exe').exists())
   self.assertFalse((dest/'evidence/native/1-partial-zero-stock/private.txt').exists())
   self.assertFalse(r['runtime_results_present']);self.assertFalse(r['production_qualified'])
   self.assertFalse(r['collection_complete'])
   self.assertIn('evidence/native/results.json',r['missing'])
   inventory=json.loads((dest/'collection.json').read_text())
   self.assertNotIn('fixtures_executed',inventory)
   self.assertEqual(inventory['scope'],'runtime-packet-build-provenance')
 def test_missing_expected_evidence_is_not_complete(self):
  with tempfile.TemporaryDirectory() as directory:
   root=pathlib.Path(directory);native=root/'evidence/native';native.mkdir(parents=True)
   (native/'results.json').write_text('{"planned":8,"trials":[]}')
   r=collector.collect(root,root/'review')
   self.assertTrue(r['runtime_results_present']);self.assertFalse(r['collection_complete'])
   self.assertIn('evidence/native/1-partial-zero-stock/trace.bin',r['missing'])
   self.assertEqual(r['rejected'],[])

 def test_only_failure_only_build_reports_are_optional(self):
  with tempfile.TemporaryDirectory() as directory:
   root=pathlib.Path(directory)
   paths=['evidence/native/results.json']+['evidence/'+name for name in ('reader-pe-imports.txt','fixture-build.txt','linked-stock.log','linked-copied.log')]
   paths += [f'evidence/native/{trial}-{case}-{variant}/{name}' for trial in (1,2) for case in ('partial-zero','positive-prefix','zero-copy') for variant in ('stock','copied') for name in ('trace.bin','reader.jsonl','controller.jsonl','launcher.log')]
   for relative in paths:
    p=root/relative;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'fixture')
   optional=['stock-build/provenance/linked-preflight-error.json','copied-build/provenance/linked-preflight-error.json']
   for i,missing in enumerate((optional,optional+['stock-build/provenance/static-review.json'])):
    def fake_build(root,destination):
     destination.mkdir();return dict(bytes=0,collection_complete=True,missing=missing)
    with patch.object(collector.build,'collect',side_effect=fake_build):r=collector.collect(root,root/f'review-{i}')
    self.assertEqual(r['collection_complete'],i==0)
    self.assertEqual(r['missing_build_expected'],[] if i==0 else ['stock-build/provenance/static-review.json'])

 def test_oversize_numeric_trace_not_silently_truncated(self):
  with tempfile.TemporaryDirectory() as directory:
   root=pathlib.Path(directory);run=root/'evidence/native/2-positive-prefix-copied';run.mkdir(parents=True)
   (run/'trace.bin').write_bytes(b'X'*(collector.TRACE_SIZE+1));dest=root/'review';r=collector.collect(root,dest)
   self.assertFalse(r['collection_complete']);self.assertEqual(r['rejected'][0]['reason'],'size-cap')
   self.assertFalse((dest/'evidence/native/2-positive-prefix-copied/trace.bin').exists())
if __name__=='__main__':unittest.main()
