#!/usr/bin/env python3
"""Copy only bounded diagnostic evidence; never export private staged binaries."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import importlib.util
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('matrix_collect',HERE/'matrix.py');matrix=importlib.util.module_from_spec(spec);spec.loader.exec_module(matrix)
LIMITS={'results.json':2*1024*1024,'fixture.json':65536,'assessment.json':65536,
        'launcher.log':65536,'trace.bin':64+16384*128}

def collect(source,destination):
    source=Path(source);destination=Path(destination)
    if source.is_symlink() or destination.exists():raise ValueError('unsafe source/existing destination')
    selected=[]
    if (source/'results.json').is_file():selected.append(Path('results.json'))
    for order,case,variant in matrix.trials():
        relative=Path(f'{order}-{case}-{variant}')
        directory=source/relative
        if directory.is_symlink():raise ValueError('symlink evidence directory')
        for name in ('fixture.json','assessment.json','launcher.log','trace.bin'):
            path=directory/name
            if path.exists():selected.append(relative/name)
    # Validate every selection before writing any destination artifact.
    for relative in selected:
        path=source/relative
        if path.is_symlink() or not path.is_file() or path.stat().st_size>LIMITS[path.name]:
            raise ValueError('unsafe/oversize evidence')
    destination.mkdir(parents=True)
    manifest=[]
    for relative in selected:
        path=source/relative;target=destination/relative;target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(path,target)
        manifest.append(dict(path=relative.as_posix(),size=target.stat().st_size,sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
    (destination/'manifest.json').write_text(json.dumps(dict(schema=2,files=manifest,native_qualified=False),indent=2)+'\n')
    return manifest

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('source',type=Path);parser.add_argument('destination',type=Path)
    args=parser.parse_args();collect(args.source,args.destination)
