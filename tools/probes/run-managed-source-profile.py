#!/usr/bin/env python3
"""Bounded native attribution only; never publishes raw profiler output."""
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

prepare=load('source_profile_fixture',HERE/'prepare-managed-source-profile.py')
runner=load('source_profile_runner',HERE.parent/'run-owned-process.py')
NUMBER=r'[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?'
STAGE=re.compile(r'SOURCE_STAGE sample=([1-6]) name=('+ '|'.join(prepare.STAGES)+r') elapsed_ms='+NUMBER)
FUNCTIONS=('compinit','compaudit','compdump','compdef','zrecompile','_omz_compdump_has_metadata','_stormtty_private_state','_zsh_highlight_load_highlighters','_omz_source')
FUNCTION=re.compile(r'SOURCE_FUNCTION sample=([1-6]) name=('+ '|'.join(FUNCTIONS)+r') calls=[0-9]+ total_ms='+NUMBER+r' self_ms='+NUMBER)
SAMPLE=re.compile(r'STARTUP_SAMPLE sample=([1-6]) fresh_state=([01]) state_prepare_ms='+NUMBER+r' launch_to_zle_ms='+NUMBER)
RESULT=re.compile(r'STARTUP_RESULT code=(?:0 fresh_samples=3 warm_samples=3|1 sample=[0-9]+ stage=[0-9]+)')

def sanitized(output):
    lines=[];stages=[];functions=[];samples=[]
    for line in output.decode('utf-8',errors='replace').splitlines():
        if match:=STAGE.fullmatch(line):stages.append((int(match[1]),match[2]));lines.append(line)
        elif match:=FUNCTION.fullmatch(line):functions.append((int(match[1]),match[2]));lines.append(line)
        elif match:=SAMPLE.fullmatch(line):samples.append((int(match[1]),int(match[2])));lines.append(line)
        elif RESULT.fullmatch(line):lines.append(line)
    valid=(stages==[(i,name) for i in range(1,7) for name in prepare.STAGES] and
           samples==[(i,int(i<=3)) for i in range(1,7)] and len(functions)==len(set(functions)) and
           all((i,name) in functions for i in range(1,7) for name in ('compinit','compaudit')) and
           'STARTUP_RESULT code=0 fresh_samples=3 warm_samples=3' in lines)
    return lines,valid

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('runtime','probe','provider','output','scratch'):
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    if os.name!='nt':p.error('native Windows required; POSIX source tests are not attribution evidence')
    a.output.mkdir(parents=True,exist_ok=True);a.scratch.mkdir(parents=True,exist_ok=True)
    failures=[]
    with tempfile.TemporaryDirectory(prefix='StormTTY source profile ',dir=a.scratch) as tmp:
        fixture=Path(tmp)/'runtime'
        identity=prepare.prepare(a.runtime,fixture)
        # Hash identity and fixed stage names only; no fixture/user path.
        (a.output/'source-profile-identity.json').write_text(json.dumps(identity,sort_keys=True)+'\n')
        for name,extra in [('inbox',[]),('packaged',[str(a.provider.resolve())])]:
            lines=[];valid=False
            try:
                result=runner.run_owned([str(a.probe.resolve()),'--source-profile',str(fixture.resolve()),*extra],timeout=240,reap_timeout=10,tail_bytes=65536,output_dir=a.output)
                lines,valid=sanitized(result.output_tail)
                valid=valid and result.exit_code==0 and result.reaped and not result.output_truncated and not result.cleanup_error
                lines.append('SOURCE_RUNNER exit_code=%d timed_out=%d reaped=%d truncated=%d cleanup_error=%d elapsed_seconds=%.3f'%(result.exit_code,result.timed_out,result.reaped,result.output_truncated,bool(result.cleanup_error),result.elapsed_seconds))
            except OSError:lines.append('SOURCE_LAUNCH_ERROR code=1')
            lines.append('SOURCE_EVIDENCE_VALID code='+str(0 if valid else 1))
            (a.output/('source-profile-'+name+'.log')).write_text('\n'.join(lines)+'\n')
            print('SOURCE_PROVIDER='+name,flush=True);print('\n'.join(lines),flush=True)
            if not valid:failures.append(name)
    return int(bool(failures))

if __name__=='__main__':
    try: code=main()
    except Exception:
        # Never echo an exception that may contain a filesystem path.
        print('SOURCE_PROFILE_SETUP code=2',flush=True);code=2
    raise SystemExit(code)
