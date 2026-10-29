#!/usr/bin/env python3
"""Portable staging and authorization-boundary guards, not runtime evidence."""
import importlib.util
import json
import re
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('bridge_qualification', HERE / 'qualify-windows.py')
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)

class Staging(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=HERE)
        self.root = Path(self.tmp.name); self.base = self.root / 'base'; self.build = self.root / 'build'; self.dest = self.root / 'dest'
        (self.base / 'usr/bin').mkdir(parents=True); self.build.mkdir()
        (self.base / 'usr/bin/msys-2.0.dll').write_bytes(b'MZsynthetic-runtime')
        self.record = {'path': 'usr/bin/msys-2.0.dll', 'directory': False, 'sha256': module.sha(self.base / 'usr/bin/msys-2.0.dll')}
        self.manifest()
        for name in ('broker', 'fixture'): (self.build / (name + '.exe')).write_bytes(b'MZsynthetic-' + name.encode())
    def tearDown(self): self.tmp.cleanup()
    def manifest(self): (self.base / 'stormtty-shell-manifest.json').write_text(json.dumps({'files': [self.record]}))
    def test_private_stage_keeps_original(self):
        identity = module.stage(self.base, self.build, self.dest)
        self.assertEqual(identity['runtime_sha256'], module.sha(self.base / 'usr/bin/msys-2.0.dll'))
        self.assertFalse(identity['default_activation_allowed'])
        self.assertFalse(identity['native_child_interop_verified'])
        self.assertFalse((self.base / 'usr/bin/stormtty-bridge-broker.exe').exists())
    def test_refuse_existing(self):
        self.dest.mkdir()
        with self.assertRaises(ValueError): module.stage(self.base, self.build, self.dest)
    def test_refuse_nested(self):
        with self.assertRaises(ValueError): module.stage(self.base, self.build, self.base / 'nested')
    def test_refuse_runtime_tamper(self):
        (self.base / 'usr/bin/msys-2.0.dll').write_bytes(b'tampered')
        with self.assertRaises(ValueError): module.stage(self.base, self.build, self.dest)
    def test_refuse_traversal(self):
        self.record['path'] = '../escape'; self.manifest()
        with self.assertRaises(ValueError): module.stage(self.base, self.build, self.dest)
    def test_refuse_non_executable(self):
        (self.build / 'broker.exe').write_bytes(b'not-PE')
        with self.assertRaises(ValueError): module.stage(self.base, self.build, self.dest)

class Stage2Outcome(unittest.TestCase):
    def test_no_partial_success(self):
        spec = importlib.util.spec_from_file_location('stage2_result', HERE / 'qualify-stage2.py')
        stage2 = importlib.util.module_from_spec(spec); spec.loader.exec_module(stage2)
        good = dict(exit_code=0, reaped=True, timed_out=False, output_truncated=False,
                    cleanup_error='', output_tail=b'BRIDGE_STAGE2 result=PASS\n')
        self.assertTrue(stage2.passed(SimpleNamespace(**good)))
        for key, value in [('exit_code', 1), ('reaped', False), ('timed_out', True),
                           ('output_truncated', True), ('cleanup_error', 'reap failed'),
                           ('output_tail', b'no completion marker')]:
            with self.subTest(key=key):
                self.assertFalse(stage2.passed(SimpleNamespace(**(good | {key: value}))))

class Stage3Outcome(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('stage3_result', HERE / 'qualify-stage3.py')
        cls.module = importlib.util.module_from_spec(spec); spec.loader.exec_module(cls.module)
    def records(self):
        return [dict(sample=n, fresh=int(n==1), phases=10, typed_echo_samples=38,
                     startup_cursor_reports=2, native_cursor_reports=1, warm_dump_unchanged=int(n==2)) for n in (1,2)]
    def test_fresh_warm_gate(self):
        good=dict(exit_code=0,reaped=True,timed_out=False,output_truncated=False,cleanup_error='',
                  output_tail=b'BRIDGE_MANAGED_RESULT fresh=1 warm=1 result=PASS')
        self.assertTrue(self.module.passed(SimpleNamespace(**good),self.records()))
        for key,value in [('exit_code',1),('reaped',False),('timed_out',True),('output_truncated',True),('cleanup_error','failed')]:
            with self.subTest(key=key): self.assertFalse(self.module.passed(SimpleNamespace(**(good|{key:value})),self.records()))
        for key,value in [('warm_dump_unchanged',0),('startup_cursor_reports',0),('native_cursor_reports',0),('typed_echo_samples',0),('phases',9)]:
            records=self.records();records[1][key]=value
            with self.subTest(key=key): self.assertFalse(self.module.passed(SimpleNamespace(**good),records))
        self.assertFalse(self.module.passed(SimpleNamespace(**good),self.records()[:1]))
    def test_timing_parser(self):
        line=b'BRIDGE_MANAGED_SAMPLE sample=1 fresh=1 phases=10 state_prepare_ms=1 launch_to_zle_observation_ms=2 typed_echo_samples=38 typed_echo_median_ms=3 typed_echo_p95_ms=4 typed_echo_max_ms=5 startup_cursor_reports=2 native_cursor_reports=1 warm_dump_unchanged=0 result=PASS'
        self.assertEqual(self.module.samples(line)[0]['typed_echo_max_ms'],5)
        with self.assertRaises(ValueError): self.module.samples(line.replace(b'state_prepare_ms=1',b'state_prepare_ms=nan'))
    def test_startup_budget_separate_from_marker_and_global_bounds(self):
        source=(HERE/'stage3-host.cpp').read_text()
        shared=(HERE/'stage2-host.cpp').read_text()
        self.assertIn('ready(b,1,0,20s)',source)
        self.assertIn('std::chrono::milliseconds budget=4s',source)
        self.assertIn('std::chrono::milliseconds budget=4s',shared)
        self.assertIn('now()-started<25s',shared)
        self.assertIn('BRIDGE_MANAGED_OBSERVATION',source)
        self.assertIn('ready_status=',source)
        self.assertIn('observe_failure(b,sample,phase)',source)
        self.assertNotIn('FIXTURE_SCREEN=',source)
    def test_shipped_prompt_and_helpers_preserved(self):
        config=(HERE/'stage3-config.zsh').read_text()
        self.assertIn('add-zle-hook-widget line-init',config)
        self.assertIn('zle -M',config)
        self.assertNotRegex(config,r'(?m)^\s*(?:PROMPT|RPROMPT|HISTFILE)=')
        self.assertNotIn('zle reset-prompt',config)
        self.assertNotRegex(config,r'(?m)^\s*(?:unsetopt|setopt) ')
        source=(HERE/'stage3-host.cpp').read_text()
        self.assertIn('prepare_managed_shell_state_at',source)
        self.assertIn('managed_shell_console_environment()',source)
        self.assertIn('fs::last_write_time(dump)==prior_time',source)
        self.assertIn('cursor.column==old_cursor.column+1',source)
        self.assertIn('b.output_bytes>old_bytes',source)
        self.assertIn('scratch.cleanup()',source)

class Workflow(unittest.TestCase):
    def test_push_only_isolation(self):
        workflow = (HERE.parents[2] / '.github/workflows/msys-pty-bridge.yml').read_text()
        self.assertIn('branches: [prototype/msys-pty-bridge]', workflow)
        self.assertIn("github.ref == 'refs/heads/prototype/msys-pty-bridge'", workflow)
        for forbidden in ('workflow_dispatch:', 'pull_request:', 'pull_request_target:', 'continue-on-error:', 'native.yml', 'codex '):
            self.assertNotIn(forbidden, workflow)
        self.assertIn('contents: read', workflow)
        self.assertIn('if: always()', workflow)
        self.assertIn('timeout-minutes: 60', workflow)
        self.assertIn('toolchain.sh', workflow)
        self.assertIn('private-runtime', workflow)
    def test_declared_helper_tool_closure(self):
        workflow = (HERE.parents[2] / '.github/workflows/msys-pty-bridge.yml').read_text()
        packages = set(re.search(r'^          install: (.+)$', workflow, re.MULTILINE).group(1).split())
        # External helper commands mapped to official MSYS package providers.
        closure = {'gcc': (), 'diffutils': ('cmp',), 'coreutils': ('realpath', 'dirname', 'mkdir', 'cp', 'chmod', 'sha256sum', 'uname', 'tee'),
                   'gawk': ('awk',), 'grep': ('grep',), 'sed': ('sed',), 'tar': ('tar',),
                   'zstd': ('zstd',), 'curl': ('curl',), 'gnupg': ('gpg',), 'binutils': ('as', 'ld')}
        self.assertTrue(set(closure) <= packages, 'Required official helper packages missing')
        commands = set(re.search(r'for tool in (.+); do', workflow).group(1).split())
        expected = {tool for tools in closure.values() for tool in tools}
        expected.update(('bash', 'pacman', 'pacman-conf', 'cygpath', 'ldd'))
        self.assertEqual(commands, expected)
        self.assertLess(workflow.index('for tool in '), workflow.index('bash tools/probes/msys-runtime-experiment/toolchain.sh'))
        self.assertIn('command -v "$tool"', workflow)
        self.assertIn('exit 127;', workflow)
    def test_stage2_real_vt_and_private_scope(self):
        source = (HERE / 'stage2-host.cpp').read_text()
        self.assertIn('terminal.feed(', source)
        self.assertIn('terminal.take_effects()', source)
        self.assertIn('send(std::move(effect.text))', source)
        self.assertIn('native_responses>0', source)
        self.assertIn('JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE', source)
        self.assertNotIn('CreatePseudoConsole', source)
        self.assertNotIn('\\x1b[1;1R', source)
        runner = (HERE / 'qualify-stage2.py').read_text()
        self.assertIn("HOME='/bridge-home',ZDOTDIR='/bridge-home'", runner)
        self.assertIn('timeout=40,reap_timeout=5', runner)
        self.assertNotIn('shell=True', runner)
        cmake = (HERE / 'CMakeLists.txt').read_text()
        self.assertIn('src/terminal/terminal.cpp', cmake)
        self.assertNotIn('src/agent', cmake)
    def test_fixed_modes_and_owned_timeout(self):
        self.assertEqual(module.MODES, ('bytes', 'raw', 'resize', 'signal', 'eof', 'flood'))
        source = (HERE / 'qualify-windows.py').read_text()
        self.assertIn('timeout=20, reap_timeout=5', source)
        self.assertIn('for mode in MODES:', source)
        self.assertNotIn('shell=True', source)

if __name__ == '__main__': unittest.main(verbosity=2)
