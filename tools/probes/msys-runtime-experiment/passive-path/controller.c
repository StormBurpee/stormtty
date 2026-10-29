/* Private diagnostic console owner. Never reads the trace while the child runs.
   Native compile/run and exact internal-path coverage remain separate gates. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define TRACE_SIZE (64u + 16384u * 128u)
static INPUT_RECORD key(WCHAR c) {
 INPUT_RECORD r; ZeroMemory(&r,sizeof r); r.EventType=KEY_EVENT;
 r.Event.KeyEvent.bKeyDown=TRUE;r.Event.KeyEvent.wRepeatCount=1;
 r.Event.KeyEvent.uChar.UnicodeChar=c;return r;
}
static INPUT_RECORD mouse(void) {
 INPUT_RECORD r;ZeroMemory(&r,sizeof r);r.EventType=MOUSE_EVENT;
 r.Event.MouseEvent.dwMousePosition.X=1;r.Event.MouseEvent.dwMousePosition.Y=1;
 r.Event.MouseEvent.dwEventFlags=MOUSE_WHEELED;
 r.Event.MouseEvent.dwButtonState=(DWORD)WHEEL_DELTA<<16;return r;
}
static void u32(unsigned char *p,unsigned offset,uint32_t n) {memcpy(p+offset,&n,4);}
static void u64(unsigned char *p,unsigned offset,uint64_t n) {memcpy(p+offset,&n,8);}
static int safe_arg(const char *p) {
 size_t n=strlen(p);return n && n<4096 && !strchr(p,'"') && !strchr(p,'\r') && !strchr(p,'\n') && p[n-1]!='\\';
}
int main(int argc,char **argv) {
 /* reader, case, reader-log, marker, controller-log, trace; NEW private paths */
 if(argc!=7 || (strcmp(argv[2],"partial-zero") && strcmp(argv[2],"positive-prefix")))return 2;
 for(int i=1;i<argc;++i)if(!safe_arg(argv[i]))return 2;
 FILE *log=NULL; if(fopen_s(&log,argv[5],"wx") || !log)return 3;
 setvbuf(log,NULL,_IONBF,0);
 HANDLE in=INVALID_HANDLE_VALUE,out=INVALID_HANDLE_VALUE,trace=INVALID_HANDLE_VALUE,job=NULL,mapping=NULL;
 unsigned char *view=NULL;void *attributes=NULL;
 PROCESS_INFORMATION pi;ZeroMemory(&pi,sizeof pi);
 STARTUPINFOEXA si;ZeroMemory(&si,sizeof si);si.StartupInfo.cb=sizeof si;
 SECURITY_ATTRIBUTES sec={sizeof sec,NULL,TRUE};
 JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;ZeroMemory(&limits,sizeof limits);
 DWORD error=0,wait=WAIT_FAILED,exitcode=STILL_ACTIVE,written=0,queued=0;
 int result=1,cleanup_ok=1,assigned=0,resumed=0,timed_out=0;
 ULONGLONG start=0;SIZE_T attrsize=0;BOOL saw_empty=FALSE;
 CONSOLE_SCREEN_BUFFER_INFO before,after;SMALL_RECT rect;
 INPUT_RECORD records[3],first=key(L'a');DWORD count=0;
 char cmd[32768];uint64_t generation=1;HANDLE inherited[3];
 if(GetFileAttributesA(argv[4])!=INVALID_FILE_ATTRIBUTES)goto fail;
 FreeConsole();
 if(!AllocConsole())goto fail;
 in=CreateFileA("CONIN$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sec,OPEN_EXISTING,0,NULL);
 out=CreateFileA("CONOUT$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sec,OPEN_EXISTING,0,NULL);
 if(in==INVALID_HANDLE_VALUE || out==INVALID_HANDLE_VALUE)goto fail;
 if(!SetConsoleMode(in,ENABLE_EXTENDED_FLAGS|ENABLE_WINDOW_INPUT|ENABLE_MOUSE_INPUT))goto fail;
 mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,&sec,PAGE_READWRITE,0,TRACE_SIZE,NULL);
 if(!mapping)goto fail;
 view=(unsigned char*)MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,TRACE_SIZE);
 if(!view)goto fail;
 memcpy(view,"STPTH001",8);u32(view,8,1);u32(view,12,64);u32(view,16,128);u32(view,20,16384);
 u32(view,40,1);u64(view,48,generation);
 /* Exclusive trace file is cold setup. No trace bytes are inspected until reap. */
 trace=CreateFileA(argv[6],GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
 if(trace==INVALID_HANDLE_VALUE)goto fail;
 job=CreateJobObjectA(NULL,NULL);
 if(!job)goto fail;
 limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
 if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits))goto fail;
 InitializeProcThreadAttributeList(NULL,1,0,&attrsize);
 attributes=HeapAlloc(GetProcessHeap(),0,attrsize);
 if(!attributes)goto fail;
 si.lpAttributeList=(LPPROC_THREAD_ATTRIBUTE_LIST)attributes;
 if(!InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&attrsize)){si.lpAttributeList=NULL;goto fail;}
 inherited[0]=in;inherited[1]=out;inherited[2]=mapping;
 if(!UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof inherited,NULL,NULL))goto fail;
 si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;si.StartupInfo.hStdInput=in;si.StartupInfo.hStdOutput=out;si.StartupInfo.hStdError=out;
 if(_snprintf_s(cmd,sizeof cmd,_TRUNCATE,"\"%s\" %s \"%s\" \"%s\" %llu %llu",argv[1],argv[2],argv[3],argv[4],(unsigned long long)(uintptr_t)mapping,(unsigned long long)generation)<0)goto fail;
 if(!CreateProcessA(argv[1],cmd,NULL,NULL,TRUE,CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&si.StartupInfo,&pi))goto fail;
 if(!AssignProcessToJobObject(job,pi.hProcess))goto fail;
 assigned=1;
 if(ResumeThread(pi.hThread)==(DWORD)-1)goto fail;
 resumed=1;
 start=GetTickCount64();
 while(GetFileAttributesA(argv[4])==INVALID_FILE_ATTRIBUTES) {
  if(GetTickCount64()-start>=4000 || WaitForSingleObject(pi.hProcess,0)!=WAIT_TIMEOUT)goto fail;
  Sleep(5);
 }
 if(!WriteConsoleInputW(in,&first,1,&written)||written!=1)goto fail;
 start=GetTickCount64();
 do {
  if(!GetNumberOfConsoleInputEvents(in,&queued))goto fail;
  if(!queued){saw_empty=TRUE;break;}Sleep(5);
 }while(GetTickCount64()-start<1500);
 fprintf(log,"{\"first_key_queue_empty\":%s,\"read_entry_proven\":false}\n",saw_empty?"true":"false");
 if(!GetConsoleScreenBufferInfo(out,&before))goto fail;
 rect=before.srWindow;if(rect.Right-rect.Left<3)goto fail;--rect.Right;
 if(!SetConsoleWindowInfo(out,TRUE,&rect)||!GetConsoleScreenBufferInfo(out,&after))goto fail;
 if(!strcmp(argv[2],"positive-prefix"))records[count++]=key(L'b');
 records[count]=mouse();records[count].Event.MouseEvent.dwMousePosition.X=(SHORT)(after.srWindow.Left+1);
 records[count++].Event.MouseEvent.dwMousePosition.Y=(SHORT)(after.srWindow.Top+1);
 records[count++]=key(!strcmp(argv[2],"positive-prefix")?L'c':L'b');
 if(!WriteConsoleInputW(in,records,count,&written)||written!=count)goto fail;
 fprintf(log,"{\"records_written\":%lu,\"width_before\":%d,\"width_after\":%d,\"exact_path\":\"requires-trace-parser\"}\n",(unsigned long)written,before.srWindow.Right-before.srWindow.Left+1,after.srWindow.Right-after.srWindow.Left+1);
 wait=WaitForSingleObject(pi.hProcess,10000);timed_out=wait==WAIT_TIMEOUT;
 if(wait!=WAIT_OBJECT_0)goto fail;
 if(!GetExitCodeProcess(pi.hProcess,&exitcode))goto fail;
 if(!GetNumberOfConsoleInputEvents(in,&queued))goto fail;
 fprintf(log,"{\"remaining_native_records\":%lu}\n",(unsigned long)queued);
 result=exitcode==0 && queued==0?0:1;goto cleanup;
fail:
 error=GetLastError();fprintf(log,"{\"fixture_error\":true,\"win32_error\":%lu}\n",(unsigned long)error);
cleanup:
 /* Stop every owned process, including failure before ResumeThread/assignment. */
 if(pi.hProcess && WaitForSingleObject(pi.hProcess,0)!=WAIT_OBJECT_0) {
  BOOL killed=assigned?TerminateJobObject(job,125):TerminateProcess(pi.hProcess,125);
  if(!killed || WaitForSingleObject(pi.hProcess,2000)!=WAIT_OBJECT_0)cleanup_ok=0;
 }
 if(assigned) {
  JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting;ZeroMemory(&accounting,sizeof accounting);
  if(!TerminateJobObject(job,125))cleanup_ok=0;
  start=GetTickCount64();
  do {
   if(!QueryInformationJobObject(job,JobObjectBasicAccountingInformation,&accounting,sizeof accounting,NULL)){cleanup_ok=0;break;}
   if(!accounting.ActiveProcesses)break;
   Sleep(5);
  }while(GetTickCount64()-start<2000);
  if(accounting.ActiveProcesses)cleanup_ok=0;
 }
 if(pi.hProcess && !GetExitCodeProcess(pi.hProcess,&exitcode))cleanup_ok=0;
 /* Finalized means all owned writers stopped, not that scenario or trace passed. */
 if(view && trace!=INVALID_HANDLE_VALUE) {
  if(cleanup_ok && assigned && resumed && exitcode!=STILL_ACTIVE)u32(view,44,1);
  if(!WriteFile(trace,view,TRACE_SIZE,&written,NULL)||written!=TRACE_SIZE||!FlushFileBuffers(trace))cleanup_ok=0;
 }
 fprintf(log,"{\"timeout\":%s,\"reader_exit\":%lu,\"cleanup_ok\":%s,\"scenario_ok\":%s}\n",timed_out?"true":"false",(unsigned long)exitcode,cleanup_ok?"true":"false",!result&&cleanup_ok?"true":"false");
 if(si.lpAttributeList)DeleteProcThreadAttributeList(si.lpAttributeList);
 if(attributes)HeapFree(GetProcessHeap(),0,attributes);
 if(pi.hThread)CloseHandle(pi.hThread);
 if(pi.hProcess)CloseHandle(pi.hProcess);
 if(job)CloseHandle(job);
 if(view)UnmapViewOfFile(view);
 if(mapping)CloseHandle(mapping);
 if(trace!=INVALID_HANDLE_VALUE)CloseHandle(trace);
 if(in!=INVALID_HANDLE_VALUE)CloseHandle(in);
 if(out!=INVALID_HANDLE_VALUE)CloseHandle(out);
 FreeConsole();fclose(log);return result||!cleanup_ok?1:0;
}
