#!/usr/bin/env python3
"""Fixed-probe, fixed-fixture native comparisons; failures are never retried away."""
import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import sys


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('probe', 'fixture', 'original', 'control', 'patched', 'provider', 'output'):
        parser.add_argument('--' + name, required=True, type=pathlib.Path)
    parser.add_argument('--rounds', type=int, default=3, choices=range(1, 6))
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('This experiment requires native Windows; cross-link is not qualification')
    args.output.mkdir()  # New evidence directory only.
    runner_path = pathlib.Path(__file__).resolve().parents[2] / 'run-owned-process.py'
    spec = importlib.util.spec_from_file_location('experiment_owned_runner', runner_path)
    runner = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = runner
    spec.loader.exec_module(runner)
    manifests = {name: json.loads((getattr(args, name) / 'stormtty-shell-manifest.json').read_text(encoding='utf-8'))
                 for name in ('original', 'control', 'patched')}
    for name in ('control', 'patched'):
        info = manifests[name]['experimental_runtime']
        assert info['variant'] == name
        assert info['experimental_dll_sha256'] == sha(getattr(args, name) / 'usr/bin/msys-2.0.dll')
    control, patched = [manifests[name]['experimental_runtime'] for name in ('control', 'patched')]
    for key in ('build_environment_sha256', 'original_dll_sha256', 'source_archive_sha256', 'patch_sha256'):
        if control[key] != patched[key]:
            raise RuntimeError('Comparison provenance differs: ' + key)
    original_dll = sha(args.original / 'usr/bin/msys-2.0.dll')
    assert original_dll == control['original_dll_sha256']
    fixed = {'probe_sha256': sha(args.probe), 'fixture_sha256': sha(args.fixture)}
    results = []
    names = ['original', 'control', 'patched']
    for round_index in range(args.rounds):
        # Prespecified rotation, not a retry conditional on prior results.
        order = names[round_index % 3:] + names[:round_index % 3]
        for mode in ('inbox', 'packaged'):
            for name in order:
                label = f'{round_index + 1}-{mode}-{name}'
                command = [str(args.probe.resolve()), str((getattr(args, name) / 'usr/bin/zsh.exe').resolve()),
                           str(args.fixture.resolve()), str((args.output / label).resolve())]
                command += ['inbox'] if mode == 'inbox' else ['--packaged-provider', str(args.provider.resolve())]
                command += ['--resize-repeats', '50']
                print('EXPERIMENT_CASE=' + label, flush=True)
                result = runner.run_owned(command, timeout=180, tail_bytes=1024 * 1024, output_dir=args.output)
                (args.output / (label + '.log')).write_bytes(result.output_tail)
                input_evidence_available = b'PTY_INPUT_EVIDENCE label=' in result.output_tail
                entry = {'input_evidence_available': input_evidence_available, 'case': label, 'variant': name, 'provider': mode,
                         'round': round_index + 1, 'exit_code': result.exit_code,
                         **fixed, **result.metadata()}
                results.append(entry)
                (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
                print('EXPERIMENT_RESULT=' + json.dumps(entry, sort_keys=True), flush=True)
    return 1 if any(result['exit_code'] or not result['input_evidence_available'] for result in results) else 0


if __name__ == '__main__':
    raise SystemExit(main())
