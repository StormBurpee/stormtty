from pathlib import Path
import unittest
import importlib.util
import tempfile
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
    def test_default_off_integration_route(self):
        s=(ROOT/'.github/workflows/completion-seed-shipping.yml').read_text()
        off=s.split('- name: Build current application with unchanged default OFF')[1].split('- name: Qualify default OFF')[0]
        self.assertNotIn('-DSTORMTTY_EXPERIMENTAL_COMPLETION_SEED=',off)
        self.assertIn('cmake --build',off);self.assertNotIn('--target',off)
        self.assertIn('Run full default OFF native regression suite',s)
        self.assertIn('--output-on-failure --timeout 120',s)
        self.assertIn('tools/probes/run-default-off-completion.py',s)
        self.assertIn('timeout=900',s)
    def test_off_guard_rejects_activation_or_generated_seed(self):
        spec=importlib.util.spec_from_file_location('off_integration',ROOT/'tools/probes/run-default-off-completion.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);cache=root/'CMakeCache.txt'
            for value in ('ON',''):
                cache.write_text('STORMTTY_EXPERIMENTAL_COMPLETION_SEED:BOOL='+value+'\n')
                with self.assertRaises(ValueError):module.assert_default_off(root)
            cache.write_text('STORMTTY_EXPERIMENTAL_COMPLETION_SEED:BOOL=OFF\n')
            module.assert_default_off(root)
            (root/'runtimes/zsh-seed').mkdir(parents=True)
            with self.assertRaises(ValueError):module.assert_default_off(root)
    def test_off_failure_diagnostics_and_bounded_smoke_output(self):
        spec=importlib.util.spec_from_file_location('off_diagnostics',ROOT/'tools/probes/run-default-off-completion.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        failure='MANAGED_STATE_RESULT code=1 phase=2 checks=7 cold_ms=0 warm_ms=0'
        success='MANAGED_STATE_RESULT code=0 phase=11 cold_samples=1 warm_samples=1 checks=508 cold_ms=5307 warm_ms=741'
        lines,ok=module.managed_evidence(('private contents\n'+failure+'\n').encode())
        self.assertEqual(lines,[failure]);self.assertFalse(ok)
        contract='MANAGED_STATE_CONTRACT index=2 passed=0'
        self.assertEqual(module.managed_evidence(contract.encode())[0],[contract])
        self.assertTrue(module.managed_evidence(success.encode())[1])
        self.assertFalse(module.managed_evidence((success+'\n'+failure).encode())[1])
        self.assertEqual(module.SMOKE_TAIL_BYTES,65536)
        from types import SimpleNamespace
        result=SimpleNamespace(exit_code=0,reaped=True,timed_out=False,output_truncated=True,cleanup_error='')
        self.assertFalse(module.good(result))
    def test_actual_factory_only_under_option(self):
        source=(ROOT/'src/platform/windows/window.cpp').read_text()
        self.assertIn('#ifdef STORMTTY_EXPERIMENTAL_COMPLETION_SEED\n        if (state)',source)
        self.assertIn('process.initialization=completion_seed_launch(',source)
        cmake=(ROOT/'cmake/StormTTYDeveloperShell.cmake').read_text()
        self.assertIn('serialized initialization" OFF)',cmake)
        self.assertIn('add_dependencies(stormtty_settings_services stormtty_completion_seed_package)',cmake)
    def test_initialization_timing_is_probe_opt_in(self):
        header=(ROOT/'include/stormtty/windows_completion_seed.hpp').read_text()
        self.assertIn('bool collect_initialization_timing = false',header)
        probe=(ROOT/'tools/probes/managed_shell_startup_probe.cpp').read_text()
        self.assertIn('completion_seed_launch(fixture.root,runtime,true)',probe)
        source=(ROOT/'src/platform/windows/shell_state.cpp').read_text()
        self.assertIn('require(FlushFileBuffers(journal.value))',source)
        self.assertIn('timing_?Clock::now():Clock::time_point{}',source)
    def test_no_environment_identity_anchor(self):
        s=(ROOT/'src/platform/windows/completion_seed.cpp').read_text()
        self.assertIn('build_seed::identity_sha256',s)
        self.assertNotIn('getenv',s)
if __name__=='__main__':unittest.main()
