#!/usr/bin/env python3
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[3]
class Edges(unittest.TestCase):
 def test_exact_new_branch_no_other_trigger(self):
  s=(ROOT/'.github/workflows/msys-refined-edges.yml').read_text()
  self.assertIn("branches: ['experiment/msys-refined-edges']",s)
  self.assertIn("paths: ['.github/workflows/msys-refined-edges.yml']",s)
  self.assertIn("github.ref == 'refs/heads/experiment/msys-refined-edges'",s)
  self.assertNotIn('workflow_dispatch',s);self.assertNotIn('pull_request:',s)
 def test_no_guessed_artifact_reuse_or_new_binary_upload(self):
  s=(ROOT/'.github/workflows/msys-refined-edges.yml').read_text()
  self.assertNotIn('download-artifact',s);self.assertNotIn('actions/cache',s)
  self.assertIn('build-refined.sh',s);self.assertIn('stock-build',s);self.assertIn('refined-build',s)
  upload=s.split('      - name: Preserve all fixed trials',1)[1]
  self.assertNotIn('*.dll',upload);self.assertNotIn('*.exe',upload)
 def test_same_native_matrix_and_bounds_only(self):
  s=(ROOT/'.github/workflows/msys-refined-edges.yml').read_text()
  self.assertEqual(s.count('refined-native/run.py --injector'),1)
  self.assertIn('timeout-minutes: 12',s);self.assertIn('timeout-minutes: 180',s)
  self.assertNotIn('compare-managed.py --host',s);self.assertNotIn('compare-refined.py --probe',s)
  self.assertNotIn('prepare-conpty-probe.ps1',s);self.assertNotIn('cmake --build',s)
  self.assertIn('check-reader-imports.py',s);self.assertIn('toolchain.sh',s)
if __name__=='__main__':unittest.main()
