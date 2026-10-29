#!/usr/bin/env python3
"""One-shot synthetic stage-1 qualification. No Zsh/native-interoperability claim."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import sys

MODES = ('bytes', 'raw', 'resize', 'signal', 'eof', 'flood')
HERE = Path(__file__).resolve().parent


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def stage(original, build, destination):
    original, build, destination = [Path(p).resolve() for p in (original, build, destination)]
    if destination.exists() or original in destination.parents or build in destination.parents:
        raise ValueError('Qualification destination must be new and outside source trees')
    manifest_path = original / 'stormtty-shell-manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    for path in original.rglob('*'):
        if path.is_symlink():
            raise ValueError('No symlinks in the staged private runtime')
    for record in manifest['files']:
        relative = PurePosixPath(record['path'])
        if relative.is_absolute() or '..' in relative.parts or '\\' in record['path'] or ':' in record['path']:
            raise ValueError('Invalid manifest path')
        target = original.joinpath(*relative.parts)
        if not record['directory'] and (not target.is_file() or sha(target) != record['sha256']):
            raise ValueError('Original package integrity mismatch: ' + record['path'])
    # Copy the exact pinned bundle; never change its DLL or the compiler's DLL.
    shutil.copytree(original, destination)
    for name in ('broker', 'fixture'):
        source = build / (name + '.exe')
        with source.open('rb') as stream:
            if stream.read(2) != b'MZ':
                raise ValueError('Expected compiled Windows PE: ' + name)
        target = destination / 'usr/bin' / ('stormtty-bridge-' + name + '.exe')
        if target.exists():
            raise ValueError('Qualification binary name collision')
        shutil.copy2(source, target)
    identity = {
        'scope': 'synthetic-broker-stage1-only', 'default_activation_allowed': False,
        'runtime_sha256': sha(destination / 'usr/bin/msys-2.0.dll'),
        'original_manifest_sha256': sha(manifest_path),
        'broker_sha256': sha(destination / 'usr/bin/stormtty-bridge-broker.exe'),
        'fixture_sha256': sha(destination / 'usr/bin/stormtty-bridge-fixture.exe'),
        'native_child_interop_verified': False, 'zsh_verified': False,
    }
    (destination / 'bridge-qualification.json').write_text(json.dumps(identity, indent=2) + '\n', encoding='utf-8')
    return identity


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('original', 'build', 'destination', 'host', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    args = p.parse_args()
    if os.name != 'nt':
        p.error('Native Windows required; cross-compile is not qualification')
    args.output.mkdir()
    identity = stage(args.original, args.build, args.destination)
    identity['host_sha256'] = sha(args.host)
    (args.output / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n', encoding='utf-8')
    runner_path = HERE.parents[1] / 'run-owned-process.py'
    spec = importlib.util.spec_from_file_location('bridge_owned_runner', runner_path)
    runner = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = runner
    spec.loader.exec_module(runner)
    # Scope environment to this qualification process and its children only.
    home = args.output.resolve() / 'scratch-home'; home.mkdir()
    os.environ.update(HOME=str(home), ZDOTDIR=str(home), TERM='xterm-256color', LANG='C.UTF-8',
                      LC_ALL='C.UTF-8', MSYS='noglob', MSYSTEM='MSYS',
                      PATH=str(args.destination.resolve() / 'usr/bin') + os.pathsep + str(Path(os.environ['SystemRoot']) / 'System32'))
    results = []
    for mode in MODES:  # Exactly once each. Never retry a failure into a pass.
        command = [str(args.host.resolve()), str(args.destination.resolve() / 'usr/bin/stormtty-bridge-broker.exe'),
                   '/usr/bin/stormtty-bridge-fixture.exe', mode]
        try:
            result = runner.run_owned(command, timeout=20, reap_timeout=5, tail_bytes=65536, output_dir=args.output)
            (args.output / (mode + '.log')).write_bytes(result.output_tail)
            entry = {'mode': mode, 'exit_code': result.exit_code, **result.metadata()}
        except OSError as error:
            entry = {'mode': mode, 'exit_code': 125, 'launch_error': str(error), 'reaped': False}
        results.append(entry)
        (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
        print('BRIDGE_STAGE1=' + json.dumps(entry, sort_keys=True), flush=True)
    # No source/runtime mutation by a successful or failed qualification.
    if (identity['runtime_sha256'] != sha(args.destination / 'usr/bin/msys-2.0.dll') or
            identity['runtime_sha256'] != sha(args.original / 'usr/bin/msys-2.0.dll')):
        raise RuntimeError('Pinned runtime changed during qualification')
    return int(any(r['exit_code'] or not r['reaped'] for r in results))


if __name__ == '__main__':
    raise SystemExit(main())
