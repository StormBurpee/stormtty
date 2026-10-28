#!/usr/bin/env python3
"""Generate the isolated modern-console cursor candidate from exact official source."""
import argparse, difflib, hashlib, pathlib
BEFORE = '535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33'
OLD = '''fhandler_console::set_cursor_maybe ()
{
  con.fillin (get_output_handle ());
  /* Nothing to do for xterm compatible mode. */
  if (wincap.has_con_24bit_colors () && !con_is_legacy)
    return;
'''
NEW = '''fhandler_console::set_cursor_maybe ()
{
  /* Nothing to do for xterm compatible mode.  Do not refresh the shared
     resize baseline on this no-op path before send_winch_maybe sees it. */
  if (wincap.has_con_24bit_colors () && !con_is_legacy)
    return;
  con.fillin (get_output_handle ());
'''
def transform(data):
    if hashlib.sha256(data).hexdigest() != BEFORE:
        raise ValueError('Exact official prepared console source required')
    text = data.decode('utf-8')
    if text.count(OLD) != 1:
        raise ValueError('Unique cursor function context required')
    return text.replace(OLD, NEW).encode('utf-8')
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('console',type=pathlib.Path); p.add_argument('output',type=pathlib.Path)
    a=p.parse_args(); before=a.console.read_bytes(); after=transform(before)
    diff=''.join(difflib.unified_diff(before.decode().splitlines(True),after.decode().splitlines(True),fromfile='a/winsup/cygwin/fhandler/console.cc',tofile='b/winsup/cygwin/fhandler/console.cc'))
    a.output.write_bytes(diff.encode())
    print('candidate_console_sha256='+hashlib.sha256(after).hexdigest())
if __name__=='__main__': main()
