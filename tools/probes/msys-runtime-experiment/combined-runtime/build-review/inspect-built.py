#!/usr/bin/env python3
"""Cold PE/object evidence extraction. Never loads candidate code or approves it."""
import argparse,hashlib,importlib.util,json
from pathlib import Path
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('combined_gate',HERE.parent/'linked-gate.py')
gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def inspect(build,variant):
    prov=build/'provenance';dll=build/'experimental-msys-2.0.dll';reader=build/'reader.exe'
    data=gate.bounded(dll,gate.pe_gate.MAX_DLL);exe=gate.bounded(reader,gate.pe_gate.MAX_DLL)
    pe=gate.pe_gate.PE(data);rp=gate.ReaderPE(exe)
    parsed=gate.pe_gate.functions(gate.bounded(prov/'console-native-disassembly.txt',32*1024*1024).decode())
    exports={name:pe.code_export(name) for name in gate.EXPORTS}
    handler=exports[gate.HANDLER]
    length,calls=gate.pe_gate.linked_body(pe,handler,*parsed[gate.HANDLER])
    emitters={target for name,target in calls if name==gate.pe_gate.EMITTER}
    if len(emitters)!=1:raise ValueError('emitter target ambiguity')
    emitter=next(iter(emitters));emit_length,emit_calls=gate.pe_gate.linked_body(pe,emitter,*parsed[gate.pe_gate.EMITTER])
    errno=pe.code_export('__errno')
    ranges={'handler':(handler,length),'emitter':(emitter,emit_length),'errno':(errno,len(gate.pe_gate.ERRNO_BYTES))}
    for label,(rva,size) in ranges.items():gate.pe_gate.dump(prov,dll,pe.base,label,rva,size)
    files=['generated-version.cc','dll-debuglink.txt','tool-binaries-before.txt','tool-binaries-after.txt','source-epoch.txt','build-time-environment.txt','makepkg.conf','compiler-packages.txt','compiler-mode.txt','console-native-disassembly.txt','console-native-symbols.txt',
           'reader-object-disassembly.txt','reader-object-symbols.txt','reader-linked-disassembly.txt','reader-pe.txt',
           'console.cc.before','console.cc.after','fhandler.h.after','termios.cc.after','diagnostic.patch','stormtty-combined-path.h']
    spec=importlib.util.spec_from_file_location('build_source_manifest',HERE/'source-manifest.py')
    manifest=importlib.util.module_from_spec(spec);spec.loader.exec_module(manifest)
    sources=manifest.check(prov/'source-before.json')
    if (prov/'tool-binaries-before.txt').read_bytes()!=(prov/'tool-binaries-after.txt').read_bytes():raise ValueError('tool binary drift')
    result=dict(schema=2,scope='cold-build-evidence-only',variant=variant,review_required=True,
        approved_for_fixture_execution=False,fixtures_executed=False,native_qualified=False,
        dll_sha256=hashlib.sha256(data).hexdigest(),reader_sha256=hashlib.sha256(exe).hexdigest(),
        provenance_sha256={name:digest(prov/name) for name in files},fixture_source_sha256=sources['fixture_source_sha256'],build_source_sha256=sources['build_source_sha256'],
        object_body_sha256={name:hashlib.sha256(parsed[name][1].encode()).hexdigest() for name in (gate.HANDLER,gate.pe_gate.EMITTER)},
        ranges={name:dict(rva=rva,size=size,sha256=hashlib.sha256(pe.at(rva,size)).hexdigest()) for name,(rva,size) in ranges.items()},
        handler_calls=calls,emitter_calls=emit_calls,reader_imports=sorted(set(rp.imports.values())),
        errno_matches_prior_leaf=pe.at(errno,len(gate.pe_gate.ERRNO_BYTES))==gate.pe_gate.ERRNO_BYTES,
        execution_blocker='New independent exact-DLL/reader/controller handler and import review required; rebuild invalidates prior binary hashes.')
    (prov/'build-review.json').write_text(json.dumps(result,indent=2)+'\n');return result
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('build',type=Path);p.add_argument('variant',choices=('copied','combined'));a=p.parse_args()
    try:result=inspect(a.build,a.variant)
    except (OSError,ValueError,KeyError,TypeError) as e:
        result=dict(schema=2,scope='cold-build-evidence-only',variant=a.variant,review_required=True,
                    approved_for_fixture_execution=False,fixtures_executed=False,native_qualified=False,error=str(e)[:300])
        (a.build/'provenance/build-review-error.json').write_text(json.dumps(result,indent=2)+'\n');raise
    print(json.dumps(result,indent=2))
