#!/usr/bin/env python3
"""Small offline integrity/ownership fixtures, not native shell execution."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("stager", Path(__file__).with_name("stage-developer-shell.py"))
stager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stager)

class StagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / "source"
        self.root.mkdir()
        self.lock = self.base / "lock.json"
        self.lock.write_text('{"bundle_id":"fixture","qualification_only":true,"default_activation_allowed":false}')
        self.preset = self.base / "preset"
        self.preset.write_text("fixture managed preset\n")
        records = []
        for name in stager.REQUIRED:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes((Path(stager.__file__).with_name(stager.CONFIG[name])).read_bytes() if name in stager.CONFIG else self.preset.read_bytes() if name.endswith(".zshrc") else b"fixture payload")
            records.append({"path":name, "bytes":path.stat().st_size, "sha256":stager.digest(path)})
        self.manifest = {"schema":1,"bundle_id":"fixture","platform":"windows-x86_64",
                         "qualification_only":True,"default_activation_allowed":False,
                         "lock_sha256":stager.digest(self.lock),"shell":stager.REQUIRED[0],
                         "managed_zdotdir":"stormtty/zsh",
                         "managed_tools_lock_sha256":stager.digest(Path(stager.__file__).with_name("shell-tools.lock.json")),"files":records}
        self.write_manifest()
        for name in ("THIRD-PARTY-NOTICES.txt", "source-manifest.json", "shell-bootstrap.lock.json"):
            path = self.root / "notices" / name
            path.parent.mkdir(exist_ok=True)
            path.write_text("fixture notices preserved")

    def write_manifest(self):
        (self.root / stager.MANIFEST).write_text(json.dumps(self.manifest))

    def validate(self):
        return stager.validate(self.root, self.lock, self.preset)

    def test_tools_lock_manifest_is_required(self):
        self.manifest.pop("managed_tools_lock_sha256")
        self.write_manifest()
        with self.assertRaisesRegex(RuntimeError, "tools lock"):
            self.validate()

    def test_copy_and_owned_replacement(self):
        files, receipt = self.validate()
        target = self.base / "app/runtimes/zsh"
        stager.stage(self.root,target,files,receipt)
        for path, expected in files.items():
            self.assertEqual(stager.digest(target / path.relative_to(self.root)),expected)
        self.assertFalse(json.loads((target/stager.RECEIPT).read_text())["release_qualified"])
        stager.stage(self.root,target,files,receipt)
        self.assertEqual((target/"notices/THIRD-PARTY-NOTICES.txt").read_text(),"fixture notices preserved")

    def test_private_creation_collision_preserves_existing_directory(self):
        target = self.base / "existing"
        target.mkdir()
        sentinel = target / "sentinel"
        sentinel.write_bytes(b"do not change")
        before = target.stat()
        with self.assertRaises(OSError): stager.create_private_runtime_directory(target)
        self.assertEqual(sentinel.read_bytes(), b"do not change")
        self.assertEqual(target.stat().st_mode, before.st_mode)

    def test_failed_private_creation_never_publishes_or_leaves_temp(self):
        files, receipt = self.validate()
        target = self.base / "app/runtimes/zsh"
        stager.stage(self.root, target, files, receipt)
        original = (target / stager.RECEIPT).read_bytes()
        create = stager.create_private_runtime_directory
        for created in (False, True):
            def fail(path):
                if created: create(path)
                raise OSError("authored staging failure")
            with patch.object(stager, "create_private_runtime_directory", side_effect=fail):
                with self.assertRaises(OSError): stager.stage(self.root, target, files, receipt)
            self.assertEqual((target / stager.RECEIPT).read_bytes(), original)
            self.assertEqual(list(target.parent.glob(".stormtty-runtime-*")), [])

    @unittest.skipUnless(os.name == "nt", "native Windows ACL evidence")
    def test_windows_staged_root_and_children_keep_private_acl_after_rename(self):
        files, receipt = self.validate()
        target = self.base / "app/runtimes/zsh"
        stager.stage(self.root, target, files, receipt)
        # Read the authored fixtures through native ACL APIs; no PowerShell
        # parser/module/runtime dependency can obscure this security assertion.
        import ctypes
        from ctypes import wintypes as w
        pointer = ctypes.c_void_p
        security = ctypes.WinDLL('advapi32', use_last_error=True)
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.LocalFree.argtypes = [pointer]; kernel.LocalFree.restype = pointer
        kernel.GetCurrentProcess.argtypes = []; kernel.GetCurrentProcess.restype = w.HANDLE
        kernel.CloseHandle.argtypes = [w.HANDLE]; kernel.CloseHandle.restype = w.BOOL
        security.OpenProcessToken.argtypes = [w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE)]
        security.OpenProcessToken.restype = w.BOOL
        security.GetTokenInformation.argtypes = [w.HANDLE, ctypes.c_int, pointer, w.DWORD, ctypes.POINTER(w.DWORD)]
        security.GetTokenInformation.restype = w.BOOL
        security.ConvertSidToStringSidW.argtypes = [pointer, ctypes.POINTER(pointer)]
        security.ConvertSidToStringSidW.restype = w.BOOL
        security.GetNamedSecurityInfoW.argtypes = [w.LPWSTR, ctypes.c_int, w.DWORD,
            ctypes.POINTER(pointer), pointer, ctypes.POINTER(pointer), pointer, ctypes.POINTER(pointer)]
        security.GetNamedSecurityInfoW.restype = w.DWORD
        security.GetSecurityDescriptorControl.argtypes = [pointer, ctypes.POINTER(w.WORD), ctypes.POINTER(w.DWORD)]
        security.GetSecurityDescriptorControl.restype = w.BOOL
        security.GetAce.argtypes = [pointer, w.DWORD, ctypes.POINTER(pointer)]
        security.GetAce.restype = w.BOOL
        class ACL(ctypes.Structure):
            _fields_ = [('revision', w.BYTE), ('sbz1', w.BYTE), ('size', w.WORD),
                        ('count', w.WORD), ('sbz2', w.WORD)]
        class ACE(ctypes.Structure):
            _fields_ = [('type', w.BYTE), ('flags', w.BYTE), ('size', w.WORD), ('mask', w.DWORD)]
        def require(ok):
            if not ok: raise ctypes.WinError(ctypes.get_last_error())
        def sid_text(sid):
            text = pointer()
            require(security.ConvertSidToStringSidW(sid, ctypes.byref(text)))
            try: return ctypes.wstring_at(text)
            finally: kernel.LocalFree(text)
        token = w.HANDLE()
        require(security.OpenProcessToken(kernel.GetCurrentProcess(), 0x0008, ctypes.byref(token)))
        try:
            size = w.DWORD()
            security.GetTokenInformation(token, 1, None, 0, ctypes.byref(size))
            self.assertGreater(size.value, 0)
            buffer = ctypes.create_string_buffer(size.value)
            require(security.GetTokenInformation(token, 1, buffer, size, ctypes.byref(size)))
            user = sid_text(ctypes.cast(buffer, ctypes.POINTER(pointer)).contents.value)
        finally: kernel.CloseHandle(token)
        def snapshot(path):
            owner = pointer(); acl = pointer(); descriptor = pointer()
            error = security.GetNamedSecurityInfoW(str(path), 1, 5, ctypes.byref(owner), None,
                                                   ctypes.byref(acl), None, ctypes.byref(descriptor))
            if error: raise ctypes.WinError(error)
            try:
                self.assertTrue(acl.value)
                control = w.WORD(); revision = w.DWORD()
                require(security.GetSecurityDescriptorControl(descriptor, ctypes.byref(control), ctypes.byref(revision)))
                rules = []
                for index in range(ctypes.cast(acl, ctypes.POINTER(ACL)).contents.count):
                    raw = pointer(); require(security.GetAce(acl, index, ctypes.byref(raw)))
                    ace = ctypes.cast(raw, ctypes.POINTER(ACE)).contents
                    self.assertEqual(ace.type, 0)  # ACCESS_ALLOWED_ACE_TYPE only
                    self.assertGreaterEqual(ace.size, ctypes.sizeof(ACE) + 8)
                    rules.append((sid_text(raw.value + ctypes.sizeof(ACE)), ace.mask, ace.flags))
                return sid_text(owner), bool(control.value & 0x1000), rules
            finally: kernel.LocalFree(descriptor)
        paths = [target, target / 'usr', target / 'usr/bin/zsh.exe']
        before = [snapshot(path) for path in paths]
        with self.assertRaises(OSError): stager.create_private_runtime_directory(target)
        self.assertEqual([snapshot(path) for path in paths], before)
        allowed = {user, 'S-1-5-18', 'S-1-5-32-544'}
        self.assertTrue(before[0][1])  # protected root DACL
        for index, (owner, protected, rules) in enumerate(before):
            self.assertIn(owner, allowed)
            self.assertEqual({r[0] for r in rules}, allowed)
            self.assertTrue(all(r[1] == 0x1f01ff for r in rules))  # FILE_ALL_ACCESS
            if index: self.assertTrue(all(r[2] & 0x10 for r in rules))  # inherited
        self.assertEqual(list(target.parent.glob('.stormtty-runtime-*')), [])

    def test_nano_and_dependency_files_cannot_be_omitted(self):
        for name in ("usr/bin/nano.exe", "usr/bin/file.exe", "usr/bin/msys-magic-1.dll",
                     "usr/bin/msys-bz2-1.dll", "usr/bin/msys-lzma-5.dll", "usr/share/misc/magic.mgc"):
            with self.subTest(name=name):
                before = self.manifest["files"]
                self.manifest["files"] = [r for r in before if r["path"] != name]
                data = (self.root / name).read_bytes()
                (self.root / name).unlink()
                self.write_manifest()
                with self.assertRaisesRegex(RuntimeError, "Required runtime file"):
                    self.validate()
                (self.root / name).write_bytes(data)
                self.manifest["files"] = before
                self.write_manifest()

    def test_changed_payload(self):
        (self.root/stager.REQUIRED[0]).write_bytes(b"tamper")
        with self.assertRaises(RuntimeError): self.validate()

    def test_missing_dependency(self):
        (self.root/stager.REQUIRED[1]).unlink()
        with self.assertRaises(OSError): self.validate()

    def test_stale_preset(self):
        self.preset.write_text("updated")
        with self.assertRaisesRegex(RuntimeError,"stale"): self.validate()

    def test_missing_or_stale_runtime_config(self):
        for name in stager.CONFIG:
            path = self.root / name
            before = path.read_bytes()
            path.unlink()
            with self.assertRaises(OSError): self.validate()
            path.write_bytes(b"stale configuration")
            record = next(r for r in self.manifest["files"] if r["path"] == name)
            record.update(bytes=path.stat().st_size, sha256=stager.digest(path))
            self.write_manifest()
            with self.assertRaisesRegex(RuntimeError, "configuration is stale"): self.validate()
            path.write_bytes(before)
            record.update(bytes=path.stat().st_size, sha256=stager.digest(path))
            self.write_manifest()

    def test_repair_missing_runtime_config(self):
        target=self.base/"target";stamp=self.base/"stamp"
        args=["stage", "--source",str(self.root),"--lock",str(self.lock),"--preset",str(self.preset),
              "--destination",str(target),"--stamp",str(stamp),"--repair-if-needed"]
        with patch("sys.argv",args): self.assertEqual(stager.main(),0)
        for name in stager.CONFIG:
            (target/name).unlink()
            with patch("sys.argv",args): self.assertEqual(stager.main(),0)
            self.assertEqual((target/name).read_bytes(), (self.root/name).read_bytes())

    def test_unlisted_file(self):
        (self.root/"usr/bin/extra.exe").write_bytes(b"unexpected")
        with self.assertRaisesRegex(RuntimeError,"Unlisted"): self.validate()

    def test_duplicate_case_and_traversal(self):
        self.manifest["files"].append(dict(self.manifest["files"][0],path="USR/bin/ZSH.EXE"))
        self.write_manifest()
        with self.assertRaisesRegex(RuntimeError,"Duplicate"): self.validate()
        self.manifest["files"][-1]["path"]="../escape"
        self.write_manifest()
        with self.assertRaises(RuntimeError): self.validate()

    def test_links_rejected_before_payload_open(self):
        outside=self.base/"outside";outside.mkdir()
        child=self.root/"usr/bin"
        shutil.rmtree(child)
        try: child.symlink_to(outside,target_is_directory=True)
        except OSError as error: self.skipTest(str(error))
        with self.assertRaisesRegex(RuntimeError,"linked/reparse directory"): self.validate()

    def test_root_reparse_rejected(self):
        link=self.base/"linked"
        try: link.symlink_to(self.root,target_is_directory=True)
        except OSError as error: self.skipTest(str(error))
        with self.assertRaises(RuntimeError): stager.validate(link,self.lock,self.preset)

    def test_notice_bytes_bounded(self):
        with patch.object(stager,"MAX_BYTES",sum(p.stat().st_size for p in self.root.rglob("*") if p.is_file())-1):
            with self.assertRaisesRegex(RuntimeError,"total byte"): self.validate()

    def test_source_change_after_validation(self):
        files,receipt=self.validate()
        (self.root/stager.REQUIRED[0]).write_bytes(b"changed after validation")
        with self.assertRaisesRegex(RuntimeError,"after validation"):
            stager.stage(self.root,self.base/"target",files,receipt)
        self.assertFalse((self.base/"target").exists())

    def test_nonowned_destination_and_overlap(self):
        files,receipt=self.validate()
        target=self.base/"target";target.mkdir();(target/"user.txt").write_text("keep")
        with self.assertRaises(OSError): stager.stage(self.root,target,files,receipt)
        self.assertEqual((target/"user.txt").read_text(),"keep")
        with self.assertRaisesRegex(RuntimeError,"overlap"):
            stager.stage(self.root,self.root/"nested",files,receipt)

    def test_noncanonical_source_overlap(self):
        source=self.root/".."/self.root.name
        files,receipt=stager.validate(source,self.lock,self.preset)
        with self.assertRaisesRegex(RuntimeError,"overlap"):
            stager.stage(source,self.root/"nested",files,receipt)
        target=self.base/"canonical-target"
        stager.stage(source,target,files,receipt)
        self.assertEqual((target/stager.REQUIRED[0]).read_bytes(),(self.root/stager.REQUIRED[0]).read_bytes())

    def test_incremental_metadata_change_is_validated(self):
        target=self.base/"target";stamp=self.base/"stamp"
        args=["stage", "--source",str(self.root),"--lock",str(self.lock),"--preset",str(self.preset),
              "--destination",str(target),"--stamp",str(stamp),"--repair-if-needed"]
        with patch("sys.argv",args): self.assertEqual(stager.main(),0)
        original=(target/stager.REQUIRED[0]).read_bytes()
        (self.root/stager.REQUIRED[0]).write_bytes(b"tampered new source")
        with patch("sys.argv",args): self.assertEqual(stager.main(),1)
        self.assertEqual((target/stager.REQUIRED[0]).read_bytes(),original)

    def test_repair_and_noop(self):
        target=self.base/"target";stamp=self.base/"stamp"
        args=["stage", "--source",str(self.root),"--lock",str(self.lock),"--preset",str(self.preset),
              "--destination",str(target),"--stamp",str(stamp),"--repair-if-needed"]
        with patch("sys.argv",args): self.assertEqual(stager.main(),0)
        before=(target/stager.REQUIRED[0]).stat().st_mtime_ns
        with patch("sys.argv",args),patch.object(stager,"validate",side_effect=AssertionError("no-op revalidated")):
            self.assertEqual(stager.main(),0)
        self.assertEqual((target/stager.REQUIRED[0]).stat().st_mtime_ns,before)
        (target/stager.REQUIRED[1]).unlink()
        with patch("sys.argv",args): self.assertEqual(stager.main(),0)
        self.assertTrue((target/stager.REQUIRED[1]).is_file())
        shutil.rmtree(target)
        with patch("sys.argv",args): self.assertEqual(stager.main(),0)
        self.assertTrue((target/stager.REQUIRED[0]).is_file())

class BuildPreparationTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("prepare", Path(__file__).with_name("prepare-bundled-shell.py"))
        self.prepare = importlib.util.module_from_spec(spec); spec.loader.exec_module(self.prepare)
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name); self.root = self.base/"runtime"; self.cache=self.base/"cache"
        self.calls=[]

    def run_helper(self, command, check):
        self.assertTrue(check); self.calls.append(command)
        if command[1].endswith("shell-bootstrap.py"):
            root=Path(command[command.index("--destination")+1])
            for name in (*stager.REQUIRED, stager.MANIFEST):
                p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text("{}" if name==stager.MANIFEST else "fixture")
            (root/"notices").mkdir();(root/"notices/upstream.txt").write_text("preserve")
        else:
            output=Path(command[command.index("--output")+1]);(output/"runtime-notices").mkdir(parents=True)
            (output/"runtime-notices/THIRD-PARTY-NOTICES.txt").write_text("review")
            (output/"packet-manifest.json").write_text('{"source_packet_emitted":false}')

    def test_preparation_noop_and_deleted_output_repair(self):
        with patch.object(self.prepare,"recipe_digest",return_value="recipe"), patch.object(self.prepare.subprocess,"run",side_effect=self.run_helper):
            self.prepare.prepare(self.root,self.cache,True)
            self.assertEqual(len(self.calls),2)
            self.assertIn("--offline",self.calls[0]);self.assertNotIn("--sources",self.calls[0])
            self.assertNotIn("--source-packet",self.calls[1])
            self.assertEqual((self.root/"notices/upstream.txt").read_text(),"preserve")
            self.assertTrue((self.root/"notices/stormtty-review/THIRD-PARTY-NOTICES.txt").is_file())
            self.assertFalse(json.loads((self.root/stager.MANIFEST).read_text())["release_notices_complete"])
            self.assertTrue(json.loads((self.root/stager.MANIFEST).read_text())["base_runtime_notices_complete"])
            self.prepare.prepare(self.root,self.cache,True);self.assertEqual(len(self.calls),2)
            (self.root/stager.REQUIRED[0]).unlink()
            self.prepare.prepare(self.root,self.cache,True);self.assertEqual(len(self.calls),4)

    def test_nano_payload_deletion_repairs_prepared_source(self):
        with patch.object(self.prepare, "recipe_digest", return_value="recipe"), patch.object(self.prepare.subprocess, "run", side_effect=self.run_helper):
            self.prepare.prepare(self.root, self.cache, True)
            for name in ("usr/bin/nano.exe", "usr/bin/file.exe", "usr/bin/msys-magic-1.dll",
                         "usr/bin/msys-bz2-1.dll", "usr/bin/msys-lzma-5.dll", "usr/share/misc/magic.mgc"):
                with self.subTest(name=name):
                    before = len(self.calls)
                    (self.root / name).unlink()
                    self.prepare.prepare(self.root, self.cache, True)
                    self.assertEqual(len(self.calls), before + 2)
                    self.assertTrue((self.root / name).is_file())

    def test_changed_recipe_refreshes_owned_runtime(self):
        with patch.object(self.prepare.subprocess, "run", side_effect=self.run_helper):
            with patch.object(self.prepare, "recipe_digest", return_value="pre-nano"):
                self.prepare.prepare(self.root, self.cache, True)
            with patch.object(self.prepare, "recipe_digest", return_value="nano"):
                self.prepare.prepare(self.root, self.cache, True)
            self.assertEqual(len(self.calls), 4)
            receipt = json.loads(self.root.with_name(self.root.name + ".build-recipe.json").read_text())
            self.assertEqual(receipt["recipe"], "nano")
            self.assertTrue((self.root / "usr/bin/nano.exe").is_file())

    def test_unowned_runtime_not_replaced(self):
        self.root.mkdir();(self.root/"user.txt").write_text("keep")
        with self.assertRaisesRegex(RuntimeError,"not owned"):
            self.prepare.prepare(self.root,self.cache)
        self.assertEqual((self.root/"user.txt").read_text(),"keep")

    def test_failed_refresh_keeps_old_runtime_and_cleans_guard(self):
        with patch.object(self.prepare,"recipe_digest",return_value="old"), patch.object(self.prepare.subprocess,"run",side_effect=self.run_helper):
            self.prepare.prepare(self.root,self.cache)
        with patch.object(self.prepare,"recipe_digest",return_value="new"), patch.object(self.prepare.subprocess,"run",side_effect=RuntimeError("fixture failed")):
            with self.assertRaisesRegex(RuntimeError,"fixture failed"):
                self.prepare.prepare(self.root,self.cache)
        self.assertEqual((self.root/stager.REQUIRED[0]).read_text(),"fixture")
        self.assertFalse(self.root.with_name(self.root.name+".build-active").exists())

if __name__ == "__main__": unittest.main()
