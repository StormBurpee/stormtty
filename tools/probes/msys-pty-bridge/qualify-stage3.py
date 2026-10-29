#!/usr/bin/env python3
"""Full shipped managed loader with fresh/warm scratch state; no default switch."""
import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import sys
HERE=Path(__file__).resolve().parent

def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    mod=importlib.util.module_from_spec(spec);sys.modules[name]=mod;spec.loader.exec_module(mod)
    return mod

def samples(tail):
    found=[]
    for line in tail.decode('utf-8',errors='replace').splitlines():
        if not line.startswith('BRIDGE_MANAGED_SAMPLE ') or not line.endswith(' result=PASS'):continue
        fields=dict(item.split('=',1) for item in line.split()[1:])
        for key in ('state_prepare_ms','launch_to_zle_observation_ms','typed_echo_median_ms','typed_echo_p95_ms','typed_echo_max_ms'):
            fields[key]=float(fields[key])
            if not math.isfinite(fields[key]) or fields[key]<0:raise ValueError('Invalid timing')
        for key in ('sample','fresh','phases','typed_echo_samples','startup_cursor_reports','native_cursor_reports','warm_dump_unchanged'):
            fields[key]=int(fields[key])
        found.append(fields)
    return found

def passed(result,records):
    return (result.exit_code==0 and result.reaped and not result.timed_out and not result.output_truncated
            and not result.cleanup_error and b'BRIDGE_MANAGED_RESULT fresh=1 warm=1 result=PASS' in result.output_tail
            and len(records)==2 and [r['sample'] for r in records]==[1,2]
            and [r['fresh'] for r in records]==[1,0] and records[1]['warm_dump_unchanged']==1
            and all(r['phases']==10 and r['startup_cursor_reports']>0 and r['native_cursor_reports']>0 and r['typed_echo_samples']>0 for r in records))

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('original','build','destination','host','native','output','engine'):
        p.add_argument('--'+name,type=Path,required=True)
    args=p.parse_args()
    if os.name!='nt':p.error('Native Windows required')
    args.output.mkdir()
    stage1=load('managed_bridge_stage1',HERE/'qualify-windows.py')
    identity=stage1.stage(args.original,args.build,args.destination)
    native=args.destination/'usr/bin/stormtty-bridge-native.exe'
    if native.exists():raise ValueError('Native fixture collision')
    shutil.copy2(args.native,native)
    identity.update(scope='full-shipped-managed-loader-fresh-warm-stage3',
                    host_sha256=stage1.sha(args.host),native_sha256=stage1.sha(native),
                    console_setup_sha256=stage1.sha(args.host.parent/'stormtty-console-setup.exe'),
                    zsh_sha256=stage1.sha(args.destination/'usr/bin/zsh.exe'),
                    shipped_loader_sha256=stage1.sha(args.destination/'stormtty/zsh/.zshrc'),
                    observer_config_sha256=stage1.sha(HERE/'stage3-config.zsh'),
                    ghostty_sha256=stage1.sha(args.host.parent/'ghostty-vt.dll'),
                    engine=json.loads((args.engine/'stormtty-build.json').read_text()))
    (args.output/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    # Reset all managed feature overrides just as the existing native startup
    # probe does. Parent/user environment and all installed assets are untouched.
    for name in ('MSYS','ENV','BASH_ENV','STORMTTY_MSYS_RESIZE_TRACE_DIR','STORMTTY_SHELL_TOOLS',
                 'STORMTTY_FZF','STORMTTY_FZF_COMPLETION','STORMTTY_FZF_KEYS','STORMTTY_ZOXIDE',
                 '_ZO_DATA_DIR','STORMTTY_AUTOSUGGESTIONS','STORMTTY_HIGHLIGHTING','STORMTTY_PROMPT',
                 'ZSH_AUTOSUGGEST_USE_ASYNC','ZSH_AUTOSUGGEST_STRATEGY','SHELL'):
        os.environ.pop(name,None)
    os.environ.update(TERM='xterm-256color',COLORTERM='truecolor',LANG='C.UTF-8',LC_ALL='C.UTF-8',MSYSTEM='MSYS',
                      STORMTTY_BRIDGE_NATIVE='/usr/bin/stormtty-bridge-native.exe',
                      PATH=str(args.destination.resolve()/'usr/bin')+os.pathsep+str(Path(os.environ['SystemRoot'])/'System32'))
    runner=load('managed_bridge_owned',HERE.parents[1]/'run-owned-process.py')
    result=runner.run_owned([str(args.host.resolve()),str(args.destination.resolve()),str(HERE/'stage3-config.zsh')],
                            timeout=80,reap_timeout=5,tail_bytes=65536,output_dir=args.output)
    (args.output/'case.log').write_bytes(result.output_tail)
    records=samples(result.output_tail)
    entry={'exit_code':result.exit_code,**result.metadata(),'samples':records,
           'timing_scope':'synthetic host enqueue to real VT glyph/cursor observation; includes 2ms poll, not physical display latency'}
    (args.output/'results.json').write_text(json.dumps(entry,indent=2)+'\n')
    print('BRIDGE_MANAGED_OWNED='+json.dumps(entry,sort_keys=True),flush=True)
    for base in (args.original,args.destination):
        if stage1.sha(base/'usr/bin/msys-2.0.dll')!=identity['runtime_sha256']:raise RuntimeError('Runtime changed')
    if stage1.sha(args.destination/'stormtty/zsh/.zshrc')!=identity['shipped_loader_sha256']:raise RuntimeError('Shipped loader changed')
    return int(not passed(result,records))
if __name__=='__main__':raise SystemExit(main())
