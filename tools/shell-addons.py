#!/usr/bin/env python3
"""Build-only pinned source modules. No installers or plugin managers are run."""
import hashlib
import json
from pathlib import Path
import re
import tarfile
import tempfile
import urllib.request
import urllib.parse

LOCK = Path(__file__).with_name('shell-addons.lock.json')
OMZ_LIBS = {'async_prompt.zsh', 'compfix.zsh', 'completion.zsh', 'history.zsh',
            'key-bindings.zsh', 'git.zsh'}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def read_lock():
    lock = json.loads(LOCK.read_text())
    if lock['schema'] != 1: raise RuntimeError('Unknown addons lock')
    for c in lock['components']:
        if not re.fullmatch('[a-z-]+', c['name']) or not re.fullmatch('[0-9a-f]{40}', c['revision']):
            raise RuntimeError('Invalid addon identity')
        repo = c['repository'].removeprefix('https://github.com/')
        if not re.fullmatch(r'[A-Za-z0-9_-]+/[A-Za-z0-9_-]+', repo): raise RuntimeError('Invalid source repository')
        if c['url'] != f'https://codeload.github.com/{repo}/tar.gz/{c["revision"]}': raise RuntimeError('Non-pinned source URL')
        if c['filename'] != f'{c["name"]}-{c["revision"]}.tar.gz': raise RuntimeError('Invalid cache name')
        if not re.fullmatch('[0-9a-f]{64}', c['sha256']) or not 0 < c['bytes'] < 16*1024*1024: raise RuntimeError('Invalid integrity pin')
    return lock

def acquire(c, cache, offline):
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / c['filename']
    if path.is_symlink(): raise RuntimeError('Linked addon cache refused')
    if not path.exists():
        if offline: raise RuntimeError(f'Offline addon archive missing: {path}')
        with tempfile.NamedTemporaryFile(dir=cache, delete=False) as out:
            tmp = Path(out.name)
            try:
                with urllib.request.urlopen(c['url'], timeout=120) as response:
                    if urllib.parse.urlsplit(response.geturl()).netloc != 'codeload.github.com': raise RuntimeError('Unexpected source redirect')
                    data = response.read(c['bytes'] + 1)
                if len(data) != c['bytes'] or hashlib.sha256(data).hexdigest() != c['sha256']: raise RuntimeError('Addon download integrity failed')
                out.write(data); out.close(); tmp.replace(path)
            finally: tmp.unlink(missing_ok=True)
    if not path.is_file() or path.stat().st_size != c['bytes'] or digest(path) != c['sha256']: raise RuntimeError('Addon cache integrity failed')
    return path

def selected(component, name):
    if name == component['notice']: return True
    if component['name'] == 'oh-my-zsh':
        return name == 'oh-my-zsh.sh' or name.startswith('functions/') or name.startswith('completions/') or (name.startswith('lib/') and name[4:] in OMZ_LIBS)
    if component['name'] == 'zsh-autosuggestions': return name == 'zsh-autosuggestions.zsh'
    return name in ('zsh-syntax-highlighting.zsh', '.version', '.revision-hash') or name.startswith('highlighters/') and name.endswith('.zsh')

def patch_loader(data, revision):
    old = b'_omz_git_head || REPLY="$(builtin cd -q "$ZSH"; git rev-parse HEAD 2>/dev/null)"'
    if data.count(old) != 1: raise RuntimeError('Upstream loader patch no longer applies')
    data = data.replace(old, f'REPLY={revision} # StormTTY: immutable packaged revision, no startup Git fork'.encode())
    # Keep the real OMZ loader/libraries when persistence is unavailable. Only
    # its cache-writing completion setup is replaced with secure no-dump init.
    first = b'# Make sure $ZSH_CACHE_DIR is writable'
    last = b'# Check for updates on initial load...'
    if data.count(first) != 1 or data.count(last) != 1: raise RuntimeError('OMZ cache patch no longer applies')
    begin, end = data.index(first), data.index(last)
    data = data[:begin] + b'if [[ $STORMTTY_EPHEMERAL != 1 ]]; then\n' + data[begin:end] + b'fi\n\n' + data[end:]
    first = b'# Construct zcompdump OMZ metadata.'
    last = b'_omz_source() {'
    if data.count(first) != 1 or data.count(last) != 1: raise RuntimeError('OMZ completion patch no longer applies')
    begin, end = data.index(first), data.index(last)
    data = data[:begin] + b'if [[ $STORMTTY_EPHEMERAL == 1 ]]; then\n  compinit -i -D\n  unset -f _omz_git_head\nelse\n' + data[begin:end] + b'fi\n\n' + data[end:]
    # Avoid two cold-state utility processes, without changing compinit's audit,
    # cache freshness checks, lock protocol, or the upstream module set.
    old = b'  command rm -f "$ZSH_COMPDUMP"'
    if data.count(old) != 1: raise RuntimeError('OMZ absent dump patch no longer applies')
    data = data.replace(old, b'  [[ ! -e "$ZSH_COMPDUMP" ]] || command rm -f "$ZSH_COMPDUMP"')
    old = b'''  # Use `tee` in case the $ZSH_COMPDUMP filename is invalid, to silence the error
  # See https://github.com/ohmyzsh/ohmyzsh/commit/dd1a7269#commitcomment-39003489
  tee -a "$ZSH_COMPDUMP" &>/dev/null <<EOF

$zcompdump_revision
$zcompdump_fpath
EOF'''
    if data.count(old) != 1: raise RuntimeError('OMZ metadata append patch no longer applies')
    data = data.replace(old, b'''  # StormTTY: builtin append, no utility process for fixed metadata.
  { print -rl -- "" "$zcompdump_revision" "$zcompdump_fpath" >> "$ZSH_COMPDUMP" } 2>/dev/null''')
    return data

def stage(root, cache, offline, records):
    lock = read_lock()
    def write(name, data, package):
        target = root / name
        if target.exists() or name.casefold() in records: raise RuntimeError('Addon payload collision')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        records[name.casefold()] = dict(path=name, directory=False, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), package=package)
    for c in lock['components']:
        archive = acquire(c, cache, offline)
        seen = set(); count = total = archive_bytes = 0
        with tarfile.open(archive, 'r:gz') as tar:
            for member in tar:
                count += 1
                if count > 10000: raise RuntimeError('Addon member bound exceeded')
                parts = member.name.rstrip('/').split('/')
                if member.name.startswith('/') or '\\' in member.name or any(not p or p in ('.','..') or ':' in p or any(ord(ch) < 32 for ch in p) or p.endswith((' ', '.')) or re.fullmatch(r'(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?', p) for p in parts): raise RuntimeError('Unsafe addon path')
                expected_root = c['repository'].rsplit('/', 1)[-1] + '-' + c['revision']
                if parts[0] != expected_root: raise RuntimeError('Unexpected addon archive root')
                archive_bytes += member.size
                if archive_bytes > 64*1024*1024: raise RuntimeError('Addon expanded archive bound exceeded')
                name = '/'.join(parts[1:])
                if not name or member.isdir() or not selected(c, name): continue
                if not member.isfile() or member.size > 2*1024*1024 or name.casefold() in seen: raise RuntimeError('Unsafe addon member')
                seen.add(name.casefold()); total += member.size
                if total > 16*1024*1024: raise RuntimeError('Addon payload bound exceeded')
                data = tar.extractfile(member).read()
                if c['name'] == 'oh-my-zsh' and name == 'oh-my-zsh.sh':
                    data = patch_loader(data, c['revision'])
                write(f'stormtty/vendor/{c["name"]}/{name}', data, c['name'])
        if c['notice'].casefold() not in seen: raise RuntimeError('Missing upstream license')
    write('stormtty/vendor/oh-my-zsh/tools/check_for_upgrade.sh', b'# StormTTY immutable bundle: no migration, network, or self-update.\nreturn 0\n', 'stormtty-addon-policy')
    write('stormtty/vendor/oh-my-zsh/custom/.keep', b'', 'stormtty-addon-policy')
    write('stormtty/vendor/addons.lock.json', LOCK.read_bytes(), 'stormtty-addon-policy')
