#!/usr/bin/env python3
"""POSIX semantic experiments with supplied upstream functions; no startup claims.

Usage: python3 completion-seed-semantics-tests.py --functions-root DIR
DIR must contain compinit, compaudit, compdump, compinstall from the staged bundle.
No production loader, runtime inventory, or existing user cache is modified.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--functions-root', type=Path, required=True)
args, remaining = parser.parse_known_args()
ROOT = args.functions_root.resolve()

@unittest.skipUnless(os.name != 'nt' and shutil.which('zsh'), 'POSIX semantic fixture only')
class SeedSemantics(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='completion-seed-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.support = self.root / 'support'
        self.support.mkdir()
        for name in ('compinit', 'compaudit', 'compdump', 'compinstall'):
            shutil.copyfile(ROOT / name, self.support / name)
        self.first = self.root / 'first'
        self.first.mkdir()
        self.completion(self.first, '_seed', 'seed-command')
        self.dump = self.root / 'dump'

    def completion(self, directory, name, command):
        (directory / name).write_text(f'#compdef {command}\nreturn 0\n')

    def run_init(self, directories, extra=''):
        # Pass paths as argv, not interpolated executable shell text.
        script = '''
            dump=$1; shift
            fpath=( "$@" )
            zmodload zsh/zprof
            autoload -Uz compinit
            compinit -i -d "$dump"
            print -r -- "MAP=${_comps[seed-command]-missing}"
            print -r -- "OTHER=${_comps[other-command]-missing}"
            print -r -- "SECURE=${_comp_secure-no}"
            zprof
        ''' + extra
        result = subprocess.run(['zsh', '-f', '-i', '-c', script, 'fixture', str(self.dump),
                                 str(self.support), *map(str, directories)],
                                capture_output=True, text=True, timeout=20,
                                env={**os.environ, 'HOME': str(self.root), 'ZDOTDIR': str(self.root)})
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_relocation_and_secure_fast_path(self):
        self.assertIn('MAP=_seed', self.run_init([self.first]))
        original = self.dump.read_bytes()
        self.assertNotIn(str(self.root).encode(), original)
        relocated = self.root / 'relocated'
        self.first.rename(relocated)
        output = self.run_init([relocated])
        self.assertIn('MAP=_seed', output)
        self.assertIn('compaudit', output)
        self.assertNotIn('compdef', output)
        self.assertEqual(self.dump.read_bytes(), original)

    def test_same_count_content_change_is_not_detected_by_compinit(self):
        self.run_init([self.first])
        self.completion(self.first, '_seed', 'other-command')
        output = self.run_init([self.first])
        self.assertIn('MAP=_seed', output)  # stale, proves fingerprint is required
        self.assertIn('OTHER=missing', output)
        self.dump.unlink()
        self.assertIn('OTHER=_seed', self.run_init([self.first]))

    def test_same_count_fpath_precedence_change_is_not_detected(self):
        second = self.root / 'second'
        second.mkdir()
        self.completion(second, '_seed', 'other-command')
        self.run_init([self.first, second])
        output = self.run_init([second, self.first])
        self.assertIn('MAP=_seed', output)  # stale order must be gated separately
        self.dump.unlink()
        self.assertIn('OTHER=_seed', self.run_init([second, self.first]))

    def test_audit_excludes_newly_insecure_directory_before_loading_dump(self):
        self.run_init([self.first])
        self.first.chmod(0o777)
        output = self.run_init([self.first])
        self.assertIn('compaudit', output)
        self.assertIn('SECURE=yes', output)
        self.assertIn('MAP=missing', output)

    def test_added_completion_forces_rebuild(self):
        self.run_init([self.first])
        self.completion(self.first, '_other', 'other-command')
        output = self.run_init([self.first])
        self.assertIn('OTHER=_other', output)
        self.assertIn('compdef', output)

    def test_newer_compiled_sibling_overrides_plaintext(self):
        self.run_init([self.first], '\nzcompile "$dump"\n')
        compiled = Path(str(self.dump) + '.zwc')
        self.assertTrue(compiled.is_file())
        text = self.dump.read_text().replace("'seed-command' '_seed'", "'seed-command' '_changed'")
        self.assertIn("'seed-command' '_changed'", text)
        self.dump.write_text(text)
        os.utime(self.dump, (1000000000, 1000000000))
        os.utime(compiled, (1000000010, 1000000010))
        self.assertIn('MAP=_seed', self.run_init([self.first]))
        compiled.unlink()
        self.assertIn('MAP=_changed', self.run_init([self.first]))

if __name__ == '__main__':
    unittest.main(argv=[__file__, *remaining])
