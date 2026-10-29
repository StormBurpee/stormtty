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
from unittest import mock

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
        self.assertEqual(len(lock["packages"]), 21)
        self.assertEqual(len(lock["sources"]), 21)
        self.assertTrue(lock["default_activation_allowed"])
        self.assertFalse(lock["public_redistribution_ready"])
        self.assertEqual(lock["native_qualification"]["status"], "passed")
        self.assertEqual(sum(p["bytes"] for p in lock["packages"]), 16467938)

    def test_nano_dependency_closure_and_pending_qualification(self):
        lock = shell.read_lock(shell.LOCK)
        names = {p["name"] for p in lock["packages"]} | {"sh"}  # bash owns sh.exe
        for package in lock["packages"]:
            for dependency in package.get("dependencies", []):
                self.assertIn(dependency, names)
        self.assertTrue({"nano", "file", "libbz2", "liblzma"} <= names)
        self.assertEqual(lock["nano_qualification"]["status"], "pending")
        self.assertFalse(lock["public_source_availability_verified"])

    def test_nano_write_prompt_tracks_pinned_source_version(self):
        # nano-9.2/src/files.c write_it_out(): msg = _("Write to File").
        # An upgrade needs an explicit source/UI handshake review, not accepting
        # arbitrary output and potentially sending Enter before the save prompt.
        lock = shell.read_lock(shell.LOCK)
        nano = next(p for p in lock["packages"] if p["name"] == "nano")
        self.assertEqual(nano["version"], "9.2-1")
        fixture = (shell.LOCK.parent.parent / "tests/managed_nano_probe.cpp").read_text()
        self.assertIn('probe.expect("Write to File")', fixture)
        self.assertNotIn('probe.expect("File Name to Write")', fixture)
        self.assertNotIn('probe.expect("Wrote 1 line")', fixture)
        self.assertIn('probe.saved()', fixture)
        self.assertIn('check(probe.exact_file())', fixture)
        self.assertIn('diagnostic.file_exact = value == expected', fixture)
        self.assertIn('MANAGED_NANO_DIAGNOSTIC phase=', fixture)
        self.assertIn('probe.expect("QNANO_EXIT_0")', fixture)

    def test_nano_edit_keeps_one_automatic_final_newline(self):
        fixture = (shell.LOCK.parent.parent / "tests/managed_nano_probe.cpp").read_text()
        edit = fixture.split('        phase = 3;', 1)[1].split('        phase = 4;', 1)[0]
        self.assertIn('probe.key(Key::a, true); probe.key(Key::k, true);', edit)
        self.assertIn('probe.text("edited caf', edit)
        self.assertNotIn('probe.key(Key::enter)', edit)
        self.assertIn('diagnostic.file_exact = value == expected', fixture)
        self.assertIn('phase = 43; check(probe.exact_file())', fixture)

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

    def test_runtime_config_is_manifest_tracked_and_nonoverwriting(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            records = {}
            shell.stage_runtime_config(root, records)
            for name in ("etc/fstab", "etc/nsswitch.conf"):
                self.assertEqual(records[name]["sha256"], shell.digest(root / name))
                self.assertEqual(records[name]["package"], "stormtty-original-config")
            self.assertIn("none / cygdrive binary,posix=0,noacl,user 0 0", (root / "etc/fstab").read_text())
            self.assertIn("none /tmp usertemp", (root / "etc/fstab").read_text())
            self.assertIn("db_home: env windows", (root / "etc/nsswitch.conf").read_text())
            before = (root / "etc/fstab").read_bytes()
            with self.assertRaisesRegex(RuntimeError, "collides"):
                shell.stage_runtime_config(root, {})
            self.assertEqual((root / "etc/fstab").read_bytes(), before)

    def test_runtime_config_rejects_case_colliding_package(self):
        with tempfile.TemporaryDirectory() as tmp, self.assertRaisesRegex(RuntimeError, "collides"):
            shell.stage_runtime_config(Path(tmp), {"etc/fstab": {"path": "ETC/FSTAB"}})

    def test_runtime_config_rejects_parent_alias(self):
        with tempfile.TemporaryDirectory() as tmp, self.assertRaisesRegex(RuntimeError, "collides"):
            shell.stage_runtime_config(Path(tmp), {"etc": {"path": "ETC", "directory": True}})

    def test_size_budget(self):
        old = shell.MAX_MEMBER_BYTES
        try:
            shell.MAX_MEMBER_BYTES = 1
            with tempfile.TemporaryDirectory() as tmp, self.assertRaises(RuntimeError):
                self.extract([("usr/bin/large", b"xx", tarfile.REGTYPE)], Path(tmp))
        finally:
            shell.MAX_MEMBER_BYTES = old

    def test_explicit_decoder_never_downloads(self):
        with mock.patch.object(shell, "stdlib_zstd_available", side_effect=AssertionError):
            self.assertEqual(shell.select_decompressor("/trusted/zstd", Path("cache"), True), "/trusted/zstd")

    def test_stdlib_decoder_never_downloads(self):
        with mock.patch.object(shell, "stdlib_zstd_available", return_value=True):
            self.assertIsNone(shell.select_decompressor(None, Path("cache"), True))

    def test_unsupported_host_decoder_is_actionable(self):
        with mock.patch.object(shell, "stdlib_zstd_available", return_value=False), \
                mock.patch.object(shell.sys, "platform", "linux"):
            with self.assertRaisesRegex(RuntimeError, "--zstd"):
                shell.select_decompressor(None, Path("cache"), True)

    def test_windows_auto_decoder_uses_private_helper(self):
        helper = mock.Mock()
        helper.provision.return_value = Path("cache/tools/zstd.exe")
        with mock.patch.object(shell, "stdlib_zstd_available", return_value=False), \
                mock.patch.object(shell.sys, "platform", "win32"), \
                mock.patch.object(shell.platform, "machine", return_value="AMD64"), \
                mock.patch.object(shell.importlib.util, "module_from_spec", return_value=helper), \
                mock.patch.object(shell.importlib.util, "spec_from_file_location") as spec:
            result = shell.select_decompressor(None, Path("cache"), True)
            self.assertEqual(result, str(Path("cache/tools/zstd.exe")))
            helper.provision.assert_called_once_with(Path("cache"), True)
            spec.return_value.loader.exec_module.assert_called_once_with(helper)


if __name__ == "__main__":
    unittest.main()
