#!/usr/bin/env python3
"""Build-only Windows diagnostic seed. Never invoked by the application."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import json


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''): h.update(block)
    return h.hexdigest()


def inventory(root):
    rows = []
    total = 0
    paths = []
    for path in root.rglob('*'):
        paths.append(path)
        if len(paths) > 65536: raise ValueError('inventory entry bound')
    for path in sorted(paths):
        st = path.lstat()
        if path.is_symlink() or getattr(st, 'st_file_attributes', 0) & 0x400:
            raise ValueError('linked entry')
        rel = path.relative_to(root).as_posix()
        if any(c in rel for c in '\t\r\n') or not (path.is_dir() or path.is_file()):
            raise ValueError('invalid entry')
        if path.is_dir(): rows.append('D\t' + rel)
        else:
            total += st.st_size
            if total > 1024**3: raise ValueError('inventory byte bound')
            rows.append('F\t' + sha(path) + '\t' + rel)
    if len(rows) > 65536 or total > 1024**3: raise ValueError('inventory bounds')
    return rows


def prepare(runtime, destination):
    if os.name != 'nt': raise ValueError('native Windows generation required')
    runtime = runtime.resolve(strict=True)
    destination = destination.absolute()
    if destination.exists() or runtime in destination.parents: raise ValueError('new external destination required')
    before = inventory(runtime)
    manifest = json.loads((runtime / 'stormtty-shell-manifest.json').read_text())
    for record in manifest['files']:
        rel = record['path']
        if '..' in Path(rel).parts or ':' in rel or '\\' in rel or rel.startswith('/'):
            raise ValueError('invalid manifest')
        if not record['directory'] and sha(runtime / rel) != record['sha256']:
            raise ValueError('manifest mismatch')
    if (runtime / 'stormtty/zsh/.zshrc.zwc').exists(): raise ValueError('compiled loader refused')
    destination.mkdir(parents=True)
    try:
        with tempfile.TemporaryDirectory(prefix='seed-build-', dir=destination.parent) as scratch:
            work = Path(scratch)
            for name in ('home', 'empty', 'out'): (work / name).mkdir()
            # -f excludes startup files; -d excludes global rc files. No shell
            # interpolation of paths supplied by Python. Match managed keymap.
            script = r'''
setopt NO_RCS NO_GLOBAL_RCS
bindkey -e
runtime=$1; work=$2
runtime=${runtime//\\//}; work=${work//\\//}
[[ $runtime == [A-Za-z]:/* ]] && runtime="/${(L)runtime[1]}${runtime[3,-1]}"
[[ $work == [A-Za-z]:/* ]] && work="/${(L)work[1]}${work[3,-1]}"
ZSH=$runtime/stormtty/vendor/oh-my-zsh
ZSH_CUSTOM=$ZSH/custom
fpath=($ZSH/{functions,completions} $ZSH_CUSTOM/{functions,completions} "$work/empty" /usr/share/zsh/functions/**/*(N/) /usr/share/zsh/$ZSH_VERSION/functions/**/*(N/))
autoload -Uz compinit
compinit -i -d "$work/out/seed.dump" || exit 10
[[ ${_comp_secure-no} == no && ${+_comps[ls]} == 1 ]] || exit 11
print -r -- "$ZSH_VERSION" > "$work/out/version"
for dir in $fpath; do
  if [[ $dir == "$runtime/"* ]]; then print -r -- "@R/${dir#$runtime/}"
  elif [[ $dir == "$work/empty" ]]; then print -r -- '@C'
  elif [[ $dir == /usr/* ]]; then print -r -- "@U$dir"
  else exit 12; fi
done > "$work/out/fpath"
'''
            env = dict(os.environ)
            for key in list(env):
                if key.startswith(('STORMTTY_', 'ZSH_')): env.pop(key)
            env.update(HOME=str(work / 'home'), ZDOTDIR=str(work / 'home'),
                       PATH=str(runtime / 'usr/bin'), MSYSTEM='MSYS', LANG='C.UTF-8', LC_ALL='C.UTF-8',
                       TERM='xterm-256color', COLORTERM='truecolor')
            result = subprocess.run([str(runtime / 'usr/bin/zsh.exe'), '-d', '-f', '-i', '-c', script,
                                     'seed-build', str(runtime), str(work)], env=env,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=90)
            if result.returncode: raise ValueError('native generation failed')
            version = (work / 'out/version').read_text().strip()
            if not version or any(c not in '0123456789.' for c in version): raise ValueError('version')
            paths = (work / 'out/fpath').read_text().splitlines()
            if len(paths) > 256 or paths.count('@C') != 1: raise ValueError('fpath')
            seed = work / 'out/seed.dump'
            if seed.stat().st_size > 1024**2: raise ValueError('seed bound')
            # No build-private absolute paths may escape in executable seed text.
            data = seed.read_bytes()
            if str(work).encode() in data or b'#omz ' in data: raise ValueError('nonpristine seed')
            shutil.copyfile(seed, destination / 'seed.dump')
            # Obtain the fixed patched OMZ metadata line, never run git.
            import re
            omz = (runtime / 'stormtty/vendor/oh-my-zsh/oh-my-zsh.sh').read_text()
            match = re.search(r'REPLY=([0-9a-f]{40}) # StormTTY: immutable packaged revision', omz)
            if not match: raise ValueError('revision marker')
            rows = ['STORMTTY_SEED_V1', version, match[1], sha(destination / 'seed.dump'),
                    str(len(paths)), *paths, *before]
            (destination / 'identity.txt').write_text('\n'.join(rows) + '\n', encoding='utf-8', newline='\n')
        if inventory(runtime) != before: raise ValueError('runtime changed during generation')
    except Exception:
        shutil.rmtree(destination)
        raise

def write_trust_header(package, destination):
    # Build output only: callers compile this anchor into their native binary.
    # A user-selected adjacent checksum file is not an equivalent trust source.
    identity = package / 'identity.txt'
    if identity.stat().st_size > 8 * 1024**2: raise ValueError('identity bound')
    digest = sha(identity)
    with destination.open('x', encoding='ascii', newline='\n') as out:
        out.write('#pragma once\nnamespace stormtty::build_seed {\n'
                  'inline constexpr char identity_sha256[] = "' + digest + '";\n}\n')

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--runtime', type=Path, required=True)
    p.add_argument('--destination', type=Path, required=True)
    p.add_argument('--trust-header', type=Path)
    args = p.parse_args()
    try:
        prepare(args.runtime, args.destination)
        if args.trust_header: write_trust_header(args.destination, args.trust_header)
        print('SEED_BUILD code=0')
    except Exception:
        print('SEED_BUILD code=1')
        raise SystemExit(1)
