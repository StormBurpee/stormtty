"""Bounded component-stage text only. Never copy trace blobs or candidate binaries."""
import argparse,hashlib,importlib.util,json,stat,sys
from pathlib import Path
HERE=Path(__file__).resolve().parent

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);sys.modules[name]=m;spec.loader.exec_module(m);return m
matrix=module('component_text_matrix',HERE.parent/'matrix.py')
trace=module('component_text_trace',HERE.parent/'trace.py')
LIMITS={'results.json':2*1024*1024,'fixture.json':65536,'assessment.json':65536,'launcher.log':65536,'trace.bin':trace.MAX_BYTES}
TOTAL=128*1024*1024

def reject_alias(path):
    for p in (path,*path.parents):
        try:info=p.lstat()
        except FileNotFoundError:continue
        if stat.S_ISLNK(info.st_mode) or getattr(info,'st_file_attributes',0)&0x400:raise ValueError('evidence alias')

def read(path,limit):
    reject_alias(path)
    if not path.is_file():raise ValueError('not regular evidence')
    with path.open('rb') as stream:data=stream.read(limit+1)
    if len(data)>limit:raise ValueError('evidence size bound')
    return data

def collect(source,destination):
    reject_alias(source);reject_alias(destination)
    if destination.exists():raise ValueError('new collection required')
    names=[Path('results.json')]
    for order,case,variant in matrix.trials():
        names += [Path(f'{order}-{case}-{variant}')/n for n in ('fixture.json','assessment.json','launcher.log','trace.bin')]
    selected=[];total=0
    for name in names:
        path=source/name
        reject_alias(path)
        if not path.exists():continue
        data=read(path,LIMITS[name.name])
        if name.name=='trace.bin':
            digest=hashlib.sha256(data).hexdigest()
            try:value={'schema':1,'raw_trace_sha256':digest,'decoded':trace.decode(data)}
            except trace.TraceError as error:value={'schema':1,'raw_trace_sha256':digest,'verdict':'INCONCLUSIVE','error':str(error)[:300]}
            data=(json.dumps(value,sort_keys=True)+'\n').encode();name=name.with_name('trace.json')
            if len(data)>8*1024*1024:raise ValueError('decoded trace bound')
        else:
            data.decode('utf-8-sig')
            if b'\0' in data or data.startswith((b'MZ',b'PK\x03\x04')):raise ValueError('not diagnostic text')
        total+=len(data)
        if total>TOTAL:raise ValueError('text collection total bound')
        selected.append((name,data))
    destination.mkdir(parents=True)
    files=[]
    for name,data in selected:
        path=destination/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
        files.append(dict(path=name.as_posix(),bytes=len(data),sha256=hashlib.sha256(data).hexdigest()))
    record=dict(schema=1,scope='15-case-component-text',native_qualified=False,runtime_promotion_allowed=False,files=files)
    (destination/'manifest.json').write_text(json.dumps(record,indent=2)+'\n');return record
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('destination',type=Path);a=p.parse_args();collect(a.source,a.destination)
