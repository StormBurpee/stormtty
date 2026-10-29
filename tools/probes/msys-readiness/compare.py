#!/usr/bin/env python3
"""One fixed official-runtime/inbox readiness diagnostic; never retries failures."""
import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[2]
OFFICIAL_DLL = 'd3b5324eef7fe377c3412bd8d9f5c526ba1136940e34105436b15e5f1ae90717'
ORIGINAL_PROBE = '251477c3587c25b498bf1bf099e355d47a2cf3e1b8f1dcd7649e406aca39beae'
EXPECTED = b'\x1b[200~PASTE_QUAL_0123456789_[x]_end\x1b[201~\r'
CELLS = (('timed', 'plain'), ('direct', 'plain'), ('timed', 'resize'), ('direct', 'resize'))


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result


owned = module('readiness_owned', REPO / 'tools/run-owned-process.py')
stager = module('readiness_validator', REPO / 'tools/stage-developer-shell.py')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def plan():
    for trial, cells in enumerate((CELLS, tuple(reversed(CELLS))), 1):
        for reader, control in cells:
            yield dict(trial=trial, reader=reader, control=control)


def fields(line):
    parts = [part.split(b'=', 1) for part in line.split()[1:]]
    if any(len(part) != 2 for part in parts) or len({part[0] for part in parts}) != len(parts):
        return {}
    return dict(parts)


def round_evidence_valid(row, index, segment):
    lines = segment.splitlines()
    values = fields(lines[0])
    expected = dict(round=str(index).encode(), timed=b'1' if row['reader'] == 'timed' else b'0',
                    mode=b'2' if row['control'] == 'resize' else b'0', consumed=b'1', writer_published=b'1',
                    complete=b'1', equal=b'1', statuses_ok=b'1', write_shape_ok=b'1', expected_hex=EXPECTED.hex().encode(),
                    child_hex=EXPECTED.hex().encode(), sysread_status_count=b'0,1;' * len(EXPECTED),
                    retried_eintr=b'unobserved', sigwinch_count=b'unobserved')
    if values != {key.encode(): value for key, value in expected.items()}:
        return False
    headers = [fields(line) for line in lines if line.startswith(b'PTY_INPUT_EVIDENCE ')]
    if len(headers) != 1 or lines.count(b'END_PTY_INPUT_EVIDENCE') != 1:
        return False
    header = headers[0]
    if header.get(b'label') != b'authored-paste-only':
        return False
    if any(header.get(key) != b'0' for key in (b'overwritten', b'byte_truncated_records', b'missing_since_after')):
        return False
    gaps = [line for line in lines if line.startswith(b'PASTE_GAPS ')]
    prefix = b' kind=2 mode=' + expected['mode'] + b' round=' + str(index).encode()
    if len(gaps) != 1 or not re.fullmatch(b'PASTE_GAPS' + prefix + rb' max_and_after_escape_us=[0-9]+:[0-9]+', gaps[0]):
        return False
    records = [fields(line) for line in lines if line.startswith(b'PTY_WRITE ')]
    try:
        if len(records) != int(header[b'retained']) or not records:
            return False
        after = int(header[b'after'])
        if int(header[b'completed']) != after + len(records):
            return False
        authored = []
        for offset, record in enumerate(records, 1):
            if (int(record[b'seq']) != after + offset or record[b'success'] != b'1' or record[b'error'] != b'0'
                    or int(record[b'offset']) != 0 or int(record[b'message']) < 1
                    or record[b'requested_hex_truncated'] != b'0' or record[b'accepted_hex_truncated'] != b'0'):
                return False
            data = bytes.fromhex(record[b'accepted_hex'].decode())
            if not (len(data) == int(record[b'accepted']) == int(record[b'reported']) == int(record[b'requested'])):
                return False
            if record[b'accepted_hex'] != record[b'requested_hex']:
                return False
            if not re.fullmatch(rb'\x1b\[[1-9][0-9]{0,4};[1-9][0-9]{0,4}R', data):
                authored.append(data)
        # The intact bracketed paste and CR must remain two complete writes.
        return authored == [EXPECTED[:-1], EXPECTED[-1:]]
    except (KeyError, ValueError):
        return False


def resize_coverage_valid(row, output, rounds):
    """Snapshot evidence only; no waits and no claim that the shell saw a resize.

    Accepted requests advance generations; the controller may coalesce them.
    Calls advance before the native call; applied advances after success. One
    call can therefore be in flight. Each resize round must nevertheless show
    a successful native completion from that round, not old cumulative activity.
    """
    lines = [line for line in output.splitlines() if line.startswith(b'PASTE_RESIZE ')]
    if not 1 <= rounds <= 20 or len(lines) != rounds:
        return False
    previous_applied = previous_calls = 0
    for index, line in enumerate(lines, 1):
        match = re.fullmatch(rb'PASTE_RESIZE kind=2 mode=([02]) round=([0-9]+) '
                             rb'requested=([0-9]+) applied=([0-9]+) calls=([0-9]+) failures=([0-9]+)', line)
        if not match:
            return False
        try:
            mode, observed_round, requested, applied, calls, failures = map(int, match.groups())
        except ValueError:
            return False
        if observed_round != index or mode != (2 if row['control'] == 'resize' else 0):
            return False
        if row['control'] == 'plain':
            if (requested, applied, calls, failures) != (0, 0, 0, 0):
                return False
        else:
            if requested != 4 * index or failures != 0 or not 4 * (index - 1) < applied <= requested:
                return False
            # There is at most one in-flight native call, whose generation must
            # be newer than applied. Skipped/coalesced generations are allowed.
            in_flight_bound = int(applied < requested)
            if not previous_calls < calls <= min(requested, applied + in_flight_bound):
                return False
            if calls - previous_calls > applied - previous_applied + in_flight_bound:
                return False
        previous_applied, previous_calls = applied, calls
    return True


def assess(row, metadata, output):
    bounded = (metadata.get('reaped') is True and metadata.get('timed_out') is False
               and metadata.get('output_truncated') is False and metadata.get('cleanup_error') == '')
    lines = output.splitlines()
    finals = [line for line in lines if line.startswith(b'READINESS_RESULT ')]
    pattern = (rb'READINESS_RESULT code=([01]) reader=(timed|direct) control=(plain|resize) '
               rb'provider=inbox runtime=official production_cause_established=0')
    match = re.fullmatch(pattern, finals[0]) if len(finals) == 1 else None
    identity = bool(match and match[2].decode() == row['reader'] and match[3].decode() == row['control'])
    code = int(match[1]) if match else None
    cleanup = [line for line in lines if line.startswith(b'READINESS_CLEANUP ')]
    closed = len(cleanup) == 1 and bool(re.fullmatch(
        rb'READINESS_CLEANUP closed=1 exit_present=1 exit_code=[0-9]+ error_present=0 requested_close=1', cleanup[0]))
    scratch_lines = [line for line in lines if line.startswith(b'READINESS_SCRATCH ')]
    scratch = scratch_lines == [b'READINESS_SCRATCH removed=1']
    cases = [line for line in lines if line.startswith(b'PASTE_CASE ')]
    mode = b'2' if row['control'] == 'resize' else b'0'
    expected_case = b'PASTE_CASE kind=2 mode=' + mode + b' planned=20 completed=20 code=0'
    segments = output.split(b'READINESS_ROUND ')[1:]
    all_rounds = len(segments) == 20 and all(
        round_evidence_valid(row, index, b'READINESS_ROUND ' + segment)
        for index, segment in enumerate(segments, 1))
    headers = [fields(line) for line in lines if line.startswith(b'PTY_INPUT_EVIDENCE ')]
    continuity = len(headers) == 20 and all(
        headers[i].get(b'after') == headers[i - 1].get(b'completed') for i in range(1, len(headers)))
    resize_coverage = resize_coverage_valid(row, output, len(segments))
    all_rounds = all_rounds and continuity
    complete = bounded and identity and code == metadata.get('returncode') and closed and scratch
    passed = complete and code == 0 and cases == [expected_case] and all_rounds and resize_coverage
    return dict(passed=passed, complete=complete, returned_code=metadata.get('returncode'),
                case_summary=[line.decode('ascii', 'replace') for line in cases], rounds_observed=len(segments),
                child_byte_mismatches=sum(fields(line).get(b'consumed') == b'1' and fields(line).get(b'equal') == b'0'
                                          for line in lines if line.startswith(b'READINESS_ROUND ')),
                gap_reports=[line.decode('ascii', 'replace') for line in lines if line.startswith(b'PASTE_GAPS ')],
                observed_resize_coverage_valid=resize_coverage, resize_coverage_complete=resize_coverage and len(segments) == 20,
                exact_round_evidence_valid=all_rounds, retried_eintr='unobserved', sigwinch_count='unobserved',
                production_qualified=False, causal_proof=False)


def verify_runtime(root):
    manifest = stager.load(root / stager.MANIFEST)
    if 'experimental_runtime' in manifest or sha(root / 'usr/bin/msys-2.0.dll') != OFFICIAL_DLL:
        raise ValueError('Only untouched pinned official runtime is allowed')
    files, receipt = stager.validate(root, REPO / 'tools/shell-bootstrap.lock.json', REPO / 'tools/shell-preset.zsh')
    return files, dict(dll_sha256=OFFICIAL_DLL, manifest_sha256=receipt['manifest_sha256'],
                       zsh_sha256=sha(root / 'usr/bin/zsh.exe'), lock_sha256=sha(REPO / 'tools/shell-bootstrap.lock.json'))


def run(args):
    runtime = args.runtime.resolve(strict=True)
    output = args.output.resolve()
    if runtime == output or runtime in output.parents or output in runtime.parents:
        raise ValueError('Evidence destination and official runtime must not overlap')
    if args.output.exists() or args.output.is_symlink():
        raise ValueError('New evidence destination required; no overwrite or retry')
    if sha(REPO / 'tests/paste_input_probe.cpp') != ORIGINAL_PROBE:
        raise ValueError('Original qualification probe changed')
    args.output.mkdir(parents=True)
    report = dict(schema=1, planned=8, ordered_plan=list(plan()), rounds_per_launch=20, cases=[], provider='inbox', runtime='untouched-official',
                  internal_path_coverage='UNINSTRUMENTED', retried_eintr='unobserved', sigwinch_count='unobserved',
                  production_qualified=False, causal_proof=False,
                  timing_caveat='Removing poll/select and adding equal reader bookkeeping changes scheduling; a difference alone is not causal proof.')
    target = args.output / 'results.json'

    def save():
        target.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

    save()  # A preflight failure cannot masquerade as eight executed cases.
    try:
        files, report['runtime_identity'] = verify_runtime(args.runtime)
        stager.regular(args.probe)
        stager.regular(args.probe.parent / 'ghostty-vt.dll')
        files.update({args.probe: sha(args.probe), args.probe.parent / 'ghostty-vt.dll': sha(args.probe.parent / 'ghostty-vt.dll')})
        report['binaries'] = {str(p.name): digest for p, digest in files.items() if p in (args.probe, args.probe.parent / 'ghostty-vt.dll')}
        report['source_sha256'] = {p.relative_to(REPO).as_posix(): sha(p) for p in
            (HERE / 'probe.cpp', HERE / 'evidence-contract.hpp', HERE / 'contract-tests.cpp',
             HERE / 'compare.py', HERE / 'tests.py', HERE / 'README.md',
             REPO / 'CMakeLists.txt', REPO / '.github/workflows/msys-stock-readiness.yml',
             REPO / 'tests/paste_input_probe.cpp', REPO / 'tools/run-owned-process.py')}
        report['commit'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True).strip()
        save()
        runtime_snapshot = stager.inventory(args.runtime)
        for row in plan():
            if stager.inventory(args.runtime) != runtime_snapshot:
                raise ValueError('Official runtime inventory changed during diagnostic')
            for path, digest in files.items():
                stager.regular(path)
                if sha(path) != digest:
                    raise ValueError('Validated input bytes changed during diagnostic')
            name = f"{row['trial']}-{row['reader']}-{row['control']}"
            record = dict(row, passed=False, complete=False)
            try:
                result = owned.run_owned([str(args.probe), str(args.runtime), row['reader'], row['control']],
                                         timeout=180, reap_timeout=10, tail_bytes=1048576, output_dir=args.output)
                (args.output / (name + '.log')).write_bytes(result.output_tail)
                record['runner'] = result.metadata()
                record.update(assess(row, record['runner'], result.output_tail))
            except OSError as error:
                record['launch_error'] = type(error).__name__
                (args.output / (name + '.log')).write_text('READINESS_LAUNCH_ERROR ' + type(error).__name__ + '\n')
            log = args.output / (name + '.log')
            record['log_bytes'] = log.stat().st_size
            record['log_sha256'] = sha(log)
            report['cases'].append(record)
            save()
            print('READINESS_TRIAL ' + json.dumps(record, sort_keys=True), flush=True)
        for path, digest in files.items():
            stager.regular(path)
            if sha(path) != digest:
                raise ValueError('Validated input bytes changed after diagnostic')
        if stager.inventory(args.runtime) != runtime_snapshot:
            raise ValueError('Official runtime inventory changed after diagnostic')
        report['executed_launches'] = len(report['cases'])
        report['passed_launches'] = sum(row['passed'] for row in report['cases'])
        report['lifecycle_complete_launches'] = sum(row['complete'] for row in report['cases'])
        report['final_identity_verified'] = True
        save()
    except (OSError, ValueError, RuntimeError) as error:
        report['preflight_or_identity_error'] = str(error)
        save()
        raise
    return int(any(not row['passed'] for row in report['cases']))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('runtime', 'probe', 'output'):
        parser.add_argument('--' + name, type=pathlib.Path, required=True)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Native Windows required; portable tests are not native execution')
    for name in ('runtime', 'probe', 'output'):
        setattr(args, name, getattr(args, name).absolute())
    raise SystemExit(run(args))
