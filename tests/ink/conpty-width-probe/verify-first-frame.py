"""Native qualification preflight; positive and fail-closed fixture tests."""
import hashlib
import pathlib
import subprocess
import sys

exe, fixture, directory = map(lambda s: pathlib.Path(s).resolve(), sys.argv[1:])
data = fixture.read_bytes()
assert len(data) == 1942
assert hashlib.sha256(data).hexdigest() == '1b400fe291a59ff91cb66d07eb4e80a86c68a2f7bdfa20f46cb75295c4815b59'
assert data.count(b'\n') == 19 and b'\r' not in data
assert data.startswith(b'\x1b[?1049h\x1b[?25l\x1b[?2026h')
assert data.endswith(b'\x1b[?2026l')
assert b'\x1b[K' not in data
assert not directory.exists(), 'Use a fresh private validation directory'
directory.mkdir(parents=True)


def run(args, success):
    result = subprocess.run([str(exe), *map(str, args)], timeout=20, capture_output=True)
    assert (result.returncode == 0) == success, (args, result.returncode, result.stdout, result.stderr)


run(['--verify-first-frame', fixture], True)
changed = bytes([data[0] ^ 1]) + data[1:]
for label, payload in [('changed', changed), ('truncated', data[:-1]), ('crlf', data.replace(b'\n', b'\r\n'))]:
    path = directory / (label + '.bin')
    path.write_bytes(payload)
    run(['--verify-first-frame', path], False)
    output = directory / (label + '-must-not-exist')
    run(['--first-frame', path, output], False)
    assert not output.exists(), 'Invalid replay created evidence directory before validation'
run(['--verify-first-frame', directory / 'missing.bin'], False)
print('Pinned native fixture verification and fail-closed negative controls passed.')
