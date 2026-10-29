#!/usr/bin/env python3
"""New isolated executable-relative bundles; never changes compiled originals."""
import argparse,hashlib,importlib.util,json,pathlib,shutil,subprocess,sys
HERE=pathlib.Path(__file__).resolve().parent;REPO=HERE.parents[3]
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);sys.modules[name]=m;spec.loader.exec_module(m);return m
identity=module('app_runtime_id',HERE/'identities.py')
def verify_source():
 plan=json.loads((HERE/'app-plan.json').read_text())
 for name,digest in plan['source_trees'].items():
  actual=subprocess.check_output(['git','rev-parse','HEAD:'+name],cwd=REPO,text=True).strip()
  identity.check(actual==digest,'current app tree differs: '+name)
 for name,digest in plan['source_files'].items():identity.check(identity.sha(REPO/name)==digest,'current app file differs: '+name)
 for name,digest in plan['managed_fixture_files'].items():identity.check(identity.sha(HERE/'managed-native'/name)==digest,'managed fixture differs: '+name)
 return plan
def verify_build(build,original):
 with (build/'build.ninja').open('rb') as stream:raw=stream.read(8*1024*1024+1)
 identity.check(len(raw)<=8*1024*1024,'oversize Ninja evidence')
 text=raw.decode('utf-8');blocks=text.split('\n\n')
 main=[block for block in blocks if block.startswith('build CMakeFiles/stormtty.dir/src/app/windows_main.cpp.obj:')]
 identity.check(len(main)==1,'expected one current app main object')
 identity.check('STORMTTY_HAS_BUNDLED_SHELL_RUNTIME=1' in main[0],'default discovery not compiled')
 identity.check('STORMTTY_PASSIVE' not in main[0] and 'STORMTTY_QUALIFICATION' not in main[0],'app instrumentation unexpected')
 selected={}
 for line in (build/'CMakeCache.txt').read_text().splitlines():
  if ':' in line and '=' in line and not line.startswith(('#','//')):
   key,rest=line.split(':',1);selected[key]=rest.split('=',1)[1]
 expected={'STORMTTY_BUNDLE_ZSH':'ON','STORMTTY_EXPERIMENTAL_COMPLETION_SEED':'OFF','STORMTTY_TEST_PARTIAL_APP':'ON','CMAKE_BUILD_TYPE':'RelWithDebInfo'}
 for key,value in expected.items():identity.check(selected.get(key)==value,'build configuration drift: '+key)
 identity.check(pathlib.Path(selected.get('STORMTTY_DEVELOPER_SHELL_RUNTIME','')).resolve()==original.resolve(),'build must validate only official original')
 report=dict(schema=1,selected_cache={key:selected[key] for key in (*expected,'CMAKE_CXX_COMPILER','STORMTTY_DEVELOPER_SHELL_RUNTIME')},app_main_compile_block=main[0],ninja_sha256=hashlib.sha256(raw).hexdigest(),source_plan_sha256=identity.sha(HERE/'app-plan.json'),production_qualified=False)
 (build/'partial-app-build-contract.json').write_text(json.dumps(report,indent=2)+'\n');return report
BINARIES=('stormtty.exe','stormtty-console-setup.exe','ghostty-vt.dll')
def stage(build,roots,destination):
 plan=verify_source();ids=identity.identities(roots)
 identity.check(not destination.exists(),'new app layout required')
 identity.stage.reject_links(build,recursive=True);identity.stage.reject_links(destination.parent)
 for source in (build,*roots.values()):
  identity.check(destination.resolve()!=source.resolve() and source.resolve() not in destination.resolve().parents,'app layout must be outside every source')
 contract=verify_build(build,roots['original'])
 binaries={name:identity.sha(build/name) for name in BINARIES}
 destination.mkdir();apps={}
 for variant in ('original','stock','copied'):
  target=destination/variant;target.mkdir()
  for name in BINARIES:shutil.copyfile(build/name,target/name)
  (target/'runtimes').mkdir();shutil.copytree(roots[variant],target/'runtimes/zsh')
  (target/'EXPERIMENTAL-NOT-FOR-RELEASE.txt').write_text('Private uninstrumented app qualification only. Not normal-stager or delivery eligible. Never install or activate.\n')
  apps[variant]=target
 copied_roots={name:path/'runtimes/zsh' for name,path in apps.items()}
 identity.check(identity.identities(copied_roots)==ids,'runtime copies differ')
 for root in apps.values():
  for name,digest in binaries.items():identity.check(identity.sha(root/name)==digest,'app copy differs')
 identity.check(identity.identities(roots)==ids,'original runtime roots changed')
 report=dict(schema=1,build_contract=contract,app_plan=plan,source_plan_sha256=identity.sha(HERE/'app-plan.json'),binaries=binaries,runtimes=ids,production_qualified=False,qualification_only=True,default_activation_allowed=False,public_redistribution_ready=False)
 (destination/'app-copies.json').write_text(json.dumps(report,indent=2)+'\n');return report
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--verify-source',action='store_true')
 for name in ('build','original','stock','copied','destination'):p.add_argument('--'+name,type=pathlib.Path)
 a=p.parse_args()
 if a.verify_source:print(json.dumps(verify_source(),indent=2))
 else:
  if any(getattr(a,n) is None for n in ('build','original','stock','copied','destination')):p.error('all stage paths required')
  print(json.dumps(stage(a.build,{n:getattr(a,n) for n in ('original','stock','copied')},a.destination),indent=2))
