#!/usr/bin/env python3
"""Exact source/patch checks only; not native race or runtime qualification."""
import argparse,hashlib,importlib.util,pathlib,subprocess,tempfile
HERE=pathlib.Path(__file__).resolve().parent
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
cursor=module('cursor',HERE/'make-cursor-patch.py');retire=module('retire',HERE/'make-retirement-patch.py');stage=module('stage',HERE/'stage-retirement.py')
p=argparse.ArgumentParser();p.add_argument('--source',required=True,type=pathlib.Path);a=p.parse_args();before=(a.source/'winsup/cygwin/fhandler/console.cc').read_bytes()
baseline=cursor.transform(before);combined=retire.transform(baseline)
assert hashlib.sha256(before).hexdigest()==stage.BEFORE_SHA256
assert hashlib.sha256(baseline).hexdigest()==stage.CURSOR_SHA256
assert hashlib.sha256(combined).hexdigest()==stage.AFTER_SHA256
assert combined.replace(retire.NEW.encode(),retire.OLD.encode())==baseline
assert baseline.replace(cursor.NEW.encode(),cursor.OLD.encode())==before
assert b'STORMTTY_MSYS_RESIZE_TRACE' not in combined
for invalid in (before,combined,baseline+b'\n'):
 try:retire.transform(invalid);raise AssertionError('unexpected source accepted')
 except ValueError:pass
with tempfile.TemporaryDirectory() as tmp:
 root=pathlib.Path(tmp);f=root/'winsup/cygwin/fhandler/console.cc';f.parent.mkdir(parents=True);f.write_bytes(before)
 for patch in ('0003-modern-cursor-fill.patch','0004-retirement-after-cursor.patch'):
  subprocess.run(['git','apply','--check',str(HERE/patch)],cwd=root,check=True)
  subprocess.run(['git','apply',str(HERE/patch)],cwd=root,check=True)
 assert f.read_bytes()==combined
 baselinefile=root/'baseline.cc';baselinefile.write_bytes(baseline);out=root/'generated.patch'
 subprocess.run(['python3',str(HERE/'make-retirement-patch.py'),str(baselinefile),str(out)],check=True)
 assert out.read_bytes()==(HERE/'0004-retirement-after-cursor.patch').read_bytes()
assert stage.COMPILER_MODE['baseline']==stage.COMPILER_MODE['combined']
for variant in ('baseline','combined'):
 for f in ('termios.cc','fhandler.h'):assert stage.SOURCE_FILES_SHA256[variant][f]==stage.ORIGINAL_SOURCE_FILES_SHA256[f]
print('PASS exact two-patch composition, deterministic generation, rejected wrong ordering/source, unchanged header/termios/compiler; NOT native qualification')
