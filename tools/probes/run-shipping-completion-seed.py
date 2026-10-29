#!/usr/bin/env python3
"""Bounded native shipping-factory qualification, no modified runtime/source profiler."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import sys
HERE=Path(__file__).resolve().parent

def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec)
    sys.modules[name]=m;spec.loader.exec_module(m);return m
runner=load('shipping_owned_runner',HERE.parent/'run-owned-process.py')
seed=load('shipping_seed_inventory',HERE/'prepare-completion-seed.py')
N=r'[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?'
SAMPLE=re.compile(r'STARTUP_SAMPLE sample=([1-6]) fresh_state=([01]) state_prepare_ms=('+N+r') launch_to_zle_ms=('+N+r')')
SHIPPING=re.compile(r'SHIPPING_SAMPLE sample=([1-6]) adoption=(-1|0|1) verification_publish_ms=('+N+r') native_ack=1')
RESULT='STARTUP_RESULT code=0 fresh_samples=3 warm_samples=3'

def sanitized(data,candidate):
    lines=[];samples=[];adoptions=[];metrics=[]
    for line in data.decode('utf-8',errors='replace').splitlines():
        if m:=SAMPLE.fullmatch(line):
            lines.append(line);samples.append((int(m[1]),int(m[2])))
            metrics.append({'sample':int(m[1]),'fresh':bool(int(m[2])),'total_ms':float(m[3])+float(m[4])})
        elif re.fullmatch(r'SHIPPING_VERIFY sample=[1-6] stage=[0-9]+ object=[0-9]+ win32_error=[0-9]+',line):lines.append(line)
        elif m:=SHIPPING.fullmatch(line):lines.append(line);adoptions.append((int(m[1]),int(m[2])))
        elif line==RESULT or re.fullmatch(r'STARTUP_RESULT code=1 sample=[0-9]+ stage=[0-9]+',line):lines.append(line)
    valid=(samples==[(i,int(i<=3)) for i in range(1,7)] and RESULT in lines and
           adoptions==([(i,1 if i<=3 else -1) for i in range(1,7)] if candidate else []))
    # Verification is INSIDE launch_to_zle_ms in this shipping factory. Never add twice.
    return lines,valid,metrics

def good(result):return result.exit_code==0 and result.reaped and not result.timed_out and not result.output_truncated and not result.cleanup_error

def lifecycle_evidence(data):
    allowed=re.compile(r'INITIALIZATION_CHECKS code=(?:0 contention=1 config_preserved=1 native_ack=1 child_exit_release=1 abandoned_owner_no_dump=1 public_runtime_acl=1 cross_process_contention=1|1 stage=[0-9]+)')
    journal='INITIALIZATION_JOURNAL code=0 process_exit_persistent=1 prior_journal_not_cleared=1'
    abandoned=re.compile(r'INITIALIZATION_ABANDONED recovery_no_dump=[01] recovery_event=[01] state_valid=[01] marker_present=[01] marker_error=[0-9]+')
    lines=[line for line in data.decode('utf-8',errors='replace').splitlines() if allowed.fullmatch(line) or abandoned.fullmatch(line) or line==journal]
    valid=journal in lines and any(x.startswith('INITIALIZATION_CHECKS code=0 ') for x in lines)
    return lines,valid

def main():
    p=argparse.ArgumentParser()
    for name in ('build','install','provider','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    if os.name!='nt':p.error('native Windows required')
    a.output.mkdir(parents=True,exist_ok=True)
    runtime=a.build/'runtimes/zsh';installed=a.install/'runtimes/zsh'
    if seed.inventory(runtime)!=seed.inventory(installed):raise ValueError('build/install runtime identity differs')
    for name in ('seed.dump','identity.txt'):
        if seed.sha(a.build/'runtimes/zsh-seed'/name)!=seed.sha(a.install/'runtimes/zsh-seed'/name):raise ValueError('build/install seed differs')
    for name in ('stormtty.exe','stormtty-console-setup.exe'):
        if seed.sha(a.build/name)!=seed.sha(a.install/name):raise ValueError('installed binary differs')
    identity={'runtime_manifest_sha256':seed.sha(runtime/'stormtty-shell-manifest.json'),
              'seed_identity_sha256':seed.sha(a.build/'runtimes/zsh-seed/identity.txt'),
              'seed_text_sha256':seed.sha(a.build/'runtimes/zsh-seed/seed.dump'),
              'application_sha256':seed.sha(a.install/'stormtty.exe'),'build_install_identical':True}
    (a.output/'shipping-identity.json').write_text(json.dumps(identity,sort_keys=True)+'\n')
    result=runner.run_owned([str((a.build/'managed_shell_initialization_probe.exe').resolve())],timeout=120,reap_timeout=10,tail_bytes=8192,output_dir=a.output)
    lines,valid=lifecycle_evidence(result.output_tail)
    valid=valid and good(result)
    lines.append('SHIPPING_LIFECYCLE_VALID code='+str(0 if valid else 1))
    (a.output/'shipping-lifecycle.log').write_text('\n'.join(lines)+'\n')
    if not valid:return 1
    # Installed app's real NativeWindow factory, with app-owned synthetic smoke state.
    result=runner.run_owned([str((a.install/'stormtty.exe').resolve()),'--smoke-settings','--capture',str((a.output/'shipping-settings.png').resolve())],timeout=120,reap_timeout=10,tail_bytes=65536,output_dir=a.output)
    app_ok=good(result)
    (a.output/'shipping-app.log').write_text('SHIPPING_APP exit_code=%d timed_out=%d reaped=%d truncated=%d cleanup_error=%d\n'%(result.exit_code,result.timed_out,result.reaped,result.output_truncated,bool(result.cleanup_error)))
    # The measuring executable uses the identical compiled factory and installed
    # native console helper, while keeping all measurement state authored/private.
    shutil.copyfile(a.build/'managed_shell_startup_probe.exe',a.install/'managed_shell_startup_probe.exe')
    matrix=[('build-inbox-seeded',a.build,runtime,[],True),
            ('install-inbox-baseline',a.install,installed,[],False),
            ('install-inbox-seeded',a.install,installed,[],True),
            ('install-packaged-seeded',a.install,installed,[str(a.provider.resolve())],True),
            ('install-packaged-baseline',a.install,installed,[str(a.provider.resolve())],False)]
    failed=not app_ok;summary={}
    for name,location,tree,extra,candidate in matrix:
        mode='--shipping-seed' if candidate else '--shipping-baseline'
        result=runner.run_owned([str((location/'managed_shell_startup_probe.exe').resolve()),mode,str(tree.resolve()),*extra],timeout=240,reap_timeout=10,tail_bytes=65536,output_dir=a.output)
        lines,valid,metrics=sanitized(result.output_tail,candidate);valid=valid and good(result)
        lines.append('SHIPPING_RUNNER exit_code=%d timed_out=%d reaped=%d truncated=%d cleanup_error=%d elapsed_seconds=%.3f'%(result.exit_code,result.timed_out,result.reaped,result.output_truncated,bool(result.cleanup_error),result.elapsed_seconds))
        lines.append('SHIPPING_EVIDENCE_VALID code='+str(0 if valid else 1))
        (a.output/(name+'.log')).write_text('\n'.join(lines)+'\n');print('SHIPPING_MODE='+name,flush=True);print('\n'.join(lines),flush=True)
        if valid:summary[name]=metrics
        failed|=not valid
    (a.output/'shipping-totals.json').write_text(json.dumps({'verification_already_in_launch_total':True,'conditions':summary},indent=2)+'\n')
    return int(failed)
if __name__=='__main__':
    try:code=main()
    except Exception:print('SHIPPING_SETUP code=2',flush=True);code=2
    raise SystemExit(code)
