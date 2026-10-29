#!/usr/bin/env python3
"""Dependency-free exact diagnostic trigger/bounds contract checks."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[2]
class WorkflowTests(unittest.TestCase):
    def setUp(self):self.source=(ROOT/'.github/workflows/completion-seed-native.yml').read_text()
    def test_exact_push_only(self):
        self.assertIn('branches: [diagnostic/completion-seed-native]',self.source)
        self.assertIn('paths: [.github/workflows/completion-seed-native.yml]',self.source)
        self.assertIn("if: github.event_name == 'push' && github.ref == 'refs/heads/diagnostic/completion-seed-native'",self.source)
        self.assertIn('ref: ${{ github.sha }}',self.source)
        self.assertNotIn('workflow_dispatch:',self.source);self.assertNotIn('pull_request:',self.source)
    def test_bounds_and_no_activation(self):
        for text in ('timeout-minutes: 80','timeout-minutes: 26','timeout=1500','--target managed_shell_startup_probe','-DSTORMTTY_BUNDLE_ZSH=OFF','-DSTORMTTY_BUNDLE_CONPTY=OFF'):
            self.assertIn(text,self.source)
        self.assertNotIn('continue-on-error',self.source)
        self.assertNotIn('cmake --install',self.source)
    def test_artifact_scope_and_always(self):
        tail=self.source.split('- name: Always preserve sanitized qualification evidence')[1]
        self.assertIn('if: always()',tail)
        self.assertIn('/evidence/*.log',tail);self.assertIn('/evidence/*.json',tail)
        self.assertNotIn('/scratch/',tail);self.assertNotIn('/runtime/',tail)
    def test_normal_compinit_and_user_override_order(self):
        source=(ROOT/'tools/shell-preset.zsh').read_text()
        self.assertIn('ZSH_DISABLE_COMPFIX=false',source)
        self.assertIn('compinit -i -D',source)
        self.assertLess(source.index('source "$ZSH/oh-my-zsh.sh"'),source.index('&& source "$_stormtty_state/config.zsh"'))
        self.assertNotIn('completion_seed',source)
        native=(ROOT/'src/platform/windows/completion_seed.hpp').read_text()
        self.assertIn('std::equal(ordered.begin(),ordered.end(),lines.begin()+5)',native)
        state=(ROOT/'src/platform/windows/shell_state.cpp').read_text()
        self.assertIn('require(std::filesystem::is_empty(cache))',state)
        self.assertIn('rename->ReplaceIfExists = FALSE',state)
if __name__=='__main__':unittest.main()
