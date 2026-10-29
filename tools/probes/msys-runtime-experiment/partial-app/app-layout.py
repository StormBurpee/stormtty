#!/usr/bin/env python3
"""New isolated executable-relative bundles; never changes compiled originals."""
import argparse,hashlib,importlib.util,json,pathlib,re,shutil,subprocess,sys
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
MAIN_OBJECT='CMakeFiles/stormtty.dir/src/app/windows_main.cpp.obj'
EXPECTED_CACHE={'STORMTTY_BUNDLE_ZSH':'ON','STORMTTY_EXPERIMENTAL_COMPLETION_SEED':'OFF','STORMTTY_TEST_PARTIAL_APP':'ON','CMAKE_BUILD_TYPE':'RelWithDebInfo'}
CACHE_KEYS=(*EXPECTED_CACHE,'CMAKE_CXX_COMPILER','STORMTTY_DEVELOPER_SHELL_RUNTIME')
def ninja_words(text):
 # A narrow literal-path lexer: backslash is a path separator, not an escape.
 # Expansions are reported, never evaluated or accepted in the selected edge.
 words=[];word='';expanded=False;i=0
 while i<len(text):
  c=text[i]
  if c=='$':
   if i+1<len(text) and text[i+1] in '$ :':word+=text[i+1];i+=2;continue
   expanded=True
  if c.isspace():
   if word:words.append(word);word=''
  else:word+=c
  i+=1
 if word:words.append(word)
 return words,expanded
def ninja_edges(text):
 # Read build statements plus only their indented bindings. Blank lines,
 # preceding comments, CRLF and adjacent edges cannot alter statement scope.
 physical=text.splitlines(keepends=True);lines=physical.copy();i=0
 def logical(index):
  value=''
  while index<len(lines):
   part=lines[index].rstrip('\r\n');index+=1
   continued=(len(part)-len(part.rstrip('$')))%2==1
   value+=part[:-1] if continued else part
   if not continued:return value,index
   if index<len(lines):lines[index]=lines[index].lstrip(' \t')
  raise ValueError('unterminated Ninja continuation')
 while i<len(lines):
  if not lines[i].startswith('build '):i+=1;continue
  start=i;header,i=logical(i);bindings=[]
  while i<len(lines) and lines[i].startswith((' ','\t')) and lines[i].strip():
   binding,i=logical(i);bindings.append(binding)
  yield header,bindings,''.join(physical[start:i]).rstrip('\r\n')
def app_main_edge(edges,build):
 main=[]
 for header,bindings,block in edges:
  match=re.fullmatch(r'build (.*?): (\S+)(?: (.*))?',header)
  if not match:
   identity.check(MAIN_OBJECT not in header.replace('\\','/'),'malformed current app main edge')
   continue
  outputs,expanded=ninja_words(match[1])
  if MAIN_OBJECT not in [p.replace('\\','/') for p in outputs]:continue
  main.append((outputs,expanded,match[2],match[3] or '',bindings,block))
 identity.check(len(main)==1,'expected one current app main object')
 outputs,expanded,rule,inputs,bindings,block=main[0]
 identity.check(not expanded and len(outputs)==1,'ambiguous current app main output')
 identity.check(rule in ('CXX_COMPILER__stormtty_RelWithDebInfo','CXX_COMPILER__stormtty_unscanned_RelWithDebInfo'),'unexpected current app main compiler rule')
 inputs,expanded=ninja_words(inputs);source=[]
 for token in inputs:
  if token in ('|','||','|@'):break
  source.append(token)
 identity.check(not expanded and len(source)==1,'expected one current app main source')
 identity.check((build/source[0].replace('\\','/')).resolve()==(REPO/'src/app/windows_main.cpp').resolve(),'wrong current app main source')
 identity.check('STORMTTY_PASSIVE' not in block and 'STORMTTY_QUALIFICATION' not in block,'app instrumentation unexpected')
 defines=[m[1] for line in bindings if (m:=re.fullmatch(r'\s+DEFINES\s*=\s*(.*)',line))]
 identity.check(len(defines)==1,'expected one app main DEFINES binding')
 flags,expanded=ninja_words(defines[0]);macro='STORMTTY_HAS_BUNDLED_SHELL_RUNTIME'
 discovery=[flag for flag in flags if re.match(r'[-/][DU]'+macro+r'(?:=|$)',flag)]
 identity.check(not expanded and discovery in (['-D'+macro+'=1'],['/D'+macro+'=1']),'default discovery not compiled exactly once')
 identity.check(not any(macro in line for line in bindings if not re.match(r'\s+DEFINES\s*=',line)),'unexpected default discovery override')
 return block
def verify_build(build,original,evidence=None):
 report=dict(schema=1,verification_passed=False,selected_cache={},app_main_compile_candidates=[],source_plan_sha256=identity.sha(HERE/'app-plan.json'),production_qualified=False)
 try:
  with (build/'build.ninja').open('rb') as stream:raw=stream.read(8*1024*1024+1)
  identity.check(len(raw)<=8*1024*1024,'oversize Ninja evidence')
  report['ninja_sha256']=hashlib.sha256(raw).hexdigest()
  edges=list(ninja_edges(raw.decode('utf-8')))
  # Keep only bounded relevant statements, including wrong/duplicate candidates.
  relevant=[block for header,_,block in edges if 'windows_main.cpp' in header]
  report['app_main_candidate_count']=len(relevant)
  report['candidate_evidence_complete']=len(relevant)<=8 and sum(len(b.encode('utf-8')) for b in relevant)<=64*1024
  if report['candidate_evidence_complete']:report['app_main_compile_candidates']=relevant
  with (build/'CMakeCache.txt').open('rb') as stream:cache=stream.read(1024*1024+1)
  identity.check(len(cache)<=1024*1024,'oversize CMake cache evidence')
  selected=report['selected_cache']
  for line in cache.decode('utf-8').splitlines():
   if ':' in line and '=' in line and not line.startswith(('#','//')):
    key,rest=line.split(':',1)
    if key in CACHE_KEYS:
     identity.check(key not in selected,'duplicate build configuration: '+key)
     value=rest.split('=',1)[1];identity.check(len(value)<=4096,'oversize selected build configuration: '+key);selected[key]=value
  identity.check(report['candidate_evidence_complete'],'oversize app main candidate evidence')
  report['app_main_compile_block']=app_main_edge(edges,build)
  for key,value in EXPECTED_CACHE.items():identity.check(selected.get(key)==value,'build configuration drift: '+key)
  identity.check(bool(selected.get('CMAKE_CXX_COMPILER')),'missing app compiler configuration')
  identity.check(bool(selected.get('STORMTTY_DEVELOPER_SHELL_RUNTIME')) and pathlib.Path(selected['STORMTTY_DEVELOPER_SHELL_RUNTIME']).resolve()==original.resolve(),'build must validate only official original')
  report['verification_passed']=True
 except (OSError,UnicodeError,ValueError) as error:
  report['error']=str(error);raise
 finally:
  encoded=json.dumps(report,indent=2)+'\n'
  (build/'partial-app-build-contract.json').write_text(encoded,encoding='utf-8')
  if evidence is not None:evidence.write_text(encoded,encoding='utf-8')
 return report
BINARIES=('stormtty.exe','stormtty-console-setup.exe','ghostty-vt.dll')
def stage(build,roots,destination,evidence=None):
 plan=verify_source();ids=identity.identities(roots)
 identity.check(not destination.exists(),'new app layout required')
 identity.stage.reject_links(build,recursive=True);identity.stage.reject_links(destination.parent)
 for source in (build,*roots.values()):
  identity.check(destination.resolve()!=source.resolve() and source.resolve() not in destination.resolve().parents,'app layout must be outside every source')
 contract=verify_build(build,roots['original'],evidence)
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
 p=argparse.ArgumentParser();mode=p.add_mutually_exclusive_group();mode.add_argument('--verify-source',action='store_true');mode.add_argument('--verify-build',action='store_true')
 for name in ('build','original','stock','copied','destination','build-evidence'):p.add_argument('--'+name,type=pathlib.Path)
 a=p.parse_args()
 if a.verify_source:print(json.dumps(verify_source(),indent=2))
 elif a.verify_build:
  if a.build is None or a.original is None:p.error('--build and --original required for build preflight')
  print(json.dumps(verify_build(a.build,a.original,a.build_evidence),indent=2))
 else:
  if any(getattr(a,n) is None for n in ('build','original','stock','copied','destination')):p.error('all stage paths required')
  print(json.dumps(stage(a.build,{n:getattr(a,n) for n in ('original','stock','copied')},a.destination,a.build_evidence),indent=2))
