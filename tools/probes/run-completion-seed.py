#!/usr/bin/env python3
"""Optional native seeded-vs-normal source attribution, never an app dependency."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import sys
import tempfile
HERE=Path(__file__).resolve().parent

def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    m=importlib.util.module_from_spec(spec);sys.modules[name]=m;spec.loader.exec_module(m);return m
profile=load('seed_source_profile',HERE/'run-managed-source-profile.py')
NUMBER=profile.NUMBER
ROW=re.compile(r'SEED_SAMPLE sample=([1-6]) fresh_state=([01]) adopted=([01]) verification_publish_ms=('+NUMBER+r')')

def sanitize(output, seeded):
    lines, valid=profile.sanitized(output)
    rows=[]
    for line in output.decode('utf-8',errors='replace').splitlines():
        if match:=ROW.fullmatch(line):
            rows.append(tuple(map(int,match.group(1,2,3))));lines.append(line)
    valid=valid and rows==([(i,int(i<=3),int(i<=3)) for i in range(1,7)] if seeded else [])
    return lines,valid

def good(result):
    return result.exit_code==0 and result.reaped and not result.timed_out and not result.output_truncated and not result.cleanup_error

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('runtime','probe','provider','output','scratch'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    if os.name!='nt':p.error('native Windows required')
    a.output.mkdir(parents=True,exist_ok=True);a.scratch.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='completion-seed-',dir=a.scratch) as tmp:
        runtime=Path(tmp)/'runtime with spaces é';seed=Path(tmp)/'seed'
        identity=profile.prepare.prepare(a.runtime,runtime)
        (a.output/'seed-source-identity.json').write_text(json.dumps(identity,sort_keys=True)+'\n')
        result=profile.runner.run_owned([sys.executable,str(HERE/'prepare-completion-seed.py'),'--runtime',str(runtime),'--destination',str(seed)],timeout=120,reap_timeout=10,tail_bytes=4096,output_dir=a.output)
        if not good(result) or b'SEED_BUILD code=0' not in result.output_tail:
            print('SEED_SETUP code=1');return 1
        (a.output/'seed-package-identity.json').write_text(json.dumps({
            'identity_sha256': profile.prepare.sha(seed/'identity.txt'),
            'text_sha256': profile.prepare.sha(seed/'seed.dump'),
            'text_bytes': (seed/'seed.dump').stat().st_size},sort_keys=True)+'\n')
        result=profile.runner.run_owned([str(a.probe.resolve()),'--completion-seed-checks',str(seed),str(runtime)],timeout=240,reap_timeout=10,tail_bytes=4096,output_dir=a.output)
        checks=(good(result) and b'SEED_CHECKS code=0 cases=8' in result.output_tail
                and b'SEED_FALLBACK code=0' in result.output_tail
                and b'SEED_SAMPLE sample=1 fresh_state=1 adopted=0 ' in result.output_tail
                and b'SOURCE_FUNCTION sample=1 name=compaudit ' in result.output_tail)
        check_lines=[line for line in result.output_tail.decode('utf-8',errors='replace').splitlines()
            if ROW.fullmatch(line) or profile.STAGE.fullmatch(line) or profile.FUNCTION.fullmatch(line)
            or profile.SAMPLE.fullmatch(line) or re.fullmatch(r'SEED_(?:CHECKS code=0 cases=8|FALLBACK code=0)',line)]
        check_lines.append('SEED_CHECKS_VALID code='+str(0 if checks else 1))
        (a.output/'seed-checks.log').write_text('\n'.join(check_lines)+'\n')
        if not checks:return 1
        failed=False
        for provider,extra,order in [('inbox',[],(False,True)),('packaged',[str(a.provider.resolve())],(True,False))]:
            for seeded in order:
                mode='seeded' if seeded else 'baseline'
                args=['--completion-seed-profile',str(seed)] if seeded else ['--source-profile']
                result=profile.runner.run_owned([str(a.probe.resolve()),*args,str(runtime),*extra],timeout=240,reap_timeout=10,tail_bytes=65536,output_dir=a.output)
                lines,valid=sanitize(result.output_tail,seeded);valid=valid and good(result)
                lines.append('SEED_RUNNER exit_code=%d timed_out=%d reaped=%d truncated=%d cleanup_error=%d elapsed_seconds=%.3f'%(result.exit_code,result.timed_out,result.reaped,result.output_truncated,bool(result.cleanup_error),result.elapsed_seconds))
                lines.append('SEED_EVIDENCE_VALID code='+str(0 if valid else 1))
                (a.output/f'seed-{provider}-{mode}.log').write_text('\n'.join(lines)+'\n')
                print('SEED_MODE='+provider+'-'+mode,flush=True);print('\n'.join(lines),flush=True)
                failed |= not valid
        return int(failed)
if __name__=='__main__':
    try:code=main()
    except Exception:print('SEED_SETUP code=2',flush=True);code=2
    raise SystemExit(code)
