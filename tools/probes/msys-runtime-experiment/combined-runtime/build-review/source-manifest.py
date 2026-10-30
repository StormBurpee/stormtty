#!/usr/bin/env python3
"""Cold source inventory; parse launcher constants, never import/execute launcher."""
import argparse,ast,hashlib,json,os
from pathlib import Path
HERE=Path(__file__).resolve().parent
ROOT=HERE.parent

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def relative_key(path,root,path_module=os.path):
    return path_module.relpath(path,root).replace("\\", "/")
def identities():
    assignments=[n for n in ast.parse((ROOT/'run.py').read_text()).body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='SOURCE_NAMES' for t in n.targets)]
    if len(assignments)!=1:raise ValueError('ambiguous fixture source contract')
    names=ast.literal_eval(assignments[0].value)
    if not isinstance(names,tuple) or not all(isinstance(n,str) for n in names):raise ValueError('invalid fixture source contract')
    fixture={name:digest(ROOT/name) for name in names}
    fixture['owned-runner.py']=digest(ROOT.parents[2]/'run-owned-process.py')
    paths=[p for p in ROOT.rglob('*') if p.is_file() and '__pycache__' not in p.parts and p.suffix in ('.py','.h','.hpp','.c','.sh','.ps1','.md','.patch','.json','.template')]
    paths += [ROOT.parent/n for n in ('toolchain.sh','partial-read/0007-preserve-copied-on-signal.patch','0006-refined-prefix.patch','0000-utils-const-compat.patch')]
    paths += [ROOT.parents[2]/'ci-msvc-env.ps1',ROOT.parents[2]/'run-owned-process.py',ROOT.parent/'passive-path/linked-preflight.py']
    if any(p.is_symlink() for p in paths):raise ValueError('source alias rejected')
    return dict(schema=1,fixture_source_sha256=fixture,build_source_sha256={relative_key(p,ROOT):digest(p) for p in sorted(set(paths))})
def check(path):
    expected=json.loads(path.read_text())
    if identities()!=expected:raise ValueError('source changed during build; discard evidence')
    return expected
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('path',type=Path);p.add_argument('--check',action='store_true');a=p.parse_args()
    if a.check:check(a.path)
    else:
        if a.path.exists():raise ValueError('new snapshot required')
        a.path.write_text(json.dumps(identities(),indent=2,sort_keys=True)+'\n')
