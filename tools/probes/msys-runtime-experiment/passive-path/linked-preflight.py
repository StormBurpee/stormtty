#!/usr/bin/env python3
"""Bounded PE/linked-code gate for the reviewed diagnostic, not general safety proof."""
import argparse,hashlib,json,pathlib,re,struct,subprocess
HERE=pathlib.Path(__file__).resolve().parent
MAX_DLL=64*1024*1024
HANDLER='stormtty_path_handler_v1'
EMITTER='_ZN7st_path12emit_contextEPNS_7ContextENS_5EventEmmmmmmmmmmtt'
# Reviewed x86_64 TLS accessor from the exact original bundled runtime; no call.
# mov rax,gs:[8]; sub rax,12800; ret (errno is first field of _reent).
ERRNO_BYTES=bytes.fromhex('65488b042508000000482d00320000c3')
OS_ACCESSORS={'GetLastError','SetLastError','GetCurrentThreadId'}
class PE:
 def __init__(self,data):
  self.data=data
  if len(data)>MAX_DLL or data[:2]!=b'MZ':raise ValueError('DLL bounds/signature')
  nt=self.unpack('<I',60)[0]
  if self.take(nt,4)!=b'PE\0\0' or self.unpack('<H',nt+4)[0]!=0x8664:raise ValueError('not PE AMD64')
  count=self.unpack('<H',nt+6)[0];size=self.unpack('<H',nt+20)[0];opt=nt+24
  if not 1<=count<=96 or size<240 or self.unpack('<H',opt)[0]!=0x20b:raise ValueError('PE32+ header')
  self.base=self.unpack('<Q',opt+24)[0]
  if self.unpack('<I',opt+108)[0]<2:raise ValueError('missing directories')
  self.export=self.unpack('<II',opt+112);self.imports_directory=self.unpack('<II',opt+120)
  self.sections=[self.unpack('<8sIIII',opt+size+i*40) for i in range(count)]
  for _,_,_,length,raw in self.sections:self.take(raw,length)
  self.exports=self.read_exports();self.imports=self.read_imports()
 def take(self,offset,size):
  if offset<0 or size<0 or offset+size>len(self.data):raise ValueError('PE truncated range')
  return self.data[offset:offset+size]
 def unpack(self,fmt,offset):return struct.unpack(fmt,self.take(offset,struct.calcsize(fmt)))
 def offset(self,rva,size=1):
  candidates=[raw+rva-va for _,_,va,length,raw in self.sections if va<=rva and rva+size<=va+length]
  if len(candidates)!=1:raise ValueError('unmapped/ambiguous RVA')
  self.take(candidates[0],size);return candidates[0]
 def at(self,rva,size):return self.take(self.offset(rva,size),size)
 def string(self,rva):
  value=bytearray()
  for i in range(256):
   b=self.at(rva+i,1)[0]
   if not b:return bytes(value).decode('ascii')
   value.append(b)
  raise ValueError('oversize PE name')
 def read_exports(self):
  rva,size=self.export
  if not rva or size<40:raise ValueError('missing exports')
  fields=self.unpack('<IIHHIIIIIII',self.offset(rva,40));nf,nn,functions,names,ordinals=fields[6:]
  if not 1<=nn<=nf<=16384:raise ValueError('export bounds')
  result={}
  for i in range(nn):
   name_rva=struct.unpack('<I',self.at(names+4*i,4))[0];name=self.string(name_rva)
   ordinal=struct.unpack('<H',self.at(ordinals+2*i,2))[0]
   if ordinal>=nf or name in result:raise ValueError('export ordinal/duplicate')
   target=struct.unpack('<I',self.at(functions+4*ordinal,4))[0]
   # Preserve other forwarded exports but reject them when requested as code.
   result[name]=target
  return result
 def code_export(self,name):
  target=self.exports.get(name,0);rva,size=self.export
  if not target or rva<=target<rva+size:raise ValueError('missing/forwarded code export '+name)
  self.at(target,1);return target
 def read_imports(self):
  rva,size=self.imports_directory;result={}
  if not rva or size<20:raise ValueError('missing imports')
  for i in range(min(size//20,128)):
   original,stamp,chain,name,first=struct.unpack('<IIIII',self.at(rva+i*20,20))
   if not any((original,stamp,chain,name,first)):return result
   dll=self.string(name).lower();table=original or first
   for j in range(8192):
    item=struct.unpack('<Q',self.at(table+j*8,8))[0]
    if not item:break
    if not item>>63:
     key=first+j*8
     if key in result:raise ValueError('duplicate IAT slot')
     result[key]=(dll,self.string(item+2))
   else:raise ValueError('import thunk bound')
  raise ValueError('unterminated imports')
 def os_thunk(self,rva,name):
  code=self.at(rva,6)
  if code[:2]!=b'\xff\x25':raise ValueError('non-IAT OS thunk '+name)
  target=rva+6+struct.unpack('<i',code[2:])[0]
  if self.imports.get(target)!=('kernel32.dll',name):raise ValueError('wrong OS accessor import '+name)
  return target

def functions(text):
 parts=re.split(r'(?m)^([0-9a-f]+ <[^\n]+>):\n',text);result={}
 for heading,body in zip(parts[1::2],parts[2::2]):
  start=int(heading.split()[0],16);name=heading.split('<',1)[1][:-1]
  if name in result:raise ValueError('duplicate function body')
  result[name]=(start,body)
 return result

def object_code(start,body):
 data={};relocations=[]
 for line in body.splitlines():
  match=re.match(r'\s*([0-9a-f]+):\s+IMAGE_REL_AMD64_REL32\s+(\S+)\s*$',line)
  if match:relocations.append((int(match[1],16)-start,match[2]));continue
  if 'IMAGE_REL_' in line:raise ValueError('unreviewed relocation kind')
  match=re.match(r'\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}[ \t]+)+)',line)
  if match:
   address=int(match[1],16)-start;raw=bytes.fromhex(match[2])
   for i,byte in enumerate(raw):
    if address+i in data:raise ValueError('duplicate instruction bytes')
    data[address+i]=byte
 if not data or min(data)!=0 or max(data)>=16384 or len(data)!=max(data)+1:raise ValueError('instruction range/gap')
 return bytes(data[i] for i in range(len(data))),relocations

def linked_body(pe,rva,start,body):
 code,relocations=object_code(start,body);actual=pe.at(rva,len(code));mask=set()
 for offset,_ in relocations:
  if offset<0 or offset+4>len(code):raise ValueError('relocation bounds')
  mask.update(range(offset,offset+4))
 if any(a!=b for i,(a,b) in enumerate(zip(code,actual)) if i not in mask):raise ValueError('linked/object instruction mismatch')
 calls=[]
 for offset,target in relocations:
  if offset and code[offset-1] in (0xe8,0xe9):
   calls.append((target,rva+offset+4+struct.unpack('<i',actual[offset:offset+4])[0]))
  elif target not in ('.bss','.rdata','.text'):raise ValueError('unexpected non-call relocation '+target)
 return len(code),calls

def dump(provenance,dll,base,label,rva,size):
 path=provenance/('linked-'+label+'-disassembly.txt')
 with path.open('xb') as output:
  subprocess.run(['objdump','-d','--start-address='+hex(base+rva),'--stop-address='+hex(base+rva+size),str(dll)],stdout=output,stderr=subprocess.STDOUT,check=True,timeout=20)
 if path.stat().st_size>1024*1024:raise ValueError('disassembly cap')

def inspect(build,variant):
 provenance=build/'provenance';dll=build/'experimental-msys-2.0.dll'
 with dll.open('rb') as stream:data=stream.read(MAX_DLL+1)
 digest=hashlib.sha256(data).hexdigest()
 if digest!=(provenance/'hashes.txt').read_text().split()[0]:raise ValueError('DLL changed after build')
 review=json.loads((HERE/'native-review-40916378419.json').read_text())['variants'][variant]
 for name in ('console.cc.after','passive.patch','stormtty-passive-path.h','compiler-packages.txt'):
  if hashlib.sha256((provenance/name).read_bytes()).hexdigest()!=review['files_sha256'][name]:raise ValueError('review input drift '+name)
 parsed=functions((provenance/'console-native-disassembly.txt').read_text())
 for name,expected in review['observer_helpers'].items():
  if name not in parsed or hashlib.sha256(parsed[name][1].encode()).hexdigest()!=expected['disassembly_body_sha256']:raise ValueError('reviewed observer body drift '+name)
 pe=PE(data);errno=pe.code_export('__errno')
 dump(provenance,dll,pe.base,'errno',errno,len(ERRNO_BYTES))
 if pe.at(errno,len(ERRNO_BYTES))!=ERRNO_BYTES:raise ValueError('linked errno is not reviewed leaf TLS instruction sequence')
 handler=pe.code_export(HANDLER)
 dump(provenance,dll,pe.base,'handler',handler,len(object_code(*parsed[HANDLER])[0]))
 length,calls=linked_body(pe,handler,*parsed[HANDLER])
 emitter_targets={target for name,target in calls if name==EMITTER}
 if len(emitter_targets)!=1:raise ValueError('emitter target ambiguity')
 emitter=next(iter(emitter_targets))
 dump(provenance,dll,pe.base,'emitter',emitter,len(object_code(*parsed[EMITTER])[0]))
 emit_length,emit_calls=linked_body(pe,emitter,*parsed[EMITTER])
 accessor_targets={}
 for name,target in calls+emit_calls:
  if name==EMITTER:
   if target!=emitter:raise ValueError('wrong emitter')
  elif name=='__errno':
   if target!=errno:raise ValueError('errno call does not resolve to verified export')
  elif name in OS_ACCESSORS:
   slot=pe.os_thunk(target,name)
   if name in accessor_targets and accessor_targets[name]!=(target,slot):raise ValueError('accessor target ambiguity')
   accessor_targets[name]=(target,slot)
  else:raise ValueError('unexpected linked recorder callee '+name)
 if set(accessor_targets)!=OS_ACCESSORS:raise ValueError('missing resolved accessor')
 ranges={'errno':(errno,len(ERRNO_BYTES)),'handler':(handler,length),'emitter':(emitter,emit_length)}
 result=dict(schema=1,scope='fixed-diagnostic-linked-preflight',variant=variant,dll_sha256=digest,
             linked_code_gate_passed=True,production_qualified=False,general_async_signal_safety_proven=False,
             os_tls_contract_assumption=True,fixtures_executed=False,
             verified_ranges={name:dict(rva=rva,bytes=size,sha256=hashlib.sha256(pe.at(rva,size)).hexdigest()) for name,(rva,size) in ranges.items()},
             resolved_os_accessors={name:dict(thunk_rva=value[0],iat_rva=value[1],dll='kernel32.dll') for name,value in sorted(accessor_targets.items())})
 (provenance/'linked-preflight.json').write_text(json.dumps(result,indent=2)+'\n');return result
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('build',type=pathlib.Path);p.add_argument('variant',choices=('stock','refined'));a=p.parse_args()
 try:result=inspect(a.build,a.variant)
 except (OSError,ValueError,subprocess.SubprocessError) as error:
  result=dict(schema=1,variant=a.variant,linked_code_gate_passed=False,fixtures_executed=False,review_required=True,error_type=type(error).__name__,reason=str(error)[:240])
  (a.build/'provenance/linked-preflight-error.json').write_text(json.dumps(result,indent=2)+'\n')
  print(json.dumps(result,indent=2));raise SystemExit(1)
 print(json.dumps(result,indent=2))
