#!/usr/bin/env python3
"""Offline safety/regression tests; never executes a shell or downloads packages."""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("shell_bootstrap", Path(__file__).with_name("shell-bootstrap.py"))
shell = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shell)


class ShellBootstrapTests(unittest.TestCase):
    def bundle(self, entries):
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode="w") as out:
            for name, data, kind in entries:
                member = tarfile.TarInfo(name)
                member.type = kind
                member.size = len(data) if kind == tarfile.REGTYPE else 0
                member.linkname = "../../outside" if kind in (tarfile.SYMTYPE, tarfile.LNKTYPE) else ""
                out.addfile(member, io.BytesIO(data) if kind == tarfile.REGTYPE else None)
        stream.seek(0)
        return tarfile.open(fileobj=stream, mode="r:")

    def extract(self, entries, root):
        records = {}
        with self.bundle(entries) as bundle:
            shell.extract_package(bundle, root, "test", records, {"entries": 0, "bytes": 0})
        return records

    def test_lock_exact_official_and_sources(self):
        lock = shell.read_lock(shell.LOCK)
        self.assertEqual(len(lock["packages"]), 17)
        self.assertEqual(len(lock["sources"]), 17)
        self.assertFalse(lock["default_activation_allowed"])
        self.assertEqual(sum(p["bytes"] for p in lock["packages"]), 15190478)

    def test_reject_windows_aliases_and_traversal(self):
        for name in ("../escape", "/absolute", "usr/../escape", "C:/escape", "usr\\bin\\bad",
                     "usr/file:ads", "usr/nul", "usr/COM1.txt", "usr/file.", "usr/file ",
                     "usr//file", "usr/./file", "usr/\0bad"):
            with self.subTest(name=name), self.assertRaises(RuntimeError):
                shell.portable_path(name)

    def test_reject_all_link_and_special_types(self):
        with tempfile.TemporaryDirectory() as tmp:
            for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.CHRTYPE, tarfile.FIFOTYPE):
                with self.subTest(kind=kind), self.assertRaises(RuntimeError):
                    self.extract([("usr/bad", b"", kind)], Path(tmp))

    def test_metadata_is_preserved_but_never_executed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.extract([(".INSTALL", b"SHOULD_NOT_EXECUTE", tarfile.REGTYPE),
                          ("usr/bin/zsh.exe", b"MZ", tarfile.REGTYPE)], root)
            self.assertEqual((root / ".stormtty-packages/test/.INSTALL").read_bytes(), b"SHOULD_NOT_EXECUTE")
            self.assertFalse((root / ".INSTALL").exists())
            self.assertEqual((root / "usr/bin/zsh.exe").read_bytes(), b"MZ")

    def test_duplicate_and_case_colliding_files_fail(self):
        for other in ("usr/bin/zsh.exe", "usr/bin/ZSH.EXE"):
            with tempfile.TemporaryDirectory() as tmp, self.assertRaises(RuntimeError):
                self.extract([("usr/bin/zsh.exe", b"one", tarfile.REGTYPE),
                              (other, b"two", tarfile.REGTYPE)], Path(tmp))

    def test_internal_hardlink_materialized_as_regular_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stream = io.BytesIO()
            with tarfile.open(fileobj=stream, mode="w") as out:
                original = tarfile.TarInfo("usr/bin/zsh-version.exe")
                original.size = 2
                out.addfile(original, io.BytesIO(b"MZ"))
                alias = tarfile.TarInfo("usr/bin/zsh.exe")
                alias.type = tarfile.LNKTYPE
                alias.linkname = original.name
                out.addfile(alias)
            stream.seek(0)
            with tarfile.open(fileobj=stream) as bundle:
                shell.extract_package(bundle, root, "test", {}, {"entries": 0, "bytes": 0})
            self.assertEqual((root / "usr/bin/zsh.exe").read_bytes(), b"MZ")
            self.assertFalse((root / "usr/bin/zsh.exe").is_symlink())

    def test_implicit_parent_case_collision_fails(self):
        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(RuntimeError):
            self.extract([("usr/Bin/one", b"x", tarfile.REGTYPE),
                          ("usr/bin/two", b"y", tarfile.REGTYPE)], Path(tmp))

    def test_unexpected_roots_and_metadata_fail(self):
        for name in ("home/user/.zshrc", ".UNKNOWN", ".PKGINFO/evil"):
            with tempfile.TemporaryDirectory() as tmp, self.assertRaises(RuntimeError):
                self.extract([(name, b"bad", tarfile.REGTYPE)], Path(tmp))

    def test_cached_checksum_mismatch_never_replaced(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            target = root / "package.tar.zst"
            target.write_bytes(b"wrong")
            item = {"filename": target.name, "bytes": 5, "sha256": "0" * 64}
            with self.assertRaises(RuntimeError):
                shell.download(item, root, False)
            self.assertEqual(target.read_bytes(), b"wrong")

    def test_offline_missing_fails_without_network(self):
        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(RuntimeError):
            shell.download({"filename": "missing", "bytes": 1, "sha256": "0" * 64}, Path(tmp), True)

    def test_existing_destination_never_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = root / "existing"
            output.mkdir()
            (output / "keep").write_text("mine")
            args = argparse.Namespace(lock=shell.LOCK, destination=output, cache=root / "cache",
                                      offline=True, sources=False, zstd=None)
            with self.assertRaises(RuntimeError):
                shell.stage(args)
            self.assertEqual((output / "keep").read_text(), "mine")
            self.assertFalse((root / "cache").exists())

    def test_size_budget(self):
        old = shell.MAX_MEMBER_BYTES
        try:
            shell.MAX_MEMBER_BYTES = 1
            with tempfile.TemporaryDirectory() as tmp, self.assertRaises(RuntimeError):
                self.extract([("usr/bin/large", b"xx", tarfile.REGTYPE)], Path(tmp))
        finally:
            shell.MAX_MEMBER_BYTES = old


if __name__ == "__main__":
    unittest.main()
