#!/usr/bin/env python3
"""Build-only fzf/zoxide qualification overlay; never install or execute packages.

Standalone output is NOT a complete runtime. Integrators call stage() before the
main runtime manifest is written. Public redistribution remains blocked pending
static Go/Rust dependency notices/source review and native qualification.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import tempfile

TOOLS = Path(__file__).resolve().parent
LOCK = TOOLS / 'shell-tools.lock.json'
spec = importlib.util.spec_from_file_location('shell_bootstrap', TOOLS / 'shell-bootstrap.py')
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


def read_lock():
    lock = json.loads(LOCK.read_text())
    if (lock['schema'] != 1 or lock['platform'] != 'windows-x86_64' or lock['prefix'] != 'ucrt64'
            or lock['native_qualification'] != 'pending' or lock['public_redistribution_ready'] is not False
            or lock['static_dependency_notices_complete'] is not False):
        raise RuntimeError('Unsupported tools qualification lock')
    if bootstrap.digest(TOOLS / 'shell-tools-sources.lock.json') != lock['static_source_inventory']['sha256']:
        raise RuntimeError('Static source inventory integrity mismatch')
    seen = set()
    for kind, prefix in [('packages', 'ucrt64'), ('sources', 'sources')]:
        for item in lock[kind]:
            name = item['filename']
            if len(bootstrap.portable_path(name).parts) != 1 or name in seen:
                raise RuntimeError('Invalid tool archive filename')
            seen.add(name)
            if item['url'] != f'https://repo.msys2.org/mingw/{prefix}/{name}':
                raise RuntimeError('Tool archive is not an exact official URL')
            if not re.fullmatch('[0-9a-f]{64}', item['sha256']) or not 0 < item['bytes'] < 32*1024*1024:
                raise RuntimeError('Invalid tool integrity pin')
    sources = {s['filename'] for s in lock['sources']}
    if {p['name'] for p in lock['packages']} != {'mingw-w64-ucrt-x86_64-fzf', 'mingw-w64-ucrt-x86_64-zoxide'}:
        raise RuntimeError('Unexpected tool package set')
    for package in lock['packages']:
        if package['source'] not in sources or package['runtime_dependencies']:
            raise RuntimeError('Unreviewed dependency closure')
    support = lock['msys_support']
    if {p['name'] for p in support['packages']} != {'gawk', 'libreadline', 'mpfr'}:
        raise RuntimeError('Unexpected MSYS support package set')
    expected_dependencies = {'gawk': ['libintl', 'libreadline', 'mpfr', 'sh'], 'libreadline': ['ncurses'], 'mpfr': ['gmp>=5.0']}
    support_sources = {p['filename'] for p in support['sources']}
    for kind, prefix in [('packages', 'x86_64'), ('sources', 'sources')]:
        for item in support[kind]:
            name = item['filename']
            if len(bootstrap.portable_path(name).parts) != 1 or not re.fullmatch('[0-9a-f]{64}', item['sha256']) or not 0 < item['bytes'] < 32*1024*1024:
                raise RuntimeError('Invalid support archive pin')
            if item['url'] != f'https://repo.msys2.org/msys/{prefix}/{name}':
                raise RuntimeError('Support archive is not an exact official URL')
    for package in support['packages']:
        if sorted(package['runtime_dependencies']) != expected_dependencies[package['name']] or package['source'] not in support_sources:
            raise RuntimeError('Unreviewed MSYS support dependency closure')
    for asset in support['notice_assets']:
        if not asset['path'].startswith('shell-tools-notices/texts/') or not asset['destination'].startswith('stormtty/vendor/shell-tools/notices/'):
            raise RuntimeError('Unsafe support notice location')
        bootstrap.portable_path(asset['path']); bootstrap.portable_path(asset['destination'])
        if asset['source'] not in support_sources: raise RuntimeError('Unknown support notice source')
        path = TOOLS / asset['path']
        if path.is_symlink() or path.stat().st_size != asset['bytes'] or bootstrap.digest(path) != asset['sha256']:
            raise RuntimeError('Support notice integrity mismatch')
    return lock


def merge_sources(*groups):
    result = {}
    for group in groups:
        for item in group:
            previous = result.get(item['filename'])
            if previous and (previous['sha256'], previous['bytes']) != (item['sha256'], item['bytes']):
                raise RuntimeError('Conflicting source archive identity')
            result.setdefault(item['filename'], item)
    return list(result.values())


def stage(root, cache, offline, records, zstd=None, sources=False):
    """Write to a fresh private staging root, never an installed/user runtime."""
    lock = read_lock()
    cache.mkdir(parents=True, exist_ok=True)
    zstd = bootstrap.select_decompressor(zstd, cache, offline)
    total = count = 0

    def write(name, data, package, executable=False):
        target = root / name
        if name.casefold() in records or target.exists() or target.is_symlink():
            raise RuntimeError('Tool payload collision')
        # Check every parent against manifest aliases, existing links and files.
        relative = bootstrap.portable_path(name)
        for i in range(1, len(relative.parts)):
            parent = '/'.join(relative.parts[:i]); key = parent.casefold()
            old = records.get(key)
            if old and (old['path'] != parent or not old['directory']):
                raise RuntimeError('Tool parent collision')
            path = root / parent
            if path.is_symlink() or (path.exists() and not path.is_dir()):
                raise RuntimeError('Unsafe tool parent')
            records.setdefault(key, dict(path=parent, directory=True))
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('xb') as f: f.write(data)
        target.chmod(0o755 if executable else 0o644)
        records[name.casefold()] = dict(path=name, directory=False, bytes=len(data),
            sha256=hashlib.sha256(data).hexdigest(), package=package)

    for package in lock['packages']:
        archive = bootstrap.download(package, cache, offline)
        metadata = None
        with bootstrap.open_package(archive, zstd) as tar:
            for member in tar:
                count += 1; total += member.size
                if count > 1000 or total > 32*1024*1024:
                    raise RuntimeError('Tool archive exceeds bounds')
                path = bootstrap.portable_path(member.name)
                if not (member.isdir() or member.isfile()) or member.issparse() or member.size > 16*1024*1024:
                    raise RuntimeError('Unsafe tool member')
                name = path.as_posix()
                if name in bootstrap.METADATA:
                    name = f'.stormtty-packages/{package["name"]}/{name}'
                elif path.parts[0] != 'ucrt64':
                    raise RuntimeError('Tool escaped UCRT prefix')
                if member.isdir(): continue
                data = tar.extractfile(member).read()
                if member.name == '.PKGINFO': metadata = data.decode('utf-8')
                write(name, data, package['name'], bool(member.mode & 0o111))
        if metadata is None or f'pkgname = {package["name"]}\n' not in metadata or f'pkgver = {package["version"]}\n' not in metadata:
            raise RuntimeError('Tool package identity mismatch')
        if any(line.startswith('depend = ') for line in metadata.splitlines()):
            raise RuntimeError('Unreviewed tool dependency')
        if package['notice'].casefold() not in records:
            raise RuntimeError('Missing tool license')
    support = lock['msys_support']
    budget = {'entries': len(records), 'bytes': sum(r.get('bytes', 0) for r in records.values())}
    for package in support['packages']:
        archive = bootstrap.download(package, cache, offline)
        with bootstrap.open_package(archive, zstd) as tar:
            bootstrap.extract_package(tar, root, package['name'], records, budget)
        metadata = (root / '.stormtty-packages' / package['name'] / '.PKGINFO').read_text()
        dependencies = sorted(line[9:] for line in metadata.splitlines() if line.startswith('depend = '))
        if (f"pkgname = {package['name']}\n" not in metadata or f"pkgver = {package['version']}\n" not in metadata
                or dependencies != sorted(package['runtime_dependencies'])):
            raise RuntimeError('MSYS support package identity/dependency mismatch')
    for asset in support['notice_assets']:
        write(asset['destination'], (TOOLS / asset['path']).read_bytes(), 'stormtty-tools-support-notices')
    for source, name in [('shell-tools.zsh', 'init.zsh'), ('shell-tools-zoxide.zsh', 'zoxide.zsh'), ('shell-tools.lock.json', 'tools.lock.json'), ('shell-tools-sources.lock.json', 'sources.lock.json')]:
        write('stormtty/vendor/shell-tools/' + name, (TOOLS / source).read_bytes(), 'stormtty-tools-policy')
    if sources:
        for source in merge_sources(lock['sources'], support['sources']): bootstrap.download(source, cache, offline)
    return lock


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--zstd')
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--sources', action='store_true')
    args = parser.parse_args()
    destination = args.destination.absolute()
    if destination.exists() or destination.is_symlink(): raise RuntimeError('Destination must be fresh')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=destination.parent, prefix='.shell-tools-') as tmp:
        root = Path(tmp) / 'overlay'; root.mkdir(); records = {}
        lock = stage(root, args.cache, args.offline, records, args.zstd, args.sources)
        (root / 'shell-tools-manifest.json').write_text(json.dumps(dict(schema=1,
            complete_runtime=False, native_qualification='pending', public_redistribution_ready=False,
            static_dependency_notices_complete=False, lock_sha256=bootstrap.digest(LOCK),
            sources=lock['sources'], files=[r for r in records.values() if not r['directory']]), indent=2)+'\n')
        root.rename(destination)
    print(f'Staged qualification-only tools overlay: {destination}')

if __name__ == '__main__': main()
