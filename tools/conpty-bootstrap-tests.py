#!/usr/bin/env python3
"""Portable packaging tests, NOT Windows trust/runtime qualification."""
# Emit a complete line before the heavier imports: CTest can discard an
# unterminated unittest progress line when its unchanged 30s deadline expires.
import sys
print("conpty-packaging bootstrap", file=sys.stderr, flush=True)
import faulthandler
import time
faulthandler.dump_traceback_later(20, repeat=False)
import atexit
atexit.register(faulthandler.cancel_dump_traceback_later)
import base64
import copy
import contextlib
import io
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import shutil
import tempfile
import unittest
from unittest import mock
import warnings
import zipfile

spec = importlib.util.spec_from_file_location("conpty_bootstrap", Path(__file__).with_name("prepare-bundled-conpty.py"))
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


def install_fixture_generator_args(build_guard):
    # Even project(... NONE) lets the default Windows generator discover a
    # Visual Studio installation. This fixture only needs generated install
    # code, not an IDE, SDK, compiler, or build.
    ninja = shutil.which("ninja")
    if ninja:
        return ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM:FILEPATH={Path(ninja).as_posix()}"]
    # Unix Makefiles can generate this language-free project without invoking
    # a build tool. A fail-fast guard occupies the required make-program slot
    # so CMake-only developer environments retain the same install coverage.
    # Do not run cmake --build here: cmake --install executes the real script.
    return ["-G", "Unix Makefiles", f"-DCMAKE_MAKE_PROGRAM:FILEPATH={Path(build_guard).as_posix()}"]


def write_install_fixture_build_guard(root):
    guard = root / ("must not build.cmd" if os.name == "nt" else "must not build.sh")
    if os.name == "nt":
        script = '@echo off\necho Unexpected install-fixture build-tool invocation 1>&2\ntype nul > "%~f0.invoked"\nexit /b 1\n'
    else:
        script = '#!/bin/sh\necho "Unexpected install-fixture build-tool invocation" >&2\n: > "$0.invoked"\nexit 1\n'
    guard.write_text(script)
    guard.chmod(guard.stat().st_mode | stat.S_IXUSR)
    return guard


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.started = time.monotonic()
        print(f"conpty-packaging test-start {self._testMethodName}", file=sys.stderr, flush=True)
        self.temp = tempfile.TemporaryDirectory(prefix="conpty-unit-", dir=Path(__file__).resolve().parent.parent / ".cache" if (Path(__file__).resolve().parent.parent / ".cache").is_dir() else Path(__file__).resolve().parent.parent)
        self.root = Path(self.temp.name)
        self.archive = self.root / "test.nupkg"
        self.output = self.root / "output"
        self.output.mkdir()
        self.data = b"pinned fixture, never executable"
        self.file = {"destination": "conpty.dll", "size": len(self.data), "sha256": hashlib.sha256(self.data).hexdigest()}
        self.lock = {"files": {"runtimes/win-x64/native/conpty.dll": self.file}}

    def tearDown(self):
        print("conpty-packaging cleanup-start", file=sys.stderr, flush=True)
        self.temp.cleanup()
        print(f"conpty-packaging test-end elapsed={time.monotonic() - self.started:.3f}s",
              file=sys.stderr, flush=True)

    def write_archive(self, extras=(), data=None):
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(self.archive, "w") as archive:
                archive.writestr("runtimes/win-x64/native/conpty.dll", self.data if data is None else data)
                for name, content in extras:
                    # ZipInfo.__init__ normalizes native backslashes on Windows.
                    # Restore the exact hostile archive spelling after creation
                    # so this tests extraction, not host-side fixture sanitizing.
                    entry = zipfile.ZipInfo(name) if isinstance(name, str) else name
                    if isinstance(name, str):
                        entry.filename = name
                    archive.writestr(entry, content)

    def test_allowlist_extracts_only_expected_file(self):
        self.write_archive([("unselected.txt", b"not copied")])
        bootstrap.extract(self.archive, self.output, self.lock)
        self.assertEqual((self.output / "conpty.dll").read_bytes(), self.data)
        self.assertFalse((self.output / "unselected.txt").exists())

    def test_rejects_duplicate_entries(self):
        self.write_archive([("runtimes/win-x64/native/conpty.dll", self.data)])
        with self.assertRaisesRegex(RuntimeError, "Duplicate"):
            bootstrap.extract(self.archive, self.output, self.lock)

    def test_rejects_archive_traversal_absolute_backslash_drive(self):
        for name in ("../escape", "/absolute", "C:/drive", "back\\slash", "nul\0suffix",
                     "runtimes/win-x64/native/conpty.dll\0alias") :
            with self.subTest(name=name):
                self.write_archive([(name, b"bad")])
                with zipfile.ZipFile(self.archive) as archive:
                    self.assertIn(name, [entry.orig_filename for entry in archive.infolist()],
                                  "Fixture did not preserve the original malicious filename")
                with self.assertRaisesRegex(RuntimeError, "unsafe"):
                    bootstrap.extract(self.archive, self.output, self.lock)

    def test_rejects_symlink_entries(self):
        info = zipfile.ZipInfo("link")
        info.create_system = 3
        info.external_attr = (stat.S_IFLNK | 0o777) << 16
        self.write_archive([(info, b"target")])
        with self.assertRaisesRegex(RuntimeError, "Nonregular"):
            bootstrap.extract(self.archive, self.output, self.lock)

    def test_rejects_oversize_archive_entries(self):
        self.write_archive([("large", b"x" * (bootstrap.MAX_ENTRY + 1))])
        with self.assertRaisesRegex(RuntimeError, "oversized"):
            bootstrap.extract(self.archive, self.output, self.lock)

    def test_rejects_many_entries(self):
        self.write_archive([(str(n), b"") for n in range(64)])
        with self.assertRaisesRegex(RuntimeError, "Too many"):
            bootstrap.extract(self.archive, self.output, self.lock)

    def test_rejects_missing_payload(self):
        self.write_archive()
        self.lock["files"]["absent"] = self.file
        with self.assertRaisesRegex(RuntimeError, "Missing pinned"):
            bootstrap.extract(self.archive, self.output, self.lock)

    def test_rejects_payload_hash_or_size(self):
        for data in (b"x" * len(self.data), b"wrong size"):
            with self.subTest(data=data):
                self.write_archive(data=data)
                with self.assertRaisesRegex(RuntimeError, "(hash mismatch|entry size)"):
                    bootstrap.extract(self.archive, self.output, self.lock)

    def test_refuses_overwrite(self):
        self.write_archive()
        target = self.output / "conpty.dll"
        target.write_text("keep")
        with self.assertRaises(FileExistsError):
            bootstrap.extract(self.archive, self.output, self.lock)
        self.assertEqual(target.read_text(), "keep")

    @unittest.skipIf(os.name == "nt", "symlink privilege is not assumed on Windows")
    def test_rejects_linked_parent(self):
        (self.root / "linked").symlink_to(self.output, target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, "Linked/reparse"):
            bootstrap.checked_path(self.root / "linked" / "child")

    def test_rejects_relative_and_parent_paths(self):
        for path in (Path("relative"), self.root / ".." / "escape"):
            with self.assertRaises(RuntimeError):
                bootstrap.checked_path(path)

    def test_checked_file_hash_size_regular(self):
        path = self.root / "pinned"
        path.write_bytes(self.data)
        self.assertEqual(bootstrap.checked_bytes(path, self.file), self.data)
        path.write_bytes(b"x" * len(self.data))
        with self.assertRaisesRegex(RuntimeError, "SHA256"):
            bootstrap.checked_bytes(path, self.file)

    def test_exact_source_notices_and_pins(self):
        lock = json.loads(bootstrap.LOCK.read_text())
        self.assertEqual(lock["version"], "1.25.260930003")
        self.assertEqual(lock["source_commit"], "dc8ae365847d41f62d1ebca93815bb00959bc12f")
        self.assertEqual(lock["sha256"], "02b07b349af66d801159bdf9e440d4a1ce78bb951f37fc8609731665afdae7ee")
        for name, expected in lock["notices"].items():
            data = bootstrap.checked_bytes(bootstrap.TOOLS / "conpty-notices" / name, expected)
            if name == "NOTICE.md":
                git_hash = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
                self.assertEqual(git_hash, "efe962bc0d0c60f8a8f024c648f4d5b0ec9c321d")

    def test_offline_cache_missing_and_corruption_fail(self):
        lock = json.loads(bootstrap.LOCK.read_text())
        cache = self.root / "cache"
        with self.assertRaisesRegex(RuntimeError, "offline cache"):
            bootstrap.acquire(cache, lock, True)
        (cache / f"{lock['package']}.{lock['version']}.nupkg").write_bytes(b"bad")
        with self.assertRaisesRegex(RuntimeError, "wrong-sized"):
            bootstrap.acquire(cache, lock, True)

    def test_signature_verifier_isolates_builtin_module_without_policy_bypass(self):
        powershell = self.root / "Windows" / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
        with mock.patch.dict(os.environ, {"PSModulePath": "untrusted PS7 modules", "KEEP_THIS": "kept"}), \
             mock.patch.object(bootstrap.subprocess, "run") as run:
            bootstrap.verify_authenticode(self.output, powershell)
        args = run.call_args.args[0]
        options = run.call_args.kwargs
        self.assertEqual(args[0], str(powershell))
        self.assertTrue(options["check"])
        self.assertEqual(options["env"]["PSModulePath"], str(powershell.parent / "Modules"))
        self.assertEqual(options["env"]["KEEP_THIS"], "kept")
        script = base64.b64decode(args[-1]).decode("utf-16-le")
        self.assertIn("Import-Module ($PSHOME +", script)
        self.assertIn("Microsoft.PowerShell.Security.psd1", script)
        self.assertIn("Microsoft.PowerShell.Security\\Get-AuthenticodeSignature", script)
        self.assertNotIn("ExecutionPolicy", " ".join(args) + script)
        self.assertIn("$s.Status -ne 'Valid'", script)
        self.assertIn("O=Microsoft Corporation", script)
        self.assertIn("$ErrorActionPreference='Stop'", script)
        self.assertLess(script.index("$ProgressPreference='SilentlyContinue'"), script.index("Import-Module"))
        self.assertEqual(args[args.index("-OutputFormat") + 1], "Text")
        self.assertIn('Write-Output "$p signer=', script)
        # Keep stdout/stderr attached: a real trust error must remain visible.
        self.assertNotIn("capture_output", options)
        self.assertNotIn("stdout", options)
        self.assertNotIn("stderr", options)

    def test_success_message_is_clear_and_does_not_change_preview_default(self):
        output = io.StringIO()
        with mock.patch.object(bootstrap, "prepare") as prepare, \
             mock.patch("sys.argv", ["prepare-bundled-conpty.py", "--destination", str(self.output), "--cache", str(self.root / "cache")]), \
             contextlib.redirect_stdout(output):
            self.assertEqual(bootstrap.main(), 0)
        prepare.assert_called_once()
        self.assertEqual(output.getvalue(), "ConPTY runtime verified (optional preview; System remains default).\n")


    def test_signature_command_failure_is_not_swallowed(self):
        with mock.patch.object(bootstrap.subprocess, "run", side_effect=bootstrap.subprocess.CalledProcessError(1, "trust")), \
             self.assertRaises(bootstrap.subprocess.CalledProcessError):
            bootstrap.verify_authenticode(self.output, self.root / "powershell.exe")

    def test_install_fixture_selects_ninja_explicitly(self):
        with mock.patch.object(shutil, "which", return_value="C:/build tools/ninja.exe") as which:
            args = install_fixture_generator_args("C:/fixture/must not build.cmd")
        which.assert_called_once_with("ninja")
        self.assertEqual(args, ["-G", "Ninja", "-DCMAKE_MAKE_PROGRAM:FILEPATH=C:/build tools/ninja.exe"])

    def test_install_fixture_fallback_needs_no_build_tool(self):
        with mock.patch.object(shutil, "which", return_value=None) as which:
            args = install_fixture_generator_args("C:/fixture/must not build.cmd")
        which.assert_called_once_with("ninja")
        self.assertEqual(args, ["-G", "Unix Makefiles", "-DCMAKE_MAKE_PROGRAM:FILEPATH=C:/fixture/must not build.cmd"])

    def test_install_fixture_build_guard_rejects_and_records_invocation(self):
        guard = write_install_fixture_build_guard(self.root)
        invocation = [str(guard)]
        if os.name == "nt":
            invocation = [os.environ["COMSPEC"], "/d", "/c", str(guard)]
        result = bootstrap.subprocess.run(invocation, text=True, capture_output=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unexpected install-fixture build-tool invocation", result.stderr)
        self.assertTrue(Path(str(guard) + ".invoked").is_file())

    @unittest.skipUnless(shutil.which("cmake"), "CMake not installed; native install lane also checks this")
    def test_generated_install_scripts_preserve_windows_path_bytes(self):
        self.check_generated_install_scripts()

    @unittest.skipUnless(shutil.which("cmake"), "CMake not installed; native install lane also checks this")
    def test_generated_install_scripts_without_ninja_or_ide(self):
        which = shutil.which
        # Exercise real configure and install, including a developer's inherited
        # IDE selection, without allowing Ninja or make discovery as a crutch.
        with mock.patch.object(shutil, "which", side_effect=lambda name: which(name) if name == "cmake" else None), \
             mock.patch.dict(os.environ, {"CMAKE_GENERATOR": "Visual Studio 18 2026",
                                          "CMAKE_GENERATOR_PLATFORM": "x64",
                                          "CMAKE_GENERATOR_TOOLSET": "v145",
                                          "CMAKE_GENERATOR_INSTANCE": "missing IDE"}):
            self.check_generated_install_scripts()

    def check_generated_install_scripts(self):
        source = self.root / "cmake source"
        build = self.root / "cmake build"
        source.mkdir()
        repo = bootstrap.TOOLS.parent
        install_lines = []
        for module in ("StormTTYDeveloperShell.cmake", "StormTTYConsoleRuntime.cmake"):
            install_lines += [line.strip() for line in (repo / "cmake" / module).read_text().splitlines()
                              if line.strip().startswith("install(CODE ")]
        self.assertEqual(len(install_lines), 2, "Both real runtime install checks must execute")
        # Execute the real generated install code with only the subprocess
        # primitive mocked. No downloaded runtime or Windows trust is involved.
        fixture = r"""cmake_minimum_required(VERSION 3.24)
project(InstallQuoteRegression NONE)
set(Python3_EXECUTABLE [==[C:\hostedtoolcache\windows\Python\3.12.10\x64/python.exe]==])
set(_shell_stager "stage shell.py")
set(_shell_source "source")
set(_shell_lock "lock")
set(_shell_preset "preset")
set(_conpty_prepare "prepare console.py")
set(_conpty_relative "runtimes/conpty")
set(STORMTTY_CONPTY_CACHE [==[C:\build cache\conpty]==])
add_executable(stormtty IMPORTED)
set_target_properties(stormtty PROPERTIES IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/stormtty.exe")
install(CODE [====[
function(execute_process)
    if(NOT "${ARGV1}" STREQUAL [==[C:\hostedtoolcache\windows\Python\3.12.10\x64/python.exe]==])
        message(FATAL_ERROR "Interpreter path changed")
    endif()
    set(result 0 PARENT_SCOPE)
endfunction()
]====])
"""
        (source / "CMakeLists.txt").write_text(fixture + "\n".join(install_lines) + "\n")
        cmake = shutil.which("cmake")
        build_guard = write_install_fixture_build_guard(self.root)
        generator_args = install_fixture_generator_args(build_guard)
        # -G alone does not override a developer's platform/toolset/instance.
        env = {key: value for key, value in os.environ.items()
               if key not in ("CMAKE_GENERATOR", "CMAKE_GENERATOR_PLATFORM",
                              "CMAKE_GENERATOR_TOOLSET", "CMAKE_GENERATOR_INSTANCE")}
        for phase, args in (("configure", ["-S", str(source), "-B", str(build), *generator_args]),
                            ("install", ["--install", str(build)])):
            started = time.monotonic()
            print(f"conpty-packaging cmake-{phase}-start", file=sys.stderr, flush=True)
            # Attach child output rather than holding it in Python until exit:
            # it must survive an outer CTest timeout as well as normal failures.
            result = bootstrap.subprocess.run([cmake, *args], text=True, timeout=30, env=env)
            print(f"conpty-packaging cmake-{phase}-end returncode={result.returncode} "
                  f"elapsed={time.monotonic() - started:.3f}s", file=sys.stderr, flush=True)
            self.assertEqual(result.returncode, 0)
            self.assertFalse(Path(str(build_guard) + ".invoked").exists(),
                             "Install-only fixture must not invoke a build tool")
        cache = (build / "CMakeCache.txt").read_text()
        self.assertIn(f"CMAKE_GENERATOR:INTERNAL={generator_args[1]}\n", cache)
        self.assertIn(generator_args[2].removeprefix("-D") + "\n", cache)
        self.assertNotIn("CMAKE_C_COMPILER:", cache)
        self.assertNotIn("CMAKE_CXX_COMPILER:", cache)

    def publication_fixture(self, repair):
        transaction = self.root / "transaction"
        transaction.mkdir()
        stage = transaction / "runtime"
        stage.mkdir()
        (stage / "payload").write_bytes(b"new verified fixture")
        destination = self.root / "installed"
        receipt = self.root / "installed.stormtty-conpty-owned.json"
        previous_receipt = b'{"owner":"stormtty-private-conpty-v1","version":"prior"}\n'
        if repair:
            destination.mkdir()
            (destination / "payload").write_bytes(b"prior working runtime")
            receipt.write_bytes(previous_receipt)
        unrelated = self.root / "unrelated"
        unrelated.write_bytes(b"do not touch")
        return stage, destination, receipt, previous_receipt, unrelated

    def assert_publication_rollback(self, values, repair):
        stage, destination, receipt, previous_receipt, unrelated = values
        self.assertEqual((stage / "payload").read_bytes(), b"new verified fixture")
        if repair:
            self.assertEqual((destination / "payload").read_bytes(), b"prior working runtime")
            self.assertEqual(receipt.read_bytes(), previous_receipt)
        else:
            self.assertFalse(destination.exists())
            self.assertFalse(receipt.exists())
        self.assertFalse((stage.parent / "previous").exists())
        self.assertEqual(unrelated.read_bytes(), b"do not touch")

    def receipt_failure(self, repair, writing):
        values = self.publication_fixture(repair)
        stage, destination, receipt, _, _ = values
        if writing:
            patch = mock.patch.object(Path, "write_text", side_effect=OSError("injected receipt write failure"))
        else:
            patch = mock.patch.object(bootstrap.os, "replace", side_effect=OSError("injected receipt replace failure"))
        with patch, self.assertRaisesRegex(OSError, "injected receipt"):
            bootstrap.publish_runtime(stage, destination, receipt, "new")
        self.assert_publication_rollback(values, repair)
        # A retry succeeds, rather than refusing a newly orphaned destination.
        bootstrap.publish_runtime(stage, destination, receipt, "new")
        self.assertEqual((destination / "payload").read_bytes(), b"new verified fixture")
        self.assertEqual(json.loads(receipt.read_text())["version"], "new")
        self.assertEqual(values[-1].read_bytes(), b"do not touch")

    def test_first_install_receipt_write_failure(self):
        self.receipt_failure(False, True)

    def test_first_install_receipt_replace_failure(self):
        self.receipt_failure(False, False)

    def test_repair_receipt_write_failure(self):
        self.receipt_failure(True, True)

    def test_repair_receipt_replace_failure(self):
        self.receipt_failure(True, False)

    def test_obstructed_rollback_preserves_recovery_files(self):
        values = self.publication_fixture(True)
        stage, destination, receipt, previous_receipt, unrelated = values
        rename = Path.rename
        def obstruct_rollback(path, target):
            if path == destination and target == stage:
                raise OSError("injected rollback obstruction")
            return rename(path, target)
        with mock.patch.object(Path, "rename", obstruct_rollback), \
             mock.patch.object(bootstrap.os, "replace", side_effect=OSError("receipt failure")), \
             self.assertRaisesRegex(bootstrap.PublicationRecoveryRequired, "Preserve"):
            bootstrap.publish_runtime(stage, destination, receipt, "new")
        self.assertEqual((stage.parent / "previous" / "payload").read_bytes(), b"prior working runtime")
        self.assertEqual((destination / "payload").read_bytes(), b"new verified fixture")
        self.assertEqual(receipt.read_bytes(), previous_receipt)
        self.assertEqual(unrelated.read_bytes(), b"do not touch")

    @unittest.skipIf(os.name == "nt", "non-Windows rejection test")
    def test_no_non_windows_staging_trust_bypass(self):
        with self.assertRaisesRegex(RuntimeError, "Windows trust verification"):
            bootstrap.prepare(self.output, self.root / "cache", True)
        self.assertEqual(list(self.output.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
