import importlib.util
from pathlib import Path
import subprocess
import unittest
spec=importlib.util.spec_from_file_location('paths',Path(__file__).with_name('shell-path-tests.py'))
paths=importlib.util.module_from_spec(spec);spec.loader.exec_module(paths)
class ProtocolTests(unittest.TestCase):
    def result(self,data,code=0):return subprocess.CompletedProcess([],code,data,b'')
    def test_removal_empty_and_unicode(self):
        result=paths.resolver_overrides(self.result(b'STORMTTY_EMPTY_HOME=\nSTORMTTY_NATIVE_ZSH_NO_DUMP=-\nSTORMTTY_NATIVE_ZSH_INIT_EVENT=-\nHOME=636166c3a9\n'))
        self.assertEqual(result['HOME'],'caf\u00e9');self.assertEqual(result['STORMTTY_EMPTY_HOME'],'')
        self.assertIsNone(result['STORMTTY_NATIVE_ZSH_NO_DUMP'])
    def test_missing_or_valued_reserved_fails(self):
        for data in [b'STORMTTY_EMPTY_HOME=\n',b'STORMTTY_EMPTY_HOME=\nSTORMTTY_NATIVE_ZSH_NO_DUMP=\nSTORMTTY_NATIVE_ZSH_INIT_EVENT=-\n']:
            with self.assertRaises(AssertionError):paths.resolver_overrides(self.result(data))
    def test_windows_case_insensitive_deletion_and_set(self):
        env={'stormtty_native_zsh_no_dump':'hostile','StormTTY_Native_Zsh_Init_Event':'hostile',
             'STORMTTY_NATIVE_ZSH_NO_DUMP':'also-hostile','Home':'old','other':'preserved'}
        paths.apply_overrides(env,{name:None for name in paths.RESERVED})
        paths.apply_overrides(env,{'HOME':''})
        self.assertEqual(env,{'HOME':'','other':'preserved'})
    def test_numeric_failure(self):
        with self.assertRaisesRegex(AssertionError,'exit=-1073741819 exit_u32=3221225477 stdout_bytes=0 stderr_bytes=0'):
            paths.resolver_overrides(self.result(b'',-1073741819))
if __name__=='__main__':unittest.main()
