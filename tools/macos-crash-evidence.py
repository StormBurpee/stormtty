#!/usr/bin/env python3
"""Failure-only evidence for this run's StormTTY GUI; never changes OS policy."""
import pathlib
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

root = pathlib.Path.cwd()
build = root / 'build'
report = build / 'test-results.xml'
try:
    failed = any(t.attrib.get('name') == 'native_macos_window_smoke' and
                 (t.find('failure') is not None or t.find('error') is not None)
                 for t in ET.parse(report).iter('testcase'))
except (OSError, ET.ParseError) as error:
    print(f'No readable GUI test result: {error}')
    sys.exit(0)
if not failed:
    print('Native macOS GUI test did not fail; no debugger rerun.')
    sys.exit(0)
app = (build / 'stormtty.app/Contents/MacOS/stormtty').resolve()
if not app.is_file():
    print('StormTTY executable is missing; no debugger rerun.')
    sys.exit(0)
out = build / 'artifacts'
out.mkdir(parents=True, exist_ok=True)
started = time.time()
log = out / 'macos-debugger.log'
try:
    with log.open('w') as stream:
        result = subprocess.run(['/usr/bin/lldb', '--no-lldbinit', '--batch',
                                 '-o', 'run', '-o', 'thread backtrace all',
                                 '--', str(app), '--smoke'], stdout=stream,
                                stderr=subprocess.STDOUT, timeout=45, check=False)
    print(f'Debugger rerun exit: {result.returncode}; original CTest failure remains authoritative.')
except (OSError, subprocess.TimeoutExpired) as error:
    print(f'Debugger evidence unavailable or timed out: {error}')
if log.exists():
    print(log.read_text(errors='replace')[-128_000:])
# Only this executable's newly generated report, never unrelated crash reports.
directory = pathlib.Path.home() / 'Library/Logs/DiagnosticReports'
for attempt in range(5):
    matches = []
    for path in directory.glob('*') if directory.exists() else []:
        if (not path.name.lower().startswith('stormtty') or path.suffix not in ('.ips', '.crash')
                or not path.is_file() or path.stat().st_mtime < started
                or path.stat().st_size > 8 * 1024 * 1024):
            continue
        content = path.read_text(errors='replace')
        if str(app) in content or str(app).replace('/', '\\/') in content:
            matches.append(path)
    if matches:
        newest = max(matches, key=lambda p: p.stat().st_mtime)
        destination = out / ('stormtty-crash' + newest.suffix)
        shutil.copyfile(newest, destination)
        print(f'Preserved matching current-run crash report: {destination.name}')
        break
    if attempt != 4:
        time.sleep(1)
else:
    print('No matching current-run StormTTY crash report was produced.')
