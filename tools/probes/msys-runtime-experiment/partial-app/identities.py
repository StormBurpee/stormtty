#!/usr/bin/env python3
"""Private stock versus positive partial-read comparison; no production qualification waiver."""
import hashlib
import importlib.util
import json
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
stage_spec = importlib.util.spec_from_file_location('copied_stage_contract', HERE / 'stage.py')
stage = importlib.util.module_from_spec(stage_spec)
stage_spec.loader.exec_module(stage)
VARIANTS = ('stock', 'copied')
COMMON = ('build_environment_sha256', 'compiler_packages_sha256',
          'original_dll_sha256', 'source_archive_sha256', 'patch_sha256',
          'common_utility_patch_sha256', 'compiler_dialect', 'compiler_version')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def check(condition, message):
    if not condition:
        raise ValueError(message)


def identities(roots):
    """Fail before launching anything if paired source/toolchain identity drifts."""
    check(set(roots)=={'original','stock','copied'}, 'exact three runtime roots required')
    for root in roots.values():
        stage.reject_links(root, recursive=True)
        stage.verify_dll(root / 'usr/bin/msys-2.0.dll')
    resolved=[root.resolve() for root in roots.values()]
    check(len(set(resolved))==3 and not any(a in b.parents for a in resolved for b in resolved if a!=b), 'runtime roots must be separate')
    manifests = {name: json.loads((root / 'stormtty-shell-manifest.json').read_text(encoding='utf-8'))
                 for name, root in roots.items()}
    for name, root in roots.items():
        manifest = manifests[name]
        names=[r['path'] for r in manifest['files']]
        check(len(names)==len(set(names)),name+': duplicate inventory paths')
        for rel in names:
            path=pathlib.PurePosixPath(rel)
            check(bool(rel) and not path.is_absolute() and '..' not in path.parts and '\\' not in rel and ':' not in rel,name+': unsafe manifest path')
        records = {r['path']: r for r in manifest['files'] if not r['directory']}
        for rel in ('usr/bin/msys-2.0.dll', 'usr/bin/zsh.exe'):
            check(rel in records and sha(root / rel) == records[rel]['sha256'], name + ': bundle hash mismatch ' + rel)
        if name != 'original':
            info = manifest['experimental_runtime']
            check(manifest.get('qualification_only') is True and manifest.get('default_activation_allowed') is False,
                  name + ': experimental isolation missing')
            check(info['variant'] == name and info.get('diagnostic_kind') == 'partial-app-v1',
                  name + ': wrong diagnostic variant')
            check(info.get('behavior_patch_applied') == (name == 'copied') and
                  info.get('retirement_patch_applied') is False and info.get('trace_enabled') is False,
                  name + ': behavior patch or trace mismatch')
            check(info['experimental_dll_sha256'] == sha(root / 'usr/bin/msys-2.0.dll'), name + ': DLL identity mismatch')
            check(info['patch_sha256'] == sha(HERE / '0007-preserve-copied-on-signal.patch'), name + ': candidate patch mismatch')
            check(info['common_utility_patch_sha256'] == sha(HERE.parent / '0000-utils-const-compat.patch'), name + ': utility patch mismatch')
            check(info['compiler_dialect'] == 'gnu++17' and info['compiler_version'] == '15.3.0', name + ': compiler mismatch')
            prov = root / 'experimental-runtime-provenance'
            check(sha(prov / 'dll-exports.txt') == info['dll_exports_sha256'], name+': export report changed')
            exports=(prov / 'dll-exports.txt').read_bytes()
            check(bool(exports) and b'stormtty_path' not in exports.lower(), name+': diagnostic export forbidden')
            check(sha(prov / '0007-preserve-copied-on-signal.patch')==sha(HERE / '0007-preserve-copied-on-signal.patch'), name+': patch content changed')
            check(sha(prov / '0000-utils-const-compat.patch')==sha(HERE.parent / '0000-utils-const-compat.patch'), name+': utility content changed')
            check(sha(prov / 'compiler-mode.txt') == info['compiler_mode_sha256'], name + ': compiler mode identity mismatch')
            check((prov / 'compiler-mode.txt').read_bytes() == stage.COMPILER_MODE[name], name + ': compiler mode changed')
            check(info['source_archive_sha256'] == stage.SOURCE_SHA256, name + ': source archive mismatch')
            check(info.get('source_files_sha256') == stage.SOURCE_FILES_SHA256[name], name + ': source identities mismatch')
            for filename, key in (('pacman-before.txt', 'build_environment_sha256'),
                                  ('compiler-packages.txt', 'compiler_packages_sha256'),
                                  ('msys2-runtime-3.6.11-4.src.tar.zst', 'source_archive_sha256')):
                check(sha(prov / filename) == info[key], name + ': provenance content mismatch ' + filename)
            check((prov / 'pacman-before.txt').read_bytes() == (prov / 'pacman-after.txt').read_bytes(),
                  name + ': compiler environment drift')
            for filename, digest in stage.SOURCE_FILES_SHA256[name].items():
                check(sha(prov / (filename + '.after')) == digest, name + ': staged source mismatch ' + filename)
                check(sha(prov / (filename + '.before')) == stage.ORIGINAL_SOURCE_FILES_SHA256[filename],
                      name + ': control source mismatch ' + filename)
    check('experimental_runtime' not in manifests['original'], 'original must not be an experimental bundle')
    stock, copied = [manifests[name]['experimental_runtime'] for name in VARIANTS]
    for key in COMMON:
        check(stock[key] == copied[key], 'comparison provenance differs: ' + key)
    check(stock['original_dll_sha256'] == sha(roots['original'] / 'usr/bin/msys-2.0.dll'), 'original DLL mismatch')
    # All fixture executables and other bundle content stay identical except the DLL.
    inventories = [{r['path']: r['sha256'] for r in manifests[n]['files']
                    if not r['directory'] and r['path'] != 'usr/bin/msys-2.0.dll'} for n in ('original', *VARIANTS)]
    check(inventories[0] == inventories[1] == inventories[2], 'non-runtime bundle content differs')
    for name, root in roots.items():
        for rel, digest in inventories[0].items():
            path = pathlib.PurePosixPath(rel)
            check(not path.is_absolute() and '..' not in path.parts and '\\' not in rel and ':' not in rel,
                  'unsafe manifest path')
            check(sha(root.joinpath(*path.parts)) == digest, name + ': bundle integrity mismatch ' + rel)
    return {name: {'dll_sha256': sha(root / 'usr/bin/msys-2.0.dll'),
                   'manifest_sha256': sha(root / 'stormtty-shell-manifest.json')}
            for name, root in roots.items()}
