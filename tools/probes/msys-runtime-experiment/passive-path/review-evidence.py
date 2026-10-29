#!/usr/bin/env python3
"""Check structural native evidence, not async-signal safety or path coverage."""
import argparse,hashlib,json,pathlib,re
LIMIT=32*1024*1024
EXPORTS=('stormtty_path_init_v1','stormtty_path_handler_v1')
def read(path):
 with path.open('rb') as stream: raw=stream.read(LIMIT+1)
 if len(raw)>LIMIT:raise ValueError('oversize native evidence')
 return raw.decode('utf-8',errors='strict')
def review(provenance):
 pe=read(provenance/'diagnostic-dll-pe.txt')
 # Names must occur in the actual PE export-name table, not imports or symbols.
 match=re.search(r'\[Ordinal/Name Pointer\] Table(?: -- Ordinal Base [0-9]+)?[ \t]*\n(.*?)(?:\n\n|\Z)',pe,re.S)
 if not match:raise ValueError('missing PE export-name table')
 names=re.findall(r'^[ \t]*\[[ \t]*\d+\][ \t]+(?:\+base\[[ \t]*\d+\][ \t]+[0-9a-fA-F]+[ \t]+)?(\S+)[ \t]*$',match.group(1),re.M)
 if any(names.count(name)!=1 for name in EXPORTS):raise ValueError('missing/duplicate diagnostic export')
 disassembly=read(provenance/'console-native-disassembly.txt')
 bodies={}
 for name in EXPORTS:
  match=re.search(r'^([0-9a-fA-F]+) <'+re.escape(name)+r'>:\n(.*?)(?=^[0-9a-fA-F]+ <|\Z)',disassembly,re.M|re.S)
  if not match or not re.search(r'^\s*[0-9a-fA-F]+:\s+[0-9a-fA-F]{2}',match.group(2),re.M):raise ValueError('missing actual runtime function body: '+name)
  bodies[name]=match.group(0)
 (provenance/'runtime-recorder-exports-disassembly.txt').write_text(''.join(bodies.values()),encoding='utf-8')
 result=dict(schema=1,scope='build-and-static-evidence-only',exports_verified=list(EXPORTS),
             real_runtime_translation_unit=True,async_signal_safety_verified=False,
             review_required=True,fixtures_executed=False,path_coverage='NOT_EXECUTED',
             production_qualified=False,
             evidence_sha256={name:hashlib.sha256((provenance/name).read_bytes()).hexdigest() for name in ('diagnostic-dll-pe.txt','console-native-disassembly.txt','console-native-symbols.txt','runtime-recorder-exports-disassembly.txt')})
 (provenance/'static-review.json').write_text(json.dumps(result,indent=2)+'\n')
 return result
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('provenance',type=pathlib.Path);a=p.parse_args();print(json.dumps(review(a.provenance),indent=2))
