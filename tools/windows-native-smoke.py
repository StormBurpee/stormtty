#!/usr/bin/env python3
"""Controlled real two-HWND/ConPTY ownership qualification; not physical UI QA."""
import argparse
import importlib.util
import json
import pathlib
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True, type=pathlib.Path)
    parser.add_argument('--artifacts', type=pathlib.Path)
    args = parser.parse_args()
    if sys.platform != 'win32':
        parser.error('requires actual Windows; cross-compilation is not qualification')
    spec = importlib.util.spec_from_file_location('owned_windows_fixture', pathlib.Path(__file__).with_name('run-owned-process.py'))
    runner = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = runner
    spec.loader.exec_module(runner)
    app = args.app.resolve(strict=True)
    artifacts = args.artifacts.resolve() if args.artifacts else None
    if artifacts:
        artifacts.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='StormTTY.WindowsSmoke.') as directory:
        root = pathlib.Path(directory)
        (root / 'fixture.marker').write_bytes(b'StormTTY isolated windows qualification v1\n')
        user_config = root / 'do-not-load.stormtty'
        sentinel = b'authored rejection sentinel, not user configuration\n'
        user_config.write_bytes(sentinel)
        negative = [
            ['--smoke-windows', '--config', str(user_config), '--window-fixture', str(root)],
            ['--smoke-windows', '--smoke', '--window-fixture', str(root)],
            ['--smoke-windows'],
        ]
        for command in negative:
            result = runner.run_owned([str(app), *command], timeout=10, output_dir=root)
            if (result.exit_code != 1 or not result.reaped or result.timed_out
                    or result.cleanup_error or result.output_truncated):
                raise RuntimeError('unsafe/ambiguous window fixture was not rejected cleanly: ' + json.dumps(result.metadata()))
            if user_config.read_bytes() != sentinel:
                raise RuntimeError('window fixture rejection mutated its authored config sentinel')
        user_config.unlink()
        command = [str(app), '--smoke-windows', '--window-fixture', str(root)]
        if artifacts:
            command += ['--capture', str(artifacts / 'native-windows.png')]
        result = runner.run_owned(command, timeout=70, tail_bytes=262144, output_dir=root)
        output = result.output_tail.decode('utf-8', errors='replace')
        evidence = output + '\nWINDOWS_OWNERSHIP_RESULT=' + json.dumps(result.metadata(), sort_keys=True) + '\n'
        print(evidence, end='')
        if artifacts:
            (artifacts / 'native-windows.log').write_text(evidence, encoding='utf-8')
        if (result.exit_code or not result.reaped or result.timed_out
                or result.cleanup_error or result.output_truncated):
            raise RuntimeError('native windows ownership fixture failed')
        for marker in ['Native windows rollback:', 'Native windows pending Settings:', 'Native windows ownership passed:', 'Native CI loop exit: code=0']:
            if marker not in output:
                raise RuntimeError('missing dedicated native ownership proof: ' + marker)
        if not (root / 'pending-settings.stormtty').is_file():
            raise RuntimeError('deferred authored Settings write did not finish')
        if (root / 'config.stormtty').exists() or (root / 'workspaces.stormtty').exists():
            raise RuntimeError('nonpersistent ownership fixture unexpectedly wrote a layout/config')
    if root.exists():
        raise RuntimeError('native windows ownership fixture root survived cleanup')
    completion = ('WINDOWS_OWNERSHIP_CLEANUP root_removed=1\n'
                  'PASS: controlled two-window ownership, rollback, confirmation, independent close and survivor output\n')
    if artifacts:
        with (artifacts / 'native-windows.log').open('a', encoding='utf-8') as log:
            log.write(completion)
    print(completion, end='')


if __name__ == '__main__':
    main()
