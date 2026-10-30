#!/usr/bin/env python3
"""Bounded actual Windows app diagnostics; isolated smoke only, no raw transcripts."""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import sys
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('startup_app_owned_runner',HERE.parent/'run-owned-process.py')
runner=importlib.util.module_from_spec(spec);sys.modules[spec.name]=runner;spec.loader.exec_module(runner)
CHECK=re.compile(r'STARTUP_APP_CHECK mode=([0-3]) sample=([12]) diagnostic=([01]) native_status=([0-2]) persistent=([01]) cache_expected=([01]) input_roundtrip=1 notice=([01]) code=0')
EXIT=re.compile(r'STARTUP_APP_EXIT sample=([12]) code=0')
CLEANUP=re.compile(r'STARTUP_APP_CLEANUP actors_closed=([01]) removed=([01])')
EXPECTED={
    'off':[(0,1,0,0,1,0,0)],
    'ready':[(1,1,1,0,1,0,0),(1,2,1,0,1,1,0)],
    'disabled':[(2,1,1,1,0,0,0)],
    'unavailable':[(3,1,1,2,0,0,1)],
}
def evidence(data,mode):
    lines=[]; rows=[]; exits=[]; cleanup=[]; malformed=False
    for line in data.decode('ascii',errors='replace').splitlines():
        match=CHECK.fullmatch(line)
        ended=EXIT.fullmatch(line)
        cleaned=CLEANUP.fullmatch(line)
        if match:lines.append(line);rows.append(tuple(map(int,match.groups())))
        elif ended:lines.append(line);exits.append(int(ended[1]))
        elif cleaned:lines.append(line);cleanup.append(tuple(map(int,cleaned.groups())))
        elif line.startswith(('STARTUP_APP_CHECK','STARTUP_APP_EXIT','STARTUP_APP_CLEANUP')):malformed=True
    return lines,not malformed and rows==EXPECTED[mode] and exits==[row[1] for row in EXPECTED[mode]] and cleanup==[(1,1)]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if os.name!='nt':p.error('native Windows required')
    cache=(a.build/'CMakeCache.txt').read_text(encoding='utf-8')
    if not re.search(r'^STORMTTY_EXPERIMENTAL_COMPLETION_SEED:BOOL=OFF$',cache,re.M):
        raise ValueError('this matrix qualifies ordinary OFF startup only')
    a.output.mkdir(parents=True,exist_ok=True)
    # Real engine formatter test is separate from the actual-app matrix.
    engine=runner.run_owned([str((a.build/'startup_diagnostic_observation_tests.exe').resolve())],
        timeout=30,reap_timeout=10,tail_bytes=4096,output_dir=a.output)
    expected_engine='STARTUP_OBSERVATION_TEST widths=4 malformed=3 duplicate=1 code=0'
    engine_lines=engine.output_tail.decode('ascii',errors='replace').splitlines()
    engine_ok=engine_lines==[expected_engine] and engine.exit_code==0 and engine.reaped and not engine.timed_out and not engine.output_truncated and not engine.cleanup_error
    numeric=[expected_engine] if engine_lines==[expected_engine] else []
    numeric.append('STARTUP_APP_ENGINE code=%d exit_code=%d timed_out=%d reaped=%d truncated=%d cleanup_error=%d'%
        (not engine_ok,engine.exit_code,engine.timed_out,engine.reaped,engine.output_truncated,bool(engine.cleanup_error)))
    (a.output/'startup-app-engine.log').write_text('\n'.join(numeric)+'\n',encoding='ascii')
    if not engine_ok:return 1
    valid=True
    for mode in EXPECTED:
        command=[str((a.build/'stormtty.exe').resolve()),'--smoke-shell-startup',mode]
        if mode!='off':command.append('--diagnose-shell-startup')
        # Process-local authored environment only; no persistent/system changes.
        # The app replaces nonempty fixture HOME with its own created root.
        saved=dict(os.environ)
        try:
            os.environ['HOME']='' if mode=='disabled' else 'STORMTTY_AUTHORED_HOME'
            os.environ['STORMTTY_NATIVE_STARTUP_DIAGNOSTIC']='unavailable'
            os.environ['STORMTTY_NATIVE_STARTUP_STATE_MS']='999'
            result=runner.run_owned(command,timeout=120,reap_timeout=10,tail_bytes=65536,output_dir=a.output)
        finally:
            os.environ.clear();os.environ.update(saved)
        lines,matched=evidence(result.output_tail,mode)
        ok=matched and result.exit_code==0 and result.reaped and not result.timed_out and not result.output_truncated and not result.cleanup_error
        valid &= ok
        lines.append('STARTUP_APP_RUN mode=%d code=%d exit_code=%d timed_out=%d reaped=%d truncated=%d cleanup_error=%d'%
            (list(EXPECTED).index(mode),not ok,result.exit_code,result.timed_out,result.reaped,result.output_truncated,bool(result.cleanup_error)))
        (a.output/('startup-app-'+mode+'.log')).write_text('\n'.join(lines)+'\n',encoding='ascii')
    print('STARTUP_APP_RESULT code='+str(0 if valid else 1),flush=True)
    return 0 if valid else 1
if __name__=='__main__':
    try:code=main()
    except Exception:print('STARTUP_APP_SETUP code=2',flush=True);code=2
    raise SystemExit(code)
