#!/usr/bin/env python3
"""Linux structural/mocked shell tests, NOT native MSYS2/UCRT qualification."""
import importlib.util
import json
import re
import shlex
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import io
import tarfile

TOOLS = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('shell_tools', TOOLS / 'shell-tools.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

class ToolsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='shell tools 测试 ', dir=TOOLS.parent)
        self.root = Path(self.tmp.name)
        (self.root/'stormtty/vendor/shell-tools').mkdir(parents=True)
        (self.root/'ucrt64/bin').mkdir(parents=True)
        (self.root/'ucrt64/share/fzf').mkdir(parents=True)
        for src,dst in [('shell-tools.zsh','init.zsh'),('shell-tools-zoxide.zsh','zoxide.zsh')]:
            shutil.copyfile(TOOLS/src,self.root/'stormtty/vendor/shell-tools'/dst)
        for f in ['completion.zsh','key-bindings.zsh']:
            (self.root/'ucrt64/share/fzf'/f).write_text('typeset -g FZF_SOURCED=1\n')
        self.bin = self.root/'mock'; self.bin.mkdir()
        # Stub Windows conversions reversibly. Data is never interpreted as code.
        (self.bin/'cygpath').write_text('#!/usr/bin/python3\nimport sys\nprint(sys.argv[-1])\n')
        (self.bin/'zoxide').write_text('#!/usr/bin/python3\nimport os,sys,json\nwith open(os.environ["LOG"],"a") as f: f.write(json.dumps([sys.argv[1:],os.environ.get("_ZO_DATA_DIR")])+"\\n")\nif sys.argv[1]=="query": print(os.environ["RESULT"])\n')
        for file in self.bin.iterdir(): file.chmod(0o755)
        self.env = dict(os.environ, HOME=str(self.root), ROOT=str(self.root), LOG=str(self.root/'log'), RESULT=str(self.root), PATH=str(self.bin)+':/usr/bin:/bin')
        self.prefix = 'STORMTTY_EPHEMERAL=0; _stormtty_state=$ROOT; source "$ROOT/stormtty/vendor/shell-tools/init.zsh"; '

    def tearDown(self): self.tmp.cleanup()

    def shell(self, text):
        result = subprocess.run(['zsh','-f','-i','-c',text],env=self.env,text=True,capture_output=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        return result.stdout.strip()

    def test_native_fixture_options_are_shellword_safe(self):
        # Upstream go-shellwords stops at unquoted ; & | < >. Keep this
        # authored fixture in a deliberately narrower safe subset rather than
        # pretending Python shlex is an execution test of that Go parser.
        source = (TOOLS.parent / 'tests/shell_navigation_probe.cpp').read_text()
        value = re.search(r"export FZF_DEFAULT_OPTS='([^']*)'", source).group(1)
        self.assertFalse(any(char in value for char in ";&|<>"))
        words = shlex.split(value)
        self.assertIn('--pointer=@', words)
        self.assertIn('--prompt=QFZF_SELECT:', words)
        self.assertIn('--no-scrollbar', words)

    def test_shell_default_and_override(self):
        output = self.shell('unset SHELL; ' + self.prefix + 'print -r -- "$SHELL"')
        self.assertEqual(output, str(self.root / 'usr/bin/zsh.exe'))
        output = self.shell('export SHELL=explicit; ' + self.prefix + 'print -r -- "$SHELL"')
        self.assertEqual(output, 'explicit')
        output = self.shell('export SHELL=""; ' + self.prefix + 'print -r -- "[${SHELL}]"')
        self.assertEqual(output, '[]')

    def test_lock(self):
        lock = m.read_lock()
        self.assertFalse(lock['public_redistribution_ready'])
        self.assertEqual(lock['native_qualification'],'pending')
        self.assertEqual(len(lock['packages']),2)
        self.assertEqual({p['name'] for p in lock['msys_support']['packages']}, {'gawk','libreadline','mpfr'})

    def test_source_merge_preserves_identity(self):
        item={'filename':'a','sha256':'a'*64,'bytes':1}
        self.assertEqual(m.merge_sources([item],[item]),[item])
        with self.assertRaisesRegex(RuntimeError,'Conflicting source'):
            m.merge_sources([item],[dict(item,sha256='b'*64)])

    def test_startup_no_process(self):
        self.assertEqual(self.shell(self.prefix+'print $FZF_SOURCED ${+functions[z]} ${+functions[zi]} ${+functions[cd]}'), '1 1 1 0')
        self.assertFalse((self.root/'log').exists())

    def test_optouts(self):
        self.assertEqual(self.shell('STORMTTY_FZF=0; STORMTTY_ZOXIDE=0; '+self.prefix+'print ${+FZF_SOURCED} ${+functions[z]}'), '0 0')
        self.assertEqual(self.shell('STORMTTY_SHELL_TOOLS=0; '+self.prefix+'print ${+FZF_SOURCED} ${+functions[z]}'), '0 0')

    def test_ephemeral(self):
        self.assertEqual(self.shell(self.prefix.replace('EPHEMERAL=0','EPHEMERAL=1')+'print ${+functions[z]}'), '0')
        self.assertFalse((self.root/'log').exists())

    def test_user_overrides(self):
        self.assertEqual(self.shell('function z() { print mine; }; FZF_DEFAULT_OPTS="--height 22%"; '+self.prefix+'z; print -r -- "$FZF_DEFAULT_OPTS"'), 'mine\n--height 22%')
        self.shell('export _ZO_DATA_DIR="user chosen db"; '+self.prefix+'_stormtty_zoxide_hook')
        self.assertEqual(json.loads((self.root/'log').read_text())[1], 'user chosen db')

    def test_path_and_injection(self):
        target = self.root/'space Unicode ü $(touch GOTCHA) `touch BAD`'; target.mkdir()
        self.env['RESULT']=str(target)
        self.assertEqual(self.shell(self.prefix+'z keyword; print -r -- "$PWD"'), str(target))
        self.assertFalse((self.root/'GOTCHA').exists()); self.assertFalse((self.root/'BAD').exists())
        calls=[json.loads(line) for line in (self.root/'log').read_text().splitlines()]
        self.assertEqual(calls[0][0][0], 'query'); self.assertEqual(calls[-1][0],['add','--',str(target)])
        self.assertEqual(calls[-1][1], str(self.root))

    def test_zi_and_previous_directory(self):
        target=self.root/'interactive ü space'; target.mkdir()
        self.env['RESULT']=str(target)
        output=self.shell(self.prefix+'builtin cd -- "$ROOT"; zi keyword; print -r -- "$PWD"; z -; print -r -- "$PWD"')
        self.assertEqual(output,str(target)+'\n'+str(self.root))
        calls=[json.loads(line)[0] for line in (self.root/'log').read_text().splitlines()]
        self.assertIn(['query','--interactive','--','keyword'],calls)

    def test_windows_conversion_directions(self):
        target=self.root/'Windows result ü'; target.mkdir()
        self.env['RESULT']='C:\\Windows result ü'
        self.env['POSIX_RESULT']=str(target)
        (self.bin/'cygpath').write_text('#!/usr/bin/python3\nimport os,sys\nif sys.argv[1]=="-u":\n assert sys.argv[-1]==os.environ["RESULT"]\n print(os.environ["POSIX_RESULT"])\nelse: print("C:"+sys.argv[-1])\n')
        self.assertEqual(self.shell(self.prefix+'z keyword; print -r -- "$PWD"'),str(target))
        calls=[json.loads(line) for line in (self.root/'log').read_text().splitlines()]
        self.assertTrue(calls[0][0][2].startswith('C:'))
        self.assertEqual(calls[-1][1],'C:'+str(self.root))

    def test_hook_once(self):
        self.assertEqual(self.shell(self.prefix+'source "$ROOT/stormtty/vendor/shell-tools/zoxide.zsh"; print ${(M)#chpwd_functions:#_stormtty_zoxide_hook}'), '1')

    def test_hostile_archive_members(self):
        for name, kind in [('usr/bin/fzf.exe', 'file'), ('ucrt64/../escape', 'file'),
                           ('ucrt64/bin/CON.exe', 'file'), ('ucrt64/link', 'symlink')]:
            with self.subTest(name=name):
                blob=io.BytesIO()
                with tarfile.open(fileobj=blob, mode='w') as tar:
                    member=tarfile.TarInfo(name)
                    if kind=='symlink': member.type=tarfile.SYMTYPE; member.linkname='/outside'
                    else: member.size=1
                    tar.addfile(member, io.BytesIO(b'x') if kind=='file' else None)
                blob.seek(0)
                with patch.object(m.bootstrap,'download',return_value=Path('fixture')), patch.object(m.bootstrap,'open_package',side_effect=lambda *a: tarfile.open(fileobj=blob,mode='r:')):
                    with self.assertRaises(RuntimeError):
                        m.stage(self.root, self.root/'cache', True, {}, '/usr/bin/zstd')

    def test_corrupt_cached_binary(self):
        cache=self.root/'cache'; cache.mkdir()
        item=m.read_lock()['packages'][0]
        (cache/item['filename']).write_bytes(b'not the package')
        with self.assertRaisesRegex(RuntimeError,'integrity'):
            m.stage(self.root,cache,True,{},'/usr/bin/zstd')

    def test_missing_offline(self):
        with self.assertRaisesRegex(RuntimeError,'Offline archive missing'):
            m.stage(self.root, self.root/'empty-cache', True, {}, '/usr/bin/zstd')

if __name__=='__main__': unittest.main()
