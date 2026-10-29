#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
spec = importlib.util.spec_from_file_location('runner', Path(__file__).with_name('run-nano-probe.py'))
runner = importlib.util.module_from_spec(spec); spec.loader.exec_module(runner)

class Tests(unittest.TestCase):
    def test_only_known_numeric_fields_are_retained(self):
        good = 'MANAGED_NANO_DIAGNOSTIC phase=41 file_exists=1 file_bytes=9 file_exact=0'
        raw = '\n'.join(['private terminal text', good, 'MANAGED_NANO_RESULT code=1 path=123',
                         'MANAGED_NANO_RESULT code=1 phase=private', 'MANAGED_NANO_RESULT code=1 code=0'])
        self.assertEqual(runner.sanitized(raw.encode()), good)

    def test_evidence_is_bounded(self):
        result = runner.sanitized(('MANAGED_NANO_RESULT code=1 phase=41 checks=20\n' * 1000).encode())
        self.assertEqual(len(result.splitlines()), 16)
        self.assertEqual(runner.sanitized('MANAGED_NANO_RESULT code=' + '1' * 2000), '')

    def test_both_provider_logs_survive_timeout(self):
        result = subprocess.CompletedProcess([], 0, b'MANAGED_NANO_RESULT code=0 phase=5 checks=28\n')
        error = subprocess.TimeoutExpired([], 180, output=b'private\nMANAGED_NANO_DIAGNOSTIC phase=41 file_exact=0\n')
        with tempfile.TemporaryDirectory() as temp, patch.object(runner.subprocess, 'run', side_effect=[error, result]) as run:
            self.assertEqual(runner.run(Path('probe'), Path('runtime'), Path('provider'), Path(temp)), 1)
            self.assertEqual(run.call_count, 2)
            logs = [p.read_text() for p in Path(temp).glob('*.log')]
            self.assertEqual(len(logs), 2)
            self.assertNotIn('private', ''.join(logs))
            self.assertIn('timeout=1', ''.join(logs))

    def test_missing_result_cannot_pass(self):
        result = subprocess.CompletedProcess([], 0, b'not qualification evidence')
        with tempfile.TemporaryDirectory() as temp, patch.object(runner.subprocess, 'run', return_value=result):
            self.assertEqual(runner.run(Path('probe'), Path('runtime'), Path('provider'), Path(temp)), 1)

if __name__ == '__main__': unittest.main()
