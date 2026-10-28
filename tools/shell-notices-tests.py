#!/usr/bin/env python3
"""Offline tests of the locked notice/source packet; no binaries are executed."""
import argparse
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("shell_notices", Path(__file__).with_name("shell-notices.py"))
notices = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notices)


class NoticeTests(unittest.TestCase):
    def test_all_binary_components_mapped_and_assets_exact(self):
        lock = notices.bootstrap.read_lock(notices.TOOLS / "shell-bootstrap.lock.json")
        assets, mapping = notices.validated_assets(lock)
        self.assertEqual(len(mapping["components"]), 17)
        self.assertEqual(len(assets["assets"]), 79)
        self.assertTrue(all((notices.ASSETS / a["path"]).stat().st_size == a["bytes"] for a in assets["assets"]))

    def test_static_source_closure_is_nineteen_not_seventeen(self):
        lock = notices.bootstrap.read_lock(notices.TOOLS / "shell-bootstrap.lock.json")
        records = notices.source_records(lock)
        self.assertEqual(len(records), 19)
        self.assertEqual(sum(r["bytes"] for r in records), 338606284)
        self.assertIn("readline-8.3.006-1.src.tar.zst", {r["filename"] for r in records})
        self.assertIn("binutils-2.47-1.src.tar.zst", {r["filename"] for r in records})

    def test_deterministic_zip_metadata_and_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            text = root / "input.txt"
            text.write_text("Exact source material\n")
            for name in ("one.zip", "two.zip"):
                with zipfile.ZipFile(root / name, "w") as z:
                    notices.archive_file(z, "sources/input.txt", text, notices.bootstrap.digest(text))
            self.assertEqual((root / "one.zip").read_bytes(), (root / "two.zip").read_bytes())
            with zipfile.ZipFile(root / "one.zip") as z:
                self.assertEqual(z.getinfo("sources/input.txt").date_time, (2026, 10, 28, 0, 0, 0))
                self.assertEqual(z.read("sources/input.txt"), text.read_bytes())

    def test_changed_source_rejected_while_packaging(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            text = root / "source"
            text.write_bytes(b"changed")
            with zipfile.ZipFile(root / "packet.zip", "w") as z, self.assertRaises(RuntimeError):
                notices.archive_file(z, "sources/source", text, "0" * 64)

    def test_zip_traversal_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            text = root / "source"
            text.write_bytes(b"x")
            with zipfile.ZipFile(root / "packet.zip", "w") as z, self.assertRaises(RuntimeError):
                notices.archive_file(z, "../escape", text)

    def test_tampered_notice_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "notice"
            source.write_bytes(b"tampered")
            with self.assertRaises(RuntimeError):
                notices.checked_asset(source, "0" * 64)

    def test_existing_output_not_modified(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = root / "output"
            output.mkdir()
            (output / "keep").write_text("user data")
            args = argparse.Namespace(output=output, runtime=root / "missing", cache=root / "cache",
                                      source_packet=False, offline=True)
            with self.assertRaises(RuntimeError):
                notices.emit(args)
            self.assertEqual((output / "keep").read_text(), "user data")


if __name__ == "__main__":
    unittest.main()
