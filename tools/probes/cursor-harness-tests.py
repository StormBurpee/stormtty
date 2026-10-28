#!/usr/bin/env python3
"""Local POSIX probe mechanics; not native Windows candidate qualification."""
import pathlib
import re
import subprocess
import sys

probe, shell, output = (pathlib.Path(x).resolve() for x in sys.argv[1:])
output.mkdir()
fixture = pathlib.Path(__file__).with_name('bundled_shell_fixture.zsh').read_text()
pattern = re.compile(r'^RESIZE_CASE_SUMMARY code=([0-8]) phase=([0-9]+) expected_cols=([0-9]+) expected_rows=([0-9]+) observed_cols=([0-9]+) observed_rows=([0-9]+)$', re.M)


def run(name, extra='', *, expected=0, repeats=50, normal=False):
    source = output / (name + '.zsh')
    source.write_text(fixture + extra)
    options = ['--resize-repeats', str(repeats), '--resize-summary']
    if not normal:
        options += ['--resize-one-shot']
    result = subprocess.run([str(probe), str(shell), str(source), str(output / name), *options], capture_output=True, text=True, timeout=90)
    log = result.stdout + result.stderr
    (output / (name + '.log')).write_text(log)
    assert result.returncode == expected, (name, result.returncode, log[-2000:])
    matches = pattern.findall(result.stdout)
    assert len(matches) == 1, (name, matches)
    numeric = tuple(map(int, matches[0]))
    assert all(0 <= value <= 32767 for value in numeric), numeric
    assert (numeric[0] == 0) == (expected == 0), numeric
    return log, numeric


# These functions represent every external-job route in the ordinary workload.
# If any is called in builtin-only mode, the case fails visibly.
tripwire = '\n' + '\n'.join(f'{name}() {{ print EXTERNAL_JOB_FORBIDDEN; return 71 }}' for name in (
    'stormtty_probe_native', 'stormtty_probe_native_size', 'stormtty_probe_wait',
    'stormtty_probe_resize_diagnostic')) + '\n'
log, summary = run('oneshot-50', tripwire)
assert summary == (0, 50, 100, 30, 100, 30), summary
assert log.count('text=stormtty_probe_size\n') == 50
assert '[resize phase 50] PASS' in log
assert 'EXTERNAL_JOB_FORBIDDEN' not in log
assert 'text=stormtty_probe_native' not in log
assert 'text=stormtty_probe_journal_' not in log
assert 'text=stormtty_probe_wait' not in log
log, summary = run('idle', tripwire, repeats=1)
assert summary == (0, 1, 111, 35, 111, 35), summary
assert log.count('text=stormtty_probe_size\n') == 1
log, summary = run('stale-grid', tripwire + '\nstormtty_probe_size() { stormtty_probe_emit SIZE:100x30; stormtty_probe_emit "SIZE_ACK:${stormtty_probe_sequence}:100x30" }\n', expected=1)
assert summary == (4, 1, 111, 35, 100, 30), summary
assert log.count('text=stormtty_probe_size\n') == 1
assert log.index('RESIZE_CASE_SUMMARY') < log.index('Original resize failure frame')
assert 'text=stormtty_probe_resize_diagnostic' not in log
log, summary = run('stale-sequence', '\nstormtty_probe_size() { stormtty_probe_emit SIZE:111x35; stormtty_probe_emit SIZE_ACK:0:111x35 }\n', expected=1)
assert summary == (4, 1, 111, 35, 0, 0), summary
# Out-of-range/unparseable ACK payload never enters the whitelisted summary.
for label, grid in [('oversized-grid', '32768x35'), ('overflow-grid', '999999999999999999999x35')]:
    log, summary = run(label, '\nstormtty_probe_size() { stormtty_probe_emit SIZE:111x35; stormtty_probe_emit "SIZE_ACK:${stormtty_probe_sequence}:'+grid+'" }\n', expected=1)
    assert summary == (4, 1, 111, 35, 0, 0), summary
log, summary = run('command-failed', '\nstormtty_probe_size() { return 19 }\n', expected=1)
assert summary == (3, 1, 111, 35, 0, 0), summary
assert log.count('text=stormtty_probe_size\n') == 1
log, summary = run('normal', repeats=2, normal=True)
assert summary == (0, 2, 100, 30, 100, 30), summary
assert 'text=stormtty_probe_native_size' in log
assert 'text=stormtty_probe_wait first' in log
# An early setup error has no phase or grid and no free-form summary fields.
result = subprocess.run([str(probe), str(shell), str(output/'idle.zsh'), str(output/'idle'),
                         '--resize-summary', '--resize-one-shot', '--resize-repeats', '1'],
                        capture_output=True, text=True, timeout=5)
assert result.returncode == 1
assert pattern.findall(result.stdout) == [('1', '0', '0', '0', '0', '0')]
for options in (['--resize-one-shot'], ['--resize-one-shot', '--resize-one-shot', '--resize-repeats', '1'],
                ['--resize-summary', '--resize-summary'], ['--resize-one-shot', '--resize-trace-idle', '--resize-repeats', '1']):
    result = subprocess.run([str(probe), str(shell), str(output/'idle.zsh'), str(output/'invalid'), *options], capture_output=True, timeout=5)
    assert result.returncode == 2, options
    assert not (output/'invalid').exists()
print('PASS one-shot builtin-only 50 phases, one-shot stale/command failure, bounded summaries, unchanged normal workload, CLI exclusivity')
