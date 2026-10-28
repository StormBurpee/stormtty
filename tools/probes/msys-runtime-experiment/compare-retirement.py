#!/usr/bin/env python3
"""Private cursor-only versus cursor-plus-retirement comparison; no production qualification waiver."""
import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
stage_spec = importlib.util.spec_from_file_location('cursor_stage_contract', HERE / 'stage-retirement.py')
stage = importlib.util.module_from_spec(stage_spec)
stage_spec.loader.exec_module(stage)
VARIANTS = ('baseline', 'combined')
COMMON = ('build_environment_sha256', 'compiler_packages_sha256',
          'original_dll_sha256', 'source_archive_sha256', 'patch_sha256',
          'common_utility_patch_sha256', 'cursor_patch_sha256', 'compiler_dialect', 'compiler_version')


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
            check(info['variant'] == name and info.get('diagnostic_kind') == 'cursor-retirement-v1',
                  name + ': wrong diagnostic variant')
            check(info.get('behavior_patch_applied') is True and
                  info.get('retirement_patch_applied') == (name == 'combined') and info.get('trace_enabled') is False,
                  name + ': behavior patch or trace mismatch')
            check(info['experimental_dll_sha256'] == sha(root / 'usr/bin/msys-2.0.dll'), name + ': DLL identity mismatch')
            check(info['patch_sha256'] == sha(HERE / '0004-retirement-after-cursor.patch'), name + ': candidate patch mismatch')
            check(info['cursor_patch_sha256'] == sha(HERE / '0003-modern-cursor-fill.patch'), name + ': cursor patch mismatch')
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
                check(sha(prov / (filename + '.before')) == stage.ORIGINAL_SOURCE_FILES_SHA256[filename],
                      name + ': control source mismatch ' + filename)
    check('experimental_runtime' not in manifests['original'], 'original must not be an experimental bundle')
    baseline, combined = [manifests[name]['experimental_runtime'] for name in VARIANTS]
    for key in COMMON:
        check(baseline[key] == combined[key], 'comparison provenance differs: ' + key)
    check(baseline['original_dll_sha256'] == sha(roots['original'] / 'usr/bin/msys-2.0.dll'), 'original DLL mismatch')
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


def parse_summary(data, exit_code, workload):
    """Retain only one exact numeric whitelist; never expose output text."""
    prefix = b'RESIZE_CASE_SUMMARY '
    lines = [line for line in data.splitlines() if line.startswith(b'RESIZE_CASE_SUMMARY')]
    check(len(lines) == 1, 'summary_missing_or_duplicate')
    keys = ('code', 'phase', 'expected_cols', 'expected_rows', 'observed_cols', 'observed_rows',
            'substage', 'protocol_step', 'reason', 'write_result', 'expected_prompt_sequence',
            'prompt_status_known', 'prompt_status', 'child_exit_known', 'child_exit_code', 'resize_accepted')
    pattern = prefix + b' '.join(k.encode() + rb'=([0-9]{1,10})' for k in keys)
    match = re.fullmatch(pattern, lines[0])
    check(match is not None, 'summary_invalid_schema')
    values = dict(zip(keys, map(int, match.groups())))
    check(0 <= values['code'] <= 8 and 0 <= values['phase'] <= 50, 'summary_invalid_bounds')
    check(all(0 <= values[k] <= 32767 for k in keys[2:6]), 'summary_invalid_bounds')
    bounds = {'substage': 4, 'protocol_step': 4, 'reason': 4, 'write_result': 4,
              'expected_prompt_sequence': 0xffffffff, 'prompt_status_known': 1, 'prompt_status': 255,
              'child_exit_known': 1, 'child_exit_code': 0xffffffff, 'resize_accepted': 1}
    check(all(0 <= values[k] <= bound for k, bound in bounds.items()), 'summary_invalid_protocol_bounds')
    check(values['prompt_status_known'] or values['prompt_status'] == 0, 'summary_unknown_prompt_status')
    check(values['child_exit_known'] or values['child_exit_code'] == 0, 'summary_unknown_child_exit')
    step, reason = values['protocol_step'], values['reason']
    if step == 0:
        check(all(values[k] == 0 for k in ('reason', 'write_result', 'expected_prompt_sequence',
                                          'prompt_status_known', 'prompt_status')), 'summary_nonprotocol_detail')
    else:
        check(values['expected_prompt_sequence'] >= 1, 'summary_missing_prompt_sequence')
        if step in (1, 4):
            check(reason in (1, 3, 4) and values['write_result'] == 0, 'summary_ready_detail')
            check(reason != 3 or values['prompt_status_known'] == 1, 'summary_status_mismatch_unknown')
            check(step != 1 or reason != 3, 'summary_prior_ready_status_mismatch')
        else:
            check(reason in (2, 4) and values['prompt_status_known'] == 0 and values['prompt_status'] == 0,
                  'summary_write_detail')
            check(reason != 2 or values['write_result'] in (2, 3, 4), 'summary_rejected_write_result')

    check((exit_code == 0) == (values['code'] == 0), 'summary_exit_mismatch')
    check(values['phase'] != 0 or values['code'] == 1, 'summary_invalid_setup_phase')
    check(values['phase'] <= (1 if workload == 'idle' else 50), 'summary_invalid_phase')
    expected = ((111, 35) if values['phase'] % 2 else (100, 30)) if values['phase'] else (0, 0)
    check((values['expected_cols'], values['expected_rows']) == expected, 'summary_expected_grid_mismatch')
    check((values['observed_cols'] == 0) == (values['observed_rows'] == 0), 'summary_partial_unknown_grid')
    if exit_code == 0:
        check(all(values[k] == 0 for k in keys[6:-1]) and values['resize_accepted'] == 1,
              'summary_success_protocol_state')
        check(values['phase'] == (1 if workload == 'idle' else 50), 'summary_incomplete_success')
        check((values['expected_cols'], values['expected_rows']) ==
              ((111, 35) if workload == 'idle' else (100, 30)), 'summary_expected_grid_mismatch')
        check(values['expected_cols'] == values['observed_cols'] and
              values['expected_rows'] == values['observed_rows'], 'summary_observed_grid_mismatch')
    return values


def variant_order(round_index):
    offset = round_index % len(VARIANTS)
    return VARIANTS[offset:] + VARIANTS[:offset]


def case_command(args, name, mode, scratch):
    command = [str(args.probe.resolve()), str((getattr(args, name) / 'usr/bin/zsh.exe').resolve()),
               str(args.fixture.resolve()), str(scratch.resolve())]
    command += ['inbox'] if mode == 'inbox' else ['--packaged-provider', str(args.provider.resolve())]
    command += ['--resize-summary', '--resize-repeats', '1' if args.workload == 'idle' else '50']
    if args.workload in ('idle', 'builtin-only'):
        command += ['--resize-one-shot']
    return command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('probe', 'fixture', 'original', *VARIANTS, 'provider', 'output'):
        parser.add_argument('--' + name, required=True, type=pathlib.Path)
    parser.add_argument('--rounds', type=int, default=1, choices=range(1, 6))
    parser.add_argument('--workload', choices=('idle', 'repeated', 'builtin-only'), required=True)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Native Windows required; cross-link is not runtime evidence')
    roots = {name: getattr(args, name).resolve() for name in ('original', *VARIANTS)}
    identity = identities(roots)
    check(args.probe.is_file() and args.fixture.is_file() and args.provider.is_dir(), 'probe/fixture/provider missing')
    fixed = {'probe_sha256': sha(args.probe), 'fixture_sha256': sha(args.fixture)}
    args.output.mkdir()  # Never overwrite evidence.
    spec = importlib.util.spec_from_file_location('cursor_owned_runner', HERE.parents[1] / 'run-owned-process.py')
    runner = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = runner
    spec.loader.exec_module(runner)
    results = []
    for round_index in range(args.rounds):
        order = variant_order(round_index)
        for mode in ('inbox', 'packaged'):
            for name in order:
                label = f'{round_index + 1}-{args.workload}-{mode}-{name}'
                command = case_command(args, name, mode, args.output / label)
                print('RETIREMENT_EXPERIMENT_CASE=' + label, flush=True)
                result = runner.run_owned(command, timeout=180, tail_bytes=1024 * 1024, output_dir=args.output)
                summary, error = None, ''
                try:
                    summary = parse_summary(result.output_tail, result.exit_code, args.workload)
                except ValueError as exc:
                    error = str(exc)  # Only fixed whitelist parser error codes.
                entry = {'case': label, 'variant': name, 'provider': mode, 'workload': args.workload,
                         'round': round_index + 1, 'exit_code': result.exit_code, 'summary_error': error,
                         'input_evidence_available': b'PTY_INPUT_EVIDENCE label=' in result.output_tail,
                         'summary': summary, 'identity': identity[name], **fixed, **result.metadata()}
                # Cleanup exceptions may include paths: retain only a boolean in public numeric evidence.
                entry['cleanup_error'] = bool(entry['cleanup_error'])
                results.append(entry)
                (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
                print('RETIREMENT_EXPERIMENT_RESULT=' + json.dumps(entry, sort_keys=True), flush=True)
    return 1 if any(r['exit_code'] or r['summary_error'] or not r['input_evidence_available'] or
                    r['cleanup_error'] or not r['reaped'] or r['timed_out'] for r in results) else 0


if __name__ == '__main__':
    raise SystemExit(main())
