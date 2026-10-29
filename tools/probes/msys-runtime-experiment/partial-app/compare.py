#!/usr/bin/env python3
"""Fixed uninstrumented app comparison; control failures never become passes."""
import argparse,importlib.util,json,os,pathlib,re,sys
HERE=pathlib.Path(__file__).resolve().parent;REPO=HERE.parents[3]
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);sys.modules[name]=m;spec.loader.exec_module(m);return m
identity=module('partial_app_identity',HERE/'identities.py')
layout=module('partial_app_layout',HERE/'app-layout.py')
owned=module('partial_app_owned',REPO/'tools/run-owned-process.py')
VARIANTS=('original','stock','copied');PROVIDERS=('inbox','packaged')
ORDERS=(('original','stock','copied'),('copied','original','stock'),('stock','copied','original'))
def plan(workload):
 for provider in PROVIDERS:
  if workload=='settings':
   for mode,count in (('explicit',10),('default',3)):
    for trial in range(1,count+1):
     for variant in ORDERS[(trial-1)%3]:yield dict(provider=provider,trial=trial,variant=variant,mode=mode)
  else:
   orders=ORDERS if workload=='raw' else (ORDERS[0],tuple(reversed(ORDERS[0])))
   for trial,order in enumerate(orders,1):
    for variant in order:yield dict(provider=provider,trial=trial,variant=variant,mode=workload)
def raw_summary(output):
 lines=[line for line in output.splitlines() if line.startswith(b'PASTE_RESULT ')]
 pattern=rb'PASTE_RESULT code=([01]) provider=(inbox|packaged) vt_off_control_failures=([0-4]) raw_vt_reader_failures=([0-8]) production_cause_established=0'
 if len(lines)!=1 or not (m:=re.fullmatch(pattern,lines[0])):return None
 return dict(code=int(m[1]),provider=m[2].decode(),vt_off_control_failures=int(m[3]),raw_vt_reader_failures=int(m[4]))
def assess(workload,provider,metadata,output):
 bounded=(metadata.get('reaped') is True and metadata.get('timed_out') is False and metadata.get('output_truncated') is False and metadata.get('cleanup_error')=='')
 record=dict(passed=False,complete=bounded,production_qualified=False)
 code=metadata.get('returncode')
 if workload=='raw':
  summary=raw_summary(output);record['summary']=summary
  complete=bounded and summary is not None and summary['provider']==provider and summary['code']==code
  record.update(complete=complete,passed=complete and code==0)
 elif workload=='managed':
  record['passed']=bounded and code==0 and output.splitlines().count(b'REFINED_MANAGED_RESULT fresh=1 warm=1 result=PASS')==1
 else:
  success=sum(line.startswith(b'Native CI smoke passed:') for line in output.splitlines())==1
  stages=[b'stage=native application locals destroyed',b'stage=native application returned to wWinMain']
  lifecycle=all(sum(line.endswith(stage) for line in output.splitlines())==1 for stage in stages)
  record.update(passed=bounded and code==0 and success and lifecycle,success_marker=success,lifecycle_complete=lifecycle)
 return record

def run(a):
 source=layout.verify_source();roots={n:getattr(a,n).resolve() for n in VARIANTS};ids=identity.identities(roots)
 app_roots={n:a.apps/n/'runtimes/zsh' for n in VARIANTS};identity.check(identity.identities(app_roots)==ids,'app runtimes differ')
 app_manifest=json.loads((a.apps/'app-copies.json').read_text());identity.check(app_manifest['runtimes']==ids,'app manifest differs')
 identity.check(app_manifest.get('qualification_only') is True and app_manifest.get('default_activation_allowed') is False and app_manifest.get('public_redistribution_ready') is False,'private app qualification receipt missing')
 identity.check(app_manifest['source_plan_sha256']==identity.sha(HERE/'app-plan.json'),'app plan receipt differs')
 paths={'probe':a.probe,'host':a.host,'native':a.native}
 for name,digest in source['packaged_provider_sha256'].items():
  identity.check(identity.sha(a.provider/name)==digest,'packaged provider drift');paths['provider/'+name]=a.provider/name
 for variant in VARIANTS:
  for name in layout.BINARIES:paths[variant+'/'+name]=a.apps/variant/name
 binaries={key:identity.sha(path) for key,path in paths.items()}
 for variant in VARIANTS:
  for name,digest in app_manifest['binaries'].items():identity.check(binaries[variant+'/'+name]==digest,'app binary mismatch')
 identity.check(not a.output.exists(),'new evidence output required; no overwrite/retry');a.output.mkdir(parents=True)
 captures=a.output/'private-captures'
 if a.workload=='settings':captures.mkdir()
 report=dict(schema=1,workload=a.workload,planned=len(list(plan(a.workload))),cases=[],identities=ids,app_plan=source,binaries=binaries,production_qualified=False,settings_cause_established=False,internal_path_coverage='UNINSTRUMENTED')
 destination=a.output/'results.json'
 def save():destination.write_text(json.dumps(report,indent=2)+'\n')
 save()
 for row in plan(a.workload):
  provider,variant=row['provider'],row['variant'];name=f"{provider}-{row['mode']}-{row['trial']}-{variant}"
  # Rehash immutable bytes before every fresh process; no test can silently swap DLLs.
  identity.check(identity.identities(roots)==ids and identity.identities(app_roots)==ids,'runtime changed during matrix')
  identity.check({key:identity.sha(path) for key,path in paths.items()}==binaries,'executable changed during matrix')
  if a.workload=='raw':
   command=[str(a.probe),str(roots[variant])];timeout=180
   if provider=='packaged':command.append(str(a.provider))
  elif a.workload=='managed':
   command=[str(a.host),str(roots[variant]),str(HERE/'managed-native/config.zsh'),str(a.native)];timeout=90
   if provider=='packaged':command.append(str(a.provider))
  else:
   command=[str(a.apps/variant/'stormtty.exe'),'--smoke-settings','--capture',str(captures/(name+'.png'))];timeout=30
   if row['mode']=='explicit':command+=['--shell-runtime',str(app_roots[variant])]
   if provider=='packaged':command+=['--console-runtime',str(a.provider)]
  record=dict(row,passed=False,complete=False)
  try:
   result=owned.run_owned(command,timeout=timeout,reap_timeout=10,tail_bytes=1048576,output_dir=a.output)
   (a.output/(name+'.log')).write_bytes(result.output_tail);record['runner']=result.metadata()
   record.update(assess(a.workload,provider,record['runner'],result.output_tail))
  except OSError as error:record['launch_error']=type(error).__name__
  report['cases'].append(record);save();print('PARTIAL_APP_TRIAL '+json.dumps(record,sort_keys=True),flush=True)
 identity.check(identity.identities(roots)==ids and identity.identities(app_roots)==ids,'runtime changed after matrix')
 identity.check({key:identity.sha(path) for key,path in paths.items()}==binaries,'executable changed after matrix')
 report['final_identity_verified']=True;save()
 return int(any(not row['passed'] for row in report['cases']))
if __name__=='__main__':
 p=argparse.ArgumentParser()
 for name in ('original','stock','copied','apps','provider','output','probe','host','native'):p.add_argument('--'+name,type=pathlib.Path,required=True)
 p.add_argument('--workload',choices=('raw','managed','settings'),required=True);a=p.parse_args()
 if os.name!='nt':p.error('Native Windows required')
 for name in ('original','stock','copied','apps','provider','output','probe','host','native'):
  path=getattr(a,name);identity.stage.reject_links(path.parent if name=='output' else path)
  setattr(a,name,path.resolve())
 raise SystemExit(run(a))
