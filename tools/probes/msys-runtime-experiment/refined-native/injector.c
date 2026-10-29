/* Win32-only owned-console fixture. Not a ConPTY or MSYS runtime instrument. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
static INPUT_RECORD key(WCHAR c) {
  INPUT_RECORD r; ZeroMemory(&r,sizeof r); r.EventType=KEY_EVENT;
  r.Event.KeyEvent.bKeyDown=TRUE; r.Event.KeyEvent.wRepeatCount=1;
  r.Event.KeyEvent.uChar.UnicodeChar=c; return r;
}
static INPUT_RECORD mouse(void) {
  INPUT_RECORD r; ZeroMemory(&r,sizeof r); r.EventType=MOUSE_EVENT;
  r.Event.MouseEvent.dwMousePosition.X=1; r.Event.MouseEvent.dwMousePosition.Y=1;
  r.Event.MouseEvent.dwEventFlags=MOUSE_WHEELED;
  r.Event.MouseEvent.dwButtonState=(DWORD)WHEEL_DELTA<<16; return r;
}
static int unsafe(const char *s) { return strchr(s,'"')!=NULL || strchr(s,'\n')!=NULL; }
int main(int argc,char **argv) {
  /* absolute reader.exe, scenario, reader-log, marker, injector-log */
  if(argc!=6) return 2;
  for(int i=1;i<argc;++i) if(unsafe(argv[i])) return 2;
  FILE *log=NULL; if(fopen_s(&log,argv[5],"wx") || !log) return 3;
  setvbuf(log,NULL,_IONBF,0);
  HANDLE in=INVALID_HANDLE_VALUE,out=INVALID_HANDLE_VALUE,job=NULL;
  PROCESS_INFORMATION pi; ZeroMemory(&pi,sizeof pi); int result=1; DWORD error=0;
  FreeConsole();
  if(!AllocConsole()) goto fail;
  SECURITY_ATTRIBUTES sec={sizeof sec,NULL,TRUE};
  in=CreateFileA("CONIN$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sec,OPEN_EXISTING,0,NULL);
  out=CreateFileA("CONOUT$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sec,OPEN_EXISTING,0,NULL);
  if(in==INVALID_HANDLE_VALUE || out==INVALID_HANDLE_VALUE) goto fail;
  if(!SetConsoleMode(in,ENABLE_EXTENDED_FLAGS|ENABLE_WINDOW_INPUT|ENABLE_MOUSE_INPUT)) goto fail;
  job=CreateJobObjectA(NULL,NULL); if(!job) goto fail;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits; ZeroMemory(&limits,sizeof limits);
  limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits)) goto fail;
  STARTUPINFOA si; ZeroMemory(&si,sizeof si); si.cb=sizeof si;
  si.dwFlags=STARTF_USESTDHANDLES; si.hStdInput=in; si.hStdOutput=out; si.hStdError=out;
  char cmd[32768];
  if(_snprintf_s(cmd,sizeof cmd,_TRUNCATE,"\"%s\" %s \"%s\" \"%s\"",argv[1],argv[2],argv[3],argv[4])<0) goto fail;
  if(!CreateProcessA(argv[1],cmd,NULL,NULL,TRUE,CREATE_SUSPENDED,NULL,NULL,&si,&pi)) goto fail;
  if(!AssignProcessToJobObject(job,pi.hProcess)) goto fail;
  if(ResumeThread(pi.hThread)==(DWORD)-1) goto fail;
  ULONGLONG start=GetTickCount64();
  while(GetFileAttributesA(argv[4])==INVALID_FILE_ATTRIBUTES) {
    if(GetTickCount64()-start>4000 || WaitForSingleObject(pi.hProcess,0)==WAIT_OBJECT_0) goto fail;
    Sleep(5);
  }
  fprintf(log,"{\"reader_configured\":true,\"read_entry_observed\":false}\n");
  DWORD written=0,queued=0; INPUT_RECORD a=key(L'a');
  if(!WriteConsoleInputW(in,&a,1,&written) || written!=1) goto fail;
  start=GetTickCount64(); BOOL saw_empty=FALSE;
  do {
    if(!GetNumberOfConsoleInputEvents(in,&queued)) goto fail;
    if(!queued) { saw_empty=TRUE; break; }
    Sleep(5);
  } while(GetTickCount64()-start<1500);
  fprintf(log,"{\"first_key_queue_empty\":%s,\"wait_ms\":%llu,\"copied_chars_observed\":false}\n",saw_empty?"true":"false",(unsigned long long)(GetTickCount64()-start));
  CONSOLE_SCREEN_BUFFER_INFO before,after;
  if(!GetConsoleScreenBufferInfo(out,&before)) goto fail;
  SMALL_RECT rect=before.srWindow;
  if(rect.Right-rect.Left<3) goto fail;
  --rect.Right;
  if(!SetConsoleWindowInfo(out,TRUE,&rect)) goto fail;
  if(!GetConsoleScreenBufferInfo(out,&after)) goto fail;
  fprintf(log,"{\"width_before\":%d,\"width_after\":%d,\"height_before\":%d,\"height_after\":%d}\n",before.srWindow.Right-before.srWindow.Left+1,after.srWindow.Right-after.srWindow.Left+1,before.srWindow.Bottom-before.srWindow.Top+1,after.srWindow.Bottom-after.srWindow.Top+1);
  INPUT_RECORD records[5]; DWORD count=0;
  if(!strcmp(argv[2],"surrogate")) records[count++]=key(0xd83d);
  records[count]=mouse();
  records[count].Event.MouseEvent.dwMousePosition.X=(SHORT)(after.srWindow.Left+1);
  records[count++].Event.MouseEvent.dwMousePosition.Y=(SHORT)(after.srWindow.Top+1);
  if(!strcmp(argv[2],"surrogate")) records[count++]=key(0xde00);
  records[count++]=key(L'b');
  if(!strcmp(argv[2],"canonical") || !strcmp(argv[2],"longjmp")) records[count++]=key(L'\n');
  if(!WriteConsoleInputW(in,records,count,&written) || written!=count) goto fail;
  fprintf(log,"{\"batch_records_requested\":%lu,\"batch_records_written\":%lu,\"exact_mouse_path\":\"unobserved\"}\n",(unsigned long)count,(unsigned long)written);
  DWORD wait=WaitForSingleObject(pi.hProcess,10000),exitcode=0;
  if(wait==WAIT_TIMEOUT) { TerminateJobObject(job,124); WaitForSingleObject(pi.hProcess,2000); }
  if(!GetExitCodeProcess(pi.hProcess,&exitcode)) goto fail;
  if(!GetNumberOfConsoleInputEvents(in,&queued)) goto fail;
  fprintf(log,"{\"timeout\":%s,\"reader_exit\":%lu,\"remaining_native_records\":%lu}\n",wait==WAIT_TIMEOUT?"true":"false",(unsigned long)exitcode,(unsigned long)queued);
  result=(wait==WAIT_OBJECT_0 && exitcode==0)?0:1;
  goto cleanup;
fail:
  error=GetLastError(); fprintf(log,"{\"fixture_error\":true,\"win32_error\":%lu}\n",(unsigned long)error);
cleanup:
  if(pi.hProcess && WaitForSingleObject(pi.hProcess,0)==WAIT_TIMEOUT) { TerminateProcess(pi.hProcess,125); WaitForSingleObject(pi.hProcess,2000); }
  if(pi.hThread) CloseHandle(pi.hThread);
  if(pi.hProcess) CloseHandle(pi.hProcess);
  if(job) CloseHandle(job);
  if(in!=INVALID_HANDLE_VALUE) CloseHandle(in);
  if(out!=INVALID_HANDLE_VALUE) CloseHandle(out);
  FreeConsole(); fclose(log); return result;
}
