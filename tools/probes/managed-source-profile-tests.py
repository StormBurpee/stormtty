#!/usr/bin/env python3
"""Portable safety/parser checks, not native Windows timing evidence."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('source_profile_tests_runner',HERE/'run-managed-source-profile.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
p=m.prepare

class ProfileTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.original=self.root/'original';self.dest=self.root/'profile'
        for relative,src in [('stormtty/zsh/.zshrc','shell-preset.zsh'),('stormtty/vendor/addons.lock.json','shell-addons.lock.json'),('stormtty/vendor/shell-tools/tools.lock.json','shell-tools.lock.json')]:
            f=self.original/relative;f.parent.mkdir(parents=True,exist_ok=True);f.write_bytes((p.TOOLS/src).read_bytes())
        self.manifest()

    def manifest(self):
        entries=[dict(path=f.relative_to(self.original).as_posix(),directory=False,sha256=p.sha(f),bytes=f.stat().st_size) for f in self.original.rglob('*') if f.is_file() and f.name!='stormtty-shell-manifest.json']
        (self.original/'stormtty-shell-manifest.json').write_text(json.dumps(dict(files=entries)))

    def test_fixture_only_loader_changes(self):
        before={f.relative_to(self.original):f.read_bytes() for f in self.original.rglob('*') if f.is_file()}
        detail=p.prepare(self.original,self.dest)
        for rel,data in before.items():self.assertEqual((self.original/rel).read_bytes(),data)
        for rel,data in before.items():
            if str(rel).replace('\\','/') not in ('stormtty/zsh/.zshrc','stormtty-shell-manifest.json'):self.assertEqual((self.dest/rel).read_bytes(),data)
        source=(self.dest/'stormtty/zsh/.zshrc').read_text()
        self.assertEqual(source.count('zmodload zsh/zprof'),1)
        self.assertIn('compinit -i -D',source);self.assertIn('ZSH_DISABLE_COMPFIX=false',source)
        self.assertEqual(detail['stages'],list(p.STAGES))
        self.assertFalse(json.loads((self.dest/'stormtty-shell-manifest.json').read_text())['default_activation_allowed'])

    def test_no_overwrite_or_inside_original(self):
        self.dest.mkdir()
        for target in (self.dest,self.original/'child'):
            with self.assertRaises(ValueError):p.prepare(self.original,target)

    def test_corruption_and_extra_file_rejected(self):
        loader=self.original/'stormtty/zsh/.zshrc';loader.write_text('changed')
        with self.assertRaises(ValueError):p.prepare(self.original,self.dest)
        loader.write_bytes((p.TOOLS/'shell-preset.zsh').read_bytes())
        extra=self.original/'stormtty/zsh/.zshrc.zwc';extra.write_bytes(b'compiled')
        with self.assertRaises(ValueError):p.prepare(self.original,self.dest)
        self.assertFalse(self.dest.exists())

    def test_marker_drift_fails_closed(self):
        with self.assertRaises(ValueError):p.instrument('empty')

    def complete(self):
        rows=[]
        for i in range(1,7):
            rows += [f'SOURCE_STAGE sample={i} name={name} elapsed_ms=1.23' for name in p.STAGES]
            rows += [f'SOURCE_FUNCTION sample={i} name={name} calls=1 total_ms=1.0 self_ms=0.5' for name in ('compinit','compaudit')]
            rows += [f'STARTUP_SAMPLE sample={i} fresh_state={int(i<=3)} state_prepare_ms=1 launch_to_zle_ms=2']
        rows += ['STARTUP_RESULT code=0 fresh_samples=3 warm_samples=3']
        return '\n'.join(rows).encode()

    def test_sanitizer_requires_all_stages_and_audit(self):
        output=self.complete();self.assertTrue(m.sanitized(output)[1])
        for corrupt in (output.replace(b'name=compaudit',b'name=private/path'),output.replace(b'name=omz',b'name=missing'),output+ b'\nSOURCE_FUNCTION sample=1 name=compinit calls=1 total_ms=1 self_ms=1'):
            self.assertFalse(m.sanitized(corrupt)[1])

    def test_no_paths_or_raw_zprof_published(self):
        output=self.complete()+b'\nsecret/path/history\n1) 1 123 123 10% 2 2 1% /private/path\nSOURCE_STAGE sample=1 name=omz elapsed_ms=secret\n'
        lines,ok=m.sanitized(output);self.assertTrue(ok)
        self.assertNotIn('secret','\n'.join(lines));self.assertNotIn('/private','\n'.join(lines))

if __name__=='__main__':unittest.main()
