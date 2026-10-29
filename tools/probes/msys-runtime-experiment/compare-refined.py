#!/usr/bin/env python3
"""Fixed two-variant native experiment. Preserve failures; never retry to green."""
import argparse
import importlib.util
import json
import os
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[2]

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); sys.modules[name] = module; spec.loader.exec_module(module)
    return module

identity = load('refined_identity', HERE/'refined-identities.py')
runner = load('refined_owned_runner', REPO/'tools/run-owned-process.py')
PROBE_SOURCE_SHA256 = '251477c3587c25b498bf1bf099e355d47a2cf3e1b8f1dcd7649e406aca39beae'

def plan():
    # Prespecified order, not randomized/repeated in response to outcomes.
    for provider in ('inbox', 'packaged'):
        for trial in range(1, 4):
            for variant in (('stock', 'refined') if trial != 2 else ('refined', 'stock')):
                yield provider, trial, variant

def raw_summary(output):
    lines = [line for line in output.splitlines() if line.startswith(b'PASTE_RESULT ')]
    pattern = rb'PASTE_RESULT code=([01]) provider=(inbox|packaged) vt_off_control_failures=([0-4]) raw_vt_reader_failures=([0-8]) production_cause_established=0'
    if len(lines) != 1 or not (match := re.fullmatch(pattern, lines[0])):
        return None
    return dict(code=int(match[1]), provider=match[2].decode(), vt_off_control_failures=int(match[3]), raw_vt_reader_failures=int(match[4]))

def run(args):
    roots = {name: getattr(args, name).resolve() for name in ('original', 'stock', 'refined')}
    identities = identity.identities(roots)
    identity.check(identity.sha(REPO/'tests/paste_input_probe.cpp') == PROBE_SOURCE_SHA256, 'PR7 probe/classifier changed')
    identity.check(not args.output.exists(), 'evidence directory must be new; no overwrite/retry')
    args.output.mkdir(parents=True)
    executable = args.probe.resolve() if args.workload == 'raw' else args.app.resolve()
    records = []
    report = dict(schema=1, workload=args.workload, planned=12, probe_source_sha256=PROBE_SOURCE_SHA256,
                  executable_sha256=identity.sha(executable), identities=identities, cases=records,
                  interpretation='Private experiment; no production promotion, no retry, no cause claim')
    for provider, trial, variant in plan():
        name = f'{provider}-{trial}-{variant}'
        command = [str(executable)]
        if args.workload == 'raw':
            command += [str(roots[variant])]
            if provider == 'packaged': command += [str(args.provider.resolve())]
            timeout = 180
        else:
            captures=args.output/'private-captures'; captures.mkdir(exist_ok=True)
            command += ['--smoke-settings', '--shell-runtime', str(roots[variant]), '--capture', str((captures/(name+'.png')).resolve())]
            if provider == 'packaged': command += ['--console-runtime', str(args.provider.resolve())]
            timeout = 30 # Existing settings watchdog, not extended for this experiment.
        record = dict(provider=provider, trial=trial, variant=variant, code=125, summary=None)
        try:
            result = runner.run_owned(command, timeout=timeout, reap_timeout=10, tail_bytes=1048576, output_dir=args.output)
            (args.output/(name+'.log')).write_bytes(result.output_tail)
            # Cleanup metadata refers only to the retained owned process handle;
            # the runner does not claim descendant-tree cleanup on timeout.
            record.update(code=result.exit_code, runner=result.metadata())
            if args.workload == 'raw':
                record['summary'] = raw_summary(result.output_tail)
                if record['summary'] is None or record['summary']['provider'] != provider:
                    record['evidence_error'] = 'missing_or_invalid_final_summary'
                elif record['summary']['code'] != result.exit_code:
                    record['evidence_error'] = 'summary_exit_disagreement'
            if result.timed_out or result.output_truncated or not result.reaped or result.cleanup_error:
                record['evidence_error'] = 'incomplete_output_or_owned_cleanup'
        except OSError:
            record['evidence_error'] = 'owned_process_launch_error'
        records.append(record)
        (args.output/'results.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
        print('REFINED_TRIAL '+json.dumps(record, sort_keys=True), flush=True)
    return int(any(r['code'] or r.get('evidence_error') for r in records))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('original', 'stock', 'refined', 'provider', 'output', 'probe', 'app'):
        parser.add_argument('--'+name, type=pathlib.Path, required=True)
    parser.add_argument('--workload', choices=('raw', 'settings'), required=True)
    args = parser.parse_args()
    if os.name != 'nt': parser.error('Native Windows required; not cross-build evidence')
    return run(args)

if __name__ == '__main__':
    raise SystemExit(main())
