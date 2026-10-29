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
STOCK_SHA256 = '378d6584d510ff9061e50367149e1545e9f3ecad96dede2bc6f901a7075373ff'
AFTER_SHA256 = '65518094c0c99fd54aa350ea1bb4fa284bad3245a89ab436578d363126d287ab'
ORIGINAL_SOURCE_FILES_SHA256 = {'console.cc': '535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33', 'termios.cc': 'e65b607b331586533fd495dc228361b01b10f4ec85682abb2342b5c29867a36f', 'fhandler.h': 'c8e4a76e2a7aa5963763c62319e5f51ab08445be2f7ea6e3c1c314de91223650'}
SOURCE_FILES_SHA256 = {'stock': {'console.cc': '378d6584d510ff9061e50367149e1545e9f3ecad96dede2bc6f901a7075373ff', 'termios.cc': 'e65b607b331586533fd495dc228361b01b10f4ec85682abb2342b5c29867a36f', 'fhandler.h': 'c8e4a76e2a7aa5963763c62319e5f51ab08445be2f7ea6e3c1c314de91223650'}, 'copied': {'console.cc': '65518094c0c99fd54aa350ea1bb4fa284bad3245a89ab436578d363126d287ab', 'termios.cc': 'e65b607b331586533fd495dc228361b01b10f4ec85682abb2342b5c29867a36f', 'fhandler.h': 'c8e4a76e2a7aa5963763c62319e5f51ab08445be2f7ea6e3c1c314de91223650'}}
COMPILER_MODE = {
 'stock': b'CXX=stormtty-gxx-15 -std=gnu++17 -DSTORMTTY_PASSIVE_CONSOLE_TRACE=1\n__cplusplus=201703L\n',
 'copied': b'CXX=stormtty-gxx-15 -std=gnu++17 -DSTORMTTY_PASSIVE_CONSOLE_TRACE=1\n__cplusplus=201703L\n',
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
    if variant not in ('stock', 'copied'):
        raise ValueError('Unknown experimental build variant')
    dll = build / 'experimental-msys-2.0.dll'
    with dll.open('rb') as stream:
        if stream.read(2) != b'MZ':
            raise ValueError('Not a Windows DLL')
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
    expected_patch = pathlib.Path(__file__).resolve().parent.parent / '0007-preserve-copied-on-signal.patch'
    if sha(expected_patch) != sha(provenance / expected_patch.name):
        raise ValueError('Experimental patch provenance mismatch')
    common_patch = pathlib.Path(__file__).resolve().parents[2] / '0000-utils-const-compat.patch'
    if sha(common_patch) != sha(provenance / common_patch.name):
        raise ValueError('Common utility patch provenance mismatch')
    diagnostic = pathlib.Path(__file__).resolve().parent
    lock = json.loads((diagnostic / 'identities.json').read_text())
    if (provenance / 'passive-identities.json').read_bytes() != (diagnostic / 'identities.json').read_bytes():
        raise ValueError('Diagnostic identity drift')
    if sha(provenance / 'passive.patch') != lock['variants'][variant]['patch_sha256'] or sha(diagnostic / (variant + '.patch')) != lock['variants'][variant]['patch_sha256']:
        raise ValueError('Diagnostic patch mismatch')
    if sha(provenance / 'stormtty-passive-path.h') != lock['header_sha256'] or sha(diagnostic / 'recorder.hpp') != lock['header_sha256']:
        raise ValueError('Diagnostic header mismatch')
    pe = (provenance / 'diagnostic-dll-pe.txt').read_text()
    if not all(name in pe for name in ('stormtty_path_init_v1', 'stormtty_path_handler_v1')):
        raise ValueError('Missing diagnostic exports')
    linked = json.loads((provenance / 'linked-preflight.json').read_text())
    if (linked.get('scope') != 'fixed-diagnostic-linked-preflight' or linked.get('variant') != variant or
        linked.get('linked_code_gate_passed') is not True or linked.get('dll_sha256') != sha(dll) or
        linked.get('production_qualified') is not False or linked.get('fixtures_executed') is not False):
        raise ValueError('Linked native preflight missing or mismatched')
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
    manifest.update(bundle_id=manifest['bundle_id'] + '-PASSIVE-DIAGNOSTIC-' + variant,
                    qualification_only=True, default_activation_allowed=False,
                    native_qualification={'status': 'unverified-experimental'},
                    public_redistribution_ready=False,
                    public_source_availability_verified=False,
                    release_notices_complete=False)
    manifest['experimental_runtime'] = {
        'variant': variant, 'original_dll_sha256': original_hash,
        'diagnostic_kind': 'partial-read-v1',
        'passive_patch_sha256': lock['variants'][variant]['patch_sha256'],
        'passive_header_sha256': lock['header_sha256'],
        'linked_preflight_sha256': sha(provenance / 'linked-preflight.json'),
        'behavior_patch_applied': variant == 'copied',
        'retirement_patch_applied': False,
        'trace_enabled': True,
        'compiler_dialect': 'gnu++17', 'compiler_version': '15.3.0',
        'source_files_sha256': SOURCE_FILES_SHA256[variant],
        'experimental_dll_sha256': sha(dll),
        'source_archive_sha256': '4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204',
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
    for name in ('base', 'build', 'destination'):
        parser.add_argument(name, type=pathlib.Path)
    args = parser.parse_args()
    print(json.dumps(stage(args.base, args.build, args.destination), indent=2))
