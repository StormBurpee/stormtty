#!/usr/bin/env python3
"""Extend the cold allowlist with exactly eight fixed synthetic trial paths."""
import argparse,hashlib,importlib.util,json,pathlib,stat
HERE=pathlib.Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('passive_build_collect',HERE/'collect-evidence.py');build=importlib.util.module_from_spec(spec);spec.loader.exec_module(build)
TRACE_SIZE=64+16384*128
def collect(root,destination):
 baseline=build.collect(root,destination);total=baseline['bytes']
 # The build-only collector cannot describe later runtime execution. Keep its
 # provenance inventory, but avoid a misleading packet-wide fixtures=false.
 baseline.pop('fixtures_executed',None)
 baseline['scope']='runtime-packet-build-provenance'
 baseline['runtime_outcomes_file']='evidence/native/results.json'
 (destination/'collection.json').write_text(json.dumps(baseline,indent=2)+'\n')
 result=dict(schema=1,scope='fixed-synthetic-runtime',production_qualified=False,files=[],missing=[],rejected=[])
 names=[(pathlib.Path('evidence/native/results.json'),1024*1024)]
 names += [(pathlib.Path('evidence')/name,1024*1024) for name in ('reader-pe-imports.txt','fixture-build.txt','linked-stock.log','linked-refined.log')]
 for trial in (1,2):
  for case in ('partial-zero','positive-prefix'):
   for variant in ('stock','refined'):
    parent=pathlib.Path('evidence/native')/f'{trial}-{case}-{variant}'
    names += [(parent/name,TRACE_SIZE if name=='trace.bin' else 65536) for name in ('trace.bin','reader.jsonl','controller.jsonl','launcher.log')]
 for relative,cap in names:
  source=root/relative
  if not source.exists():result['missing'].append(relative.as_posix());continue
  if not source.is_file() or any(stat.S_ISLNK(p.lstat().st_mode) or getattr(p.lstat(),'st_file_attributes',0)&0x400 for p in (source,*source.parents)):
   result['rejected'].append(dict(path=relative.as_posix(),reason='alias-or-nonregular'));continue
  with source.open('rb') as stream:data=stream.read(cap+1)
  if len(data)>cap or total+len(data)>build.TOTAL_LIMIT:
   result['rejected'].append(dict(path=relative.as_posix(),reason='size-cap'));continue
  target=destination/relative;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data);total+=len(data)
  result['files'].append(dict(path=relative.as_posix(),bytes=len(data),sha256=hashlib.sha256(data).hexdigest()))
 result['missing_build_expected']=[name for name in baseline['missing'] if not name.endswith('/linked-preflight-error.json')]
 result['total_packet_bytes']=total;result['collection_complete']=baseline['collection_complete'] and not result['missing_build_expected'] and not result['missing'] and not result['rejected']
 result['runtime_results_present']=(destination/'evidence/native/results.json').is_file()
 (destination/'runtime-collection.json').write_text(json.dumps(result,indent=2)+'\n');return result
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('root',type=pathlib.Path);p.add_argument('destination',type=pathlib.Path);a=p.parse_args();r=collect(a.root,a.destination)
 print(json.dumps({k:v for k,v in r.items() if k not in ('files','missing')},indent=2));raise SystemExit(int(not r['collection_complete']))
