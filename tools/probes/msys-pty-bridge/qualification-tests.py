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
