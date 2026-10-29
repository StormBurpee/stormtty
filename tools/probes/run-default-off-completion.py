#!/usr/bin/env python3
"""Exact default-OFF integration checks; owned fixtures and numeric evidence only."""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import sys
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('off_owned_runner',HERE.parent/'run-owned-process.py')
runner=importlib.util.module_from_spec(spec);sys.modules[spec.name]=runner;spec.loader.exec_module(runner)

def assert_default_off(build):
    cache=(build/'CMakeCache.txt').read_text(encoding='utf-8')
    if not re.search(r'^STORMTTY_EXPERIMENTAL_COMPLETION_SEED:BOOL=OFF$',cache,re.M):
        raise ValueError('default completion option is not OFF')
    if (build/'runtimes/zsh-seed').exists() or (build/'generated/completion-seed').exists():
        raise ValueError('default build unexpectedly generated a completion seed')

def good(result):
    return result.exit_code==0 and result.reaped and not result.timed_out and not result.output_truncated and not result.cleanup_error

def main():
    p=argparse.ArgumentParser()
    for name in ('build','provider','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    if os.name!='nt':p.error('native Windows required')
    a.output.mkdir(parents=True,exist_ok=True);assert_default_off(a.build)
    valid=True
    for mode in ('layout','modal','projects','settings'):
        result=runner.run_owned([str((a.build/'stormtty.exe').resolve()),'--smoke-'+mode,
            '--capture',str((a.output/('default-off-'+mode+'.png')).resolve())],
            timeout=120,reap_timeout=10,tail_bytes=8192,output_dir=a.output)
        ok=good(result);valid &= ok
        (a.output/('default-off-'+mode+'.log')).write_text('DEFAULT_OFF_SMOKE mode=%s code=%d exit_code=%d timed_out=%d reaped=%d truncated=%d cleanup_error=%d\n'%
            (mode,0 if ok else 1,result.exit_code,result.timed_out,result.reaped,result.output_truncated,bool(result.cleanup_error)))
    number=r'[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?'
    state=re.compile(r'MANAGED_STATE_RESULT code=0 phase=11 cold_samples=1 warm_samples=1 checks=[0-9]+ cold_ms='+number+r' warm_ms='+number)
    typing=re.compile(r'MANAGED_TYPING_RESULT code=[01] characters=[0-9]+ frames=[0-9]+ retreats=[0-9]+ echo_total_us=[0-9]+ echo_max_us=[0-9]+')
    for provider,extra in [('inbox',[]),('packaged',[str(a.provider.resolve())])]:
        result=runner.run_owned([str((a.build/'managed_shell_state_probe.exe').resolve()),str((a.build/'runtimes/zsh').resolve()),*extra],
            timeout=180,reap_timeout=10,tail_bytes=16384,output_dir=a.output)
        lines=[line for line in result.output_tail.decode('utf-8',errors='replace').splitlines() if state.fullmatch(line) or typing.fullmatch(line)]
        ok=good(result) and any(state.fullmatch(line) for line in lines);valid &= ok
        lines.append('DEFAULT_OFF_MANAGED provider=%s code=%d exit_code=%d timed_out=%d reaped=%d truncated=%d cleanup_error=%d'%
            (provider,0 if ok else 1,result.exit_code,result.timed_out,result.reaped,result.output_truncated,bool(result.cleanup_error)))
        (a.output/('default-off-'+provider+'.log')).write_text('\n'.join(lines)+'\n')
    print('DEFAULT_OFF_INTEGRATION code='+str(0 if valid else 1),flush=True)
    return int(not valid)
if __name__=='__main__':
    try:code=main()
    except Exception:print('DEFAULT_OFF_SETUP code=2',flush=True);code=2
    raise SystemExit(code)
