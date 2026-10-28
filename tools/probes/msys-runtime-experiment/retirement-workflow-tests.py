#!/usr/bin/env python3
"""Static workflow isolation checks; no native execution."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[3]
p = root / '.github/workflows/msys-cursor-retirement.yml'
s = p.read_text()
assert "branches: ['experiment/msys-cursor-retirement']" in s
assert "if: github.ref == 'refs/heads/experiment/msys-cursor-retirement'" in s
assert 'contents: read' in s and 'contents: write' not in s
assert s.count('bash tools/probes/msys-runtime-experiment/build-retirement.sh') == 2
assert 'build-trace.sh' not in s and '0001-retire' not in s
assert 'timeout-minutes: 300' in s
for work in ('idle','repeated','builtin-only'):
    assert '--workload '+work+' --rounds '+('1' if work == 'idle' else '3') in s
    assert '/evidence/'+work+'/results.json' in s
assert s.count("if: always() && steps.staging.outcome == 'success'") == 3
assert '--original' in s and '--baseline' in s and '--combined' in s
assert '--control' not in s and '--candidate' not in s
uploads = s.split('          path: |')[-1]
assert '*.log' not in uploads and '.dll' not in uploads and 'scratch' not in uploads
assert '*/*.json' not in uploads
for action in re.findall(r'uses: ([^\s]+)',s):
    assert re.fullmatch(r'[^@]+@[0-9a-f]{40}',action),action
assert '4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204' in s
assert '128907415' in s
print('PASS retirement workflow static isolation (not native qualification)')
