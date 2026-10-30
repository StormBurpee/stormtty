#!/usr/bin/env python3
"""Build-only exact source check; no compilation or runtime loading."""
import argparse,hashlib,json
from pathlib import Path
HERE=Path(__file__).resolve().parent.parent
TERM='e65b607b331586533fd495dc228361b01b10f4ec85682abb2342b5c29867a36f'
def verify(variant,root,phase):
    identity=json.loads((HERE/'identities.json').read_text());pin=identity['variants'][variant]
    expected={'fhandler/console.cc':pin['source_sha256' if phase=='before' else 'instrumented_sha256'],
              'fhandler/termios.cc':TERM,'local_includes/fhandler.h':pin['fhandler_sha256']}
    if phase=='after':expected['local_includes/stormtty-combined-path.h']=identity['header_sha256']
    for name,digest in expected.items():
        if hashlib.sha256((root/name).read_bytes()).hexdigest()!=digest:raise ValueError('source drift: '+name)
    return expected
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('variant',choices=('copied','combined'));p.add_argument('root',type=Path);p.add_argument('phase',choices=('before','after'));a=p.parse_args();print(json.dumps(verify(a.variant,a.root,a.phase),indent=2))
