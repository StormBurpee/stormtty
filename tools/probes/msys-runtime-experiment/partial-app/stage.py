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
STOCK_SHA256 = '535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33'
AFTER_SHA256 = 'cf4eba2150f2b99dfe7c55c8d73ac0f578d7675e649b8468a364bfb8fe2efdb2'
ORIGINAL_SOURCE_FILES_SHA256 = {'console.cc': '535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33', 'termios.cc': 'e65b607b331586533fd495dc228361b01b10f4ec85682abb2342b5c29867a36f', 'fhandler.h': 'c8e4a76e2a7aa5963763c62319e5f51ab08445be2f7ea6e3c1c314de91223650'}
SOURCE_FILES_SHA256 = {'stock': {'console.cc': '535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33', 'termios.cc': 'e65b607b331586533fd495dc228361b01b10f4ec85682abb2342b5c29867a36f', 'fhandler.h': 'c8e4a76e2a7aa5963763c62319e5f51ab08445be2f7ea6e3c1c314de91223650'}, 'copied': {'console.cc': 'cf4eba2150f2b99dfe7c55c8d73ac0f578d7675e649b8468a364bfb8fe2efdb2', 'termios.cc': 'e65b607b331586533fd495dc228361b01b10f4ec85682abb2342b5c29867a36f', 'fhandler.h': 'c8e4a76e2a7aa5963763c62319e5f51ab08445be2f7ea6e3c1c314de91223650'}}
COMPILER_MODE = {
 'stock': b'CXX=stormtty-gxx-15 -std=gnu++17\n__cplusplus=201703L\n',
 'copied': b'CXX=stormtty-gxx-15 -std=gnu++17\n__cplusplus=201703L\n',
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


def verify_dll(path):
    """Conservative binary gate: no diagnostic marker anywhere, not just exports.

    Validate bounded x64 PE DLL headers. Any diagnostic export must have its
    ASCII name in the PE file; rejecting all matching bytes is stricter and may
    conservatively reject an unrelated debug string rather than allow a tracer.
    """
    import struct
    with pathlib.Path(path).open('rb') as stream:
        data=stream.read(128*1024*1024+1)
    if len(data)>128*1024*1024 or len(data)<64 or data[:2]!=b'MZ':
        raise ValueError('Invalid bounded PE DLL')
    pe=struct.unpack_from('<I',data,60)[0]
    if pe>len(data)-26 or data[pe:pe+4]!=b'PE\0\0':
        raise ValueError('Invalid PE header')
    machine=struct.unpack_from('<H',data,pe+4)[0]
    optional=struct.unpack_from('<H',data,pe+20)[0]
    flags=struct.unpack_from('<H',data,pe+22)[0]
    if machine!=0x8664 or not flags&0x2000 or optional<112 or pe+24+optional>len(data) or struct.unpack_from('<H',data,pe+24)[0]!=0x20b:
        raise ValueError('Expected x64 PE32+ DLL')
    if b'stormtty_path' in data.lower():
        raise ValueError('Diagnostic runtime forbidden')
    return sha(path)


def stage(base, build, destination):
    reject_links(base, recursive=True)
    reject_links(build, recursive=True)
    reject_links(destination.parent)
    if os.path.lexists(destination):
        raise ValueError('Destination must be absent, including dangling aliases')
    base, build, destination = [p.resolve() for p in (base, build, destination)]
    if destination.exists() or destination == base or base in destination.parents or destination == build or build in destination.parents:
        raise ValueError('Destination must be new and outside base bundle')
    provenance = build / 'provenance'
    variant = (provenance / 'variant.txt').read_text().strip()
    if variant not in ('stock', 'copied'):
        raise ValueError('Unknown experimental build variant')
    dll = build / 'experimental-msys-2.0.dll'
    verify_dll(dll)
    exports = (provenance / 'dll-exports.txt').read_bytes()
    if not exports or b'stormtty_path' in exports.lower():
        raise ValueError('Diagnostic export evidence invalid')
    if sha(provenance / 'msys2-runtime-3.6.11-4.src.tar.zst') != SOURCE_SHA256:
        raise ValueError('Experimental source archive provenance mismatch')
    expected_after = AFTER_SHA256 if variant == 'copied' else STOCK_SHA256
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
    expected_patch = pathlib.Path(__file__).with_name('0007-preserve-copied-on-signal.patch')
    if sha(expected_patch) != sha(provenance / expected_patch.name):
        raise ValueError('Experimental patch provenance mismatch')
    common_patch = pathlib.Path(__file__).resolve().parent.parent / '0000-utils-const-compat.patch'
    if sha(common_patch) != sha(provenance / common_patch.name):
        raise ValueError('Common utility patch provenance mismatch')
    manifest_path = base / 'stormtty-shell-manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if 'experimental_runtime' in manifest:
        raise ValueError('Base must be untouched official reference')
    names=[r['path'] for r in manifest['files']]
    if len(names)!=len(set(names)):
        raise ValueError('Duplicate manifest path')
    verify_dll(base / 'usr/bin/msys-2.0.dll')
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
        'diagnostic_kind': 'partial-app-v1',
        'behavior_patch_applied': variant == 'copied',
        'retirement_patch_applied': False,
        'trace_enabled': False,
        'compiler_dialect': 'gnu++17', 'compiler_version': '15.3.0',
        'source_files_sha256': SOURCE_FILES_SHA256[variant],
        'experimental_dll_sha256': sha(dll),
        'dll_exports_sha256': sha(provenance / 'dll-exports.txt'),
        'source_archive_sha256': SOURCE_SHA256,
        'common_utility_patch_sha256': sha(provenance / common_patch.name),
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
    parser.add_argument('--verify-dll', type=pathlib.Path)
    parser.add_argument('paths', nargs='*', type=pathlib.Path)
    args = parser.parse_args()
    if args.verify_dll:
        if args.paths: parser.error('verify-dll accepts no staging paths')
        verify_dll(args.verify_dll)
    else:
        if len(args.paths)!=3: parser.error('expected BASE BUILD NEW_DEST')
        print(json.dumps(stage(*args.paths), indent=2))
