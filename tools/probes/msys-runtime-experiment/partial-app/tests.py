#!/usr/bin/env python3
"""Portable fixed app-matrix contracts, not Windows execution."""
import collections,copy,hashlib,importlib.util,json,pathlib,subprocess,sys,tempfile,unittest
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
 def setUp(self):
  self.temporary=tempfile.TemporaryDirectory();self.addCleanup(self.temporary.cleanup)
  self.build=pathlib.Path(self.temporary.name);self.original=self.build/'original';self.original.mkdir()
  self.fields={**layout.EXPECTED_CACHE,'CMAKE_CXX_COMPILER':'cl','STORMTTY_DEVELOPER_SHELL_RUNTIME':str(self.original)}
  self.cache()
 def cache(self):
  (self.build/'CMakeCache.txt').write_text('\n'.join(key+':STRING='+value for key,value in self.fields.items()))
 def path(self,value,windows=False):
  if windows:value=value.replace('/','\\')
  return value.replace('$','$$').replace(':','$:').replace(' ','$ ')
 def block(self,windows=False):
  # Structure emitted by a configure-only CMake 4.4.4/Ninja 1.13.2 Windows
  # cross configuration. Backslashes model native MSVC EncodePath output;
  # the failed native run retained the build log, not its build.ninja bytes.
  obj=self.path(layout.MAIN_OBJECT,windows);source=self.path((layout.REPO/'src/app/windows_main.cpp').as_posix(),windows)
  return f'build {obj}: CXX_COMPILER__stormtty_unscanned_RelWithDebInfo {source} || cmake_object_order_depends_target_stormtty\n  CONFIG = RelWithDebInfo\n  DEFINES = -DNOMINMAX -DSTORMTTY_HAS_BUNDLED_SHELL_RUNTIME=1 -DUNICODE\n  DEP_FILE = {obj}.d\n  FLAGS = -O2 -g -DNDEBUG\n  OBJECT_DIR = CMakeFiles/stormtty.dir\n  OBJECT_FILE_DIR = CMakeFiles/stormtty.dir/src/app\n  TARGET_COMPILE_PDB = CMakeFiles/stormtty.dir/\n  TARGET_PDB = stormtty.pdb\n  TARGET_SUPPORT_DIR = CMakeFiles/stormtty.dir\n'
 def write(self,text):
  raw=text.encode('utf-8');(self.build/'build.ninja').write_bytes(raw);return raw
 def verify(self):return layout.verify_build(self.build,self.original)
 def rejects(self,text,message):
  self.write(text)
  with self.assertRaisesRegex(ValueError,message):self.verify()
  report=json.loads((self.build/'partial-app-build-contract.json').read_text())
  self.assertFalse(report['verification_passed']);self.assertIn('error',report)
 def test_emitted_structure_windows_separators_crlf_comments_and_raw_hash(self):
  for windows in (False,True):
   for newline in ('\n','\r\n'):
    with self.subTest(windows=windows,newline=repr(newline)):
     block=self.block(windows).replace('\n',newline)
     raw=self.write('# Preceding CMake comment'+newline+block)
     report=self.verify();self.assertTrue(report['verification_passed']);self.assertFalse(report['production_qualified'])
     self.assertEqual(report['ninja_sha256'],hashlib.sha256(raw).hexdigest())
     self.assertEqual(report['app_main_compile_block'],block.rstrip('\r\n'))
     self.assertEqual(report['app_main_compile_candidates'],[block.rstrip('\r\n')])
     self.assertEqual((self.build/'build.ninja').read_bytes(),raw)
 def test_literal_escaped_paths_and_continuations(self):
  tokens,expanded=layout.ninja_words(r'C$:\a$ b\main.cpp $$literal')
  self.assertEqual(tokens,[r'C:\a b\main.cpp','$literal']);self.assertFalse(expanded)
  with patch.object(layout,'REPO',self.build/'current app $root'):
   block=self.block(True).replace(' -DUNICODE',' $\r\n    -DUNICODE')
   self.write(block);self.assertTrue(self.verify()['verification_passed'])
 def test_missing_wrong_object_and_source_rejected(self):
  for text in ('', '# '+self.block(),self.block().replace('stormtty.dir','other.dir'),self.block().replace('windows_main.cpp.obj:','windows_main.cpp.obj.extra:')):
   self.rejects(text,'expected one current app main object')
  source=self.path((layout.REPO/'src/app/windows_main.cpp').as_posix())
  self.rejects(self.block().replace(source,source+'.wrong'),'wrong current app main source')
  self.rejects(self.block().replace(source,source+' extra.cpp'),'expected one current app main source')
  self.rejects(self.block().replace(source,'$source'),'expected one current app main source')
 def test_duplicate_and_ambiguous_outputs_rejected(self):
  self.rejects(self.block()+self.block(True),'expected one current app main object')
  self.rejects(self.block()+self.block().replace('.obj: ','.obj:',1),'malformed current app main edge')
  for output in ('extra.obj ', '| extra.obj '):
   self.rejects(self.block().replace('.obj:','.obj '+output+':',1),'ambiguous current app main output')
  self.rejects(self.block().replace('CXX_COMPILER__stormtty_unscanned_RelWithDebInfo','CXX_COMPILER__other_unscanned_RelWithDebInfo'),'compiler rule')
 def test_discovery_is_exact_single_compile_define_not_comment_or_substring(self):
  flag='-DSTORMTTY_HAS_BUNDLED_SHELL_RUNTIME=1'
  for replacement in ('','-DNOT_STORMTTY_HAS_BUNDLED_SHELL_RUNTIME=1',flag+'0',flag+' '+flag,flag+' -DSTORMTTY_HAS_BUNDLED_SHELL_RUNTIME=0',flag+' -USTORMTTY_HAS_BUNDLED_SHELL_RUNTIME','$discovery'):
   self.rejects(self.block().replace(flag,replacement),'default discovery')
  self.rejects(self.block().replace('  DEFINES =','  # DEFINES ='),'DEFINES binding')
  self.rejects(self.block()+'  DEFINES = '+flag+'\n','DEFINES binding')
  self.rejects(self.block().replace('  FLAGS =','  FLAGS = /USTORMTTY_HAS_BUNDLED_SHELL_RUNTIME'),'discovery override')
 def test_other_edges_and_comments_never_supply_current_defines(self):
  block=self.block().replace('-DSTORMTTY_HAS_BUNDLED_SHELL_RUNTIME=1','')
  other=self.block().replace('stormtty.dir','other.dir')
  for text in ('# -DSTORMTTY_HAS_BUNDLED_SHELL_RUNTIME=1\n'+block,other+block,block+other):self.rejects(text,'default discovery')
 def test_instrumentation_rejected_but_adjacent_probe_not_conflated(self):
  for marker in ('STORMTTY_PASSIVE_CONSOLE_TRACE','STORMTTY_QUALIFICATION_INPUT_EVIDENCE'):
   self.rejects(self.block().replace('-DUNICODE','-D'+marker),'instrumentation')
   other=self.block().replace('stormtty.dir','probe.dir').replace('-DUNICODE','-D'+marker)
   self.write(other+self.block()+other);self.assertTrue(self.verify()['verification_passed'])
 def test_required_configuration_and_official_only_validation_preserved(self):
  self.write(self.block())
  for key in layout.EXPECTED_CACHE:
   saved=self.fields.pop(key);self.cache()
   with self.assertRaisesRegex(ValueError,'configuration drift'):self.verify()
   self.fields[key]=saved;self.cache()
  with self.assertRaisesRegex(ValueError,'only official'):layout.verify_build(self.build,self.build/'candidate')
  with (self.build/'CMakeCache.txt').open('a') as stream:stream.write('\nSTORMTTY_BUNDLE_ZSH:BOOL=OFF')
  with self.assertRaisesRegex(ValueError,'duplicate build configuration'):self.verify()
 def test_failure_retains_only_bounded_relevant_evidence_and_selected_cache(self):
  wrong=self.block(True).replace('windows_main.cpp.obj:','windows_main.cpp.obj.extra:')
  unrelated='build unrelated: phony private-other-path\n'
  raw=self.write(unrelated+wrong)
  with (self.build/'CMakeCache.txt').open('a') as stream:stream.write('\nUNRELATED_PRIVATE_VALUE:STRING=not-for-upload')
  evidence=self.build/'published-contract.json'
  with self.assertRaisesRegex(ValueError,'expected one current app main object'):layout.verify_build(self.build,self.original,evidence)
  report=json.loads(evidence.read_text());self.assertFalse(report['verification_passed'])
  self.assertEqual(report['app_main_compile_candidates'],[wrong.rstrip('\n')]);self.assertEqual(report['ninja_sha256'],hashlib.sha256(raw).hexdigest())
  self.assertNotIn('private-other-path',evidence.read_text());self.assertNotIn('not-for-upload',evidence.read_text())
  self.assertEqual(set(report['selected_cache']),set(layout.CACHE_KEYS))
 def test_oversize_ninja_and_candidate_evidence_fail_closed(self):
  self.rejects('X'*(8*1024*1024+1),'oversize Ninja evidence')
  self.rejects(self.block()+'  FLAGS = '+'X'*(512*1024)+'\n','oversize app main candidate evidence')
  report=json.loads((self.build/'partial-app-build-contract.json').read_text());self.assertFalse(report['candidate_evidence_complete'])
  self.assertLess((self.build/'partial-app-build-contract.json').stat().st_size,256*1024)
  self.rejects(self.block()*9,'oversize app main candidate evidence')
 def test_preflight_cli_needs_no_source_runtime_builds_and_writes_failure_evidence(self):
  self.write(self.block(True));evidence=self.build/'cli-contract.json'
  command=[sys.executable,str(HERE/'app-layout.py'),'--verify-build','--build',str(self.build),'--original',str(self.original),'--build-evidence',str(evidence)]
  result=subprocess.run(command,capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stderr)
  self.assertTrue(json.loads(evidence.read_text())['verification_passed'])
  self.write('');result=subprocess.run(command,capture_output=True,text=True);self.assertNotEqual(result.returncode,0)
  self.assertFalse(json.loads(evidence.read_text())['verification_passed'])

class Collection(unittest.TestCase):
 def test_exact_bounded_build_diagnostics_allowlist(self):
  entries=dict(collector.candidates())
  self.assertEqual(entries[pathlib.Path('evidence/partial-app-build-contract.json')],256*1024)
  self.assertFalse(any(p.name in ('CMakeCache.txt','build.ninja') for p in entries))
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
 def test_build_preflight_precedes_runtime_builds_and_staging_rechecks(self):
  s=(REPO/'.github/workflows/msys-partial-app.yml').read_text()
  build=s.index('cmake --build build-partial-app');preflight=s.index('partial-app/app-layout.py --verify-build')
  source=s.index('- name: Download exact official complete source archive')
  stock=s.index('- name: Build uninstrumented stock runtime control');copied=s.index('- name: Build uninstrumented stock plus0007')
  staging=s.index('partial-app/app-layout.py --build')
  self.assertLess(build,preflight);self.assertLess(preflight,source);self.assertLess(source,stock);self.assertLess(stock,copied);self.assertLess(copied,staging)
  self.assertIn('if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }',s[preflight:].splitlines()[1])
  for offset in (preflight,staging):
   command=s[offset:].splitlines()[0]
   self.assertIn('--original "$env:EXPERIMENT_ROOT/original"',command)
   self.assertIn('--build-evidence "$env:EXPERIMENT_ROOT/evidence/partial-app-build-contract.json"',command)
  self.assertIn("contract=verify_build(build,roots['original'],evidence)",(HERE/'app-layout.py').read_text())
 def test_exact_new_branch_current_app_and_bounded_steps(self):
  s=(REPO/'.github/workflows/msys-partial-app.yml').read_text()
  self.assertIn("branches: ['experiment/msys-partial-app']",s);self.assertIn("paths: ['.github/workflows/msys-partial-app.yml']",s);self.assertIn('ref: ${{ github.sha }}',s)
  for forbidden in ('workflow_dispatch','pull_request:','continue-on-error','0006-','PASSIVE_CONSOLE_TRACE') :self.assertNotIn(forbidden,s)
  self.assertEqual(s.count('partial-app/compare.py'),3);self.assertEqual(s.count("if: always() && steps.staging.outcome == 'success'"),3)
  for value in ('STORMTTY_BUNDLE_ZSH=ON','STORMTTY_EXPERIMENTAL_COMPLETION_SEED=OFF','STORMTTY_TEST_PARTIAL_APP=ON','--verify-source','timeout-minutes: 330','timeout-minutes: 60','timeout-minutes: 24','timeout-minutes: 55','/review-packet/'):self.assertIn(value,s)
  self.assertIn('partial-app/collect.py',s);self.assertNotIn('runtimes/zsh/',s.split('uses: actions/upload-artifact@')[1])
if __name__=='__main__':unittest.main()
