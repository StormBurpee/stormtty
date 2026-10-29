#!/usr/bin/env python3
"""Run fixed real-Zsh/native-child fixture with actual Ghostty response feedback."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
HERE = Path(__file__).resolve().parent

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec); sys.modules[name] = mod; spec.loader.exec_module(mod)
    return mod

def passed(result):
    return (result.exit_code == 0 and result.reaped and not result.timed_out
            and not result.output_truncated and not result.cleanup_error
            and b'result=PASS' in result.output_tail)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('original', 'build', 'destination', 'host', 'native', 'output', 'engine'):
        p.add_argument('--'+name, type=Path, required=True)
    args = p.parse_args()
    if os.name != 'nt': p.error('Native Windows required')
    args.output.mkdir()
    stage1 = load('bridge_stage1', HERE/'qualify-windows.py')
    identity = stage1.stage(args.original,args.build,args.destination)
    native = args.destination/'usr/bin/stormtty-bridge-native.exe'
    if native.exists(): raise ValueError('Native fixture collision')
    shutil.copy2(args.native,native)
    home=args.destination/'bridge-home';home.mkdir()
    shutil.copy2(HERE/'stage2.zshrc',home/'.zshrc')
    identity.update(scope='real-bundled-zsh-controlled-fixture-stage2',
                    host_sha256=stage1.sha(args.host), native_sha256=stage1.sha(native),
                    zsh_sha256=stage1.sha(args.destination/'usr/bin/zsh.exe'),
                    zshrc_sha256=stage1.sha(home/'.zshrc'),
                    ghostty_sha256=stage1.sha(args.host.parent/'ghostty-vt.dll'),
                    engine=json.loads((args.engine/'stormtty-build.json').read_text()))
    (args.output/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    os.environ.update(HOME='/bridge-home',ZDOTDIR='/bridge-home',TERM='xterm-256color',
                      LANG='C.UTF-8',LC_ALL='C.UTF-8',MSYS='noglob',MSYSTEM='MSYS',
                      STORMTTY_BRIDGE_NATIVE='/usr/bin/stormtty-bridge-native.exe',
                      PATH=str(args.destination.resolve()/'usr/bin')+os.pathsep+str(Path(os.environ['SystemRoot'])/'System32'))
    runner=load('stage2_owned_runner',HERE.parents[1]/'run-owned-process.py')
    result=runner.run_owned([str(args.host.resolve()),str(args.destination.resolve()/'usr/bin/stormtty-bridge-broker.exe')],
                            timeout=40,reap_timeout=5,tail_bytes=65536,cwd=home,output_dir=args.output)
    (args.output/'case.log').write_bytes(result.output_tail)
    entry={'exit_code':result.exit_code,**result.metadata()}
    (args.output/'results.json').write_text(json.dumps(entry,indent=2)+'\n')
    print('BRIDGE_STAGE2_OWNED='+json.dumps(entry,sort_keys=True),flush=True)
    for base in (args.original,args.destination):
        if stage1.sha(base/'usr/bin/msys-2.0.dll')!=identity['runtime_sha256']:
            raise RuntimeError('Pinned runtime changed')
    return int(not passed(result))
if __name__=='__main__': raise SystemExit(main())
