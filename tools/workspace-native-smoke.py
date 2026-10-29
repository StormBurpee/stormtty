#!/usr/bin/env python3
"""Windows-only real HWND/ConPTY workspace restart qualification; not visual QA."""
import argparse
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True, type=pathlib.Path)
    parser.add_argument('--artifacts', type=pathlib.Path, help='optional native PNG output directory')
    args = parser.parse_args()
    if __import__('sys').platform != 'win32':
        parser.error('requires actual Windows runtime; cross-compilation is not qualification')
    app = args.app.resolve(strict=True)
    artifacts = args.artifacts.resolve() if args.artifacts else None
    if artifacts:
        artifacts.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='StormTTY.WorkspaceSmoke.') as directory:
        root = pathlib.Path(directory)
        (root / 'fixture.marker').write_bytes(b'StormTTY isolated workspace qualification v1\n')
        def run(stage):
            command = [str(app), '--smoke-workspaces', stage, '--workspace-fixture', str(root)]
            if artifacts and stage != 'corrupt':
                command += ['--capture', str(artifacts / f'workspace-{stage}.png')]
            result = subprocess.run(command, timeout=60,
                                    capture_output=True, text=True, errors='replace')
            print(result.stdout, end='')
            print(result.stderr, end='')
            if result.returncode:
                raise RuntimeError(f'{stage} app exited {result.returncode}')
            if f'Workspace persistence smoke passed: {stage}' not in result.stderr:
                raise RuntimeError(f'{stage} produced no dedicated success marker')
        saved = root / 'workspaces.stormtty'
        run('seed')
        seed = saved.read_bytes()
        if not seed or b'Fixture Alpha' not in seed or b'Fixture Beta' not in seed:
            raise RuntimeError('first actual app launch did not save both workspace metadata records')
        run('restore')
        if saved.read_bytes() == seed:
            raise RuntimeError('additive preset was not persisted on second app exit')
        corrupt = b'not a workspace library\x00\xff\r\nretain exactly\n'
        saved.write_bytes(corrupt)
        run('corrupt')
        if saved.read_bytes() != corrupt:
            raise RuntimeError('corrupt workspace file was mutated')
        if artifacts:
            for stage in ('seed', 'restore'):
                for suffix in ('', '-menu'):
                    image = artifacts / f'workspace-{stage}{suffix}.png'
                    if not image.is_file() or image.read_bytes()[:8] != b'\x89PNG\r\n\x1a\n':
                        raise RuntimeError(f'Missing native renderer capture: {image}')
        print('PASS: separate native app save/boot restore/additive live actor retention/corrupt preservation')


if __name__ == '__main__':
    main()
