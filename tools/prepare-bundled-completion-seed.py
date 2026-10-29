#!/usr/bin/env python3
"""Build-only exact-final-runtime seed/header preparation with owned no-op repair."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import tempfile
SPEC=importlib.util.spec_from_file_location('seed_generator',Path(__file__).parent/'probes/prepare-completion-seed.py')
seed=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(seed)
RECEIPT='.stormtty-seed-build.json'

def header_bytes(identity):
    return ('#pragma once\nnamespace stormtty::build_seed {\ninline constexpr char identity_sha256[] = "'+identity+'";\n}\n').encode('ascii')

def prepare(runtime,destination,header):
    runtime=runtime.resolve(strict=True);destination=destination.absolute();header=header.absolute()
    for p in (destination,header,*destination.parents,*header.parents):
        if p.exists() and (p.is_symlink() or getattr(p.lstat(),'st_file_attributes',0)&0x400):raise ValueError('linked output')
    # Windows resolve expands existing 8.3 aliases (including TEMP); absolute
    # alone does not. Compare like-for-like after rejecting linked output paths.
    destination=destination.resolve(strict=False);header=header.resolve(strict=False)
    if (destination==runtime or runtime in destination.parents or destination in runtime.parents
            or header==runtime or runtime in header.parents):
        raise ValueError('outputs must be outside runtime')
    if destination.exists():
        receipt=json.loads((destination/RECEIPT).read_text())
        if receipt.get('owned_completion_seed') is not True:raise ValueError('unowned output')
        for child in destination.rglob('*'):
            if child.is_symlink() or getattr(child.lstat(),'st_file_attributes',0)&0x400:raise ValueError('linked package output')
        try:
            identity=(destination/'identity.txt').read_text().splitlines()
            count=int(identity[4]);valid=(identity[0]=='STORMTTY_SEED_V1' and 0<count<=256 and
                identity[5+count:]==seed.inventory(runtime) and seed.sha(destination/'seed.dump')==identity[3])
        except (IndexError,ValueError,OSError):valid=False
        if valid:
            expected=header_bytes(seed.sha(destination/'identity.txt'))
            header.parent.mkdir(parents=True,exist_ok=True)
            if not header.exists() or header.read_bytes()!=expected:header.write_bytes(expected)
            return False
    elif header.exists():raise ValueError('unowned trust header')
    destination.parent.mkdir(parents=True,exist_ok=True);header.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.seed-build-',dir=destination.parent) as tmp:
        fresh=Path(tmp)/'new';seed.prepare(runtime,fresh)
        (fresh/RECEIPT).write_text(json.dumps({'schema':1,'owned_completion_seed':True})+'\n')
        old=Path(tmp)/'old'
        if destination.exists():destination.rename(old)
        try:fresh.rename(destination)
        except OSError:
            if old.exists():old.rename(destination)
            raise
        staged_header=Path(tmp)/'trust.hpp';staged_header.write_bytes(header_bytes(seed.sha(destination/'identity.txt')))
        os.replace(staged_header,header)
    return True

if __name__=='__main__':
    p=argparse.ArgumentParser()
    for name in ('runtime','destination','trust-header'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    try:
        changed=prepare(a.runtime,a.destination,a.trust_header)
        print('BUNDLED_SEED code=0 rebuilt='+str(int(changed)))
    except Exception:
        print('BUNDLED_SEED code=1');raise SystemExit(1)
