#!/usr/bin/env python3
"""Private causal trace comparison, not a production fix or qualification waiver."""
import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
stage_spec = importlib.util.spec_from_file_location('trace_stage_contract', HERE / 'stage-trace.py')
stage = importlib.util.module_from_spec(stage_spec)
stage_spec.loader.exec_module(stage)
VARIANTS = ('original', 'control', 'instrumented')
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
    manifests = {name: json.loads((root / 'stormtty-shell-manifest.json').read_text(encoding='utf-8'))
                 for name, root in roots.items()}
    for name, root in roots.items():
        manifest = manifests[name]
        records = {r['path']: r for r in manifest['files'] if not r['directory']}
        for rel in ('usr/bin/msys-2.0.dll', 'usr/bin/zsh.exe'):
            check(rel in records and sha(root / rel) == records[rel]['sha256'], name + ': bundle hash mismatch ' + rel)
        if name != 'original':
            info = manifest['experimental_runtime']
            check(manifest.get('qualification_only') is True and manifest.get('default_activation_allowed') is False,
                  name + ': experimental isolation missing')
            check(info['variant'] == name and info.get('diagnostic_kind') == 'resize-observation-v1',
                  name + ': wrong diagnostic variant')
            check(info.get('behavior_patch_applied') is False and info.get('trace_enabled') == (name == 'instrumented'),
                  name + ': behavior patch or trace mismatch')
            check(info['experimental_dll_sha256'] == sha(root / 'usr/bin/msys-2.0.dll'), name + ': DLL identity mismatch')
            check(info['patch_sha256'] == sha(HERE / '0002-resize-observation-trace.patch'), name + ': trace patch mismatch')
            check(info['common_utility_patch_sha256'] == sha(HERE / '0000-utils-const-compat.patch'), name + ': utility patch mismatch')
            check(info['compiler_dialect'] == 'gnu++17' and info['compiler_version'] == '15.3.0', name + ': compiler mismatch')
            prov = root / 'experimental-runtime-provenance'
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
                check(sha(prov / (filename + '.before')) == stage.SOURCE_FILES_SHA256['control'][filename],
                      name + ': control source mismatch ' + filename)
    check('experimental_runtime' not in manifests['original'], 'original must not be an experimental bundle')
    control, instrumented = [manifests[name]['experimental_runtime'] for name in VARIANTS[1:]]
    for key in COMMON:
        check(control[key] == instrumented[key], 'comparison provenance differs: ' + key)
    check(control['original_dll_sha256'] == sha(roots['original'] / 'usr/bin/msys-2.0.dll'), 'original DLL mismatch')
    # All fixture executables and other bundle content stay identical except the DLL.
    inventories = [{r['path']: r['sha256'] for r in manifests[n]['files']
                    if not r['directory'] and r['path'] != 'usr/bin/msys-2.0.dll'} for n in VARIANTS]
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


def inspect_snapshot(directory):
    """Read bounded numeric evidence, never screen/input journals or live files."""
    check(directory.is_dir(), 'pre-diagnostic trace snapshot missing')
    files, total, records, partial = [], 0, 0, 0
    for path in sorted(directory.iterdir()):
        match = re.fullmatch(r'resize-([0-9]+)\.log', path.name)
        check(match is not None and path.is_file() and not path.is_symlink(), 'unexpected trace entry')
        size = path.stat().st_size
        total += size
        check(len(files) < 32 and size <= 2 * 1024 * 1024 and total <= 16 * 1024 * 1024, 'trace budget exceeded')
        data = path.read_bytes()
        check(len(data) == size, 'snapshot changed while reading')
        lines = data.split(b'\n')
        partial += len(lines.pop())  # Live prefix may end mid-record; retain but do not interpret it.
        seen = set()
        identity = None
        events = {}
        for line in lines:
            check(re.fullmatch(rb'-?[0-9]+(?: -?[0-9]+){11}', line) is not None, 'trace record is not 12 numeric fields')
            values = list(map(int, line.split()))
            seq, pid, tid, caller, event, unit, *rest = values
            check(1 <= seq <= 8192 and seq not in seen, 'invalid/duplicate trace sequence')
            check(pid == int(match[1]) and 0 <= pid <= 0xffffffff and 0 <= tid <= 0xffffffff, 'trace process identity mismatch')
            check(all(-0x80000000 <= v <= 0x7fffffff for v in values[3:]), 'trace int32 range exceeded')
            check(1 <= event <= 11, 'unknown trace event')
            if event == 11:
                check(identity is None and seq == 1 and caller == 0 and unit == -1 and
                      values[6:10] == [0, 0, 0, 0] and values[10] > 0,
                      'invalid/duplicate process identity record')
                identity = {'windows_pid': pid, 'posix_pid': values[10], 'initial_pgid': values[11]}
            events[event] = events.get(event, 0) + 1
            seen.add(seq)
            records += 1
        check(not seen or identity is not None, 'process identity record missing')
        check(not seen or seen == set(range(1, max(seen) + 1)), 'trace sequence gap: incomplete evidence')
        check(8192 not in seen, 'trace record cap reached: incomplete evidence')
        files.append({'name': path.name, 'bytes': size, 'sha256': sha(path), 'records': len(lines), 'process_identity': identity, 'events': events})
    return {'files': files, 'bytes': total, 'records': records, 'incomplete_suffix_bytes': partial}


def case_command(args, name, mode, scratch, trace):
    command = [str(args.probe.resolve()), str((getattr(args, name) / 'usr/bin/zsh.exe').resolve()),
               str(args.fixture.resolve()), str(scratch.resolve())]
    command += ['inbox'] if mode == 'inbox' else ['--packaged-provider', str(args.provider.resolve())]
    command += ['--resize-trace-directory', str(trace.resolve())]
    command += ['--resize-trace-idle'] if args.workload == 'idle' else ['--resize-repeats', '50']
    return command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('probe', 'fixture', *VARIANTS, 'provider', 'output'):
        parser.add_argument('--' + name, required=True, type=pathlib.Path)
    parser.add_argument('--rounds', type=int, default=1, choices=range(1, 6))
    parser.add_argument('--workload', choices=('idle', 'repeated'), required=True)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Native Windows required; cross-link is not runtime evidence')
    roots = {name: getattr(args, name).resolve() for name in VARIANTS}
    identity = identities(roots)
    check(args.probe.is_file() and args.fixture.is_file() and args.provider.is_dir(), 'probe/fixture/provider missing')
    fixed = {'probe_sha256': sha(args.probe), 'fixture_sha256': sha(args.fixture)}
    args.output.mkdir()  # Never overwrite evidence.
    spec = importlib.util.spec_from_file_location('trace_owned_runner', HERE.parents[1] / 'run-owned-process.py')
    runner = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = runner
    spec.loader.exec_module(runner)
    results = []
    for round_index in range(args.rounds):
        order = VARIANTS[round_index % 3:] + VARIANTS[:round_index % 3]
        for mode in ('inbox', 'packaged'):
            for name in order:
                label = f'{round_index + 1}-{args.workload}-{mode}-{name}'
                scratch, trace = args.output / label, args.output / (label + '-runtime-trace')
                trace.mkdir()
                command = case_command(args, name, mode, scratch, trace)
                print('TRACE_EXPERIMENT_CASE=' + label, flush=True)
                result = runner.run_owned(command, timeout=180, tail_bytes=1024 * 1024, output_dir=args.output)
                (args.output / (label + '.log')).write_bytes(result.output_tail)
                snapshots, error = [], ''
                try:
                    paths = sorted(scratch.glob('resize-trace-phase-*'))
                    check(bool(paths), 'pre-diagnostic trace snapshot missing')
                    if result.exit_code == 0:
                        check(len(paths) == (1 if args.workload == 'idle' else 50), 'successful workload snapshot count mismatch')
                    for path in paths:
                        snapshot = inspect_snapshot(path)
                        snapshots.append({'directory': path.name, **snapshot})
                        check(bool(snapshot['files']) == (name == 'instrumented') and
                              (snapshot['records'] > len(snapshot['files'])) == (name == 'instrumented'), 'variant trace availability mismatch')
                        check(snapshot['incomplete_suffix_bytes'] == 0, 'incomplete trace prefix: causal evidence limited')
                except (OSError, ValueError) as exc:
                    error = str(exc)
                entry = {'case': label, 'variant': name, 'provider': mode, 'workload': args.workload,
                         'round': round_index + 1, 'exit_code': result.exit_code, 'trace_error': error,
                         'input_evidence_available': b'PTY_INPUT_EVIDENCE label=' in result.output_tail,
                         'snapshots': snapshots, 'identity': identity[name], **fixed, **result.metadata()}
                results.append(entry)
                (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
                print('TRACE_EXPERIMENT_RESULT=' + json.dumps(entry, sort_keys=True), flush=True)
    return 1 if any(r['exit_code'] or r['trace_error'] or not r['input_evidence_available'] or r['cleanup_error'] or not r['reaped'] for r in results) else 0


if __name__ == '__main__':
    raise SystemExit(main())
