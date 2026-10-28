#!/usr/bin/env python3
"""Offline managed source tests; optionally exercise real pinned modules with --assets-cache.
Linux timings are not Windows/MSYS2 qualification. No history/config contents are printed.
"""
import argparse
import importlib.util
import json
import io
import tarfile
from unittest import mock
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import tempfile
import time
import unittest

TOOLS = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('addons', TOOLS / 'shell-addons.py')
addons = importlib.util.module_from_spec(spec); spec.loader.exec_module(addons)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--assets-cache', type=Path)
parser.add_argument('--benchmark', action='store_true')
parser.add_argument('--zsh', type=Path, help='Exact bundled zsh.exe for Windows/MSYS2 qualification')
args, remaining = parser.parse_known_args()

class ManagedTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.home = self.root / 'home'; self.home.mkdir(mode=0o700)
        self.runtime = self.root / 'runtime'
        self.zdot = self.runtime / 'stormtty/zsh'; self.zdot.mkdir(parents=True)
        shutil.copyfile(TOOLS / 'shell-preset.zsh', self.zdot / '.zshrc')
        if args.assets_cache: addons.stage(self.runtime, args.assets_cache, True, {})
        self.env = dict(os.environ, HOME=self.home.as_posix(), ZDOTDIR=self.zdot.as_posix(), STORMTTY_SESSION_ID='managed', TERM='xterm-256color')
        self.shell = str(args.zsh.resolve()) if args.zsh else shutil.which('zsh')
        if not self.shell: self.skipTest('zsh unavailable')
        if args.zsh: self.env['PATH'] = str(args.zsh.resolve().parent) + os.pathsep + self.env.get('PATH', '')
    def run_shell(self, command, **kwargs):
        return subprocess.run([self.shell, '-dic', command], env=self.env, cwd=self.root, capture_output=True, text=True, timeout=15, **kwargs)
    def test_drive_source_normalized_before_canonicalization(self):
        # Exercise the exact loader expansion before :A. Native MSYS execution
        # remains a separate gate; this proves the path-expression semantics.
        script = (TOOLS / 'shell-preset.zsh').read_text()
        start = script.index('if [[ $OSTYPE == (msys*|cygwin*) && $_stormtty_source')
        end = script.index('unset _stormtty_source', start)
        for source, expected in [
                ('D:/runtime/stormtty/zsh/.zshrc', '/d/runtime/stormtty/vendor'),
                ('/d/runtime/stormtty/zsh/.zshrc', '/d/runtime/stormtty/vendor')]:
            env = dict(self.env, QUAL_SOURCE=source)
            result = subprocess.run([self.shell, '-fc',
                'OSTYPE=msys; _stormtty_source=$QUAL_SOURCE; ' + script[start:end] +
                'print -r -- "$_stormtty_vendor"'], env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), expected)

    def test_pins_and_source_allowlist(self):
        self.assertEqual(len(addons.read_lock()['components']), 3)
        c=addons.read_lock()['components'][0]
        self.assertTrue(addons.selected(c, 'oh-my-zsh.sh'))
        self.assertFalse(addons.selected(c, 'tools/upgrade.sh'))
        self.assertFalse(addons.selected(c, 'lib/cli.zsh'))
    def test_corrupt_cached_source_rejected(self):
        c=addons.read_lock()['components'][0]
        cache=self.root/'cache'; cache.mkdir()
        (cache/c['filename']).write_bytes(b'corrupt')
        with self.assertRaisesRegex(RuntimeError, 'integrity'): addons.acquire(c, cache, True)
    def test_unsafe_source_paths_and_links_rejected(self):
        c=addons.read_lock()['components'][0]
        prefix=c['repository'].rsplit('/',1)[-1]+'-'+c['revision']
        for name, kind in [(prefix+'/../escape',tarfile.REGTYPE),
                           (prefix+'/functions/unsafe',tarfile.SYMTYPE),
                           (prefix+'/functions/CON',tarfile.REGTYPE)]:
            archive=self.root/'fixture.tar.gz'
            with tarfile.open(archive,'w:gz') as bundle:
                member=tarfile.TarInfo(name); member.type=kind; member.size=0
                bundle.addfile(member,io.BytesIO(b''))
            with mock.patch.object(addons, 'read_lock', return_value={'components':[c]}), mock.patch.object(addons, 'acquire',return_value=archive):
                with self.assertRaises(RuntimeError): addons.stage(self.root/'unsafe',self.root,True,{})
    @unittest.skipIf(os.name == 'nt', 'Windows private config blocked pending native owner/DACL validator')
    def test_dedicated_user_config_and_update_file_preserved(self):
        state=self.home/'.stormtty/zsh';state.mkdir(parents=True, mode=0o700); state.parent.chmod(0o700)
        config=state/'config.zsh';config.write_text('typeset -g USER_CONFIG_FIXTURE=yes\n')
        update=self.home/'.zsh-update';update.write_text('preserve-user-updater-state')
        result=self.run_shell('[[ $USER_CONFIG_FIXTURE == yes ]]')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(config.read_text(),'typeset -g USER_CONFIG_FIXTURE=yes\n')
        self.assertEqual(update.read_text(),'preserve-user-updater-state')
    def test_owned_loader_and_user_dotfile_isolation(self):
        (self.home / '.zshrc').write_text('touch SHOULD_NOT_EXIST\n')
        result = self.run_shell('[[ $STORMTTY_MANAGED_ZSH == 1 ]]')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.root / 'SHOULD_NOT_EXIST').exists())
    def test_empty_home_ephemeral(self):
        self.env.update(HOME='', STORMTTY_EMPTY_HOME='1')
        result=self.run_shell('[[ -z $HOME && -z ${HISTFILE-} && $SAVEHIST == 0 ]]')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.root / '.stormtty').exists())
    def test_symlink_state_not_followed(self):
        outside=self.root/'outside'; outside.mkdir()
        try: (self.home/'.stormtty').symlink_to(outside, target_is_directory=True)
        except OSError: self.skipTest('OS denied symlink fixture creation')
        result=self.run_shell('[[ -z ${HISTFILE-} && $SAVEHIST == 0 ]]')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(outside.iterdir()), [])
    @unittest.skipIf(os.name == 'nt', 'POSIX mode fixture; Windows ACL/noacl needs native filesystem inspection')
    def test_unsafe_state_permissions_not_loaded(self):
        state=self.home/'.stormtty'; state.mkdir(mode=0o777); state.chmod(0o777)
        result=self.run_shell('[[ -z ${HISTFILE-} && $SAVEHIST == 0 ]]')
        self.assertEqual(result.returncode, 0, result.stderr)
    @unittest.skipIf(os.name == 'nt', 'POSIX mode checks are not Windows ACL evidence')
    def test_safe_0755_state_is_accepted(self):
        state=self.home/'.stormtty/zsh';state.mkdir(parents=True)
        state.parent.chmod(0o755);state.chmod(0o755)
        result=self.run_shell('[[ -n $_stormtty_state ]]')
        self.assertEqual(result.returncode,0,result.stderr)
    @unittest.skipIf(os.name == 'nt', 'POSIX mode checks are not Windows ACL evidence')
    def test_unsafe_existing_state_files_never_loaded_or_modified(self):
        state=self.home/'.stormtty/zsh';state.mkdir(parents=True)
        state.parent.chmod(0o711);state.chmod(0o711)
        version=subprocess.check_output([self.shell,'-fc','print -r -- $ZSH_VERSION'],text=True).strip()
        for name in ['config.zsh','config.zsh.zwc','history','zcompdump-'+version,'zcompdump-'+version+'.zwc']:
            with self.subTest(name=name):
                file=state/name;file.write_text('touch PWNED\n');file.chmod(0o666)
                before=file.read_bytes()
                result=self.run_shell('[[ -z $_stormtty_state && -z ${HISTFILE-} && $STORMTTY_EPHEMERAL == 1 ]]')
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual(file.read_bytes(),before)
                self.assertEqual(file.stat().st_mode & 0o777,0o666)
                self.assertFalse((self.root/'PWNED').exists())
                file.unlink()
    def test_existing_config_directory_rejected(self):
        state=self.home/'.stormtty/zsh';state.mkdir(parents=True)
        state.parent.chmod(0o700);state.chmod(0o700)
        (state/'config.zsh').mkdir()
        result=self.run_shell('[[ -z $_stormtty_state && $STORMTTY_EPHEMERAL == 1 ]]')
        self.assertEqual(result.returncode,0,result.stderr)
    def test_ephemeral_keeps_real_bundled_modules(self):
        if not args.assets_cache: self.skipTest('pass --assets-cache for pinned source integration')
        self.env.update(HOME='', STORMTTY_EMPTY_HOME='1')
        result=self.run_shell('[[ $STORMTTY_OMZ_LOADED == 1 && ${+functions[git_current_branch]} == 1 && ${+functions[_zsh_autosuggest_start]} == 1 && ${+functions[_zsh_highlight]} == 1 && -z ${HISTFILE-} && $SAVEHIST == 0 ]]')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(list(self.home.iterdir()),[])
        self.assertFalse(any(self.runtime.rglob('*.zwc')))
    def test_native_noacl_policy_keeps_modules_without_persistence(self):
        if not args.assets_cache: self.skipTest('pass --assets-cache for pinned source integration')
        command='OSTYPE=msys; source "$ZDOTDIR/.zshrc"; [[ $STORMTTY_OMZ_LOADED == 1 && $STORMTTY_EPHEMERAL == 1 && -z ${HISTFILE-} && -z $_stormtty_state ]]'
        result=subprocess.run([self.shell,'-dfic',command],env=self.env,cwd=self.root,capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(list(self.home.iterdir()),[])
    @unittest.skipIf(os.name == 'nt', 'POSIX umask fixture; Windows ACL policy separate')
    def test_cache_creation_respects_private_permissions_and_restores_umask(self):
        if not args.assets_cache: self.skipTest('pass --assets-cache for pinned source integration')
        command='umask 0002; expected_umask=$(umask); source "$ZDOTDIR/.zshrc"; [[ $(umask) == $expected_umask ]]'
        result=subprocess.run([self.shell,'-dfic',command],env=self.env,cwd=self.root,capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)
        state=self.home/'.stormtty/zsh'
        for file in state.glob('zcompdump-*'):
            self.assertEqual(file.stat().st_mode & 0o022,0)
        result=self.run_shell('[[ $STORMTTY_EPHEMERAL == 0 ]]')
        self.assertEqual(result.returncode,0,result.stderr)
    def test_native_bridge_consumed_and_uses_dedicated_state(self):
        if not args.assets_cache: self.skipTest('pass --assets-cache for pinned source integration')
        if os.name == 'nt': self.skipTest('synthetic path contract fixture, native ACL proof tested by C++ helper')
        state=self.root/'native-state';state.mkdir(mode=0o700)
        (state/'config.zsh').write_text('typeset -g NATIVE_CONFIG_FIXTURE=yes\n')
        # UNC-shaped spelling resolves locally on POSIX for this contract-only
        # fixture; no native ownership/ACL qualification is implied.
        self.env['STORMTTY_NATIVE_ZSH_STATE']='/'+str(state)
        command='OSTYPE=msys; source "$ZDOTDIR/.zshrc"; [[ $STORMTTY_EPHEMERAL == 0 && $NATIVE_CONFIG_FIXTURE == yes && -z ${STORMTTY_NATIVE_ZSH_STATE+x} && -z ${_stormtty_native_state+x} ]]'
        result=subprocess.run([self.shell,'-dfic',command],env=self.env,cwd=self.root,capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(list(self.home.iterdir()),[])
        self.env['HOME']=''; self.env['STORMTTY_EMPTY_HOME']='1'
        command='OSTYPE=msys; source "$ZDOTDIR/.zshrc"; [[ $STORMTTY_EPHEMERAL == 1 && -z ${STORMTTY_NATIVE_ZSH_STATE+x} ]]'
        result=subprocess.run([self.shell,'-dfic',command],env=self.env,cwd=self.root,capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)
    def test_custom_prompt_is_not_overwritten(self):
        result=self.run_shell('STORMTTY_PROMPT=custom; PROMPT="custom> "; false; _stormtty_precmd; [[ $? == 0 && $PROMPT == "custom> " && ${STORMTTY_CONTEXT[exit]} == 1 ]]')
        self.assertEqual(result.returncode,0,result.stderr)
    @unittest.skipIf(os.name == 'nt', 'POSIX PTY test; actual Windows ConPTY editing requires native gate')
    def test_interactive_failure_keeps_later_hooks_and_suggestion_editing(self):
        if not args.assets_cache: self.skipTest('pass --assets-cache for pinned source integration')
        try: import pexpect
        except ImportError: self.skipTest('optional pexpect unavailable')
        child=pexpect.spawn(self.shell,['-di'],env=self.env,cwd=str(self.root),encoding='utf-8',timeout=10)
        self.addCleanup(child.close,force=True)
        child.expect_exact('\x1b]133;B\x07')
        child.sendline('function _after() { print -r -- AFTER:$?:${STORMTTY_CONTEXT[exit]}; }; precmd_functions+=(_after)')
        child.expect_exact('\x1b]133;B\x07')
        child.sendline('print -r -- SUGGESTION_ACCEPTED')
        child.expect_exact('\x1b]133;C\x07SUGGESTION_ACCEPTED\r\n')
        child.expect_exact('\x1b]133;B\x07')
        child.sendline('false')
        child.expect_exact('\x1b]133;D;1\x07')
        child.expect_exact('AFTER:1:1')
        child.expect_exact('\x1b]133;B\x07')
        child.send('print -r -- SUGGES')
        child.send('\x1b[C')
        child.sendline('')
        child.expect_exact('\x1b]133;C\x07SUGGESTION_ACCEPTED\r\n')
        child.expect_exact('\x1b]133;B\x07')
        child.sendline('exit')
        child.expect(pexpect.EOF)
    def test_prompt_data_not_code_or_terminal_control(self):
        git=self.root/'.git'; git.mkdir()
        (git/'HEAD').write_text('ref: refs/heads/$(touch PWNED)%F{red}\x1b]2;bad\x07\n')
        result=self.run_shell('_stormtty_precmd; [[ ${STORMTTY_CONTEXT[branch]} == *PWNED* ]] || exit 3; print -P -- "$PROMPT"')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertFalse((self.root/'PWNED').exists())
        self.assertIn('%F{red}', result.stdout)
        self.assertNotIn('\x1b]2;bad', result.stdout)
    def test_worktree_and_detached_context(self):
        metadata=self.root/'metadata'; metadata.mkdir()
        (metadata/'HEAD').write_text('a'*40+'\n'); (self.root/'.git').write_text('gitdir: metadata\n')
        result=self.run_shell('_stormtty_precmd; [[ ${STORMTTY_CONTEXT[branch]} == detached:aaaaaaaa ]]')
        self.assertEqual(result.returncode,0,result.stderr)
    def test_osc133_exit_and_semantic_palette(self):
        result=self.run_shell('_stormtty_preexec; false; _stormtty_precmd; print -r -- "$PROMPT"')
        self.assertIn('\x1b]133;C\x07',result.stdout)
        self.assertIn('\x1b]133;D;1\x07',result.stdout)
        self.assertIn('exit:1',result.stdout)
        self.assertNotIn('242',result.stdout)
    def test_real_modules(self):
        if not args.assets_cache: self.skipTest('pass --assets-cache for pinned source integration')
        result=self.run_shell('[[ $STORMTTY_OMZ_LOADED == 1 && ${+functions[git_current_branch]} == 1 && ${+functions[_zsh_autosuggest_start]} == 1 && ${+functions[_zsh_highlight]} == 1 ]]')
        self.assertEqual(result.returncode,0,result.stderr)
    @unittest.skipIf(os.name == 'nt', 'Windows persistence blocked pending native owner/DACL validator')
    def test_persistent_history(self):
        if not args.assets_cache: self.skipTest('pass --assets-cache for pinned source integration')
        result=self.run_shell('[[ $SAVEHIST == 10000 ]]')
        self.assertEqual(result.returncode,0,result.stderr)
        result=subprocess.run([self.shell,'-di'],input='print -r -- managed_history_fixture\nexit\n',env=self.env,cwd=self.root,capture_output=True,text=True,timeout=15)
        history=self.home/'.stormtty/zsh/history'
        self.assertTrue(history.is_file())
        self.assertIn('managed_history_fixture',history.read_text())
        self.assertTrue(any((self.home/'.stormtty/zsh').glob('zcompdump-*')))
        self.assertFalse(any(self.zdot.glob('.zcompdump*')))

if __name__ == '__main__':
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(ManagedTests)
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    if args.benchmark and result.wasSuccessful():
        if not args.assets_cache: parser.error('--benchmark requires --assets-cache')
        fixture=ManagedTests();fixture.setUp()
        try:
            samples=[]
            for i in range(11):
                start=time.perf_counter(); outcome=fixture.run_shell('exit'); elapsed=time.perf_counter()-start
                if outcome.returncode: raise RuntimeError(outcome.stderr)
                samples.append(round(elapsed*1000,3))
            print(json.dumps({'platform':os.name, 'persistent_state_expected':os.name != 'nt','execution':'native Windows bundled MSYS2' if os.name == 'nt' and args.zsh else 'local Zsh', 'scope':'process-to-exit, not first-prompt latency; cold means fresh app state, not OS cache flush','cold_ms':samples[0], 'warm_median_ms':round(statistics.median(samples[1:]),3), 'warm_min_ms':min(samples[1:]),'warm_max_ms':max(samples[1:])}))
        finally: fixture.doCleanups()
    raise SystemExit(not result.wasSuccessful())
