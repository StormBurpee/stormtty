#!/usr/bin/env python3
"""Generate exact diagnostic-only stock/refined patches; never edits input source."""
import argparse,difflib,hashlib,json,pathlib,re
HERE=pathlib.Path(__file__).resolve().parent
STOCK='535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33'
REFINED='bc68a642dca059ed76b6ae79b21db85e34ff3376f98a5efe4bb2b0136b79f0ae'
def replace(s,old,new,count=1):
 if s.count(old)!=count:raise ValueError('source anchor count differs: '+old[:70])
 return s.replace(old,new)
def instrument(source):
 s=replace(source,'#include "psapi.h"','#include "psapi.h"\n#include "stormtty-passive-path.h"')
 # Common returning read path: no new input/signal/locking decisions.
 a=s.index('void\nfhandler_console::read (');b=s.index('\nfhandler_console::input_states\nfhandler_console::process_input_message',a)
 t=s[a:b]
 t=replace(t,'  size_t copied_chars = 0;','''  size_t copied_chars = 0;
  st_path::read_begin (buflen, !!(get_ttyp ()->ti.c_lflag & ICANON),
                       get_ttyp ()->ti.c_cc[VMIN], get_ttyp ()->ti.c_cc[VTIME],
                       is_nonblocking ());''')
 t=t.replace('return;', 'st_path::read_end (buflen, copied_chars, errno); return;')
 t=replace(t,'''    get_readahead_into_buffer (buf + copied_chars, buflen - copied_chars);''','''    get_readahead_into_buffer (buf + copied_chars, buflen - copied_chars);
  st_path::copy (copied_chars, con_ra.ralen, ::input_ready, buflen - copied_chars);''')
 t=replace(t,'''    goto read_more;''','''    {
      st_path::event (st_path::READ_MORE, copied_chars, buflen,
                       get_ttyp ()->ti.c_cc[VMIN]);
      goto read_more;
    }''')
 t=replace(t,'''  set_sig_errno (EINTR);
  buflen = (size_t) -1;
}''','''  set_sig_errno (EINTR);
  buflen = (size_t) -1;
  st_path::read_end (buflen, copied_chars, errno);
}''')
 s=s[:a]+t+s[b:]
 a=s.index('fhandler_console::process_input_message (');b=s.index('\nvoid\nfhandler_console::discard_key_events',a);t=s[a:b]
 t=replace(t,'  DWORD error = GetLastError ();','''  DWORD error = GetLastError ();
  st_path::peek (r, error, r ? total_read : 0, len, con_ra.ralen, ::input_ready);''')
 t=replace(t,'      switch (input_rec[i].EventType)','''      st_path::dispatch (input_rec[i].EventType, i, total_read, stat,
                          con.use_mouse, con_ra.ralen);
      switch (input_rec[i].EventType)''')
 t=re.sub(r'return (input_error|input_ok|input_empty|input_winch|stat);',r'return st_path::process_result (\1, con_ra.ralen, ::input_ready);',t)
 s=s[:a]+t+s[b:]
 a=s.index('bool\nfhandler_console::send_winch_maybe (');b=s.index('\n/* Check whether a mouse event',a);t=s[a:b]
 t=replace(t,'  con.fillin (get_output_handle ());','''  bool st_path_query_ok = con.fillin (get_output_handle ());
  st_path::geometry (x, y, con.dwWinSize.X, con.dwWinSize.Y, st_path_query_ok);''')
 t=replace(t,'      get_ttyp ()->kill_pgrp (SIGWINCH);','''      st_path::signal_begin (SIGWINCH);
      get_ttyp ()->kill_pgrp (SIGWINCH);
      st_path::signal_end (SIGWINCH);''')
 s=s[:a]+t+s[b:]
 a=s.index('void\nfhandler_console::discard_key_events (');b=s.index('\nbool\ndev_console::fillin',a);t=s[a:b]
 t=replace(t,'  DWORD discarded = 0;','''  const size_t st_path_explicit = n;
  const size_t st_path_before = con.num_processed;
  const size_t st_path_processed = num_input_events_processed;
  DWORD discarded = 0;''')
 t=replace(t,'  INPUT_RECORD input_rec[INREC_SIZE];','''  st_path::retire_begin (st_path_explicit, n, st_path_processed, st_path_before);
  INPUT_RECORD input_rec[INREC_SIZE];''')
 t=replace(t,'''      if (!ReadConsoleInputW (get_handle (), input_rec, n1, &n1) || !n1)''','''      DWORD st_path_requested = n1;
      BOOL st_path_result = ReadConsoleInputW (get_handle (), input_rec, n1, &n1);
      DWORD st_path_error = GetLastError ();
      st_path::retire_api (st_path_requested, st_path_result,
                           st_path_result ? n1 : 0, st_path_error);
      if (!st_path_result || !n1)''')
 t=replace(t,'  con.num_processed -= min (con.num_processed, discarded);','''  con.num_processed -= min (con.num_processed, discarded);
  st_path::end (st_path::RETIRE_END, discarded, n, st_path_before, con.num_processed);''')
 s=s[:a]+t+s[b:]
 # Existing input-mutex wrappers: capture original native return, preserve errors.
 s=replace(s,'  DWORD res = WaitForSingleObject (input_mutex, ms);','  DWORD res = st_path::wait (input_mutex, ms, 1);')
 a=s.index('fhandler_console::__release_input_mutex (');b=s.index('\nDWORD\nfhandler_console::__acquire_output_mutex',a);t=s[a:b]
 t=replace(t,'  ReleaseMutex (input_mutex);','  st_path::release (input_mutex, 1);');s=s[:a]+t+s[b:]
 # Master source observations: same queue APIs/parameters/ordering, no extra read.
 a=s.index('void\nfhandler_console::cons_master_thread (');b=s.index('\n}',a)+2;t=s[a:b]
 t=replace(t,'      DWORD total_read, n, i;', '      DWORD total_read, n, i;\n      st_path::master_event (0, 0, 0, false);')
 t=t.replace('WaitForSingleObject (p->input_mutex, mutex_timeout)','st_path::wait (p->input_mutex, mutex_timeout, 1, 2)')
 t=t.replace('ReleaseMutex (p->input_mutex)','st_path::release (p->input_mutex, 1, 2)')
 old='''      for (i = con.num_processed; i < total_read; i++)
\t{'''
 t=replace(t,old,old+'''
          st_path::master_event (input_rec[i].EventType, i, total_read, false);''')
 old='''\t      ReadConsoleInputW (p->input_handle, input_rec + total_read,
\t\t\t\t min (inrec_size - total_read, inrec_size),
\t\t\t\t &len);'''
 new='''          BOOL st_path_result = ReadConsoleInputW (p->input_handle, input_rec + total_read,
                                 min (inrec_size - total_read, inrec_size), &len);
          DWORD st_path_error = GetLastError ();
          st_path::master_queue (1, min (inrec_size - total_read, inrec_size),
                                  st_path_result, st_path_result ? len : 0,
                                  st_path_error, total_read, con.num_processed);'''
 t=replace(t,old,new)
 old='''\t\t  WriteConsoleInputW (p->input_handle, input_rec + n,
\t\t\t\t      min (total_read - n, inrec_size), &len);'''
 new='''                  BOOL st_path_result = WriteConsoleInputW (p->input_handle, input_rec + n,
                                      min (total_read - n, inrec_size), &len);
                  DWORD st_path_error = GetLastError ();
                  st_path::master_queue (2, min (total_read - n, inrec_size),
                                          st_path_result, st_path_result ? len : 0,
                                          st_path_error, total_read, con.num_processed);'''
 t=replace(t,old,new)
 t=t.replace('ttyp->kill_pgrp (SIGWINCH);','st_path::signal_begin (SIGWINCH); ttyp->kill_pgrp (SIGWINCH); st_path::signal_end (SIGWINCH);')
 # Capture existing geometry queries, not additional queries. Separate scopes already exist.
 t=t.replace('con.fillin (p->output_handle);','bool st_path_query_ok = con.fillin (p->output_handle);\n          st_path::geometry (x, y, con.dwWinSize.X, con.dwWinSize.Y, st_path_query_ok);')
 s=s[:a]+t+s[b:]
 return s

def generate(source,variant):
 digest=hashlib.sha256(source.encode()).hexdigest()
 if digest!={'stock':STOCK,'refined':REFINED}[variant]:raise ValueError('unverified exact source')
 new=instrument(source)
 patch=''.join(difflib.unified_diff(source.splitlines(True),new.splitlines(True),fromfile='a/winsup/cygwin/fhandler/console.cc',tofile='b/winsup/cygwin/fhandler/console.cc'))
 header=(HERE/'recorder.hpp').read_text()
 patch+=''.join(difflib.unified_diff([],header.splitlines(True),fromfile='/dev/null',tofile='b/winsup/cygwin/local_includes/stormtty-passive-path.h'))
 return new,patch
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('source',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);p.add_argument('variant',choices=('stock','refined'));a=p.parse_args()
 new,patch=generate(a.source.read_text(),a.variant);a.output.mkdir(exist_ok=False)
 (a.output/'console.cc').write_text(new);(a.output/'passive.patch').write_text(patch)
 (a.output/'identity.json').write_text(json.dumps({'scope':'passive-diagnostic-only','variant':a.variant,'source_sha256':hashlib.sha256(a.source.read_bytes()).hexdigest(),'instrumented_sha256':hashlib.sha256(new.encode()).hexdigest(),'patch_sha256':hashlib.sha256(patch.encode()).hexdigest(),'header_sha256':hashlib.sha256((HERE/'recorder.hpp').read_bytes()).hexdigest(),'native_qualified':False},indent=2)+'\n')
