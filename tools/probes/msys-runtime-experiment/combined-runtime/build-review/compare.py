#!/usr/bin/env python3
"""Compare two independent cold-build text collections. Never authorize execution."""
import argparse,hashlib,json,re
from pathlib import Path
MAX_FILE=32*1024*1024
HEX=re.compile(r'[0-9a-f]{64}')
def pairs(items):
    result={}
    for key,value in items:
        if key in result:raise ValueError('duplicate JSON key')
        result[key]=value
    return result

def read_json(data):
    return json.loads(data.decode('utf-8-sig'),object_pairs_hook=pairs,
                      parse_constant=lambda value:(_ for _ in ()).throw(ValueError('nonfinite JSON')))

def read(path):
    if any(p.is_symlink() for p in (path,*path.parents)):raise ValueError('symlink in evidence path')
    with path.open('rb') as stream:data=stream.read(MAX_FILE+1)
    if len(data)>MAX_FILE:raise ValueError('evidence bound')
    return data

def collection(root):
    manifest=read_json(read(root/'collection.json'))
    if not isinstance(manifest,dict) or manifest.get('schema')!=2 or manifest.get('scope')!='cold-build-text-only' or manifest.get('collection_complete') is not True or manifest.get('both_build_reports_present') is not True:
        raise ValueError('incomplete build collection')
    if manifest.get('fixtures_executed') is not False or manifest.get('rejected')!=[]:raise ValueError('invalid collection scope')
    entries=manifest.get('files');result={}
    if not isinstance(entries,list) or not 1<=len(entries)<=200:raise ValueError('manifest file bound')
    for entry in entries:
        name=entry['path'];path=Path(name)
        if not isinstance(name,str) or path.is_absolute() or '..' in path.parts or '\\' in name or name in result:raise ValueError('invalid manifest path')
        data=read(root/path)
        if type(entry['bytes']) is not int or len(data)!=entry['bytes'] or hashlib.sha256(data).hexdigest()!=entry['sha256']:raise ValueError('manifest hash mismatch: '+name)
        result[name]=data
    return result

def identity(files,repeat):
    def value(name):return read_json(files[name])
    def digest(value):
        if not isinstance(value,str) or not HEX.fullmatch(value):raise ValueError('invalid SHA256')
        return value
    run=value('evidence/independent-build.json')
    if type(run.get('repeat')) is not int or run['repeat']!=repeat or run.get('clean_root') is not True:raise ValueError('independent clean build marker missing')
    if not all(isinstance(run.get(k),str) and run[k] for k in ('run_id','run_attempt','commit')):raise ValueError('run identity missing')
    if not re.fullmatch('[0-9a-f]{40}',run['commit']):raise ValueError('invalid source commit')
    result={'run':{k:run[k] for k in ('run_id','run_attempt','commit')},'variants':{}}
    snapshots=[]
    for variant in ('copied','combined'):
        prefix=variant+'-build/provenance/'
        report=value(prefix+'build-review.json')
        if report.get('schema')!=2 or report.get('scope')!='cold-build-evidence-only' or report.get('variant')!=variant or report.get('fixtures_executed') is not False or report.get('approved_for_fixture_execution') is not False or report.get('review_required') is not True or report.get('native_qualified') is not False or 'error' in report:raise ValueError('invalid cold report')
        snapshot=value(prefix+'source-before.json');snapshots.append(snapshot)
        if report['fixture_source_sha256']!=snapshot['fixture_source_sha256'] or report['build_source_sha256']!=snapshot['build_source_sha256']:raise ValueError('source report mismatch')
        provenance=report['provenance_sha256']
        for name,sha in provenance.items():
            if hashlib.sha256(files[prefix+name]).hexdigest()!=digest(sha):raise ValueError('report provenance mismatch')
        names=('tool-binaries-before.txt','tool-binaries-after.txt','source-epoch.txt','build-time-environment.txt','makepkg.conf','compiler-packages.txt','compiler-mode.txt')
        if any(name not in provenance for name in names):raise ValueError('required provenance absent')
        if files[prefix+'tool-binaries-before.txt']!=files[prefix+'tool-binaries-after.txt']:raise ValueError('toolchain changed during build')
        tools=files[prefix+'tool-binaries-before.txt'].decode('utf-8').splitlines()
        if len(tools)<5 or any(not re.fullmatch(r'[0-9a-f]{64}  .+',line) for line in tools):raise ValueError('incomplete toolchain hashes')
        epoch=files[prefix+'source-epoch.txt'].decode('utf-8').splitlines()
        if len(epoch)!=3 or epoch[:2]!=['867eb7748e6e5a0e8430ce9f46af27b4b1e8f204','1790890720']:raise ValueError('source epoch mismatch')
        if files[prefix+'build-time-environment.txt'].decode('utf-8').strip()!='SOURCE_DATE_EPOCH=1790890720':raise ValueError('build epoch mismatch')
        result['variants'][variant]={'dll':digest(report['dll_sha256']),'reader':digest(report['reader_sha256']),
            'source':snapshot,'metadata':{name:digest(provenance[name]) for name in names}}
    controller=value('controller/controller-build-review.json')
    if controller.get('schema')!=2 or controller.get('scope')!='controller-build-only' or controller.get('fixtures_executed') is not False or controller.get('review_required') is not True:raise ValueError('invalid controller scope')
    before=value('controller/tool-binaries-before.json');after=value('controller/tool-binaries-after.json')
    if before!=after or not isinstance(before,list) or not before:raise ValueError('controller toolchain changed or absent')
    for tool in before:digest(tool['sha256'])
    snapshot=value('controller/source-before.json');snapshots.append(snapshot)
    if any(snapshot!=snapshots[0] for snapshot in snapshots):raise ValueError('source snapshots differ within build')
    if not snapshots[0].get('fixture_source_sha256') or not snapshots[0].get('build_source_sha256'):raise ValueError('empty source identities')
    for mapping in ('fixture_source_sha256','build_source_sha256'):
        for sha in snapshots[0][mapping].values():digest(sha)
    result['controller']={key:digest(controller[key]) for key in ('controller_sha256','object_sha256','source_sha256')}
    result['controller']['tools']=before
    result['controller']['config']=value('controller/build-config.json')
    if not result['controller']['config']:raise ValueError('controller configuration missing')
    return result

def compare(first,second):
    result={'schema':1,'scope':'two-clean-build-cold-evidence','fixtures_executed':False,
            'native_qualified':False,'approved_for_fixture_execution':False,'independent_linked_review_required':True,
            'verdict':'INCONCLUSIVE','exact_match':False}
    try:
        a=identity(collection(first),1);b=identity(collection(second),2)
        if a['run']!=b['run']:raise ValueError('different workflow/source identities')
        result['identities']=[a,b]
        result['exact_match']=a==b
        result['verdict']='EXACT_MATCH' if a==b else 'MISMATCH'
    except (OSError,ValueError,KeyError,TypeError,AttributeError) as error:result['reason']=str(error)[:300]
    return result
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('first',type=Path);parser.add_argument('second',type=Path);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    result=compare(args.first,args.second);args.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2));raise SystemExit(0 if result['exact_match'] else 1)
