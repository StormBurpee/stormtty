#!/usr/bin/env python3
"""Offline build-tool cache safety tests; never executes the downloaded PE."""
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import tempfile
import unittest
from unittest import mock
import zipfile

spec = importlib.util.spec_from_file_location("shell_zstd_bootstrap", Path(__file__).with_name("shell-zstd-bootstrap.py"))
zstd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(zstd)


class ZstdBootstrapTests(unittest.TestCase):
    def fixture(self, root, extra=()):
        lock = zstd.read_lock()
        cache = root / "cache"
        cache.mkdir()
        archive = cache / lock["archive"]["filename"]
        with zipfile.ZipFile(archive, "w") as bundle:
            for item in lock["members"]:
                data = b"fixture, never executed: " + item["target"].encode()
                item["bytes"] = len(data)
                item["sha256"] = hashlib.sha256(data).hexdigest()
                bundle.writestr(item["member"], data)
            for entry, data in extra:
                bundle.writestr(entry, data)
        lock["archive"]["bytes"] = archive.stat().st_size
        lock["archive"]["sha256"] = zstd.digest(archive)
        lock_path = root / "lock.json"
        lock_path.write_text(json.dumps(lock))
        return cache, lock_path, lock

    def test_exact_official_lock_and_notice_assets(self):
        lock = zstd.read_lock()
        self.assertEqual(lock["provenance"]["github_asset_id"], 233456224)
        self.assertTrue(lock["build_tool_only"])
        self.assertEqual(lock["archive"]["bytes"], 1747181)
        for notice in lock["notices"]:
            zstd.checked(Path(zstd.__file__).parent / notice["source"], notice)

    def test_fresh_and_reused_cache_extracts_only_selected_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cache, path, lock = self.fixture(root, [("zstd-v1.5.7-win64/unneeded.dll", b"do not extract")])
            executable = zstd.provision(cache, True, path)
            self.assertEqual(executable.parent, cache / "tools/zstd-v1.5.7-win64")
            self.assertEqual({p.name for p in executable.parent.iterdir()}, {"zstd.exe", "README.md", "LICENSE", "COPYING"})
            self.assertEqual(zstd.provision(cache, True, path), executable)
            self.assertFalse((cache / "tools/.zstd-bootstrap.active").exists())

    def test_corrupt_cached_executable_is_not_repaired(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache, path, _ = self.fixture(Path(tmp))
            executable = zstd.provision(cache, True, path)
            executable.write_bytes(b"changed")
            with self.assertRaisesRegex(RuntimeError, "integrity"):
                zstd.provision(cache, True, path)
            self.assertEqual(executable.read_bytes(), b"changed")

    def test_corrupt_cached_zip_is_not_repaired(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache, path, lock = self.fixture(Path(tmp))
            archive = cache / lock["archive"]["filename"]
            archive.write_bytes(b"changed")
            with self.assertRaisesRegex(RuntimeError, "integrity"):
                zstd.provision(cache, False, path)
            self.assertEqual(archive.read_bytes(), b"changed")

    def test_offline_missing_never_opens_network(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(zstd.urllib.request, "build_opener", side_effect=AssertionError):
            with self.assertRaisesRegex(RuntimeError, "Offline"):
                zstd.provision(Path(tmp), True)

    def test_reject_unsafe_even_unselected_zip_paths(self):
        for name in ("../escape", "/absolute", "a\\b", "a:ads", "nul", "a/../b", "a/COM1.txt", "a/", "zstd-v1.5.7-win64/ZSTD.EXE"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                # a/ is a valid empty directory, but nonempty directory data/type is rejected.
                info = zipfile.ZipInfo(name)
                info.external_attr = stat.S_IFREG << 16
                cache, path, _ = self.fixture(Path(tmp), [(info, b"bad")])
                with self.assertRaises(RuntimeError):
                    zstd.provision(cache, True, path)
                self.assertFalse((cache / "tools/zstd-v1.5.7-win64").exists())

    def test_symlink_zip_entry_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            entry = zipfile.ZipInfo("zstd-v1.5.7-win64/link")
            entry.create_system = 3
            entry.external_attr = (stat.S_IFLNK | 0o777) << 16
            cache, path, _ = self.fixture(Path(tmp), [(entry, b"../../elsewhere")])
            with self.assertRaises(RuntimeError):
                zstd.provision(cache, True, path)

    def test_extra_tool_file_rejected_before_execution(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache, path, _ = self.fixture(Path(tmp))
            exe = zstd.provision(cache, True, path)
            (exe.parent / "unexpected.dll").write_bytes(b"bad")
            with self.assertRaisesRegex(RuntimeError, "Unexpected file"):
                zstd.provision(cache, True, path)

    def test_unofficial_or_insecure_redirect_rejected(self):
        for url in ("http://github.com/a", "https://github.com.evil/a", "https://example.com/a", "https://user@github.com/a"):
            with self.subTest(url=url), self.assertRaises(RuntimeError):
                zstd.https_origin(url)
        zstd.https_origin("https://release-assets.githubusercontent.com/asset?signature=opaque")

    def test_lock_cannot_select_other_executable_or_origin(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache, path, lock = self.fixture(Path(tmp))
            lock["archive"]["url"] = "https://example.com/zstd.zip"
            path.write_text(json.dumps(lock))
            with self.assertRaises(RuntimeError):
                zstd.read_lock(path)


if __name__ == "__main__":
    unittest.main()
