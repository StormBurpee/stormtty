from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[2]
class WorkflowTests(unittest.TestCase):
    def test_exact_route_and_bounds(self):
        s=(ROOT/'.github/workflows/completion-seed-shipping.yml').read_text()
        for token in ('branches: [diagnostic/completion-seed-shipping]',"github.event_name == 'push' && github.ref == 'refs/heads/diagnostic/completion-seed-shipping'",'ref: ${{ github.sha }}','timeout=1800','timeout-minutes: 31','if: always()'):
            self.assertIn(token,s)
        self.assertNotIn('paths:',s);self.assertNotIn('pull_request:',s)
        self.assertNotIn('workflow_dispatch:',s);self.assertNotIn('continue-on-error',s)
        self.assertIn('-DSTORMTTY_EXPERIMENTAL_COMPLETION_SEED=ON',s)
        self.assertIn('0057852bfaa89a56745cba8c7296529d2fc39830',s)
    def test_actual_factory_only_under_option(self):
        source=(ROOT/'src/platform/windows/window.cpp').read_text()
        self.assertIn('#ifdef STORMTTY_EXPERIMENTAL_COMPLETION_SEED\n        if (state)',source)
        self.assertIn('process.initialization=completion_seed_launch(',source)
        cmake=(ROOT/'cmake/StormTTYDeveloperShell.cmake').read_text()
        self.assertIn('serialized initialization" OFF)',cmake)
        self.assertIn('add_dependencies(stormtty_settings_services stormtty_completion_seed_package)',cmake)
    def test_no_environment_identity_anchor(self):
        s=(ROOT/'src/platform/windows/completion_seed.cpp').read_text()
        self.assertIn('build_seed::identity_sha256',s)
        self.assertNotIn('getenv',s)
if __name__=='__main__':unittest.main()
