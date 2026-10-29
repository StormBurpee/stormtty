#!/usr/bin/env python3
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
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

    def test_environment_restored_on_error(self):
        previous=dict(os.environ)
        with self.assertRaises(RuntimeError):
            with demo.child_environment({'ONLY_OWNED_DEMO':'1'}):
                self.assertEqual(dict(os.environ),{'ONLY_OWNED_DEMO':'1'})
                raise RuntimeError('fixture')
        self.assertEqual(dict(os.environ),previous)

if __name__=='__main__':unittest.main()
