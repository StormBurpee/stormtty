#!/usr/bin/env python3
"""Portable generator inventory tests; never claimed as native execution."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import os
spec = importlib.util.spec_from_file_location('seed_build', Path(__file__).with_name('prepare-completion-seed.py'))
m = importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class InventoryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);(self.root/'a').mkdir();(self.root/'a'/'_x').write_text('#compdef first\n')
    def test_same_count_content_change_changes_identity(self):
        before=m.inventory(self.root);(self.root/'a'/'_x').write_text('#compdef other\n')
        self.assertNotEqual(before,m.inventory(self.root))
    def test_empty_directory_and_new_file_change_identity(self):
        before=m.inventory(self.root);(self.root/'empty').mkdir()
        self.assertNotEqual(before,m.inventory(self.root));before=m.inventory(self.root)
        (self.root/'empty'/'_new').write_text('');self.assertNotEqual(before,m.inventory(self.root))
    @unittest.skipIf(os.name=='nt','link permission not assumed')
    def test_link_rejected(self):
        (self.root/'link').symlink_to(self.root/'a'/'_x')
        with self.assertRaises(ValueError):m.inventory(self.root)
    @unittest.skipIf(os.name=='nt','Windows filenames forbid control characters')
    def test_record_injection_rejected(self):
        (self.root/'bad\nname').write_text('')
        with self.assertRaises(ValueError):m.inventory(self.root)
    def test_repeatable_inventory(self):
        self.assertEqual(m.inventory(self.root),m.inventory(self.root))

# Separate test runner below also exercises sanitized native-result contracts.

class ResultContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec=importlib.util.spec_from_file_location('seed_run',Path(__file__).with_name('run-completion-seed.py'))
        cls.runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(cls.runner)
    def output(self,seeded):
        rows=[]
        for i in range(1,7):
            rows += [f'SOURCE_STAGE sample={i} name={name} elapsed_ms=1' for name in self.runner.profile.prepare.STAGES]
            rows += [f'SOURCE_FUNCTION sample={i} name={name} calls=1 total_ms=1 self_ms=1' for name in ('compinit','compaudit')]
            rows += [f'STARTUP_SAMPLE sample={i} fresh_state={int(i<=3)} state_prepare_ms=1 launch_to_zle_ms=2']
            if seeded:rows += [f'SEED_SAMPLE sample={i} fresh_state={int(i<=3)} adopted={int(i<=3)} verification_publish_ms=3']
        rows += ['STARTUP_RESULT code=0 fresh_samples=3 warm_samples=3']
        return ('\n'.join(rows)+'\n').encode()
    def test_both_modes_require_complete_evidence(self):
        for seeded in (False,True):self.assertTrue(self.runner.sanitize(self.output(seeded),seeded)[1])
        self.assertFalse(self.runner.sanitize(self.output(True).replace(b'name=compaudit',b'name=unknown'),True)[1])
    def test_fallback_is_not_counted_as_seeded_success(self):
        output=self.output(True).replace(b'adopted=1',b'adopted=0',1)
        self.assertFalse(self.runner.sanitize(output,True)[1])
    def test_raw_path_and_invalid_numeric_output_not_published(self):
        lines,valid=self.runner.sanitize(self.output(True)+b'private/user/path\nSEED_SAMPLE sample=1 fresh_state=1 adopted=1 verification_publish_ms=private/path\n',True)
        self.assertTrue(valid);self.assertNotIn('private/','\n'.join(lines))
if __name__=='__main__':unittest.main()
