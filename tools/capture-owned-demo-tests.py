#!/usr/bin/env python3
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import stat
from unittest import mock
import unittest
from types import SimpleNamespace

spec=importlib.util.spec_from_file_location('demo',Path(__file__).with_name('capture-owned-demo.py'))
demo=importlib.util.module_from_spec(spec);spec.loader.exec_module(demo)

class OwnedFixture(unittest.TestCase):
    def test_real_git_state_and_private_environment(self):
        git=shutil.which('git')
        self.assertIsNotNone(git,'Git required to test real fixtures')
        with tempfile.TemporaryDirectory(prefix='StormTTY.Presentation.') as directory:
            root=Path(directory);env=demo.fixture_environment(root,Path(git))
            self.assertEqual(env['HOME'],str(root/'home'))
            self.assertEqual(env['GIT_CONFIG_NOSYSTEM'],'1')
            self.assertEqual(Path(env['GIT_CONFIG_GLOBAL']).read_text(),'')
            demo.create_fixture(root,Path(git),env)
            self.assertEqual((root/'fixture.marker').read_text(),demo.MARKER)
            for name in ('Theme Lab','Notes'):
                repo=root/name
                status=subprocess.check_output([git,'-C',str(repo),'status','--short','--branch'],env=env,text=True)
                self.assertIn('## palette-review',status)
                self.assertIn(' M theme.toml',status)
                self.assertIn('?? ideas.md',status)
                diff=subprocess.check_output([git,'-C',str(repo),'diff','--stat'],env=env,text=True)
                self.assertIn('1 insertion',diff)
                self.assertTrue((repo/'.git').is_dir())
            with self.assertRaises(ValueError):demo.create_fixture(root,Path(git),env)
            info=root.lstat()
            demo.remove_fixture(root,(info.st_dev,info.st_ino),False,None)
            self.assertFalse(root.exists())

    def test_existing_content_is_never_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'user-file').write_text('keep')
            with self.assertRaises(ValueError):demo.create_fixture(root,Path('git'),{})
            self.assertEqual((root/'user-file').read_text(),'keep')

    def test_uncertain_cleanup_retains_owned_root(self):
        clean=dict(reaped=True,cleanup_error='',timed_out=False,returncode=0)
        self.assertTrue(demo.can_remove_fixture(True,SimpleNamespace(**clean)))
        for change in ({'cleanup_error':'owned-child reap uncertain'}, {'timed_out':True},
                       {'reaped':False}, {'returncode':2}):
            self.assertFalse(demo.can_remove_fixture(True,SimpleNamespace(**(clean|change))))
        self.assertFalse(demo.can_remove_fixture(True,None))
        self.assertTrue(demo.can_remove_fixture(False,None))

    def cleanup_fixture(self, parent):
        root=Path(tempfile.mkdtemp(prefix='StormTTY.Presentation.',dir=parent))
        (root/'fixture.marker').write_text(demo.MARKER,encoding='utf-8',newline='\n')
        blob=root/'Notes'/'.git'/'objects'/'ab'/('c'*38)
        blob.parent.mkdir(parents=True);blob.write_bytes(b'owned blob')
        info=root.lstat()
        return root,blob,(info.st_dev,info.st_ino)

    def test_readonly_owned_git_blob_cleanup(self):
        # Real Windows attribute + unlink behavior on Windows CI; Linux still
        # checks complete removal of the owned tree without changing permissions.
        with tempfile.TemporaryDirectory() as parent:
            root,blob,identity=self.cleanup_fixture(parent)
            blob.chmod(stat.S_IREAD)
            demo.remove_fixture(root,identity,False,None)
            self.assertFalse(root.exists())

    def test_windows_readonly_retry_is_bounded(self):
        for retry_fails in (False,True):
            with self.subTest(retry_fails=retry_fails), tempfile.TemporaryDirectory() as parent:
                root,blob,identity=self.cleanup_fixture(parent)
                original_unlink=os.unlink;original_check=demo.checked_fixture_path
                attempts=[]
                def unlink(path,*args,**kwargs):
                    # rmtree's fd-based Linux path may be relative.
                    if Path(path).name==blob.name:
                        attempts.append(path)
                        if len(attempts)==1 or retry_fails:
                            error=PermissionError('simulated readonly Git blob');error.winerror=5
                            raise error
                    return original_unlink(path,*args,**kwargs)
                def checked(root,path,identity):
                    info=original_check(root,path,identity)
                    if path==blob:
                        return SimpleNamespace(st_mode=info.st_mode,
                            st_file_attributes=stat.FILE_ATTRIBUTE_READONLY)
                    return info
                with mock.patch.object(demo.sys,'platform','win32'), \
                     mock.patch.object(demo.os,'unlink',side_effect=unlink), \
                     mock.patch.object(demo,'checked_fixture_path',side_effect=checked), \
                     mock.patch.object(demo.os,'chmod') as chmod:
                    if retry_fails:
                        with self.assertRaises(PermissionError):demo.remove_fixture(root,identity,False,None)
                        self.assertTrue(blob.exists());self.assertTrue((root/'fixture.marker').exists())
                    else:
                        demo.remove_fixture(root,identity,False,None)
                        self.assertFalse(root.exists())
                    self.assertEqual(len(attempts),2)
                    chmod.assert_called_once_with(blob,stat.S_IWRITE)

    def test_non_readonly_or_non_git_permission_error_is_not_repaired(self):
        for git_object,readonly in ((True,False),(False,True)):
            with self.subTest(git_object=git_object,readonly=readonly), tempfile.TemporaryDirectory() as parent:
                root,blob,identity=self.cleanup_fixture(parent)
                if not git_object:
                    other=blob.parent/'not-an-object';blob.rename(other);blob=other
                original_unlink=os.unlink;original_check=demo.checked_fixture_path
                def unlink(path,*args,**kwargs):
                    if Path(path).name==blob.name:
                        error=PermissionError('not eligible');error.winerror=5;raise error
                    return original_unlink(path,*args,**kwargs)
                def checked(root,path,identity):
                    info=original_check(root,path,identity)
                    if path==blob:
                        return SimpleNamespace(st_mode=info.st_mode,
                            st_file_attributes=stat.FILE_ATTRIBUTE_READONLY if readonly else 0)
                    return info
                with mock.patch.object(demo.sys,'platform','win32'), \
                     mock.patch.object(demo.os,'unlink',side_effect=unlink), \
                     mock.patch.object(demo,'checked_fixture_path',side_effect=checked), \
                     mock.patch.object(demo.os,'chmod') as chmod:
                    with self.assertRaises(PermissionError):demo.remove_fixture(root,identity,False,None)
                    chmod.assert_not_called()
                self.assertTrue(blob.exists());self.assertTrue((root/'fixture.marker').exists())

    def test_cleanup_refuses_unowned_outside_and_changed_identity(self):
        with tempfile.TemporaryDirectory() as parent:
            root,blob,identity=self.cleanup_fixture(parent)
            outside=Path(parent)/'keep';outside.write_text('keep')
            with self.assertRaises(ValueError):demo.checked_fixture_path(root,outside,identity)
            with self.assertRaises(ValueError):demo.remove_fixture(root,(0,0),False,None)
            (root/'fixture.marker').write_text('unowned')
            with self.assertRaises(ValueError):demo.remove_fixture(root,identity,False,None)
            self.assertTrue(blob.exists());self.assertEqual(outside.read_text(),'keep')

    def test_cleanup_refuses_links_before_any_deletion(self):
        with tempfile.TemporaryDirectory() as parent:
            root,blob,identity=self.cleanup_fixture(parent)
            outside=Path(parent)/'keep';outside.write_text('keep')
            link=root/'link'
            try:link.symlink_to(outside)
            except OSError:self.skipTest('symlink creation unavailable')
            with self.assertRaises(ValueError):demo.remove_fixture(root,identity,False,None)
            self.assertTrue(blob.exists());self.assertEqual(outside.read_text(),'keep')
            link.unlink()
            os.link(outside,link)
            with self.assertRaises(ValueError):demo.remove_fixture(root,identity,False,None)
            self.assertTrue(blob.exists());self.assertEqual(outside.read_text(),'keep')

    def test_cleanup_refuses_reparse_attribute(self):
        with tempfile.TemporaryDirectory() as parent:
            root,blob,identity=self.cleanup_fixture(parent)
            original=Path.lstat
            def lstat(path,*args,**kwargs):
                info=original(path,*args,**kwargs)
                if path==blob.parent:
                    return SimpleNamespace(st_mode=info.st_mode,st_file_attributes=stat.FILE_ATTRIBUTE_REPARSE_POINT)
                return info
            with mock.patch.object(Path,'lstat',lstat):
                with self.assertRaises(ValueError):demo.remove_fixture(root,identity,False,None)
            self.assertTrue(blob.exists())

    def test_cleanup_retains_all_files_for_live_or_uncertain_process(self):
        clean=dict(reaped=True,cleanup_error='',timed_out=False,returncode=0)
        with tempfile.TemporaryDirectory() as parent:
            root,blob,identity=self.cleanup_fixture(parent)
            for result in [None]+[SimpleNamespace(**(clean|change)) for change in
                    ({'reaped':False},{'timed_out':True},{'cleanup_error':'uncertain'},{'returncode':1})]:
                with self.assertRaises(ValueError):demo.remove_fixture(root,identity,True,result)
                self.assertTrue(blob.exists());self.assertTrue((root/'fixture.marker').exists())
            demo.remove_fixture(root,identity,True,SimpleNamespace(**clean))
            self.assertFalse(root.exists())

    def test_environment_restored_on_error(self):
        previous=dict(os.environ)
        with self.assertRaises(RuntimeError):
            with demo.child_environment({'ONLY_OWNED_DEMO':'1'}):
                self.assertEqual(dict(os.environ),{'ONLY_OWNED_DEMO':'1'})
                raise RuntimeError('fixture')
        self.assertEqual(dict(os.environ),previous)

if __name__=='__main__':unittest.main()
