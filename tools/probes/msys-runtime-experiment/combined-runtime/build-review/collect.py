#!/usr/bin/env python3
"""Export a stopped build's bounded text allowlist, never binaries or packages."""
import argparse,hashlib,json,stat
from pathlib import Path
FILE_LIMIT=32*1024*1024
TOTAL_LIMIT=128*1024*1024
PROVENANCE=('generated-version.cc','dll-debuglink.txt','tool-binaries-before.txt','tool-binaries-after.txt','reader-linker-help.txt','source-epoch.txt','makepkg.conf','build-time-environment.txt','source-before.json','combined-plan.md','variant.txt','compiler-mode.txt','compiler-packages.txt','selected-toolchain.txt',
 'system-toolchain.txt','pacman-before.txt','pacman-after.txt','configured-cxx.txt','hashes.txt','package-hashes.txt',
 'console-before.cc','console-after.cc','console.cc.before','console.cc.after','termios.cc.before','termios.cc.after',
 'fhandler.h.before','fhandler.h.after','PKGBUILD','0007-preserve-copied-on-signal.patch','0006-refined-prefix.patch',
 '0000-utils-const-compat.patch','all-packaging-and-experimental.patch','diagnostic.patch','stormtty-combined-path.h',
 'diagnostic-identities.json','diagnostic-dll-pe.txt','console-native-disassembly.txt','console-native-symbols.txt',
 'console-native-object-sha256.txt','reader-object-disassembly.txt','reader-object-symbols.txt','reader-pe.txt',
 'reader-linked-disassembly.txt','reader-hashes.txt','linked-errno-disassembly.txt','linked-handler-disassembly.txt',
 'linked-emitter-disassembly.txt','build-review.json','build-review-error.json')
CONTROLLER=('build-config.json','tool-binaries-before.json','tool-binaries-after.json','compiler-help.txt','linker-help.txt','source-before.json','compiler-sha256.txt','compiler.log','link.log','controller-pe.txt','controller-disassembly.txt',
 'controller-symbols.txt','controller-build-review.json')
def collect(root,destination):
    if destination.exists():raise ValueError('new output required')
    destination.mkdir(parents=True)
    paths=[Path(v+'-build/provenance')/n for v in ('copied','combined') for n in PROVENANCE]
    paths += [Path('controller')/n for n in CONTROLLER]
    paths += [Path('evidence')/n for n in ('independent-build.json','commit.txt','source-identity.txt','gcc15-preflight.log','copied-build.log','combined-build.log')]
    report=dict(schema=2,scope='cold-build-text-only',fixtures_executed=False,review_required=True,files=[],missing=[],rejected=[])
    total=0
    for relative in paths:
        source=root/relative
        if not source.exists():report['missing'].append(relative.as_posix());continue
        if not source.is_file() or any(stat.S_ISLNK(p.lstat().st_mode) or getattr(p.lstat(),'st_file_attributes',0)&0x400 for p in (source,*source.parents)):
            report['rejected'].append(relative.as_posix());continue
        size=source.stat().st_size
        if size>FILE_LIMIT or total+size>TOTAL_LIMIT:report['rejected'].append(relative.as_posix());continue
        with source.open('rb') as stream:data=stream.read(FILE_LIMIT+1)
        if len(data)!=size:report['rejected'].append(relative.as_posix());continue
        try:data.decode('utf-8-sig')
        except UnicodeDecodeError:report['rejected'].append(relative.as_posix());continue
        if data.startswith((b'MZ',b'PK\x03\x04',b'\x28\xb5\x2f\xfd')) or b'\0' in data:
            report['rejected'].append(relative.as_posix());continue
        target=destination/relative;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data);total+=size
        report['files'].append(dict(path=relative.as_posix(),bytes=size,sha256=hashlib.sha256(data).hexdigest()))
    report['bytes']=total;report['collection_complete']=not report['rejected']
    report['both_build_reports_present']=all((destination/(v+'-build/provenance/build-review.json')).is_file() for v in ('copied','combined'))
    (destination/'collection.json').write_text(json.dumps(report,indent=2)+'\n');return report
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('destination',type=Path);a=p.parse_args();r=collect(a.root,a.destination);print(json.dumps(r,indent=2));raise SystemExit(bool(r['rejected']))
