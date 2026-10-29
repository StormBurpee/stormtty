import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
spec=importlib.util.spec_from_file_location('bundled_seed',Path(__file__).with_name('prepare-bundled-completion-seed.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.root=Path(self.tmp.name)
        self.runtime=self.root/'runtime';self.runtime.mkdir();(self.runtime/'payload').write_text('original')
        self.package=self.root/'seed';self.header=self.root/'generated/trust.hpp';self.calls=0
    def generate(self,runtime,destination):
        self.calls+=1;destination.mkdir();(destination/'seed.dump').write_text('# authored native generation stand-in\n')
        identity=['STORMTTY_SEED_V1','5.9.2','0'*40,m.seed.sha(destination/'seed.dump'),'1','@C',*m.seed.inventory(runtime)]
        (destination/'identity.txt').write_text('\n'.join(identity)+'\n')
    def prepare(self):
        with mock.patch.object(m.seed,'prepare',side_effect=self.generate):return m.prepare(self.runtime,self.package,self.header)
    def test_generate_noop_and_deleted_output_repair(self):
        self.assertTrue(self.prepare());self.assertEqual(self.calls,1)
        self.assertFalse(self.prepare());self.assertEqual(self.calls,1)
        self.header.unlink();self.assertFalse(self.prepare());self.assertTrue(self.header.exists())
        (self.package/'seed.dump').unlink();self.assertTrue(self.prepare());self.assertEqual(self.calls,2)
    def test_actual_content_change_rebuilds_anchor(self):
        self.prepare();before=self.header.read_bytes();(self.runtime/'payload').write_text('modified')
        self.assertTrue(self.prepare());self.assertNotEqual(before,self.header.read_bytes())
    def test_unowned_output_not_replaced(self):
        self.package.mkdir();(self.package/'keep').write_text('user data')
        with self.assertRaises(Exception):self.prepare()
        self.assertEqual((self.package/'keep').read_text(),'user data')
    def test_output_cannot_enter_runtime(self):
        with mock.patch.object(m.seed,'prepare') as generate:
            for destination,header in ((self.runtime/'seed',self.header),
                                       (self.runtime,self.header),
                                       (self.root,self.header),
                                       (self.package,self.runtime/'trust.hpp'),
                                       (self.package,self.runtime)):
                with self.subTest(destination=destination,header=header):
                    with self.assertRaises(ValueError):m.prepare(self.runtime,destination,header)
            generate.assert_not_called()
    def test_dotdot_output_cannot_enter_runtime(self):
        other=self.root/'other';other.mkdir()
        with mock.patch.object(m.seed,'prepare') as generate:
            with self.assertRaises(ValueError):
                m.prepare(self.runtime,other/'..'/'runtime'/'seed',self.header)
            generate.assert_not_called()
    def test_install_uses_same_final_tree_and_option_off(self):
        cmake=(Path(__file__).parents[1]/'cmake/StormTTYDeveloperShell.cmake').read_text()
        self.assertIn('option(STORMTTY_EXPERIMENTAL_COMPLETION_SEED ',cmake)
        self.assertIn('serialized initialization" OFF)',cmake)
        self.assertIn('install(DIRECTORY "$<TARGET_FILE_DIR:stormtty>/runtimes/zsh/"',cmake)
        self.assertIn('install(DIRECTORY "$<TARGET_FILE_DIR:stormtty>/runtimes/zsh-seed/"',cmake)
if __name__=='__main__':unittest.main()
