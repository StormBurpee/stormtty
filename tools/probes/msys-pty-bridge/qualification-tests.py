#!/usr/bin/env python3
"""Portable staging and authorization-boundary guards, not runtime evidence."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

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

class Workflow(unittest.TestCase):
    def test_push_only_isolation(self):
        workflow = (HERE.parents[2] / '.github/workflows/msys-pty-bridge.yml').read_text()
        self.assertIn('branches: [prototype/msys-pty-bridge]', workflow)
        self.assertIn("github.ref == 'refs/heads/prototype/msys-pty-bridge'", workflow)
        for forbidden in ('workflow_dispatch:', 'pull_request:', 'pull_request_target:', 'continue-on-error:', 'native.yml', 'cmake ', 'zsh.exe', 'codex '):
            self.assertNotIn(forbidden, workflow)
        self.assertIn('contents: read', workflow)
        self.assertIn('if: always()', workflow)
        self.assertIn('timeout-minutes: 60', workflow)
        self.assertIn('toolchain.sh', workflow)
        self.assertIn('private-runtime', workflow)
    def test_fixed_modes_and_owned_timeout(self):
        self.assertEqual(module.MODES, ('bytes', 'raw', 'resize', 'signal', 'eof', 'flood'))
        source = (HERE / 'qualify-windows.py').read_text()
        self.assertIn('timeout=20, reap_timeout=5', source)
        self.assertIn('for mode in MODES:', source)
        self.assertNotIn('shell=True', source)

if __name__ == '__main__': unittest.main(verbosity=2)
