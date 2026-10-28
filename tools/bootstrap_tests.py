"""Offline unit checks for bootstrap trust boundaries; no SDK/compiler needed."""
import hashlib
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
import zipfile

import bootstrap


class BootstrapTests(unittest.TestCase):
    def test_cache_lock_excludes_concurrent_bootstrap_and_cleans_up(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            with bootstrap.cache_lock(cache):
                with self.assertRaisesRegex(RuntimeError, "already active"):
                    with bootstrap.cache_lock(cache):
                        self.fail("concurrent lock unexpectedly acquired")
            self.assertFalse((cache / "bootstrap.active").exists())

    def test_lock_has_exact_content_and_transport_pins(self):
        lock = json.loads(bootstrap.LOCK.read_text())
        self.assertRegex(lock["ghostty"]["commit"], r"^[a-f0-9]{40}$")
        self.assertEqual(lock["zig"]["version"], "0.17.0")
        self.assertEqual(len(lock["zig"]["archives"]), 6)
        for pin in lock["zig"]["archives"].values():
            self.assertTrue(pin["url"].startswith("https://ziglang.org/download/0.17.0/"))
            self.assertRegex(pin["sha256"], r"^[a-f0-9]{64}$")
        for pin in lock["dependencies"]:
            self.assertTrue(pin["url"].startswith("https://"))
            self.assertRegex(pin["archive_sha256"], r"^[a-f0-9]{64}$")
            self.assertTrue(pin["zig_hash"])

    def test_existing_archive_must_match(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "archive"
            archive.write_bytes(b"expected")
            expected = hashlib.sha256(b"expected").hexdigest()
            bootstrap.download("https://example.invalid/never-used", archive, expected, True)
            archive.write_bytes(b"tampered")
            with self.assertRaisesRegex(RuntimeError, "SHA256 mismatch"):
                bootstrap.download("https://example.invalid/never-used", archive, expected, True)
            self.assertEqual(archive.read_bytes(), b"tampered")

    def test_offline_missing_archive_fails_without_network(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "Offline input missing"):
                bootstrap.download("https://example.invalid/never-used", Path(directory) / "missing", "0" * 64, True)

    def test_zip_path_traversal_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "tool.zip"
            with zipfile.ZipFile(archive, "w") as bundle:
                bundle.writestr("../escape", "bad")
            with self.assertRaisesRegex(RuntimeError, "Unsafe archive path"):
                bootstrap.extract_tool(archive, Path(directory) / "destination")

    def test_regular_zip_extracts(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "tool.zip"
            with zipfile.ZipFile(archive, "w") as bundle:
                bundle.writestr("tool/zig.exe", "ok")
            bootstrap.extract_tool(archive, Path(directory) / "destination")
            self.assertEqual((Path(directory) / "destination/tool/zig.exe").read_text(), "ok")

    def test_tar_links_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "tool.tar.gz"
            with tarfile.open(archive, "w:gz") as bundle:
                member = tarfile.TarInfo("link")
                member.type = tarfile.SYMTYPE
                member.linkname = "/outside"
                bundle.addfile(member)
            with self.assertRaisesRegex(RuntimeError, "Unsupported tool archive entry"):
                bootstrap.extract_tool(archive, Path(directory) / "destination")

    def test_regular_tar_extracts(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "tool.tar.gz"
            with tarfile.open(archive, "w:gz") as bundle:
                member = tarfile.TarInfo("tool/zig")
                member.size = 2
                member.mode = 0o755
                bundle.addfile(member, io.BytesIO(b"ok"))
            bootstrap.extract_tool(archive, Path(directory) / "destination")
            self.assertEqual((Path(directory) / "destination/tool/zig").read_bytes(), b"ok")


if __name__ == "__main__":
    unittest.main()
