#!/usr/bin/env python3
"""Fail closed on exact local diagnostic patch/header and source identities."""
import argparse,hashlib,json,pathlib
HERE=pathlib.Path(__file__).resolve().parent
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def verify(variant,source,phase):
 lock=json.loads((HERE/'identities.json').read_text())
 if lock['schema']!=1 or lock['scope']!='passive-diagnostic-only':raise ValueError('diagnostic schema')
 expected=lock['variants'][variant]
 if sha(HERE/'recorder.hpp')!=lock['header_sha256']:raise ValueError('recorder drift')
 if sha(HERE/(variant+'.patch'))!=expected['patch_sha256']:raise ValueError('patch drift')
 if sha(source)!=expected['source_sha256' if phase=='before' else 'instrumented_sha256']:raise ValueError('console drift')
 if phase=='after' and sha(source.parent.parent/'local_includes/stormtty-passive-path.h')!=lock['header_sha256']:raise ValueError('applied header drift')
 return expected
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('variant',choices=('stock','copied'));p.add_argument('source',type=pathlib.Path);p.add_argument('phase',choices=('before','after'));a=p.parse_args()
 print(json.dumps(verify(a.variant,a.source,a.phase),sort_keys=True))
