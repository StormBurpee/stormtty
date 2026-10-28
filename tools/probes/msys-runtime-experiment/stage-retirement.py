#!/usr/bin/env python3
"""Copy a private bundle into a NEW, explicitly experimental destination.
Never modifies the base bundle, system/build MSYS DLL, or production lock.
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import os
import stat


SOURCE_SHA256 = '4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204'
BEFORE_SHA256 = '535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33'
CURSOR_SHA256 = '25a3fb9355235614819f4c56e2a60c8ecb3b4bd4cd49a6befa7ff56ab03cc0b3'
AFTER_SHA256 = 'da2efe83f28c324d4d3a9085240ccfc0da38c180f94bab78cb7b6f3612a7edab'
ORIGINAL_SOURCE_FILES_SHA256 = {'console.cc': '535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33', 'termios.cc': 'e65b607b331586533fd495dc228361b01b10f4ec85682abb2342b5c29867a36f', 'fhandler.h': 'c8e4a76e2a7aa5963763c62319e5f51ab08445be2f7ea6e3c1c314de91223650'}
SOURCE_FILES_SHA256 = {'baseline': {'console.cc': '25a3fb9355235614819f4c56e2a60c8ecb3b4bd4cd49a6befa7ff56ab03cc0b3', 'termios.cc': 'e65b607b331586533fd495dc228361b01b10f4ec85682abb2342b5c29867a36f', 'fhandler.h': 'c8e4a76e2a7aa5963763c62319e5f51ab08445be2f7ea6e3c1c314de91223650'}, 'combined': {'console.cc': 'da2efe83f28c324d4d3a9085240ccfc0da38c180f94bab78cb7b6f3612a7edab', 'termios.cc': 'e65b607b331586533fd495dc228361b01b10f4ec85682abb2342b5c29867a36f', 'fhandler.h': 'c8e4a76e2a7aa5963763c62319e5f51ab08445be2f7ea6e3c1c314de91223650'}}
COMPILER_MODE = {
 'baseline': b'CXX=stormtty-gxx-15 -std=gnu++17\n__cplusplus=201703L\n',
 'combined': b'CXX=stormtty-gxx-15 -std=gnu++17\n__cplusplus=201703L\n',
}



def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def reject_links(path, recursive=False):
    """Reject symlinks/reparse aliases before resolution; never walk through one."""
    path = pathlib.Path(path).absolute()
    def inspect(candidate):
        info = candidate.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise ValueError('Symlink/reparse path rejected: ' + str(candidate))
        if not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
            raise ValueError('Nonregular path rejected: ' + str(candidate))
        return info
    for ancestor in reversed(path.parents):
        inspect(ancestor)
    inspect(path)
    if recursive:
        pending = [path]
        while pending:
            directory = pending.pop()
            with os.scandir(directory) as entries:
                for entry in entries:
                    candidate = pathlib.Path(entry.path)
                    info = inspect(candidate)
                    if stat.S_ISDIR(info.st_mode):
                        pending.append(candidate)


def stage(base, build, destination):
    reject_links(base, recursive=True)
    reject_links(build, recursive=True)
    reject_links(destination.parent)
    if os.path.lexists(destination):
        raise ValueError('Destination must be absent, including dangling aliases')
    base, build, destination = [p.resolve() for p in (base, build, destination)]
    if destination.exists() or destination == base or base in destination.parents:
        raise ValueError('Destination must be new and outside base bundle')
    provenance = build / 'provenance'
    variant = (provenance / 'variant.txt').read_text().strip()
    if variant not in ('baseline', 'combined'):
        raise ValueError('Unknown experimental build variant')
    dll = build / 'experimental-msys-2.0.dll'
    with dll.open('rb') as stream:
        if stream.read(2) != b'MZ':
            raise ValueError('Not a Windows DLL')
    if sha(provenance / 'msys2-runtime-3.6.11-4.src.tar.zst') != SOURCE_SHA256:
        raise ValueError('Experimental source archive provenance mismatch')
    expected_after = AFTER_SHA256 if variant == 'combined' else CURSOR_SHA256
    if sha(provenance / 'console-before.cc') != BEFORE_SHA256 or sha(provenance / 'console-after.cc') != expected_after:
        raise ValueError('Unexpected experimental source contents')
    for filename, expected in SOURCE_FILES_SHA256[variant].items():
        if sha(provenance / (filename + '.after')) != expected:
            raise ValueError('Unexpected traced source: ' + filename)
        if sha(provenance / (filename + '.before')) != ORIGINAL_SOURCE_FILES_SHA256[filename]:
            raise ValueError('Unexpected control source: ' + filename)
    if (provenance / 'compiler-mode.txt').read_bytes() != COMPILER_MODE[variant]:
        raise ValueError('Unexpected runtime compiler dialect')
    if (provenance / 'pacman-before.txt').read_bytes() != (provenance / 'pacman-after.txt').read_bytes():
        raise ValueError('Build environment changed during compilation')
    if sha(dll) != (provenance / 'hashes.txt').read_text().split()[0]:
        raise ValueError('Experimental DLL changed after build')
    expected_patch = pathlib.Path(__file__).with_name('0004-retirement-after-cursor.patch')
    cursor_patch = pathlib.Path(__file__).with_name('0003-modern-cursor-fill.patch')
    if sha(cursor_patch) != sha(provenance / cursor_patch.name):
        raise ValueError('Cursor baseline patch provenance mismatch')
    if sha(expected_patch) != sha(provenance / expected_patch.name):
        raise ValueError('Experimental patch provenance mismatch')
    common_patch = pathlib.Path(__file__).with_name('0000-utils-const-compat.patch')
    if sha(common_patch) != sha(provenance / common_patch.name):
        raise ValueError('Common utility patch provenance mismatch')
    manifest_path = base / 'stormtty-shell-manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    for path in base.rglob('*'):
        if path.is_symlink():
            raise ValueError('Private bundle must not contain symbolic links')
    for record in manifest['files']:
        relative = pathlib.PurePosixPath(record['path'])
        if relative.is_absolute() or '..' in relative.parts or '\\' in record['path'] or ':' in record['path']:
            raise ValueError('Invalid manifest path')
        path = base.joinpath(*relative.parts)
        if not record['directory'] and (not path.is_file() or sha(path) != record['sha256']):
            raise ValueError('Base bundle integrity mismatch: ' + record['path'])
    target = 'usr/bin/msys-2.0.dll'
    records = [r for r in manifest['files'] if r['path'] == target]
    if len(records) != 1:
        raise ValueError('Expected one runtime DLL record')
    original_hash = records[0]['sha256']
    shutil.copytree(base, destination)
    metadata = destination / 'experimental-runtime-provenance'
    shutil.copytree(provenance, metadata)
    shutil.copy2(manifest_path, metadata / 'original-bundle-manifest.json')
    shutil.copy2(dll, destination / target)
    records[0].update(sha256=sha(dll), bytes=dll.stat().st_size,
                      package='experimental-msys2-runtime-' + variant)
    manifest.update(bundle_id=manifest['bundle_id'] + '-EXPERIMENTAL-' + variant,
                    qualification_only=True, default_activation_allowed=False,
                    native_qualification={'status': 'unverified-experimental'},
                    public_redistribution_ready=False,
                    public_source_availability_verified=False,
                    release_notices_complete=False)
    manifest['experimental_runtime'] = {
        'variant': variant, 'original_dll_sha256': original_hash,
        'diagnostic_kind': 'cursor-retirement-v1',
        'behavior_patch_applied': True,
        'retirement_patch_applied': variant == 'combined',
        'trace_enabled': False,
        'compiler_dialect': 'gnu++17', 'compiler_version': '15.3.0',
        'source_files_sha256': SOURCE_FILES_SHA256[variant],
        'experimental_dll_sha256': sha(dll),
        'source_archive_sha256': '4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204',
        'common_utility_patch_sha256': sha(provenance / common_patch.name),
        'cursor_patch_sha256': sha(provenance / cursor_patch.name),
        'patch_sha256': sha(provenance / expected_patch.name),
        'build_environment_sha256': sha(provenance / 'pacman-before.txt'),
        'compiler_packages_sha256': sha(provenance / 'compiler-packages.txt'),
        'compiler_mode_sha256': sha(provenance / 'compiler-mode.txt'),
        'scope': 'Private qualification only; not an approved production bundle',
    }
    (destination / 'stormtty-shell-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (destination / 'EXPERIMENTAL-NOT-FOR-RELEASE.txt').write_text(
        'Isolated runtime experiment. Original licenses/notices preserved.\n'
        'Modified runtime source archive, packaging diff, candidate patch, toolchain and hashes\n'
        'are retained in experimental-runtime-provenance. No release approval.\n', encoding='utf-8')
    return manifest['experimental_runtime']


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('base', 'build', 'destination'):
        parser.add_argument(name, type=pathlib.Path)
    args = parser.parse_args()
    print(json.dumps(stage(args.base, args.build, args.destination), indent=2))
