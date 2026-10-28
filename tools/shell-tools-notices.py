#!/usr/bin/env python3
"""Offline supplemental source/notice packet for qualification-only shell tools.

This preserves a conservative dependency-source superset. It intentionally does
not certify public redistribution, exact Rust linked closure or toolchain notices.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import tempfile
import zipfile

TOOLS=Path(__file__).resolve().parent


def emit(cache, destination):
    lock=json.loads((TOOLS/'shell-tools-sources.lock.json').read_text())
    destination=destination.absolute()
    if destination.exists() or destination.is_symlink(): raise RuntimeError('Destination must be fresh')
    destination.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=destination.parent,prefix='.tools-notices-') as tmp:
        root=Path(tmp)/'packet'; (root/'sources').mkdir(parents=True); (root/'texts').mkdir()
        for row in lock['sources']:
            name=row['filename']; path=cache/name
            if Path(name).name!=name or path.is_symlink() or not path.is_file(): raise RuntimeError('Unsafe/missing source cache entry')
            raw=path.read_bytes()
            if len(raw)!=row['bytes'] or hashlib.sha256(raw).hexdigest()!=row['sha256']: raise RuntimeError('Source integrity mismatch')
            shutil.copyfile(path,root/'sources'/name)
            if name.endswith('.crate'):
                archive=tarfile.open(path,'r:gz'); reader=lambda n: archive.extractfile(n).read()
            else:
                archive=zipfile.ZipFile(path); reader=archive.read
            with archive:
                for notice in row['license_files']:
                    rel=PurePosixPath(notice['path'])
                    if rel.is_absolute() or '..' in rel.parts or '\\' in notice['path']: raise RuntimeError('Unsafe notice path')
                    data=reader(notice['path'])
                    if len(data)!=notice['bytes'] or hashlib.sha256(data).hexdigest()!=notice['sha256']: raise RuntimeError('Notice integrity mismatch')
                    output=root/'texts'/name/rel
                    output.parent.mkdir(parents=True,exist_ok=True); output.write_bytes(data)
        shutil.copyfile(TOOLS/'shell-tools-sources.lock.json',root/'source-manifest.json')
        (root/'REVIEW-REQUIRED.txt').write_text('Qualification packet only. Includes Cargo.lock superset (dev/build/other-target dependencies), not exact Rust linked closure.\nCompiler/standard-library/MinGW static runtime notice review remains pending. Missing notice files are listed in the source inventory.\nDo not merge this manifest over the base MSYS/OMZ source inventory: concatenate all inventories and retain original notices.\nTop-level MSYS2 source archives and original package notices are separately pinned in shell-tools.lock.json and retained by shell-tools.py.\nPublic redistribution is NOT approved or complete.\n')
        root.rename(destination)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--cache',type=Path,required=True); p.add_argument('--destination',type=Path,required=True)
    a=p.parse_args(); emit(a.cache,a.destination)
