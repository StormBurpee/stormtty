#!/usr/bin/env python3
"""Match SDK knL frame shape and reject non-frame diagnostic/private text."""
import ast
from pathlib import Path
import re

source = Path(__file__).with_name('diagnose-renderer-teardown.py').read_text()
module = ast.parse(source)
patterns = {}
for node in ast.walk(module):
    if isinstance(node, ast.Assign) and isinstance(node.value, ast.Call) and isinstance(node.value.func, ast.Attribute) and node.value.func.attr == 'match':
        patterns[node.targets[0].id] = ast.literal_eval(node.value.args[0])
assert '~* knL' in source
for line, symbol in [
    ('00 000000c0`4ebfe748 00007ff9`1011f91e ntdll!NtWaitForSingleObject+0x14', 'ntdll!NtWaitForSingleObject+0x14'),
    ('01 000000c0`4ebfe750 00007ff9`1011f920 renderer_smoke!Renderer::Impl::~Impl+0x35 [D:\\private\\file.cpp @ 512]', 'renderer_smoke!Renderer::Impl::~Impl+0x35'),
]:
    match = re.match(patterns['frame'], line, re.I)
    assert match and match[2] == symbol
for line in ['CommandLine: secret token', 'Environment=SECRET', 'ModLoad: C:\\private\\path', 'arbitrary memory string']:
    assert not any(re.match(pattern, line, re.I) for pattern in patterns.values())
assert re.match(patterns['thread'], '.  0  Id: 123.456 Suspend: 1', re.I)[1] == '0'
print('PASS CDB numbered frame extraction and private-text exclusion')
