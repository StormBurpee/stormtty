#!/usr/bin/env python3
"""Exact experimental retirement delta AFTER the modern cursor candidate."""
import argparse,difflib,hashlib,pathlib
BEFORE='25a3fb9355235614819f4c56e2a60c8ecb3b4bd4cd49a6befa7ff56ab03cc0b3'
OLD='''\tcase WINDOW_BUFFER_SIZE_EVENT:
\t  if (send_winch_maybe ())
\t    {
\t      stat = input_winch;
\t      goto out;
\t    }
\t  /* fall through */
\tdefault:
'''
NEW='''\tcase WINDOW_BUFFER_SIZE_EVENT:
\t  /* Retire this peek prefix before send_winch_maybe can release
\t     input_mutex, avoiding a second discard after that release.
\t     An earlier mouse event can already have released the mutex;
\t     this change does not validate that older peek snapshot or
\t     guarantee complete retirement if discard_key_events fails. */
\t  discard_key_events (i + 1);
\t  return send_winch_maybe () ? input_winch : stat;
\tdefault:
'''
def transform(data):
    if hashlib.sha256(data).hexdigest()!=BEFORE:raise ValueError('Exact cursor-only source required')
    text=data.decode()
    if text.count(OLD)!=1:raise ValueError('Unique resize branch required')
    return text.replace(OLD,NEW).encode()
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('console',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);a=p.parse_args()
    before=a.console.read_bytes();after=transform(before)
    a.output.write_bytes(''.join(difflib.unified_diff(before.decode().splitlines(True),after.decode().splitlines(True),fromfile='a/winsup/cygwin/fhandler/console.cc',tofile='b/winsup/cygwin/fhandler/console.cc')).encode())
    print('combined_console_sha256='+hashlib.sha256(after).hexdigest())
if __name__=='__main__':main()
