#!/usr/bin/env python3
"""Cold bounded allowlist; never uploads DLLs, packages, keys or terminal bytes."""
import argparse,hashlib,json,pathlib,stat
FILE_LIMIT=32*1024*1024
TOTAL_LIMIT=128*1024*1024
NAMES=('variant.txt','compiler-mode.txt','compiler-packages.txt','selected-toolchain.txt',
 'system-toolchain.txt','pacman-before.txt','pacman-after.txt','configured-cxx.txt',
 'cached-toolchain-packages.sha256.txt','hashes.txt','package-hashes.txt','disk-before.txt','disk-after.txt',
 'makeinfo.txt','console-before.cc','console-after.cc','console.cc.before','console.cc.after',
 'termios.cc.before','termios.cc.after','fhandler.h.before','fhandler.h.after','PKGBUILD','makepkg.conf',
 '0006-refined-prefix.patch','0000-utils-const-compat.patch','all-packaging-and-experimental.patch',
 'passive.patch','stormtty-passive-path.h','passive-identities.json','diagnostic-dll-pe.txt',
 'console-native-disassembly.txt','console-native-symbols.txt','console-native-object-sha256.txt',
 'recorder-native-disassembly.txt','recorder-native-symbols.txt',
 'runtime-recorder-exports-disassembly.txt','static-review.json',
 'linked-errno-disassembly.txt','linked-handler-disassembly.txt','linked-emitter-disassembly.txt',
 'linked-preflight.json','linked-preflight-error.json')
def collect(root,destination):
 if destination.exists():raise ValueError('new evidence destination required')
 destination.mkdir(parents=True);report=dict(schema=1,scope='build-only',fixtures_executed=False,review_required=True,files=[],missing=[],rejected=[])
 candidates=[pathlib.Path('evidence')/name for name in ('commit.txt','source-identity.txt','gcc15-preflight.log','stock-build.log','refined-build.log')]
 candidates += [pathlib.Path(variant+'-build/provenance')/name for variant in ('stock','refined') for name in NAMES]
 candidates += [pathlib.Path('gcc15/provenance')/name for name in ('verification-keyring.txt','compiler-packages.txt','compiler-search.txt','compiler-headers-link.txt')]
 for package in ('gcc-15.3.0-1-x86_64.pkg.tar.zst','gcc-libs-15.3.0-1-x86_64.pkg.tar.zst','gcc-15.3.0-1.src.tar.zst'):
  candidates += [pathlib.Path('gcc15/provenance')/(package+suffix) for suffix in ('.status.txt','.signature.txt')]
  if '.src.' not in package:
   candidates += [pathlib.Path('gcc15/provenance')/(package+suffix) for suffix in ('.PKGINFO.txt','.BUILDINFO.txt')]
 total=0
 for relative in candidates:
  source=root/relative
  if not source.exists():report['missing'].append(relative.as_posix());continue
  if (not source.is_file() or any(stat.S_ISLNK(p.lstat().st_mode) or
      getattr(p.lstat(),'st_file_attributes',0)&0x400 for p in (source,*source.parents))):
   report['rejected'].append(dict(path=relative.as_posix(),reason='nonregular-or-alias'));continue
  size=source.stat().st_size
  if size>FILE_LIMIT or total+size>TOTAL_LIMIT:
   report['rejected'].append(dict(path=relative.as_posix(),reason='size-cap',bytes=size));continue
  target=destination/relative;target.parent.mkdir(parents=True,exist_ok=True)
  # Bounded read rejects concurrent growth; these are stopped cold build steps.
  with source.open('rb') as stream: data=stream.read(FILE_LIMIT+1)
  if len(data)!=size:report['rejected'].append(dict(path=relative.as_posix(),reason='size-changed'));continue
  target.write_bytes(data);total+=size
  report['files'].append(dict(path=relative.as_posix(),bytes=size,sha256=hashlib.sha256(data).hexdigest()))
 report['bytes']=total;report['collection_complete']=not report['rejected']
 report['both_static_reports_present']=all((destination/(v+'-build/provenance/static-review.json')).is_file() for v in ('stock','refined'))
 (destination/'collection.json').write_text(json.dumps(report,indent=2)+'\n')
 return report
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('root',type=pathlib.Path);p.add_argument('destination',type=pathlib.Path);a=p.parse_args();r=collect(a.root,a.destination)
 print(json.dumps({k:v for k,v in r.items() if k not in ('files','missing')},indent=2));raise SystemExit(int(bool(r['rejected'])))
