#!/usr/bin/env python3
"""POSIX harness checks only, never Windows shell qualification.

Usage: python3 tools/probes/bundled_shell_probe_tests.py PROBE ZSH NEW_OUTPUT_DIR
Keeps controlled fixture mutations and full failure logs in NEW_OUTPUT_DIR.
"""
import os
import pathlib
import subprocess
import sys

probe, shell, output = map(lambda p: pathlib.Path(p).resolve(), sys.argv[1:])
output.mkdir()  # Never overwrite evidence or user files.
fixture = pathlib.Path(__file__).with_name('bundled_shell_fixture.zsh').read_text()


def run(name, text, expected, repeats='6'):
    source = output / (name + '.zsh')
    source.write_text(text)
    result = subprocess.run([str(probe), str(shell), str(source), str(output / name),
                             '--resize-repeats', repeats], capture_output=True, text=True, timeout=90)
    log = result.stdout + result.stderr
    (output / (name + '.log')).write_text(log)
    assert result.returncode == expected, (name, result.returncode, log[-2000:])
    return log


positive = run('positive', fixture, 0)
assert '[resize phase 6] PASS' in positive
assert 'windows_control=not-applicable' in positive
assert 'GLOBAL_DOTFILE_WAS_READ' not in positive
# Nonzero command status must survive successful post-failure observations.
failed = run('command-failed', fixture + '\nstormtty_probe_size() { return 19 }\n', 1)
assert 'Original resize failure frame (before diagnostics)' in failed
assert '__STORMTTY_RESIZE_DIAG_END:' in failed
assert 'FAIL: shell marker not observed in real terminal frame' in failed
assert '[resize phase 1] PASS' not in failed
assert failed.count('text=stormtty_probe_size\n') == 1  # No retry after failed command.
# Correct-looking dimensions with a stale sequence are not current evidence.
stale = run('stale-ack', fixture.replace(
    'SIZE_ACK:${stormtty_probe_sequence}:', 'SIZE_ACK:0:'), 1)
assert 'FAIL: shell did not observe terminal resize' in stale
assert '__STORMTTY_RESIZE_DIAG_END:' in stale
assert '[resize phase 1] PASS' not in stale
for invalid in ['0', '51', '-1', 'abc', '100']:
    result = subprocess.run([str(probe), '--resize-repeats', invalid], capture_output=True, timeout=5)
    assert result.returncode == 2, invalid
# Exercise the ring in isolation: there must be no file until explicit flush,
# and >256 synthetic records must retain only the newest 256, in order.
ring_source = output / 'ring.zsh'
ring_source.write_text(fixture + '''
unfunction preexec
stormtty_probe_sequence=5
stormtty_probe_journal_reset
for (( n=1; n<=600; ++n )); do stormtty_probe_journal "synthetic=$n"; done
[[ ! -e $STORMTTY_PROBE_JOURNAL ]] || exit 71
(( ${#stormtty_probe_journal_records} == 256 )) || exit 72
stormtty_probe_journal_flush
''')
ring_path = output / 'ring.log'
result = subprocess.run([str(shell), '-df', str(ring_source)],
                        env={**os.environ, 'STORMTTY_PROBE_JOURNAL': str(ring_path)},
                        capture_output=True, text=True, timeout=5)
assert result.returncode == 0, result.stderr
records = ring_path.read_text().splitlines()
assert len(records) == 257 and 'retained-from=345' in records[0]
assert 'synthetic=345' in records[1] and 'synthetic=600' in records[-1]
print('PASS POSIX probe positive, failure preservation, stale ACK, CLI bounds, memory-ring bounds/no per-key file writes')
