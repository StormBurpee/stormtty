#!/usr/bin/env python3
"""Cold, bounded, exact allowlist. No executables, private HOME or screenshots."""
import argparse,hashlib,importlib.util,json,pathlib,stat
HERE=pathlib.Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('app_matrix',HERE/'compare.py');matrix=importlib.util.module_from_spec(spec);spec.loader.exec_module(matrix)
TOTAL=128*1024*1024;LOG=1024*1024
PROVENANCE=('variant.txt','compiler-mode.txt','compiler-packages.txt','selected-toolchain.txt','system-toolchain.txt','pacman-before.txt','pacman-after.txt','configured-cxx.txt','cached-toolchain-packages.sha256.txt','hashes.txt','package-hashes.txt','disk-before.txt','disk-after.txt','makeinfo.txt','console-before.cc','console-after.cc','console.cc.before','console.cc.after','termios.cc.before','termios.cc.after','fhandler.h.before','fhandler.h.after','PKGBUILD','makepkg.conf','0007-preserve-copied-on-signal.patch','0000-utils-const-compat.patch','all-packaging-and-experimental.patch','dll-exports.txt','strace-before.cc','strace-after.cc','cygcheck-before.cc','cygcheck-after.cc','dump_setup-before.cc','dump_setup-after.cc')
def candidates():
 names=[(pathlib.Path('evidence')/n,8*LOG) for n in ('commit.txt','app-plan.json','source-identity.txt','gcc15-preflight.log','stock-build.log','copied-build.log','app-build.log','app-copies.json','upstream-buildinfo.txt')]
 names += [(pathlib.Path(v+'-build/provenance')/n,8*LOG) for v in ('stock','copied') for n in PROVENANCE]
 for kind in ('raw','managed','settings'):
  names.append((pathlib.Path('evidence')/kind/'results.json',4*LOG))
  names.extend((pathlib.Path('evidence')/kind/f"{r['provider']}-{r['mode']}-{r['trial']}-{r['variant']}.log",LOG) for r in matrix.plan(kind))
 for name in ('verification-keyring.txt','compiler-packages.txt','compiler-search.txt','compiler-headers-link.txt'):
  names.append((pathlib.Path('gcc15/provenance')/name,LOG))
 for package in ('gcc-15.3.0-1-x86_64.pkg.tar.zst','gcc-libs-15.3.0-1-x86_64.pkg.tar.zst','gcc-15.3.0-1.src.tar.zst'):
  for suffix in ('.status.txt','.signature.txt')+(() if '.src.' in package else ('.PKGINFO.txt','.BUILDINFO.txt')):names.append((pathlib.Path('gcc15/provenance')/(package+suffix),LOG))
 return names
def collect(root,destination):
 if destination.exists():raise ValueError('new collection destination required')
 destination.mkdir(parents=True);report=dict(schema=1,scope='fixed-uninstrumented-app',production_qualified=False,files=[],missing=[],rejected=[],bytes=0)
 for relative,cap in candidates():
  source=root/relative
  if not source.exists():report['missing'].append(str(relative));continue
  if not source.is_file() or any(stat.S_ISLNK(p.lstat().st_mode) or getattr(p.lstat(),'st_file_attributes',0)&0x400 for p in (source,*source.parents)):
   report['rejected'].append(dict(path=str(relative),reason='nonregular-or-alias'));continue
  with source.open('rb') as stream:data=stream.read(cap+1)
  if len(data)>cap or report['bytes']+len(data)>TOTAL:
   report['rejected'].append(dict(path=str(relative),reason='size-cap'));continue
  target=destination/relative;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data);report['bytes']+=len(data)
  report['files'].append(dict(path=relative.as_posix(),bytes=len(data),sha256=hashlib.sha256(data).hexdigest()))
 report['matrix_inventory_complete']={}
 for kind in ('raw','managed','settings'):
  try:
   d=json.loads((destination/'evidence'/kind/'results.json').read_text())
   expected={(r['provider'],r['mode'],r['trial'],r['variant']) for r in matrix.plan(kind)}
   actual=[(r['provider'],r['mode'],r['trial'],r['variant']) for r in d['cases']]
   good=d['planned']==len(expected)==len(actual) and set(actual)==expected and d.get('final_identity_verified') is True
  except (OSError,ValueError,KeyError,TypeError):good=False
  report['matrix_inventory_complete'][kind]=good
 report['collection_complete']=not report['missing'] and not report['rejected'] and all(report['matrix_inventory_complete'].values())
 (destination/'collection.json').write_text(json.dumps(report,indent=2)+'\n');return report
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('root',type=pathlib.Path);p.add_argument('destination',type=pathlib.Path);a=p.parse_args();r=collect(a.root,a.destination)
 print(json.dumps({k:v for k,v in r.items() if k!='files'},indent=2));raise SystemExit(int(not r['collection_complete']))
