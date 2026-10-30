#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
spec=importlib.util.spec_from_file_location('startup_app_test_driver',HERE/'run-shell-startup-app.py')
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)
class StartupAppTests(unittest.TestCase):
    def row(self,v):
        return 'STARTUP_APP_CHECK mode=%d sample=%d diagnostic=%d native_status=%d persistent=%d cache_expected=%d input_roundtrip=1 notice=%d code=0'%v
    def test_only_fixed_numeric_expected_records_are_retained(self):
        for mode,rows in p.EXPECTED.items():
            lines=[self.row(row) for row in rows]+[f'STARTUP_APP_EXIT sample={row[1]} code=0' for row in rows]+['STARTUP_APP_CLEANUP actors_closed=1 removed=1']
            kept,ok=p.evidence(('private path/history\n'+'\n'.join(lines)+'\n').encode(),mode)
            self.assertTrue(ok);self.assertEqual(kept,lines)
            self.assertFalse(p.evidence(('\n'.join(lines+lines)).encode(),mode)[1])
            self.assertFalse(p.evidence(b'',mode)[1])
            valid='\n'.join(lines)
            self.assertFalse(p.evidence((valid+'\nSTARTUP_APP_CHECK malformed').encode(),mode)[1])
            self.assertFalse(p.evidence(valid.replace('actors_closed=1','actors_closed=0').encode(),mode)[1])
            self.assertFalse(p.evidence(valid.replace('removed=1','removed=0').encode(),mode)[1])
            self.assertFalse(p.evidence('\n'.join(lines[:-1]).encode(),mode)[1])
    def test_partial_or_wrong_observations_fail(self):
        rows=p.EXPECTED['ready']
        self.assertFalse(p.evidence(self.row(rows[0]).encode(),'ready')[1])
        self.assertFalse(p.evidence(self.row(rows[1]).encode(),'ready')[1])
        self.assertFalse(p.evidence(self.row(rows[0]).replace('input_roundtrip=1','input_roundtrip=0').encode(),'ready')[1])
    def test_explicit_isolated_gate_and_unmodified_timing_matrix(self):
        host=(ROOT/'src/platform/windows/window.cpp').read_text()
        self.assertIn('std::filesystem::create_hard_link(root/L"authored-history",prepared.directory/L"history")',host)
        self.assertIn('options_.smoke_test && options_.smoke_shell_startup=="unavailable" && !smoke_shell_state_root_.empty()',host)
        self.assertIn('if (frame && options_.smoke_shell_startup.empty()) for',host)
        driver=(HERE/'run-shell-startup-app.py').read_text()
        self.assertIn('timeout=120,reap_timeout=10',driver)
        self.assertIn("if mode!='off':command.append('--diagnose-shell-startup')",driver)
        matrix=(HERE/'run-shipping-completion-seed.py').read_text()
        self.assertNotIn('--diagnose-shell-startup',matrix)
    def test_dedicated_workflow_never_changes_seed_gate(self):
        workflow=(ROOT/'.github/workflows/shell-startup.yml').read_text()
        self.assertIn('branches: [diagnostic/shell-startup]',workflow)
        self.assertIn("github.ref == 'refs/heads/diagnostic/shell-startup'",workflow)
        self.assertIn('ref: ${{ github.sha }}',workflow)
        self.assertIn('timeout=600',workflow)
        self.assertIn('evidence/startup-app-*.log',workflow)
        self.assertNotIn('evidence/*.log',workflow)
        self.assertNotIn('STORMTTY_EXPERIMENTAL_COMPLETION_SEED=ON',workflow)
        self.assertNotIn('run-shell-startup-app.py',(ROOT/'.github/workflows/completion-seed-shipping.yml').read_text())
if __name__=='__main__':unittest.main()
