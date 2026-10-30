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
import re
from urllib.parse import unquote

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
    def npm_completion(self, setup='', query='npm run bu', words='npm run bu'):
        # Source once with simulated host kind; this is not Windows runtime proof.
        if os.name != 'nt': self.env['STORMTTY_NATIVE_CONSOLE_SETUP'] = shutil.which('true')
        code = '''OSTYPE=msys
source "$ZDOTDIR/.zshrc"
[[ $_comps[npm] == _stormtty_npm_completion ]] || exit 91
compadd() { [[ $1 == -- ]] || return 92; shift; printf '%s\\0' "$@"; }
''' + setup + '\n' + f'''BUFFER={json.dumps(query)}
words=({words})
CURRENT=${{#words}}
_stormtty_npm_completion
'''
        return subprocess.run([self.shell, '-dfic', code], env=self.env, cwd=self.root,
                              capture_output=True, text=True, timeout=15)

    def fake_npm(self, output, status=0):
        import sys
        bindir = self.root / 'bin'; bindir.mkdir()
        record = self.root / 'npm-call.json'
        npm = bindir / 'npm'
        npm.write_text(f'''#!{sys.executable}
import json, os, sys
with open({str(record)!r}, 'w') as f:
    json.dump({{'args': sys.argv[1:], 'term': os.getenv('TERM'),
               'msystem': os.getenv('MSYSTEM'), 'point': os.getenv('COMP_POINT'),
               'line': os.getenv('COMP_LINE'), 'word': os.getenv('COMP_CWORD'),
               'notifier': os.getenv('NPM_CONFIG_UPDATE_NOTIFIER')}}, f)
sys.stdout.write({output!r})
sys.stderr.write('npm diagnostic must not corrupt the prompt\\n')
sys.exit({status})
''')
        npm.chmod(0o700)
        self.env.update(PATH=str(bindir) + os.pathsep + self.env['PATH'], MSYSTEM='MSYS')
        return record

    @unittest.skipIf(os.name == 'nt', 'POSIX npm executable stub; native probe separately required')
    def test_npm_completion_is_scoped_literal_and_lazy(self):
        names = ['build', 'build:watch', 'two words', '路径', '$(touch SHOULD_NOT_EXIST)', '*']
        record = self.fake_npm('\n'.join(names) + '\n')
        result = self.npm_completion('''[[ ! -e npm-call.json && $TERM == xterm-256color && $MSYSTEM == MSYS ]] || exit 93
alias npm='false'
''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.split('\0')[:-1], names)
        self.assertEqual(result.stderr, '')
        self.assertFalse((self.root / 'SHOULD_NOT_EXIST').exists())
        self.assertEqual(json.loads(record.read_text()), dict(args=['completion', '--', 'npm', 'run', 'bu'],
            term='cygwin', msystem='MSYS', point='0', line='npm run bu', word='2', notifier='false'))
        # The subsequent ordinary command sees the real inherited terminal type.
        code = '''OSTYPE=msys; source "$ZDOTDIR/.zshrc"
compadd() { :; }; BUFFER='npm run bu'; words=(npm run bu); CURRENT=3
_stormtty_npm_completion
[[ $TERM == xterm-256color && $MSYSTEM == MSYS && ! -v COMP_POINT && ! -v NPM_CONFIG_UPDATE_NOTIFIER ]] || exit 94
command npm --version >/dev/null 2>/dev/null
'''
        result = subprocess.run([self.shell, '-dfic', code], env=self.env, cwd=self.root, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(record.read_text())['term'], 'xterm-256color')
        self.assertEqual(json.loads(record.read_text())['args'], ['--version'])

    @unittest.skipIf(os.name == 'nt', 'POSIX npm executable stub')
    def test_npm_completion_failure_does_not_offer_partial_output(self):
        self.fake_npm('not a valid candidate\n', status=1)
        result = self.npm_completion()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, '')
        self.assertEqual(result.stderr, '')

    @unittest.skipIf(os.name == 'nt', 'POSIX npm executable stub')
    def test_npm_completion_empty_output_is_not_a_match(self):
        self.fake_npm('')
        result = self.npm_completion()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, '')

    def test_npm_completion_posix_untouched(self):
        if os.name == 'nt': self.skipTest('POSIX profile assertion')
        result = self.run_shell('[[ $_comps[npm] != _stormtty_npm_completion && ${+functions[_stormtty_npm_completion]} == 0 ]]')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_npm_completion_inactive_managed_profile_untouched(self):
        self.env.pop('STORMTTY_SESSION_ID', None)
        code = 'OSTYPE=msys; source "$ZDOTDIR/.zshrc"; [[ ${+functions[_stormtty_npm_completion]} == 0 ]]'
        result = subprocess.run([self.shell, '-dfic', code], env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipIf(os.name == 'nt', 'synthetic native-state contract, not ACL qualification')
    def test_npm_completion_user_config_can_override(self):
        state = self.root / 'native-state'; state.mkdir(mode=0o700)
        (state / 'config.zsh').write_text('compdef _user_npm_completion npm\n')
        self.env['STORMTTY_NATIVE_ZSH_STATE'] = '/' + str(state)
        code = 'OSTYPE=msys; source "$ZDOTDIR/.zshrc"; [[ $_comps[npm] == _user_npm_completion ]]'
        result = subprocess.run([self.shell, '-dfic', code], env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_npm_completion_installed_npm_protocol(self):
        if not shutil.which('npm'): self.skipTest('npm unavailable')
        names = ['build', 'dev', 'test', 'build:watch', 'two words', '路径', '$(touch SHOULD_NOT_EXIST)']
        (self.root / 'package.json').write_text(json.dumps({'name': 'completion-fixture', 'version': '1.0.0',
            'scripts': {name: 'touch SHOULD_NOT_EXECUTE' for name in names}}))
        result = self.npm_completion()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(set(result.stdout.split('\0')[:-1]), set(names))
        self.assertFalse((self.root / 'SHOULD_NOT_EXECUTE').exists())
        self.assertFalse((self.root / 'SHOULD_NOT_EXIST').exists())
        # Options and aliases remain npm-owned, rather than a hand-maintained list.
        result = self.npm_completion(query='npm run-script bu', words='npm run-script bu')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(set(result.stdout.split('\0')[:-1]), set(names))

    @unittest.skipIf(os.name == 'nt', 'POSIX state fixture; native lease separately qualified')
    def test_no_dump_keeps_private_config_history_and_custom_completions(self):
        if not args.assets_cache: self.skipTest('requires pinned modules')
        first=self.run_shell('[[ $STORMTTY_EPHEMERAL == 0 ]]')
        self.assertEqual(first.returncode,0,first.stderr)
        state=self.home/'.stormtty/zsh'
        dump=next(p for p in state.glob('zcompdump-*') if p.is_file() and not p.name.endswith('.zwc'))
        sentinel=self.root/'cache-was-executed'
        dump.write_text(dump.read_text()+f"\nprint BAD > '{sentinel}'\n")
        (state/'config.zsh').write_text("HISTSIZE=4321\nSTORMTTY_USER_OVERRIDE=yes\nfpath+=(/authored/user/override)\n")
        (state/'history').write_text('authored history fixture\n')
        custom=state/'cache/completions/_stormtty_fixture'
        custom.write_text('#compdef stormtty_fixture\nreturn 0\n')
        # Both text and a fresh compiled sibling are present and must be ignored.
        result=subprocess.run([self.shell,'-fc','zcompile "$1"','fixture',str(dump)],capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)
        before={p.name:p.read_bytes() for p in (dump,Path(str(dump)+'.zwc'),state/'config.zsh',state/'history')}
        self.env['STORMTTY_NATIVE_ZSH_NO_DUMP']='1'
        result=self.run_shell('[[ $STORMTTY_EPHEMERAL == 0 && $STORMTTY_USER_OVERRIDE == yes && $HISTSIZE == 4321 && $fpath[-1] == /authored/user/override && ${+_comps[stormtty_fixture]} == 1 && -n $HISTFILE && -z ${STORMTTY_NATIVE_ZSH_NO_DUMP+x} ]]')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertFalse(sentinel.exists())
        for name,data in before.items():self.assertEqual((state/name).read_bytes(),data)

    def run_shell(self, command, **kwargs):
        return subprocess.run([self.shell, '-dic', command], env=self.env, cwd=self.root, capture_output=True, text=True, timeout=15, **kwargs)
    @unittest.skipIf(os.name == 'nt', 'POSIX helper stub; actual sidecar tested in native ConPTY')
    def test_console_bridge_consumed_and_failure_is_not_success(self):
        helper = self.root / "helper O'Connor 路径"
        helper.write_text('#!/bin/sh\nexit 0\n'); helper.chmod(0o700)
        self.env['STORMTTY_NATIVE_CONSOLE_SETUP'] = str(helper)
        code = 'OSTYPE=msys; source "$ZDOTDIR/.zshrc"; [[ $STORMTTY_CONSOLE_UTF8 == 1 && ${+STORMTTY_NATIVE_CONSOLE_SETUP} == 0 && ${+_stormtty_console_setup} == 0 ]]'
        result = subprocess.run([self.shell, '-dfic', code], env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        helper.write_text('#!/bin/sh\nexit 3\n')
        result = subprocess.run([self.shell, '-dfic', code.replace('== 1', '== 0')], env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('native Unicode input setup failed', result.stderr)

    @unittest.skipIf(os.name == 'nt', 'POSIX helper stub; native launch policy tested separately')
    def test_non_windows_console_bridge_is_not_executed(self):
        helper = self.root / 'helper'
        marker = self.root / 'executed'
        helper.write_text('#!/bin/sh\ntouch "' + str(marker) + '"\n'); helper.chmod(0o700)
        self.env['STORMTTY_NATIVE_CONSOLE_SETUP'] = str(helper)
        result = self.run_shell('[[ $STORMTTY_CONSOLE_UTF8 == 0 && ${+STORMTTY_NATIVE_CONSOLE_SETUP} == 0 ]]')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(marker.exists())

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
    def test_native_history_suggestions_do_not_fork_for_unmatched_text(self):
        if not args.assets_cache: self.skipTest('pass --assets-cache for pinned source integration')
        # Execute the real pinned history strategy. Replacing only its async
        # entry point makes any fork-path regression fail, including no match.
        command = '''OSTYPE=msys; source "$ZDOTDIR/.zshrc"
          [[ ${+ZSH_AUTOSUGGEST_USE_ASYNC} == 0 ]] || exit 20
          _zsh_autosuggest_async_request() { return 91; }
          print -s -- 'echo typing-history-fixture'
          BUFFER='echo typing-history'; POSTDISPLAY=''
          _zsh_autosuggest_fetch || exit 21
          [[ $POSTDISPLAY == '-fixture' ]] || exit 22
          BUFFER='git commit -m "unmatched typing fixture'; POSTDISPLAY='old'
          _zsh_autosuggest_fetch || exit 23
          [[ -z $POSTDISPLAY ]] || exit 24
          exit 0
        '''
        # Feed separate interactive lines: zsh -c does not populate $history.
        result=subprocess.run([self.shell,'-dfi'],input=command,env=self.env,cwd=self.root,capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)
    @unittest.skipIf(os.name == 'nt', 'POSIX upstream policy')
    def test_posix_keeps_upstream_async_suggestions(self):
        if not args.assets_cache: self.skipTest('pass --assets-cache for pinned source integration')
        result=self.run_shell('[[ ${+ZSH_AUTOSUGGEST_USE_ASYNC} == 1 ]]')
        self.assertEqual(result.returncode,0,result.stderr)
    def test_native_custom_strategy_and_explicit_async_optin(self):
        if not args.assets_cache: self.skipTest('pass --assets-cache for pinned source integration')
        for setup, check in [
            ('ZSH_AUTOSUGGEST_STRATEGY=(completion)', '[[ $ZSH_AUTOSUGGEST_STRATEGY == completion ]]'),
            ('ZSH_AUTOSUGGEST_USE_ASYNC=1', '[[ $ZSH_AUTOSUGGEST_STRATEGY == history ]]')]:
            command='OSTYPE=msys; '+setup+'; source "$ZDOTDIR/.zshrc"; '+check+' && [[ ${+ZSH_AUTOSUGGEST_USE_ASYNC} == 1 ]]'
            result=subprocess.run([self.shell,'-dfic',command],env=self.env,cwd=self.root,capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stderr)
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
    @unittest.skipIf(os.name == 'nt', 'POSIX process-spy fixture, not native performance evidence')
    def test_native_warm_state_does_not_spawn_mkdir(self):
        state=self.root/'native-state'
        (state/'cache/completions').mkdir(parents=True, mode=0o700)
        spy=self.root/'bin'; spy.mkdir()
        (spy/'mkdir').write_text('#!/bin/sh\nexit 97\n')
        (spy/'mkdir').chmod(0o700)
        self.env.update(STORMTTY_NATIVE_ZSH_STATE='/'+str(state), PATH=str(spy)+os.pathsep+self.env['PATH'])
        code='OSTYPE=msys; source "$ZDOTDIR/.zshrc"; [[ $_stormtty_native_validated == 1 && $_stormtty_state == $STORMTTY_EXPECT_STATE ]]'
        self.env['STORMTTY_EXPECT_STATE']='/'+str(state)
        result=subprocess.run([self.shell,'-dfic',code],env=self.env,cwd=self.root,capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)

    @unittest.skipIf(os.name == 'nt', 'POSIX process-spy fixture, not native performance evidence')
    def test_cold_cache_metadata_without_tee_or_absent_dump_remove(self):
        if not args.assets_cache: self.skipTest('pass --assets-cache for pinned source integration')
        spy=self.root/'bin'; spy.mkdir()
        marker=self.root/'unexpected-process'
        for name in ['tee','rm']:
            real=shutil.which(name)
            # rm for the compiler lock remains legitimate; missing-dump removal does not.
            test='true' if name == 'tee' else '[ "$1" = "-f" ]'
            (spy/name).write_text('#!/bin/sh\nif '+test+'; then echo invoked >> "'+str(marker)+'"; exit 97; fi\nexec "'+real+'" "$@"\n')
            (spy/name).chmod(0o700)
        self.env['PATH']=str(spy)+os.pathsep+self.env['PATH']
        result=self.run_shell('[[ $STORMTTY_OMZ_LOADED == 1 && ${+_comps[ls]} == 1 ]]')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertFalse(marker.exists())
        dump=next(p for p in (self.home/'.stormtty/zsh').glob('zcompdump-*') if p.suffix != '.zwc')
        self.assertIn('#omz revision:',dump.read_text())
        self.assertIn('#omz fpath:',dump.read_text())
        timestamp=dump.stat().st_mtime_ns
        result=self.run_shell('[[ ${+_comps[ls]} == 1 ]]')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(dump.stat().st_mtime_ns,timestamp)
        self.assertFalse(marker.exists())

    def test_stale_metadata_still_rebuilds_completion_cache(self):
        if not args.assets_cache: self.skipTest('pass --assets-cache for pinned source integration')
        result=self.run_shell('[[ ${+_comps[ls]} == 1 ]]')
        self.assertEqual(result.returncode,0,result.stderr)
        dump=next(p for p in (self.home/'.stormtty/zsh').glob('zcompdump-*') if p.suffix != '.zwc')
        dump.write_text(dump.read_text().replace('#omz revision:', '#stale revision:'))
        result=self.run_shell('[[ ${+_comps[ls]} == 1 && $STORMTTY_OMZ_LOADED == 1 ]]')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('#omz revision:',dump.read_text())
        self.assertNotIn('#stale revision:',dump.read_text())

    def test_custom_prompt_is_not_overwritten(self):
        result=self.run_shell('STORMTTY_PROMPT=custom; PROMPT="custom> "; false; _stormtty_precmd; [[ $? == 0 && $PROMPT == "custom> " && ${STORMTTY_CONTEXT[exit]} == 1 ]]')
        self.assertEqual(result.returncode,0,result.stderr)
    @unittest.skipIf(os.name == 'nt', 'POSIX PTY test; actual Windows ConPTY editing requires native gate')
    def test_interactive_native_history_policy_keeps_suggestion_editing(self):
        if not args.assets_cache: self.skipTest('pass --assets-cache for pinned source integration')
        # Select the MSYS module policy only, after POSIX state setup. This
        # exercises actual ZLE widgets, not Windows console/fork behavior.
        state=self.home/'.stormtty/zsh'; state.mkdir(parents=True,mode=0o700)
        state.parent.chmod(0o700)
        (state/'config.zsh').write_text('OSTYPE=msys\n')
        self.test_interactive_failure_keeps_later_hooks_and_suggestion_editing()
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
    @unittest.skipIf(os.name == 'nt', 'POSIX PTY test; Windows host smoke qualifies its native paste path')
    def test_bulk_fixture_paste_preserves_real_modules_and_exit(self):
        if not args.assets_cache: self.skipTest('pass --assets-cache for pinned source integration')
        try: import pexpect
        except ImportError: self.skipTest('optional pexpect unavailable')
        child=pexpect.spawn(self.shell,['-di'],env=self.env,cwd=str(self.root),encoding='utf-8',timeout=10)
        self.addCleanup(child.close,force=True)
        child.expect_exact('\x1b[?2004h')
        # First bytes/prompt metadata can precede ZLE. Start only after its
        # negotiated paste mode, with real highlighting/autosuggestions loaded.
        command='[[ $STORMTTY_OMZ_LOADED == 1 && ${+functions[_zsh_highlight]} == 1 && ${+functions[_zsh_autosuggest_start]} == 1 ]] && print -r -- BULK_FIXTURE_OK'
        child.send('\x1b[200~'+command+'\x1b[201~')
        child.sendline('')
        child.expect_exact('BULK_FIXTURE_OK\r\n')
        child.expect_exact('\x1b[?2004h')
        child.send('\x1b[200~exit 0\x1b[201~')
        child.sendline('')
        child.expect(pexpect.EOF)
        child.close()
        self.assertEqual(child.exitstatus,0)
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
    def context_packets(self, output):
        return re.findall(r'\x1b\]7400;stormtty-context;1;([^\x07]*)\x07', output)
    def decoded_context(self, packet):
        fields=packet.split(';')
        self.assertEqual(len(fields),6)
        self.assertEqual(fields[0],'set')
        return [unquote(value) for value in fields[1:]]
    def test_native_context_quoted_unicode_paths_and_order(self):
        path=self.home/'folder é 路径 % ; $(touch PWNED) `noop`'
        path.mkdir()
        (path/'.git').mkdir()
        head='feature/é%;$(touch PWNED)'
        (path/'.git/HEAD').write_text('ref: refs/heads/'+head+'\n')
        self.env['QUAL_PATH']=str(path)
        result=self.run_shell('cd -- "$QUAL_PATH"; PATH=/no-external-commands; STORMTTY_PROMPT=custom; _stormtty_precmd')
        self.assertEqual(result.returncode,0,result.stderr)
        packets=self.context_packets(result.stdout)
        self.assertEqual(len(packets),1)
        key,display,root,kind,branch=self.decoded_context(packets[0])
        self.assertIn('\x1b]7;'+key+'\x07\x1b]7400;',result.stdout)
        self.assertEqual(unquote(key.removeprefix('file://')),str(path))
        self.assertEqual(display,'~/'+path.name)
        self.assertEqual(root,display)
        self.assertEqual(kind,'branch')
        self.assertEqual(branch,head)
        self.assertFalse((path/'PWNED').exists())
        self.assertTrue(packets[0].isascii())
    def test_context_leaving_repo_clears_all_git_fields(self):
        repo=self.root/'repository';repo.mkdir();(repo/'.git').mkdir()
        (repo/'.git/HEAD').write_text('ref: refs/heads/main\n')
        outside=self.root/'outside';outside.mkdir()
        self.env.update(QUAL_REPO=str(repo),QUAL_OUTSIDE=str(outside))
        result=self.run_shell('cd -- "$QUAL_REPO"; _stormtty_precmd; cd -- "$QUAL_OUTSIDE"; _stormtty_precmd')
        self.assertEqual(result.returncode,0,result.stderr)
        first,last=map(self.decoded_context,self.context_packets(result.stdout))
        self.assertEqual(first[-2:],['branch','main'])
        self.assertEqual(last[2:],['','none',''])
        self.assertNotEqual(first[0],last[0])
    def test_context_indirection_and_detached(self):
        metadata=self.root/'metadata';metadata.mkdir()
        (metadata/'HEAD').write_text('ABCDEF0123456789'+'a'*24+'\n')
        (self.root/'.git').write_text('gitdir: metadata\n')
        result=self.run_shell('_stormtty_precmd')
        self.assertEqual(result.returncode,0,result.stderr)
        fields=self.decoded_context(self.context_packets(result.stdout)[0])
        self.assertEqual(fields[2:], [str(self.root),'detached','ABCDEF01'])
    def test_context_gitdir_absolute_path_classification(self):
        # Run the real resolver with a bounded-read stub so Windows paths can
        # be classified on POSIX without pretending they are native I/O proof.
        (self.root/'.git').write_text('fixture\n')
        for platform, path, absolute in [
                ('msys', 'C:/metadata é % ; $(touch PWNED)', True),
                ('cygwin', 'd:/metadata', True),
                ('msys', '//server/share/metadata', True),
                ('msys', '/absolute/metadata', True),
                ('msys', 'C:relative', False),
                ('linux-gnu', 'C:/metadata', False),
                ('msys', '../metadata', False)]:
            with self.subTest(platform=platform,path=path):
                self.env.update(QUAL_PLATFORM=platform,QUAL_GITDIR=path)
                result=self.run_shell('''OSTYPE=$QUAL_PLATFORM
_stormtty_read_small() {
  if [[ $1 == */.git ]]; then REPLY="gitdir: $QUAL_GITDIR"
  else print -r -- "READ_PATH=$1"; REPLY='ref: refs/heads/main'; fi
}
PATH=/no-external-commands
_stormtty_git_context
''')
                self.assertEqual(result.returncode,0,result.stderr)
                expected=path if absolute else str(self.root)+'/'+path
                self.assertIn('READ_PATH='+expected+'/HEAD\n',result.stdout)
                self.assertFalse((self.root/'PWNED').exists())

    def test_context_optout_emits_clear_for_custom_prompt(self):
        result=self.run_shell('STORMTTY_SHELL_CONTEXT=0; STORMTTY_PROMPT=custom; PROMPT="custom> "; false; _stormtty_precmd; [[ $PROMPT == "custom> " && ${STORMTTY_CONTEXT[exit]} == 1 ]]')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(self.context_packets(result.stdout),['clear'])
        self.assertNotIn('\x1b]7;',result.stdout)
    def test_context_controls_and_oversized_fields_clear(self):
        commands=[
            "PWD=$'/path\\e]2;injected\\a'; _stormtty_emit_context",
            "STORMTTY_CONTEXT[head]=${(l:257::a:)}; _stormtty_emit_context",
            "PWD=/${(l:2050::a:)}; _stormtty_emit_context",
            "PWD=/${(l:690::é:)}; _stormtty_emit_context",
            "PWD=relative; _stormtty_emit_context",
            "PWD=/${(l:500::%:)}; STORMTTY_CONTEXT[repository_root]=${(l:2000::%:)}; STORMTTY_CONTEXT[kind]=branch; STORMTTY_CONTEXT[head]=main; _stormtty_emit_context",
        ]
        for command in commands:
            with self.subTest(command=command):
                result=self.run_shell('_stormtty_git_context; '+command)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual(self.context_packets(result.stdout),['clear'])
                self.assertNotIn('\x1b]2;',result.stdout)
                if 'head]=' not in command and 'repository_root]=' not in command:
                    self.assertNotIn('\x1b]7;',result.stdout)
                else:
                    self.assertIn('\x1b]7;',result.stdout)
    def test_context_drive_home_and_unc_display(self):
        for value,home,expected in [('/c/Users/Storm/work','/c/Users/Storm','~/work'),
                                   ('/d/projects','/c/Users/Storm','D:/projects'),
                                   ('/c','/c/Users/Storm','C:/'),
                                   ('//server/share/project','/c/Users/Storm','//server/share/project'),
                                   ('/home/a','/home/a','~'),
                                   ('/home/another','/home/a','/home/another')]:
            self.env.update(QUAL_VALUE=value,QUAL_HOME=home)
            result=self.run_shell('OSTYPE=msys; HOME=$QUAL_HOME; _stormtty_display_directory "$QUAL_VALUE"; print -r -- "$REPLY"')
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(result.stdout.strip(),expected)
    def test_context_encoder_preserves_multibyte_option(self):
        result=self.run_shell('setopt MULTIBYTE; _stormtty_encode_context "é% ;" 256; [[ -o MULTIBYTE && $REPLY == "%C3%A9%25%20%3B" ]]')
        self.assertEqual(result.returncode,0,result.stderr)

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
