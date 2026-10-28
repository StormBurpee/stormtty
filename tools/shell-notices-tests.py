#!/usr/bin/env python3
"""Offline tests of the locked notice/source packet; no binaries are executed."""
import argparse
import hashlib
import importlib.util
import json
import shutil
from pathlib import Path
import tempfile
import unittest
from unittest import mock
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

    def test_exact_notice_recovery_preserves_checkout(self):
        original = b"Pinned vendor notice\nSecond line\n"
        checksum = hashlib.sha256(original).hexdigest()
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "notice"
            for raw in (original, original.replace(b"\n", b"\r\n"),
                        b"\xef\xbb\xbf" + original,
                        b"\xef\xbb\xbf" + original.replace(b"\n", b"\r\n")):
                with self.subTest(raw=raw):
                    source.write_bytes(raw)
                    self.assertEqual(notices.notice_bytes(source, checksum, len(original)), original)
                    self.assertEqual(source.read_bytes(), raw)

    def test_notice_recovery_requires_exact_hash_and_size(self):
        original = b"Pinned vendor notice\n"
        raw = original.replace(b"\n", b"\r\n")
        checksum = hashlib.sha256(original).hexdigest()
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "notice"
            for data, pin, size in ((raw, "0" * 64, len(original)),
                                    (raw, checksum, len(original) + 1),
                                    (raw + b"tampered", checksum, len(original))):
                with self.subTest(data=data, pin=pin, size=size):
                    source.write_bytes(data)
                    with self.assertRaises(RuntimeError) as caught:
                        notices.notice_bytes(source, pin, size)
                    self.assertIn(pin, str(caught.exception))
                    self.assertIn(hashlib.sha256(data).hexdigest(), str(caught.exception))
                    self.assertEqual(source.read_bytes(), data)
            source.write_bytes(raw)
            with self.assertRaises(RuntimeError):
                notices.checked_asset(source, checksum, len(original))

    def test_pinned_original_crlf_notice_is_preserved(self):
        original = b"Original upstream CRLF\r\n"
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "notice"
            source.write_bytes(original)
            self.assertEqual(notices.notice_bytes(source, hashlib.sha256(original).hexdigest(), len(original)), original)
            self.assertEqual(source.read_bytes(), original)

    def test_overlay_and_full_source_packet_emit_exact_recovered_notice(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            assets = root / "assets"
            shutil.copytree(notices.ASSETS, assets)
            source = assets / "texts/zsh/LICENCE"
            original = source.read_bytes()
            raw = b"\xef\xbb\xbf" + original.replace(b"\n", b"\r\n")
            source.write_bytes(raw)
            lock = notices.bootstrap.read_lock(notices.TOOLS / "shell-bootstrap.lock.json")
            runtime = root / "runtime"
            records = []
            for package in lock["packages"]:
                name = ".stormtty-packages/" + package["name"] + "/.BUILDINFO"
                path = runtime / name
                path.parent.mkdir(parents=True)
                path.write_bytes(b"inert build evidence\n")
                records.append({"path": name, "sha256": notices.bootstrap.digest(path), "bytes": path.stat().st_size})
            (runtime / "stormtty-shell-manifest.json").write_text(json.dumps({
                "lock_sha256": notices.bootstrap.digest(notices.TOOLS / "shell-bootstrap.lock.json"), "files": records}))
            archive = root / "source-fixture.tar.zst"
            archive.write_bytes(b"opaque unit-test source archive")
            source_record = {"filename": archive.name, "bytes": archive.stat().st_size, "sha256": notices.bootstrap.digest(archive)}
            output = root / "output"
            args = argparse.Namespace(output=output, runtime=runtime, cache=root / "cache", source_packet=True, offline=True)
            with mock.patch.object(notices, "ASSETS", assets), \
                    mock.patch.object(notices, "source_records", return_value=[source_record]), \
                    mock.patch.object(notices.bootstrap, "download", return_value=archive):
                notices.emit(args)
            self.assertEqual(source.read_bytes(), raw)
            self.assertEqual((output / "runtime-notices/texts/zsh/LICENCE").read_bytes(), original)
            with zipfile.ZipFile(output / "corresponding-source.zip") as bundle:
                self.assertEqual(bundle.read("notices/texts/zsh/LICENCE"), original)
                self.assertEqual(bundle.read("stormtty-packaging/tools/shell-notices/texts/zsh/LICENCE"), original)
                for asset in ("shell-runtime.fstab", "shell-runtime.nsswitch.conf", "shell-path-tests.py"):
                    self.assertEqual(bundle.read("stormtty-packaging/tools/" + asset),
                                     (notices.TOOLS / asset).read_bytes())

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
