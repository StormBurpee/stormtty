#!/usr/bin/env python3
"""Generate a diagnostic-only patch against exact official packaging-prepared source.
Never modifies input source. Output is reviewed/applied separately, never installed.
"""
import argparse, difflib, hashlib, json, pathlib, re
PIN = {
 'winsup/cygwin/fhandler/console.cc': 'a7cdbed500c6235e21f41fe9d0be5075a8c471f1',
 'winsup/cygwin/fhandler/termios.cc': '8f01a8b5e7a2890fa811713576b9ca075c696c30',
 'winsup/cygwin/local_includes/fhandler.h': '70c2270fa6b27a03fe802c102f67107d10475c7d',
}
DECL = '''
/* Private opt-in qualification trace; never captures terminal payload. */
void stormtty_resize_trace (int caller, int event, int unit,
                           int ox, int oy, int nx, int ny,
                           int target_pid, int target_pgid);
'''
IMPL = r'''
/* Private diagnostic build only. No handle/cache survives fork or exec.
   Synchronous native append may perturb scheduling: this is NOT qualification.
   Codes: 1 fillin change, 2 master resize event, 3 reader resize event,
   4 resize compare, 5 group signal request, 6 selected target,
   7 excluded native target, 8 signal send attempt, 9 reserved,
   10 same-tty target excluded for different foreground group,
   11 Windows/POSIX process identity (first record, POSIX pid/pgid in targets).
   caller is original-source line number; unknown unit is -1. */
void
stormtty_resize_trace (int caller, int event, int unit,
                      int ox, int oy, int nx, int ny,
                      int target_pid, int target_pgid)
{
#if defined(STORMTTY_MSYS_RESIZE_TRACE) && STORMTTY_MSYS_RESIZE_TRACE == 1
  DWORD saved_error = GetLastError ();
  int saved_errno = errno;
  WCHAR directory[200];
  DWORD n = GetEnvironmentVariableW (L"STORMTTY_MSYS_RESIZE_TRACE_DIR",
                                     directory, 200);
  /* Require an explicit existing drive-absolute local destination. No mkdir,
     UNC/device path, inherited descriptor, terminal write, or implicit fallback. */
  if (n < 3 || n >= 180 || directory[1] != L':'
      || directory[2] != L'\\'
      || !((directory[0] >= L'A' && directory[0] <= L'Z')
           || (directory[0] >= L'a' && directory[0] <= L'z')))
    goto restore;
  {
    WCHAR drive[4] = {directory[0], L':', L'\\', 0};
    if (GetDriveTypeW (drive) != DRIVE_FIXED)
      goto restore;
    /* Reject ambiguous components and every existing reparse ancestor. */
    for (DWORD i = 3; i <= n; ++i)
      {
        if (directory[i] == L'/' || directory[i] == L':')
          goto restore;
        if (directory[i] == L'\\' || directory[i] == 0)
          {
            if (directory[i - 1] == L'.' || directory[i - 1] == L' '
                || directory[i - 1] == L'\\')
              goto restore;
            WCHAR saved = directory[i];
            directory[i] = 0;
            DWORD attr = GetFileAttributesW (directory);
            directory[i] = saved;
            if (attr == INVALID_FILE_ATTRIBUTES
                || !(attr & FILE_ATTRIBUTE_DIRECTORY)
                || (attr & FILE_ATTRIBUTE_REPARSE_POINT))
              goto restore;
          }
      }
    DWORD attributes = GetFileAttributesW (directory);
    if (attributes == INVALID_FILE_ATTRIBUTES
        || !(attributes & FILE_ATTRIBUTE_DIRECTORY)
        || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
      goto restore;
    if (!myself)
      goto restore;
    static LONG NO_COPY count;
    /* Saturating per-process budget; copied DLL used only in disposable bundle. */
    LONG seq;
    do
      {
        seq = InterlockedCompareExchange (&count, 0, 0);
        if (seq >= 8192)
          goto restore;
      }
    while (InterlockedCompareExchange (&count, seq + (seq == 0 ? 2 : 1), seq) != seq);
    WCHAR path[240];
    __small_swprintf (path, L"%W\\resize-%u.log", directory,
                     GetCurrentProcessId ());
    HANDLE file = CreateFileW (path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                              NULL);
    if (file != INVALID_HANDLE_VALUE)
      {
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle (file, &info)
            || info.nNumberOfLinks != 1
            || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT
                                         | FILE_ATTRIBUTE_DIRECTORY)))
          {
            CloseHandle (file);
            goto restore;
          }
        char record[512];
        int len = 0;
        if (seq == 0)
          {
            len = __small_sprintf (record,
              "1 %u %u 0 11 -1 0 0 0 0 %d %d\n",
              GetCurrentProcessId (), GetCurrentThreadId (),
              myself->pid, myself->pgid);
            ++seq;
          }
        len += __small_sprintf (record + len,
          "%d %u %u %d %d %d %d %d %d %d %d %d\n",
          (int) seq + 1, GetCurrentProcessId (), GetCurrentThreadId (),
          caller, event, unit, ox, oy, nx, ny, target_pid, target_pgid);
        DWORD written;
        if (len > 0 && len < (int) sizeof record)
          WriteFile (file, record, len, &written, NULL);
        CloseHandle (file);
      }
  }
restore:
  errno = saved_errno;
  SetLastError (saved_error);
#else
  (void) caller; (void) event; (void) unit;
  (void) ox; (void) oy; (void) nx; (void) ny;
  (void) target_pid; (void) target_pgid;
#endif
}
'''
def blob(data):
 return hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
def generate(source):
 before={p:(source/p).read_bytes() for p in PIN}
 for p,h in PIN.items():
  if blob(before[p]) != h: raise ValueError('Pinned prepared source mismatch: '+p)
 after={p:v.decode() for p,v in before.items()}
 c=after['winsup/cygwin/fhandler/console.cc']
 # Preserve original callsite line numbers in records, even as later edits grow file.
 lines=c.splitlines(keepends=True)
 for i,line in enumerate(lines):
  if 'fillin (' in line and not line.startswith('dev_console::fillin'):
   # All calls in pinned console source have one HANDLE expression.
   line=re.sub(r'((?:con\.)?fillin) \((.*?)\);',lambda m: m[1]+' ('+m[2]+f', {i+1}, '+('unit' if m[1].startswith('con.') else '-1')+');',line)
   # A single fillin in an if condition requires distinct balanced-expression match.
   line=line.replace('con.fillin (get_output_handle ()))', f'con.fillin (get_output_handle (), {i+1}, unit))')
   lines[i]=line
 c=''.join(lines)
 c=c.replace('dev_console::fillin (HANDLE h)\n{', 'dev_console::fillin (HANDLE h, int caller, int trace_unit)\n{\n  SHORT trace_x = dwWinSize.X, trace_y = dwWinSize.Y;')
 marker='  return ret;\n}\n\nvoid\ndev_console::scroll_buffer'
 assert c.count(marker)==1
 c=c.replace(marker,'  if (trace_x != dwWinSize.X || trace_y != dwWinSize.Y)\n    stormtty_resize_trace (caller, 1, trace_unit, trace_x, trace_y,\n                          dwWinSize.X, dwWinSize.Y, 0, 0);\n'+marker)
 c=c.replace('    case WINDOW_BUFFER_SIZE_EVENT:\n', '    case WINDOW_BUFFER_SIZE_EVENT:\n      stormtty_resize_trace (553, 2, p->unit, con.dwWinSize.X, con.dwWinSize.Y,\n                            input_rec[i].Event.WindowBufferSizeEvent.dwSize.X,\n                            input_rec[i].Event.WindowBufferSizeEvent.dwSize.Y, 0, ttyp->pgid);\n',1)
 # Master function uses its local unit (verify no behavior edit).
 c=c.replace('553, 2, p->unit', '553, 2, unit')
 c=c.replace('\tcase WINDOW_BUFFER_SIZE_EVENT:\n', '\tcase WINDOW_BUFFER_SIZE_EVENT:\n          stormtty_resize_trace (1834, 3, unit, con.dwWinSize.X, con.dwWinSize.Y,\n                                input_rec[i].Event.WindowBufferSizeEvent.dwSize.X,\n                                input_rec[i].Event.WindowBufferSizeEvent.dwSize.Y,\n                                0, get_ttyp ()->pgid);\n',1)
 # Both change comparisons (including legacy path) record old/current values.
 c=c.replace('      if (y != con.dwWinSize.Y || x != con.dwWinSize.X)', '      stormtty_resize_trace (557, 4, unit, x, y, con.dwWinSize.X,\n                            con.dwWinSize.Y, 0, ttyp->pgid);\n      if (y != con.dwWinSize.Y || x != con.dwWinSize.X)')
 c=c.replace('\t  if (y != con.dwWinSize.Y || x != con.dwWinSize.X)', '\t  stormtty_resize_trace (516, 4, unit, x, y, con.dwWinSize.X,\n                                con.dwWinSize.Y, 0, ttyp->pgid);\n\t  if (y != con.dwWinSize.Y || x != con.dwWinSize.X)')
 # Dedicated reader send_winch_maybe context, not a broad replacement.
 needle='  if (y != con.dwWinSize.Y || x != con.dwWinSize.X)\n    {'
 assert c.count(needle)==1
 c=c.replace(needle,'  stormtty_resize_trace (1231, 4, unit, x, y, con.dwWinSize.X,\n                        con.dwWinSize.Y, 0, get_ttyp ()->pgid);\n'+needle)
 c=c.replace('#define CONVERT_LIMIT NT_MAX_PATH', IMPL+'\n#define CONVERT_LIMIT NT_MAX_PATH')
 after['winsup/cygwin/fhandler/console.cc']=c
 h=after['winsup/cygwin/local_includes/fhandler.h']
 h=h.replace('  bool fillin (HANDLE);','  bool fillin (HANDLE, int caller = 0, int trace_unit = -1);')
 after['winsup/cygwin/local_includes/fhandler.h']=h+'\n'+DECL
 t=after['winsup/cygwin/fhandler/termios.cc']
 t=t.replace('  target_pgid = target_pgid ?: pgid;', '  target_pgid = target_pgid ?: pgid;\n  if (sig == SIGWINCH)\n    stormtty_resize_trace (136, 5, -1, 0, 0, 0, 0, 0, target_pgid);')
 t=t.replace('      if (!p || !p->exists () || p->ctty != ntty || p->pgid != target_pgid)', '      if (sig == SIGWINCH && p && p->exists () && p->ctty == ntty\n          && p->pgid != target_pgid)\n        stormtty_resize_trace (151, 10, -1, 0, 0, 0, 0, p->pid, p->pgid);\n      if (!p || !p->exists () || p->ctty != ntty || p->pgid != target_pgid)')
 t=t.replace('      if (p->process_state & PID_NOTCYGWIN)\n\tcontinue;', '      if (sig == SIGWINCH)\n        stormtty_resize_trace (155, (p->process_state & PID_NOTCYGWIN) ? 7 : 6,\n                              -1, 0, 0, 0, 0, p->pid, target_pgid);\n      if (p->process_state & PID_NOTCYGWIN)\n\tcontinue;')
 t=t.replace('      else\n\tsig_send (p, si);','      else\n        {\n          if (sig == SIGWINCH)\n            stormtty_resize_trace (163, 8, -1, 0, 0, 0, 0, p->pid, target_pgid);\n          sig_send (p, si);\n        }')
 t=t.replace('  if (killself)\n    sig_send (myself, si);','  if (killself)\n    {\n      if (sig == SIGWINCH)\n        stormtty_resize_trace (166, 8, -1, 0, 0, 0, 0, myself->pid, target_pgid);\n      sig_send (myself, si);\n    }')
 after['winsup/cygwin/fhandler/termios.cc']=t
 patch=''.join(''.join(difflib.unified_diff(before[p].decode().splitlines(True), after[p].splitlines(True),fromfile='a/'+p,tofile='b/'+p)) for p in PIN)
 return patch,after
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);a=p.parse_args()
 patch,after=generate(a.source)
 with a.output.open('x') as f:f.write(patch)
 print(json.dumps({p:hashlib.sha256(s.encode()).hexdigest() for p,s in after.items()},indent=2))
