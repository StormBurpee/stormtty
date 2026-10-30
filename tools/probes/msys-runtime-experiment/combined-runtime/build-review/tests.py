#!/usr/bin/env python3
"""Source/build-plan tests only. No download, native compilation or execution."""
import argparse,importlib.util,json,subprocess,sys,tempfile,unittest
from pathlib import Path
HERE=Path(__file__).resolve().parent
SOURCE=None
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
collector=module('build_collector',HERE/'collect.py')
verify=module('build_verify',HERE/'verify-source.py')
manifest=module('build_manifest',HERE/'source-manifest.py')
class BuildReview(unittest.TestCase):
    def test_all_python_sources_compile(self):
        for path in HERE.glob('*.py'):
            compile(path.read_text(),str(path),'exec')
    def test_cold_inspector_error_report(self):
        with tempfile.TemporaryDirectory(dir=HERE) as d:
            root=Path(d);(root/'provenance').mkdir()
            run=subprocess.run([sys.executable,str(HERE/'inspect-built.py'),str(root),'copied'],capture_output=True,text=True)
            self.assertNotEqual(run.returncode,0)
            result=json.loads((root/'provenance/build-review-error.json').read_text())
            self.assertIn('error',result)
            self.assertFalse(result['fixtures_executed'])
            self.assertFalse(result['approved_for_fixture_execution'])
            self.assertFalse(result['native_qualified'])
            self.assertNotIn('SyntaxError',run.stderr)
            self.assertNotIn('NameError',run.stderr)
    def test_manifest_contract_and_drift(self):
        import ast
        root=HERE.parent
        names=next(ast.literal_eval(n.value) for n in ast.parse((root/'run.py').read_text()).body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='SOURCE_NAMES' for t in n.targets))
        expected={name:manifest.digest(root/name) for name in names}
        expected['owned-runner.py']=manifest.digest(root.parents[2]/'run-owned-process.py')
        current=manifest.identities()
        self.assertEqual(current['fixture_source_sha256'],expected)
        self.assertIn('../toolchain.sh',current['build_source_sha256'])
        with tempfile.TemporaryDirectory() as d:
            snapshot=Path(d)/'snapshot.json';snapshot.write_text(json.dumps(current))
            manifest.check(snapshot)
            current['fixture_source_sha256']['native/reader.c']='0'*64
            snapshot.write_text(json.dumps(current))
            with self.assertRaises(ValueError):manifest.check(snapshot)
        shell=(HERE/'build.sh').read_text();ps=(HERE/'build-controller.ps1').read_text()
        self.assertLess(shell.index('source-manifest.py'),shell.index('makepkg --config'))
        self.assertIn('source-before.json" --check',shell)
        self.assertLess(ps.index('source-manifest.py'),ps.index('& cl.exe'))
        self.assertIn('source-before.json" --check',ps)

    def test_manifest_keys_are_platform_independent(self):
        import ntpath,posixpath
        for suffix in ("build-review/tests.py", "../toolchain.sh", "../../../run-owned-process.py"):
            windows=manifest.relative_key(ntpath.join(r"C:\source\tools\probes\runtime\combined-runtime",suffix.replace("/", "\\")),r"C:\source\tools\probes\runtime\combined-runtime",ntpath)
            posix=manifest.relative_key(posixpath.join("/source/tools/probes/runtime/combined-runtime",suffix),"/source/tools/probes/runtime/combined-runtime",posixpath)
            self.assertEqual(windows,posix)
            self.assertEqual(windows,suffix)
        self.assertTrue(all("\\" not in key for key in manifest.identities()["build_source_sha256"]))

    def test_shell_syntax(self):subprocess.run(['bash','-n',str(HERE/'build.sh')],check=True)
    def test_source_contract_and_drift(self):
        for variant in ('copied','combined'):
            with tempfile.TemporaryDirectory(dir=HERE) as d:
                tree=Path(d)
                for name in ('fhandler/console.cc','fhandler/termios.cc','local_includes/fhandler.h'):
                    p=tree/'winsup/cygwin'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes((SOURCE/'winsup/cygwin'/name).read_bytes())
                for patch in ['partial-read/0007-preserve-copied-on-signal.patch']+(['0006-refined-prefix.patch'] if variant=='combined' else []):
                    subprocess.run(['git','apply',str(HERE.parent.parent/patch)],cwd=tree,check=True,capture_output=True)
                verify.verify(variant,tree/'winsup/cygwin','before')
                subprocess.run(['git','apply',str(HERE.parent/(variant+'.patch'))],cwd=tree,check=True,capture_output=True)
                verify.verify(variant,tree/'winsup/cygwin','after')
                p=tree/'winsup/cygwin/fhandler/termios.cc';p.write_bytes(p.read_bytes()+b'\n')
                with self.assertRaises(ValueError):verify.verify(variant,tree/'winsup/cygwin','after')
    def test_no_execution_or_warning_relaxation(self):
        shell=(HERE/'build.sh').read_text();ps=(HERE/'build-controller.ps1').read_text()
        self.assertIn('-DSTORMTTY_COMBINED_CONSOLE_TRACE=1',shell)
        self.assertIn('-std=gnu++17',shell);self.assertIn('15.3.0',shell)
        self.assertIn('/W4 /WX',ps)
        for forbidden in ('-Wno-', '--skipinteg', '--nocheck', 'linked-preflight.py', 'run.py'):
            self.assertNotIn(forbidden,'\n'.join(line for line in shell.splitlines() if not line.lstrip().startswith('#')))
        self.assertNotIn('& "$Output/controller.exe"',ps)
        self.assertNotIn('"$output/reader.exe"\n',shell.split('objdump -p')[0])
        self.assertNotIn('LoadLibrary',(HERE/'inspect-built.py').read_text())
    def test_template_inactive_and_actions_pinned(self):
        text=(HERE/'workflow.yml.template').read_text()
        self.assertIn('branches: [experiment/msys-combined-build-review]',text);self.assertNotIn('workflow_dispatch:',text);self.assertNotIn('\n  pull_request:',text)
        import re
        actions=re.findall(r'uses: (\S+)',text)
        self.assertTrue(actions);self.assertTrue(all(re.fullmatch(r'[^@]+@[0-9a-f]{40}',a) for a in actions))
        self.assertIn('path: ${{ runner.temp }}/combined-review-text/',text)
        self.assertNotIn('path: ${{ env.EXPERIMENT_ROOT }}',text)
    def test_minimal_msys_path_has_native_python_dependency(self):
        # setup-python configures Windows Python; msys2 path-type:minimal does
        # not inherit that PATH. build.sh invokes python3 inside MSYS.
        import re
        template=(HERE/'workflow.yml.template').read_text()
        self.assertIn('path-type: minimal',template)
        packages=re.search(r'install: >-\n((?: +[^\n]+\n)+?)(?=      - name:)',template)
        self.assertIsNotNone(packages)
        self.assertIn('python',packages.group(1).split())
        active=HERE.parents[4]/'.github/workflows/msys-combined-build-review.yml'
        if active.exists():self.assertEqual(active.read_text(),template)

    def test_verified_epoch_and_metadata_only_controls(self):
        bare=SOURCE.parent/'msys2-runtime/msys2-runtime'
        commit=subprocess.check_output(['git','--git-dir='+str(bare),'rev-parse','refs/tags/cygwin-3.6.11^{commit}'],text=True).strip()
        self.assertEqual(commit,'867eb7748e6e5a0e8430ce9f46af27b4b1e8f204')
        epoch=subprocess.check_output(['git','--git-dir='+str(bare),'show','-s','--format=%ct',commit],text=True).strip()
        self.assertEqual(epoch,'1790890720')
        shell=(HERE/'build.sh').read_text();ps=(HERE/'build-controller.ps1').read_text()
        self.assertIn('export SOURCE_DATE_EPOCH='+epoch,shell)
        self.assertIn('--no-insert-timestamp',shell)
        self.assertIn('/Brepro',ps);self.assertIn('/WX /Brepro',ps)
        self.assertIn('REPRO',ps)
        for text in (shell,ps):
            self.assertIn('tool-binaries-before',text);self.assertIn('tool-binaries-after',text)
            self.assertNotIn('prefix-map',text)
        workflow=(HERE/'workflow.yml.template').read_text()
        self.assertIn('repeat: [1, 2]',workflow);self.assertIn('fail-fast: false',workflow)
        self.assertIn('compare.py',workflow);self.assertIn('independent-build.json',workflow)
        self.assertNotIn('continue-on-error: true',workflow)

    def test_collector_only_text_allowlist(self):
        with tempfile.TemporaryDirectory(dir=HERE) as d:
            root=Path(d)/'private';p=root/'copied-build/provenance';p.mkdir(parents=True)
            (p/'variant.txt').write_text('copied\r\n');(p/'experimental.dll').write_bytes(b'MZsecret')
            (p/'hashes.txt').write_bytes(b'MZbinary');(p/'termios.cc.after').write_text('safe source')
            result=collector.collect(root,Path(d)/'out')
            self.assertEqual({r['path'] for r in result['files']},{'copied-build/provenance/variant.txt','copied-build/provenance/termios.cc.after'})
            self.assertEqual(result['rejected'],['copied-build/provenance/hashes.txt'])
            self.assertFalse(result['both_build_reports_present'])
    def test_collector_rejects_alias(self):
        with tempfile.TemporaryDirectory(dir=HERE) as d:
            root=Path(d)/'private';p=root/'controller';p.mkdir(parents=True)
            target=Path(d)/'secret';target.write_text('private')
            (p/'compiler.log').symlink_to(target)
            result=collector.collect(root,Path(d)/'out')
            self.assertEqual(result['rejected'],['controller/compiler.log']);self.assertFalse(result['files'])
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,required=True);a,rest=p.parse_known_args();SOURCE=a.source_root
    unittest.main(argv=[__file__]+rest)
