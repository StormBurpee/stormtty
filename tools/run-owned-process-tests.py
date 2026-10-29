#!/usr/bin/env python3
"""Portable regressions for the CI-only process-handle completion contract."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

spec = importlib.util.spec_from_file_location("owned_runner", Path(__file__).with_name("run-owned-process.py"))
runner = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runner
spec.loader.exec_module(runner)


class OwnedProcessTests(unittest.TestCase):
    def test_exit_code_and_merged_output(self):
        result = runner.run_owned([sys.executable, "-c", "import sys; print('out',flush=True); print('err',file=sys.stderr,flush=True); sys.exit(37)"])
        self.assertEqual(result.returncode, 37)
        self.assertEqual(result.exit_code, 37)
        self.assertFalse(result.timed_out)
        self.assertTrue(result.reaped)
        self.assertIsNone(result.alive_at_timeout)
        self.assertIn(b"out", result.output_tail)
        self.assertIn(b"err", result.output_tail)

    def test_actual_live_child_timeout(self):
        result = runner.run_owned([sys.executable, "-c", "import time; print('started',flush=True); time.sleep(30)"], timeout=0.4, reap_timeout=3)
        self.assertTrue(result.timed_out)
        self.assertTrue(result.alive_at_timeout)
        self.assertTrue(result.reaped)
        self.assertEqual(result.exit_code, 124)
        self.assertIn(b"started", result.output_tail)
        self.assertLess(result.elapsed_seconds, 5)

    def test_bounded_tail(self):
        result = runner.run_owned([sys.executable, "-c", "import sys; sys.stdout.write('x'*200000+'END')"], tail_bytes=128)
        self.assertEqual(len(result.output_tail), 128)
        self.assertTrue(result.output_truncated)
        self.assertTrue(result.output_tail.endswith(b"END"))

    def test_exited_child_with_descendant_holding_stdout(self):
        # The short-lived descendant intentionally inherits the capture handle.
        # It is not killed by the runner. An explicit marker lets the test wait
        # for its output handles to close before deleting our temporary directory.
        with tempfile.TemporaryDirectory() as directory:
            finished = Path(directory) / "descendant-finished"
            # Publish only after write_text has closed its file. exists() on the
            # written file races that close and causes WinError 32 in cleanup.
            pending = finished.with_name('descendant-pending')
            descendant = ("import os,pathlib,time; time.sleep(3); os.close(1); os.close(2); "
                          "pathlib.Path(" + repr(str(pending)) + ").write_text('done'); "
                          "os.replace(" + repr(str(pending)) + "," + repr(str(finished)) + ")")
            parent = ("import subprocess,sys; subprocess.Popen([sys.executable,'-c'," + repr(descendant) +
                      "],stdout=sys.stdout,stderr=sys.stderr); print('parent done',flush=True); sys.exit(23)")
            result = runner.run_owned([sys.executable, "-c", parent], timeout=1.5, output_dir=directory)
            try:
                self.assertEqual(result.exit_code, 23)
                self.assertFalse(result.timed_out)
                self.assertTrue(result.reaped)
                self.assertIn(b"parent done", result.output_tail)
                self.assertFalse(finished.exists(), "descendant should still hold stdout when parent wait completes")
            finally:
                deadline = time.monotonic() + 8
                while not finished.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertTrue(finished.exists(), "controlled descendant did not publish completion")
                self.assertEqual(finished.read_text(), "done")
                self.assertFalse(pending.exists())
                # The descendant closes its inherited output handles before
                # the marker, so Windows can remove the capture directory.
                # No process-tree kill is used even in this test.

    def test_launch_failure_and_argument_validation(self):
        with self.assertRaises(OSError):
            runner.run_owned([str(Path(tempfile.gettempdir()) / "stormtty-nonexistent-child-executable")])
        for timeout in (0, -1, float("inf"), float("nan")):
            with self.assertRaises(ValueError):
                runner.run_owned([sys.executable], timeout=timeout)

    def test_cli_preserves_exit(self):
        result = subprocess.run([sys.executable, str(Path(__file__).with_name("run-owned-process.py")),
                                 "--timeout", "3", "--", sys.executable, "-c", "raise SystemExit(19)"],
                                stdout=subprocess.PIPE, timeout=5)
        self.assertEqual(result.returncode, 19)
        self.assertIn(b'"returncode": 19', result.stdout)


if __name__ == "__main__":
    unittest.main()
