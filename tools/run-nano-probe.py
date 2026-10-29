#!/usr/bin/env python3
"""Run both native nano providers; retain only bounded numeric fixture evidence."""
import argparse
from pathlib import Path
import re
import subprocess

FIELDS = frozenset('code phase checks file_exists file_bytes file_read_ok file_exact file_original extra_lf crlf seen_title seen_write_prompt seen_wrote seen_save_question seen_write_error seen_exit_zero process_exited session_error inspection_error'.split())


def sanitized(output):
    if isinstance(output, bytes):
        output = output.decode('ascii', errors='replace')
    lines = []
    for line in (output or '')[-32768:].splitlines():
        if len(line) > 1024 or not re.fullmatch(r'MANAGED_NANO_(?:RESULT|DIAGNOSTIC)(?: [a-z_]+=[0-9]+)+', line):
            continue
        fields = [part.split('=', 1)[0] for part in line.split()[1:]]
        if all(name in FIELDS for name in fields) and len(fields) == len(set(fields)):
            lines.append(line)
    return '\n'.join(lines[-16:])


def run(probe, runtime, provider, output):
    output.mkdir(parents=True, exist_ok=True)
    failed = False
    for name, extra in [('inbox', []), ('packaged', [str(provider)])]:
        timed_out = False
        try:
            result = subprocess.run([str(probe), str(runtime), *extra], stdout=subprocess.PIPE,
                                    stderr=subprocess.DEVNULL, timeout=180, check=False)
            code, text = result.returncode, sanitized(result.stdout)
        except subprocess.TimeoutExpired as error:
            timed_out = True
            code, text = 124, sanitized(error.stdout)
        except OSError:
            code, text = 125, ''  # No executable/path/error text is published.
        log = f'MANAGED_NANO_PROVIDER={name}\n{text}\nMANAGED_NANO_RUN code={code} timeout={int(timed_out)}\n'
        (output / f'nano-{name}.log').write_text(log, encoding='ascii')
        print(log, end='', flush=True)
        failed |= code != 0 or 'MANAGED_NANO_RESULT code=0 phase=5 ' not in text
    return int(failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('probe', 'runtime', 'provider', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    return run(args.probe.resolve(), args.runtime.resolve(), args.provider.resolve(), args.output)


if __name__ == '__main__':
    raise SystemExit(main())
