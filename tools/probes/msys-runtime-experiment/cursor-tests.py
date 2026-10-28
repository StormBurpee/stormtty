#!/usr/bin/env python3
"""Exact candidate patch and portable cursor-path model; NOT native MSYS qualification."""
import argparse, hashlib, importlib.util, pathlib, subprocess, tempfile
HERE=pathlib.Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('cursor_generator',HERE/'make-cursor-patch.py')
gen=importlib.util.module_from_spec(spec);spec.loader.exec_module(gen)
spec=importlib.util.spec_from_file_location('cursor_stage',HERE/'stage-cursor.py')
stage=importlib.util.module_from_spec(spec);spec.loader.exec_module(stage)
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',required=True,type=pathlib.Path);args=p.parse_args()
source=args.source/'winsup/cygwin/fhandler/console.cc';before=source.read_bytes();after=gen.transform(before)
assert hashlib.sha256(after).hexdigest()==stage.AFTER_SHA256
assert after.count(b'con.fillin (get_output_handle ());')==before.count(b'con.fillin (get_output_handle ());')
assert after.replace(gen.NEW.encode(),gen.OLD.encode())==before
assert b'STORMTTY_MSYS_RESIZE_TRACE' not in after
for suffix in ('\n','\r\n'):
 try: gen.transform(before+suffix.encode());raise AssertionError('changed source accepted')
 except ValueError: pass
with tempfile.TemporaryDirectory() as tmp:
 root=pathlib.Path(tmp); target=root/'winsup/cygwin/fhandler/console.cc';target.parent.mkdir(parents=True);target.write_bytes(before)
 out=root/'candidate.patch';subprocess.run(['python3',str(HERE/'make-cursor-patch.py'),str(source),str(out)],check=True)
 assert out.read_bytes()==(HERE/'0003-modern-cursor-fill.patch').read_bytes()
 subprocess.run(['git','apply','--check',str(out)],cwd=root,check=True)
 subprocess.run(['git','apply',str(out)],cwd=root,check=True)
 assert target.read_bytes()==after
 text=after.decode();start=text.index('void\nfhandler_console::set_cursor_maybe ()');end=text.index('\n/* Workaround',start)
 body=text[start:end]
 stub=r'''
#include <cassert>
using DWORD=unsigned;
struct Coord {int X=0,Y=0;};
struct Console {int fills=0; unsigned owner=1; Coord dwLastCursorPosition;struct {Coord dwCursorPosition;} b;void fillin(int){++fills;}};
struct Caps {bool modern=false;bool has_con_24bit_colors(){return modern;}} wincap;
static int cursor_calls=0,locks=0,attaches=0;
void SetConsoleCursorPosition(int,Coord){++cursor_calls;}
struct fhandler_console {Console con;bool con_is_legacy=false;int mutex_timeout=0;int get_output_handle(){return 1;}void acquire_attach_mutex(int){++locks;}DWORD attach_console(DWORD){++attaches;return 1;}void detach_console(DWORD,DWORD){--attaches;}void release_attach_mutex(){--locks;}void set_cursor_maybe();};
'''
 main=r'''
int main(){for(bool modern:{false,true})for(bool legacy:{false,true})for(bool moved:{false,true}){fhandler_console f;wincap.modern=modern;f.con_is_legacy=legacy;f.con.b.dwCursorPosition.X=moved;cursor_calls=0;f.set_cursor_maybe();bool noop=modern&&!legacy;assert(f.con.fills==(noop?0:1));assert(cursor_calls==(!noop&&moved?1:0));assert(locks==0&&attaches==0);}}
'''
 cpp=root/'test.cpp';cpp.write_text('#include <initializer_list>\n'+stub+body+main)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(root/'test')],check=True)
 subprocess.run([str(root/'test')],check=True)
for variant in ('control','candidate'):
 assert stage.COMPILER_MODE[variant]==stage.COMPILER_MODE['control']
 for f in ('termios.cc','fhandler.h'):assert stage.SOURCE_FILES_SHA256[variant][f]==stage.SOURCE_FILES_SHA256['control'][f]
print('PASS exact cursor-only patch, deterministic generation, unchanged other source, 8 mocked mode/cursor combinations; native ABI/runtime untested')
