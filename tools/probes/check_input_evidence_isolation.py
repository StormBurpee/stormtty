#!/usr/bin/env python3
"""Read-only Ninja artifact check (MSVC or GNU); never executes Windows binaries."""
import pathlib
import re
import sys


def check(build, settings_raw=False):
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
    if settings_raw:
        diagnostic = link('stormtty_settings_raw_probe')
        for name in ('session', 'process'):
            assert has_archive(diagnostic, f'stormtty_qualification_{name}'), diagnostic
            assert not has_archive(diagnostic, f'stormtty_{name}'), diagnostic
        raw = (build / 'stormtty_settings_raw_probe.exe').read_bytes()
        for marker in (b'PTY_INPUT_EVIDENCE', b'PTY_WRITE', b'SETTINGS_RAW_RESULT', b'SETTINGS_RAW_POST'):
            assert marker not in normal, f'production artifact contains {marker!r}'
            assert marker in raw, f'raw diagnostic artifact missing {marker!r}'
    print('PASS Windows link/artifact isolation (static inspection; binaries not executed)')


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    parser.add_argument('--settings-raw', action='store_true')
    args = parser.parse_args()
    check(args.build, settings_raw=args.settings_raw)
