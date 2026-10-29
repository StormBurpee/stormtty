#!/usr/bin/env python3
"""Offline POSIX source-stage diagnostic; NOT Windows or first-ZLE measurement."""
import argparse
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

TOOLS = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--assets-cache', type=Path, required=True)
parser.add_argument('--tools-runtime', type=Path, required=True,
                    help='Prepared pinned runtime supplying real fzf/zoxide source files')
parser.add_argument('--samples', type=int, default=8)
parser.add_argument('--zprof', action='store_true', help='Print function attribution after numeric samples')
args = parser.parse_args()
if os.name != 'posix' or not 2 <= args.samples <= 100:
    parser.error('requires POSIX and 2..100 samples; use native probe on Windows')
if (args.tools_runtime/'stormtty/vendor/shell-tools/tools.lock.json').read_bytes() != (TOOLS/'shell-tools.lock.json').read_bytes():
    parser.error('tools runtime lock differs from this checkout')
manifest = json.loads((args.tools_runtime/'stormtty-shell-manifest.json').read_text())
records = {entry['path']:entry for entry in manifest['files']}
for relative in ['ucrt64/share/fzf/completion.zsh', 'ucrt64/share/fzf/key-bindings.zsh', 'ucrt64/share/zsh/site-functions/_zoxide']:
    if hashlib.sha256((args.tools_runtime/relative).read_bytes()).hexdigest() != records[relative]['sha256']:
        parser.error('runtime source inventory mismatch: '+relative)
spec = importlib.util.spec_from_file_location('addons', TOOLS / 'shell-addons.py')
addons = importlib.util.module_from_spec(spec); spec.loader.exec_module(addons)
with tempfile.TemporaryDirectory(prefix='startup-source-', dir=TOOLS.parent) as temporary:
    root = Path(temporary); runtime = root/'runtime'; home = root/'home'
    home.mkdir(mode=0o700)
    addons.stage(runtime, args.assets_cache, True, {})
    for relative in ['ucrt64/share/fzf', 'ucrt64/share/zsh']:
        shutil.copytree(args.tools_runtime/relative, runtime/relative)
    tools_dir = runtime/'stormtty/vendor/shell-tools'; tools_dir.mkdir()
    for src, dest in [('shell-tools.zsh','init.zsh'), ('shell-tools-zoxide.zsh','zoxide.zsh')]:
        shutil.copyfile(TOOLS/src, tools_dir/dest)
    zdot = runtime/'stormtty/zsh'; zdot.mkdir()
    source = (TOOLS/'shell-preset.zsh').read_text()
    # Instrument only the disposable fixture, never the packaged startup path.
    stages = [('_stormtty_private_state\nunfunction', 'private_state'),
              ('autoload -Uz add-zsh-hook compinit', 'fpath'),
              ('if [[ $STORMTTY_OMZ_LOADED != 1 ]]', 'omz'),
              ('# Only packaged modules;', 'tools_and_config'),
              ('zmodload zsh/datetime', 'autosuggestions'),
              ('typeset +x STORMTTY_MANAGED_ZSH', 'context_and_highlighting')]
    prefix = 'zmodload zsh/datetime\ntypeset -F _measure_last=$EPOCHREALTIME\n'
    def tick(name):
        return ('print -r -- "STAGE '+name+' $(( (EPOCHREALTIME - _measure_last) * 1000 ))"\n'
                '_measure_last=$EPOCHREALTIME\n')
    for marker, name in stages:
        assert source.count(marker) == 1, marker
        if name == 'private_state':
            source = source.replace(marker, '_stormtty_private_state\n'+tick(name)+'unfunction')
        else:
            source = source.replace(marker, tick(name)+marker)
    (zdot/'.zshrc').write_text(prefix+source)
    env = dict(os.environ, HOME=str(home), ZDOTDIR=str(zdot), STORMTTY_SESSION_ID='source-profile', TERM='xterm-256color')
    for name in ['STORMTTY_AUTOSUGGESTIONS','STORMTTY_HIGHLIGHTING','STORMTTY_SHELL_TOOLS','STORMTTY_FZF','STORMTTY_FZF_KEYS','STORMTTY_FZF_COMPLETION','STORMTTY_ZOXIDE','STORMTTY_EMPTY_HOME','STORMTTY_NATIVE_ZSH_STATE']:
        env.pop(name, None)
    proof = '[[ $STORMTTY_OMZ_LOADED == 1 && $STORMTTY_EPHEMERAL == 0 && ${+functions[_zsh_autosuggest_start]} == 1 && ${+functions[_zsh_highlight]} == 1 && ${+functions[fzf-file-widget]} == 1 && ${+functions[z]} == 1 && ${+_comps[ls]} == 1 ]]'
    for sample in range(args.samples):
        command = 'source "$ZDOTDIR/.zshrc"; '+proof
        if args.zprof: command = 'zmodload zsh/zprof; '+command+' || exit 1; zprof'
        start = time.perf_counter()
        result = subprocess.run(['zsh','-dfic',command],cwd=root,env=env,capture_output=True,text=True,timeout=30)
        elapsed = (time.perf_counter()-start)*1000
        if result.returncode: raise RuntimeError('source/module validation failed: '+result.stderr)
        values = {line.split()[1]:round(float(line.split()[2]),3) for line in result.stdout.splitlines() if line.startswith('STAGE ')}
        print(json.dumps(dict(sample=sample+1, fresh_state=sample == 0, platform='POSIX source diagnostic', process_to_exit_ms=round(elapsed,3), stages_ms=values)))
        if args.zprof: print('\n'.join(line for line in result.stdout.splitlines() if not line.startswith('STAGE ')))
