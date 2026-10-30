"""Synthetic privacy/schema/failure tests: no native process or capture required."""
import contextlib
from dataclasses import replace
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import native_application_receipt as receipts

spec = importlib.util.spec_from_file_location("receipt_test_runner", Path(__file__).with_name("run-owned-process.py"))
runner = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runner
spec.loader.exec_module(runner)


def result(**changes):
    original = runner.Result(987654321, 0, False, None, True, 123.5, b"", False, "")
    return replace(original, **changes)


class ReceiptTests(unittest.TestCase):
    def decode(self, value):
        encoded = receipts.receipt_bytes("teardown-explicit-1", value)
        self.assertLessEqual(len(encoded), receipts.MAX_RECEIPT_BYTES)
        return json.loads(encoded)

    def test_exact_schema_and_types(self):
        data = self.decode(result())
        self.assertEqual(set(data), {"schema_version", "case", "returncode", "exit_code", "timed_out", "alive_at_timeout", "reaped", "output_truncated", "cleanup_error_present", "signatures"})
        self.assertEqual(data["schema_version"], 1)
        self.assertEqual(data["case"], "teardown-explicit-1")
        self.assertIs(type(data["returncode"]), int)
        self.assertIs(type(data["exit_code"]), int)
        for name in ("timed_out", "reaped", "output_truncated", "cleanup_error_present"):
            self.assertIs(type(data[name]), bool)
        self.assertIsNone(data["alive_at_timeout"])
        self.assertEqual(data["signatures"], {"missing_expected_marker_observed": None, "malformed_paste_observed": None, "stale_marker_observed": None})

    def test_timeout_and_unreaped_results_remain_exact(self):
        data = self.decode(result(returncode=-9, timed_out=True, alive_at_timeout=True))
        self.assertEqual((data["returncode"], data["exit_code"]), (-9, 124))
        self.assertTrue(data["alive_at_timeout"])
        data = self.decode(result(returncode=None, reaped=False))
        self.assertIsNone(data["returncode"])
        self.assertEqual(data["exit_code"], 125)
        self.assertFalse(data["reaped"])
        self.assertEqual(self.decode(result(returncode=37))["exit_code"], 37)

    def test_authored_case_allowlist(self):
        self.assertEqual(len(receipts.CASE_IDS), 29)
        for case in receipts.CASE_IDS:
            self.assertEqual(json.loads(receipts.receipt_bytes(case, result()))["case"], case)
        for case in ("../secret", "C:\\private", "teardown-explicit-11", "teardown-default-4", "arbitrary", "smoke"):
            with self.assertRaises(ValueError):
                receipts.receipt_bytes(case, result())

    def test_private_data_never_serialized(self):
        private = "SECRET_TOKEN=/private/user --argv PASSWORD=value"
        encoded = receipts.receipt_bytes("installed-console-settings", result(output_tail=private.encode(), cleanup_error=private))
        for forbidden in (private.encode(), b"987654321", b"123.5", b"output_tail", b'"pid"', b"argv", b"PASSWORD", b"/private"):
            self.assertNotIn(forbidden, encoded)
        self.assertTrue(json.loads(encoded)["cleanup_error_present"])

    def test_only_reviewed_signature_is_positive(self):
        data = self.decode(result(output_tail=receipts.MISSING_MARKER, output_truncated=True))
        self.assertIs(data["signatures"]["missing_expected_marker_observed"], True)
        for output in (b"[200~", b"stale marker", b"history.new", receipts.MISSING_MARKER[:-1], b"", b"Native smoke phase deadline expired."):
            data = self.decode(result(output_tail=output, output_truncated=True, timed_out=True, returncode=1))
            self.assertTrue(all(value is None for value in data["signatures"].values()))

    def test_invalid_and_oversized_data(self):
        for value in (result(returncode=True), result(timed_out=1), result(alive_at_timeout=0), result(output_tail=b"x" * (receipts.MAX_TAIL_BYTES + 1))):
            with self.assertRaises(ValueError):
                self.decode(value)
        with patch.object(receipts, "MAX_RECEIPT_BYTES", 10):
            with self.assertRaises(ValueError):
                self.decode(result())

    def test_atomic_replacement_and_independent_case_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertTrue(receipts.write_receipt("teardown-default-1", result(), root))
            self.assertTrue(receipts.write_receipt("teardown-default-1", result(returncode=7), root))
            self.assertTrue(receipts.write_receipt("teardown-default-2", result(), root))
            self.assertEqual(json.loads((root / "teardown-default-1.json").read_text())["exit_code"], 7)
            self.assertEqual(len(list(root.iterdir())), 2)

    def test_write_failure_does_not_replace_result_or_stop_next_case(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            value = result(returncode=37)
            self.assertTrue(receipts.write_receipt("teardown-default-1", value, root))
            before = (root / "teardown-default-1.json").read_bytes()
            warning = io.StringIO()
            with patch.object(receipts.os, "replace", side_effect=OSError("SECRET /private/path")), contextlib.redirect_stderr(warning):
                self.assertFalse(receipts.write_receipt("teardown-default-1", result(), root))
            self.assertEqual(warning.getvalue(), receipts.WARNING + "\n")
            self.assertEqual(value.exit_code, 37)
            self.assertEqual((root / "teardown-default-1.json").read_bytes(), before)
            self.assertEqual(list(root.glob("*.tmp")), [])
            self.assertTrue(receipts.write_receipt("teardown-default-2", result(), root))

    def test_directory_failure_is_fixed_warning_only(self):
        with patch.object(receipts.Path, "mkdir", side_effect=OSError("SECRET /private/path")), contextlib.redirect_stderr(io.StringIO()) as warning:
            self.assertFalse(receipts.write_receipt("packaged-smoke", result()))
        self.assertEqual(warning.getvalue(), receipts.WARNING + "\n")


if __name__ == "__main__":
    unittest.main()
