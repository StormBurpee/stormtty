#!/usr/bin/env python3
"""CI-only stacks from one owned synthetic renderer fixture; never a passing gate."""
import json
import os
import re
from pathlib import Path
import subprocess
import sys


def diagnose(build):
    build = Path(build).resolve()
    output = build / 'artifacts'
    output.mkdir(parents=True, exist_ok=True)
    debugger = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Windows Kits/10/Debuggers/x64/cdb.exe'
    raw_stacks = output / 'teardown-stacks.private.tmp'
    metadata = {'diagnostic_only': True, 'debugger': str(debugger), 'sampled': False}
    child = None
    debug = None
    try:
        if not debugger.is_file():
            raise RuntimeError('Installed Windows SDK x64 cdb.exe unavailable')
        with (output / 'teardown-fixture.log').open('wb') as fixture_log, raw_stacks.open('wb') as stack_log:
            child = subprocess.Popen([str(build / 'renderer_smoke.exe'), '--final-teardown'], stdin=subprocess.DEVNULL, stdout=fixture_log, stderr=subprocess.STDOUT, close_fds=True)
            metadata['pid'] = child.pid
            try:
                child.wait(timeout=10)
                metadata['finished_before_sample'] = True
            except subprocess.TimeoutExpired:
                # PID is from our retained live handle, never a process-name lookup.
                # Noninvasive attach reads stacks; no injected code or memory edits.
                debug = subprocess.Popen([str(debugger), '-pv', '-p', str(child.pid), '-y', str(build), '-c', '.reload; ~* knL; lm; qd'], stdin=subprocess.DEVNULL, stdout=stack_log, stderr=subprocess.STDOUT, close_fds=True)
                try:
                    debug.wait(timeout=20)
                    metadata['sampled'] = debug.returncode == 0
                except subprocess.TimeoutExpired:
                    metadata['debugger_timed_out'] = True
                finally:
                    if debug.poll() is None:
                        debug.kill()
                    debug.wait(timeout=10)
                    metadata['debugger_returncode'] = debug.returncode
    except Exception as error:
        metadata['error'] = str(error)
    finally:
        # Never publish raw debugger output, process memory, arguments or paths.
        # Retain only frame indices/symbols and module names from numbered, source-hidden `knL`/`lm`.
        if raw_stacks.exists():
            try:
                lines = raw_stacks.read_text(encoding='utf-8', errors='replace').splitlines()
                safe = []
                for line in lines:
                    frame = re.match(r'^\s*([0-9a-f]{2})\s+[0-9a-f`]+\s+[0-9a-f`]+\s+([A-Za-z0-9_.!+<>~?:\-]+)', line, re.I)
                    module = re.match(r'^([0-9a-f`]{8,})\s+([0-9a-f`]{8,})\s+([A-Za-z0-9_]+)\s', line, re.I)
                    thread = re.match(r'^\s*\.?\s*([0-9]+)\s+Id:', line)
                    if frame:
                        safe.append('frame ' + frame[1] + ' ' + frame[2])
                    elif module:
                        safe.append('module ' + module[3])
                    elif thread:
                        safe.append('thread ' + thread[1])
                (output / 'teardown-stacks.log').write_text('\n'.join(safe) + '\n', encoding='utf-8')
                metadata['sanitized_frame_count'] = sum(line.startswith('frame ') for line in safe)
            except Exception as error:
                metadata['sanitize_error'] = type(error).__name__
            finally:
                try:
                    raw_stacks.unlink()
                except OSError:
                    metadata['raw_cleanup_error'] = True
        if child is not None:
            try:
                if child.poll() is None:
                    child.kill()
                    metadata['fixture_killed'] = True
                child.wait(timeout=10)
                metadata['fixture_returncode'] = child.returncode
                metadata['fixture_reaped'] = True
            except Exception as error:
                metadata['cleanup_error'] = str(error)
        (output / 'teardown-diagnostic.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
        print(json.dumps(metadata), flush=True)
    return 0 if metadata.get('sampled') and metadata.get('fixture_reaped') and metadata.get('sanitized_frame_count', 0) > 0 and not any(metadata.get(key) for key in ('cleanup_error', 'sanitize_error', 'raw_cleanup_error')) else 1


if __name__ == '__main__':
    raise SystemExit(diagnose(sys.argv[1] if len(sys.argv) == 2 else 'build-ui'))
