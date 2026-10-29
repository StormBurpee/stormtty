#!/usr/bin/env python3
"""Portable fixed app-matrix contracts, not Windows execution."""
import collections,copy,hashlib,importlib.util,json,pathlib,subprocess,tempfile,unittest
from unittest.mock import patch
HERE=pathlib.Path(__file__).resolve().parent;REPO=HERE.parents[3]
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
matrix=module('app_test_matrix',HERE/'compare.py');layout=matrix.layout;collector=module('app_test_collect',HERE/'collect.py')
class Matrix(unittest.TestCase):
 def test_fixed_counts_and_balanced_cells(self):
  for kind,total,repeats in [('raw',18,3),('managed',12,2),('settings',78,13)]:
   rows=list(matrix.plan(kind));self.assertEqual(len(rows),total)
   self.assertEqual(collections.Counter((r['provider'],r['variant']) for r in rows),{(p,v):repeats for p in matrix.PROVIDERS for v in matrix.VARIANTS})
   self.assertEqual(len({(r['provider'],r['mode'],r['trial'],r['variant']) for r in rows}),total)
 def test_settings_current_explicit10_default3_unchanged(self):
  rows=list(matrix.plan('settings'))
  for p in matrix.PROVIDERS:
   for v in matrix.VARIANTS:self.assertEqual(collections.Counter(r['mode'] for r in rows if r['provider']==p and r['variant']==v),{'explicit':10,'default':3})
 def test_fixed_order_not_retry(self):
  rows=list(matrix.plan('raw'));self.assertEqual([r['variant'] for r in rows[:3]],list(matrix.ORDERS[0]));self.assertEqual([r['variant'] for r in rows[3:6]],list(matrix.ORDERS[1]))
 def setUp(self):
  self.meta=dict(returncode=0,reaped=True,timed_out=False,output_truncated=False,cleanup_error='')
  self.raw=b'PASTE_RESULT code=0 provider=inbox vt_off_control_failures=0 raw_vt_reader_failures=0 production_cause_established=0\n'
  self.managed=b'REFINED_MANAGED_RESULT fresh=1 warm=1 result=PASS\n'
  self.settings=b'Native CI smoke passed: real fixture\nNative CI teardown: tick_ms=1 thread=1 stage=native application locals destroyed\nNative CI teardown: tick_ms=2 thread=1 stage=native application returned to wWinMain\n'
 def test_exact_completion_each_workload(self):
  for kind,data in [('raw',self.raw),('managed',self.managed),('settings',self.settings)]:self.assertTrue(matrix.assess(kind,'inbox',self.meta,data)['passed'])
 def test_cleanup_and_failure_never_inherit_marker(self):
  for key,value in [('returncode',2),('reaped',False),('timed_out',True),('output_truncated',True),('cleanup_error','failed')]:
   for kind,data in [('raw',self.raw),('managed',self.managed),('settings',self.settings)]:
    meta=dict(self.meta);meta[key]=value;self.assertFalse(matrix.assess(kind,'inbox',meta,data)['passed'])
 def test_missing_cleanup_fields_fail_closed(self):
  for key in self.meta:
   meta=dict(self.meta);del meta[key]
   self.assertFalse(matrix.assess('settings','inbox',meta,self.settings)['passed'])
 def test_stock_vt_off_failure_remains_failure(self):
  data=b'PASTE_RESULT code=1 provider=inbox vt_off_control_failures=4 raw_vt_reader_failures=0 production_cause_established=0\n'
  r=matrix.assess('raw','inbox',dict(self.meta,returncode=1),data);self.assertFalse(r['passed']);self.assertTrue(r['complete']);self.assertEqual(r['summary']['vt_off_control_failures'],4)
 def test_wrong_missing_duplicate_raw_summary_rejected(self):
  for output in (b'',self.raw+self.raw,self.raw.replace(b'provider=inbox',b'provider=packaged'),self.raw.replace(b'established=0',b'established=1')):self.assertFalse(matrix.assess('raw','inbox',self.meta,output)['passed'])
 def test_lifecycle_not_just_exit_or_success_marker(self):
  for output in (b'',self.settings.splitlines()[0]+b'\n',self.settings+self.settings):self.assertFalse(matrix.assess('settings','inbox',self.meta,output)['passed'])
 def test_managed_marker_missing_duplicate_fails(self):
  for output in (b'',self.managed+self.managed):self.assertFalse(matrix.assess('managed','inbox',self.meta,output)['passed'])
 def test_current_source_tree_and_fixture_locked(self):
  p=layout.verify_source();self.assertEqual(p['app_base_remote'],'4d65a9fa1af839fa5ac5e878700a7557850a74b2')
  self.assertEqual(p['source_files']['tests/paste_input_probe.cpp'],'251477c3587c25b498bf1bf099e355d47a2cf3e1b8f1dcd7649e406aca39beae')
  self.assertFalse(p['production_qualified'])
 def test_exact_process_budgets_no_observer(self):
  s=(HERE/'compare.py').read_text()
  for timeout in (30,90,180):self.assertIn('timeout='+str(timeout),s)
  self.assertIn('reap_timeout=10',s);self.assertNotIn('sleep(',s)
  self.assertIn("if row['mode']=='explicit'",s)
  self.assertIn("'--shell-runtime',str(app_roots[variant])",s)
 def test_actual_autocrlf_checkout_preserves_locked_source_bytes(self):
  plan=json.loads((HERE/'app-plan.json').read_text())
  paths=list(plan['source_files'])+[(HERE/'managed-native'/name).relative_to(REPO).as_posix() for name in plan['managed_fixture_files']]
  with tempfile.TemporaryDirectory() as temporary:
   destination=pathlib.Path(temporary)
   subprocess.run(['git','-c','core.autocrlf=true','checkout-index','--prefix='+destination.as_posix()+'/', '--',*paths],cwd=REPO,check=True,capture_output=True)
   for relative,digest in plan['source_files'].items():self.assertEqual(hashlib.sha256((destination/relative).read_bytes()).hexdigest(),digest,relative)
   for name,digest in plan['managed_fixture_files'].items():
    relative=(HERE/'managed-native'/name).relative_to(REPO)
    self.assertEqual(hashlib.sha256((destination/relative).read_bytes()).hexdigest(),digest,str(relative))
 def test_provider_hashes_preserved(self):
  p=json.loads((HERE/'app-plan.json').read_text());s=(REPO/'tools/prepare-conpty-probe.ps1').read_text()
  for digest in p['packaged_provider_sha256'].values():self.assertIn(digest,s)
class Layout(unittest.TestCase):
 def test_new_copies_preserve_sources_and_identical_apps(self):
  with tempfile.TemporaryDirectory() as temporary:
   r=pathlib.Path(temporary);build=r/'build';build.mkdir();roots={n:r/n for n in matrix.VARIANTS}
   for name in layout.BINARIES:(build/name).write_bytes(b'MZ'+name.encode())
   for n,p in roots.items():p.mkdir();(p/'fixture.txt').write_text(n)
   ids={n:{'dll_sha256':n,'manifest_sha256':n} for n in roots}
   with patch.object(layout,'verify_source',return_value={}),patch.object(layout,'verify_build',return_value={}),patch.object(layout.identity,'identities',return_value=ids):
    report=layout.stage(build,roots,r/'apps')
   for n in roots:
    self.assertEqual((roots[n]/'fixture.txt').read_text(),n)
    self.assertEqual((r/'apps'/n/'runtimes/zsh/fixture.txt').read_text(),n)
    for binary in layout.BINARIES:self.assertEqual((build/binary).read_bytes(),(r/'apps'/n/binary).read_bytes())
   self.assertFalse(report['production_qualified'])
 def test_source_nested_layout_rejected(self):
  with tempfile.TemporaryDirectory() as temporary:
   r=pathlib.Path(temporary);build=r/'build';build.mkdir()
   with patch.object(layout,'verify_source',return_value={}),patch.object(layout,'verify_build',return_value={}),patch.object(layout.identity,'identities',return_value={}):
    with self.assertRaisesRegex(ValueError,'outside'):layout.stage(build,{'original':r/'source'},build/'apps')
class BuildContract(unittest.TestCase):
 def test_exact_default_discovery_and_official_validation_contract(self):
  with tempfile.TemporaryDirectory() as temporary:
   r=pathlib.Path(temporary);original=r/'original';original.mkdir()
   block='build CMakeFiles/stormtty.dir/src/app/windows_main.cpp.obj: CXX_COMPILER__stormtty_RelWithDebInfo source\n  DEFINES = -DSTORMTTY_HAS_BUNDLED_SHELL_RUNTIME=1\n'
   (r/'build.ninja').write_text(block)
   fields={'STORMTTY_BUNDLE_ZSH':'ON','STORMTTY_EXPERIMENTAL_COMPLETION_SEED':'OFF','STORMTTY_TEST_PARTIAL_APP':'ON','CMAKE_BUILD_TYPE':'RelWithDebInfo','CMAKE_CXX_COMPILER':'cl','STORMTTY_DEVELOPER_SHELL_RUNTIME':str(original)}
   (r/'CMakeCache.txt').write_text('\n'.join(key+':STRING='+value for key,value in fields.items()))
   report=layout.verify_build(r,original);self.assertFalse(report['production_qualified'])
   (r/'build.ninja').write_text(block.replace('STORMTTY_HAS_BUNDLED_SHELL_RUNTIME=1','NO_BUNDLED_RUNTIME=1'))
   with self.assertRaisesRegex(ValueError,'default discovery'):layout.verify_build(r,original)
   (r/'build.ninja').write_text(block)
   with self.assertRaisesRegex(ValueError,'only official'):layout.verify_build(r,r/'candidate')

class Collection(unittest.TestCase):
 def test_only_fixed_logs_sources_not_binary_home_capture(self):
  names=[p.as_posix() for p,_ in collector.candidates()];self.assertEqual(len(names),len(set(names)))
  self.assertEqual(sum(p.endswith('.log') and any('/'+k+'/' in p for k in ('raw','managed','settings')) for p in names),108)
  for name in names:self.assertNotIn('private-captures',name);self.assertFalse(name.endswith(('.exe','.dll','.png')))
 def test_windows_path_serialization_counts_same_fixed_logs(self):
  names=[pathlib.PureWindowsPath(*p.parts).as_posix() for p,_ in collector.candidates()]
  self.assertEqual(sum(p.endswith('.log') and any('/'+kind+'/' in p for kind in ('raw','managed','settings')) for p in names),108)
  self.assertEqual(names,[p.as_posix() for p,_ in collector.candidates()])
 def test_missing_evidence_incomplete_preserved(self):
  with tempfile.TemporaryDirectory() as temporary:
   r=pathlib.Path(temporary);(r/'evidence/raw').mkdir(parents=True);p=r/'evidence/raw/inbox-raw-1-stock.log';p.write_text('FAIL retained')
   report=collector.collect(r,r/'packet');self.assertFalse(report['collection_complete']);self.assertTrue(report['missing']);self.assertEqual((r/'packet'/p.relative_to(r)).read_text(),'FAIL retained')
 def test_complete_failed_matrix_is_retained_not_success_relabelled(self):
  with tempfile.TemporaryDirectory() as temporary:
   r=pathlib.Path(temporary)
   for relative,_ in collector.candidates():
    p=r/relative;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'fixed evidence')
   for kind in ('raw','managed','settings'):
    rows=[dict(row,passed=False) for row in matrix.plan(kind)]
    (r/'evidence'/kind/'results.json').write_text(json.dumps(dict(planned=len(rows),cases=rows,final_identity_verified=True)))
   report=collector.collect(r,r/'packet');self.assertTrue(report['collection_complete'])
   saved=json.loads((r/'packet/evidence/raw/results.json').read_text());self.assertTrue(all(row['passed'] is False for row in saved['cases']))
 def test_partial_matrix_with_all_log_files_is_still_incomplete(self):
  with tempfile.TemporaryDirectory() as temporary:
   r=pathlib.Path(temporary)
   for relative,_ in collector.candidates():
    p=r/relative;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'fixed evidence')
   for kind in ('raw','managed','settings'):
    rows=list(matrix.plan(kind));(r/'evidence'/kind/'results.json').write_text(json.dumps(dict(planned=len(rows),cases=rows[:-1],final_identity_verified=True)))
   report=collector.collect(r,r/'packet');self.assertFalse(report['collection_complete']);self.assertEqual(report['missing'],[])
   self.assertFalse(any(report['matrix_inventory_complete'].values()))
 def test_oversize_rejected_not_truncated(self):
  with tempfile.TemporaryDirectory() as temporary:
   r=pathlib.Path(temporary);(r/'evidence/raw').mkdir(parents=True);p=r/'evidence/raw/inbox-raw-1-stock.log';p.write_bytes(b'X'*(collector.LOG+1))
   report=collector.collect(r,r/'packet');self.assertFalse(report['collection_complete']);self.assertTrue(report['rejected']);self.assertFalse((r/'packet'/p.relative_to(r)).exists())
class Workflow(unittest.TestCase):
 def test_exact_new_branch_current_app_and_bounded_steps(self):
  s=(REPO/'.github/workflows/msys-partial-app.yml').read_text()
  self.assertIn("branches: ['experiment/msys-partial-app']",s);self.assertIn("paths: ['.github/workflows/msys-partial-app.yml']",s);self.assertIn('ref: ${{ github.sha }}',s)
  for forbidden in ('workflow_dispatch','pull_request:','continue-on-error','0006-','PASSIVE_CONSOLE_TRACE') :self.assertNotIn(forbidden,s)
  self.assertEqual(s.count('partial-app/compare.py'),3);self.assertEqual(s.count("if: always() && steps.staging.outcome == 'success'"),3)
  for value in ('STORMTTY_BUNDLE_ZSH=ON','STORMTTY_EXPERIMENTAL_COMPLETION_SEED=OFF','STORMTTY_TEST_PARTIAL_APP=ON','--verify-source','timeout-minutes: 330','timeout-minutes: 60','timeout-minutes: 24','timeout-minutes: 55','/review-packet/'):self.assertIn(value,s)
  self.assertIn('partial-app/collect.py',s);self.assertNotIn('runtimes/zsh/',s.split('uses: actions/upload-artifact@')[1])
if __name__=='__main__':unittest.main()
