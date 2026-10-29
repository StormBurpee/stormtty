#!/usr/bin/env python3
"""Fail closed on the fixed diagnostic reader's imported DLL names."""
import pathlib,re,sys

def check(text):
    imports = {s.lower() for s in re.findall(r'DLL Name:\s*(\S+)', text)}
    allowed = {'msys-2.0.dll','kernel32.dll','ntdll.dll'}
    if 'msys-2.0.dll' not in imports or not imports <= allowed:
        raise ValueError('unexpected diagnostic reader imports: '+','.join(sorted(imports)))
    return imports
if __name__ == '__main__':
    check(pathlib.Path(sys.argv[1]).read_text(encoding='utf-8'))
