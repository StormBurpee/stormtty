#!/usr/bin/env python3
"""Read-only Ninja artifact check (MSVC or GNU); never executes Windows binaries."""
import pathlib
import re
import sys


def check(build):
    lines = (build / 'build.ninja').read_text().splitlines()
    def link(name):
        return next(line for line in lines if line.startswith(f'build {name}.exe:'))
    probe, app = link('bundled_shell_probe'), link('stormtty')
    def has_archive(line, name):
        # MSVC uses stormtty_*.lib; Windows GNU uses libstormtty_*.a.
        # Delimit complete basenames, not a substring of another target name.
        return re.search(r'(?<![A-Za-z0-9_])(?:lib)?' + re.escape(name) +
                         r'\.(?:a|lib)(?![A-Za-z0-9_])', line) is not None
    for name in ('session', 'process'):
        assert has_archive(probe, f'stormtty_qualification_{name}'), probe
        assert not has_archive(probe, f'stormtty_{name}'), probe
        assert not has_archive(app, f'stormtty_qualification_{name}'), app
        assert has_archive(app, f'stormtty_{name}'), app
    # Inspect binary content too: a stale/incorrect artifact is not graph proof.
    normal = (build / 'stormtty.exe').read_bytes()
    instrumented = (build / 'bundled_shell_probe.exe').read_bytes()
    # Literal output format survives optimized MSVC builds; C++ class/type
    # names need not survive optimization, stripping or disabled RTTI.
    for marker in (b'PTY_INPUT_EVIDENCE', b'PTY_WRITE'):
        assert marker not in normal, f'production artifact contains {marker!r}'
        assert marker in instrumented, f'probe artifact missing {marker!r}'
    print('PASS Windows link/artifact isolation (static inspection; binaries not executed)')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: check_input_evidence_isolation.py NINJA_BUILD_DIRECTORY')
    check(pathlib.Path(sys.argv[1]))
