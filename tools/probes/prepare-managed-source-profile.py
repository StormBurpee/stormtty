#!/usr/bin/env python3
"""Create a new disposable profiled runtime; never mutate the original bundle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat

TOOLS = Path(__file__).resolve().parents[1]
STAGES = ('bootstrap','private_state','fpath','omz','defaults_config','tools','autosuggestions','context_hooks','highlighting')
MARKERS = (
    ('\n_stormtty_private_state\nunfunction', 'bootstrap'),
    ('unfunction _stormtty_private_state', 'private_state'),
    ('autoload -Uz add-zsh-hook compinit', 'fpath'),
    ('if [[ $STORMTTY_EPHEMERAL == 1 ]]; then\n  unset HISTFILE', 'omz'),
    ('[[ -r $_stormtty_vendor/shell-tools/init.zsh ]]', 'defaults_config'),
    ('# Only packaged modules;', 'tools'),
    ('zmodload zsh/datetime\nzmodload zsh/stat', 'autosuggestions'),
    ('# Highlighting is loaded last,', 'context_hooks'),
    ('typeset +x STORMTTY_MANAGED_ZSH', 'highlighting'),
)

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def instrument(source):
    for marker, name in MARKERS:
        if source.count(marker) != 1: raise ValueError('loader marker drift: '+name)
        tick=(f'\n_profile_rows+=("{name} $(( (EPOCHREALTIME - _profile_last) * 1000 ))")\n'
              '_profile_last=$EPOCHREALTIME\n')
        source=source.replace(marker,tick+marker)
    prefix=('zmodload zsh/datetime\nzmodload zsh/zprof\n'
            'typeset -ga _profile_rows=()\ntypeset -gF _profile_last=$EPOCHREALTIME\n')
    suffix='''
# Private diagnostic files only, after all timed source intervals. No transcript.
[[ -n $_stormtty_state && $STORMTTY_EPHEMERAL == 0 ]] || return 1
typeset _profile_umask=$(umask)
umask 077
print -rl -- "${_profile_rows[@]}" > "$_stormtty_state/startup-source-stages"
zprof > "$_stormtty_state/startup-source-functions"
umask "$_profile_umask"
unset _profile_umask _profile_rows _profile_last
'''
    return prefix+source+suffix

def prepare(original, destination):
    original=original.absolute();destination=destination.absolute()
    if os.path.lexists(destination) or original==destination or original in destination.parents:
        raise ValueError('destination must be new and outside original')
    # Resolve no aliases, including ancestors; copying inside original through
    # a linked destination parent must not mutate the original tree either.
    for path in (*original.parents, destination.parent, *destination.parent.parents):
        info=path.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info,'st_file_attributes',0)&0x400:
            raise ValueError('linked runtime ancestor refused')
    # No symlink/reparse copying. Verify every manifest file before and after copy.
    for path in (original, *original.rglob('*')):
        info=path.lstat()
        if not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)) or getattr(info,'st_file_attributes',0)&0x400:
            raise ValueError('nonregular or linked runtime refused')
    manifest=json.loads((original/'stormtty-shell-manifest.json').read_text())
    files=[]
    for record in manifest['files']:
        rel=record['path'];path=Path(rel)
        if path.is_absolute() or '..' in path.parts or '\\' in rel or ':' in rel:
            raise ValueError('unsafe inventory path')
        if not record['directory']:
            if sha(original/path)!=record['sha256']:raise ValueError('runtime integrity mismatch')
            files.append(record)
    inventoried={record['path'] for record in files}
    permitted={'stormtty-shell-manifest.json','notices/source-manifest.json','notices/shell-bootstrap.lock.json','notices/THIRD-PARTY-NOTICES.txt'}
    for path in original.rglob('*'):
        if path.is_file() and path.relative_to(original).as_posix() not in inventoried|permitted:
            raise ValueError('uninventoried runtime file')
    if (original/'stormtty/zsh/.zshrc.zwc').exists():
        raise ValueError('compiled loader not permitted for source attribution')
    # Source lock identities are exact; immutable packaged module bytes stay intact.
    for rel,lock in [('stormtty/vendor/addons.lock.json','shell-addons.lock.json'),
                     ('stormtty/vendor/shell-tools/tools.lock.json','shell-tools.lock.json')]:
        if (original/rel).read_bytes()!=(TOOLS/lock).read_bytes():raise ValueError('source lock differs')
    loader='stormtty/zsh/.zshrc'
    baseline=(original/loader).read_text()
    if baseline!=(TOOLS/'shell-preset.zsh').read_text():raise ValueError('staged loader differs from checkout')
    measured=instrument(baseline)
    shutil.copytree(original,destination)
    for record in files:
        if sha(destination/record['path'])!=record['sha256']:raise ValueError('copy integrity mismatch')
    (destination/loader).write_bytes(measured.encode())
    details=dict(schema=1,original_loader_sha256=sha(original/loader),profile_loader_sha256=sha(destination/loader),
                 original_manifest_sha256=sha(original/'stormtty-shell-manifest.json'),
                 source_only_instrumentation=True,stages=list(STAGES))
    for record in manifest['files']:
        if record['path']==loader:record.update(sha256=details['profile_loader_sha256'],bytes=len(measured.encode()),package='private-source-profile-fixture')
    manifest.update(qualification_only=True,default_activation_allowed=False,public_redistribution_ready=False,
                    native_qualification={'status':'instrumented-diagnostic-only'})
    (destination/'stormtty-shell-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (destination/'stormtty-source-profile.json').write_text(json.dumps(details,sort_keys=True)+'\n')
    return details

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('original',type=Path);p.add_argument('destination',type=Path)
    a=p.parse_args(); print(json.dumps(prepare(a.original,a.destination),sort_keys=True))
