/* Dedicated private-console controller. It never reads the live observer or
   fixture mapping. Every child is job-owned before its first instruction. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cases.h"
#include "../controller-protocol.h"
static INPUT_RECORD st_key(WCHAR value){INPUT_RECORD r;ZeroMemory(&r,sizeof r);r.EventType=KEY_EVENT;r.Event.KeyEvent.bKeyDown=TRUE;r.Event.KeyEvent.wRepeatCount=1;r.Event.KeyEvent.uChar.UnicodeChar=value;return r;}
static void st_u32(unsigned char *p,unsigned offset,uint32_t value){memcpy(p+offset,&value,4);}
static void st_u64(unsigned char *p,unsigned offset,uint64_t value){memcpy(p+offset,&value,8);}
static int st_safe(const char *s){size_t n=strlen(s);return n&&n<4096&&!strchr(s,'"')&&!strchr(s,'\r')&&!strchr(s,'\n')&&s[n-1]!='\\';}
int main(int argc,char **argv){
 /* READER CASE TRACE RESULT GENERATION. Paths must be new private files. */
 if(argc!=6)return 2;
 for(int i=1;i<argc;++i)if(!st_safe(argv[i]))return 2;
 const st_case *c=st_find_case(argv[2]);uint64_t generation;
 if(!c||!st_decimal(argv[5],&generation))return 2;
 FILE *log=NULL;if(fopen_s(&log,argv[4],"wx")||!log)return 3;
 HANDLE input=INVALID_HANDLE_VALUE,output=INVALID_HANDLE_VALUE,trace_file=INVALID_HANDLE_VALUE;
 HANDLE trace_map=NULL,result_map=NULL,ready=NULL,job=NULL,port=NULL;
 unsigned char *trace_view=NULL;st_result *observed=NULL;void *attrs=NULL;
 st_fixture_events events;ZeroMemory(&events,sizeof events);
 st_fixture_gate_result gates[3];ZeroMemory(gates,sizeof gates);unsigned gate_count=0;
 STARTUPINFOEXA si;ZeroMemory(&si,sizeof si);si.StartupInfo.cb=sizeof si;
 PROCESS_INFORMATION pi;ZeroMemory(&pi,sizeof pi);
 SECURITY_ATTRIBUTES sa={sizeof sa,NULL,TRUE};SIZE_T attr_size=0;
 DWORD error=0,accepted=0,queue_before=0,queue_after=0,exit_code=STILL_ACTIVE;
 DWORD initial_input_mode=0,initial_output_mode=0;
 int success=0,assigned=0,resumed=0,reaped=0,timed_out=0,cleanup_errors=0,queue_after_valid=0,all_writers_stopped=0,trace_written=0;
 unsigned input_count=0;INPUT_RECORD records[3];ZeroMemory(records,sizeof records);
 CONSOLE_SCREEN_BUFFER_INFO geometry;ZeroMemory(&geometry,sizeof geometry);
 char command[32768];HANDLE inherited[8],creation_jobs[1];ULONGLONG cleanup_deadline;
 JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;ZeroMemory(&limits,sizeof limits);
 JOBOBJECT_ASSOCIATE_COMPLETION_PORT association;ZeroMemory(&association,sizeof association);
 FreeConsole(); /* Detach only this dedicated controller before allocation. */
 if(!AllocConsole())goto fail;
 input=CreateFileA("CONIN$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,NULL);
 output=CreateFileA("CONOUT$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,NULL);
 if(input==INVALID_HANDLE_VALUE||output==INVALID_HANDLE_VALUE)goto fail;
 if(!SetConsoleMode(input,ENABLE_EXTENDED_FLAGS|ENABLE_WINDOW_INPUT|ENABLE_MOUSE_INPUT)||
    !GetConsoleMode(input,&initial_input_mode)||!GetConsoleMode(output,&initial_output_mode))goto fail;
 trace_map=CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,ST_TRACE_SIZE,NULL);
 result_map=CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,sizeof(st_result),NULL);
 ready=CreateEventW(&sa,FALSE,FALSE,NULL);
 if(!trace_map||!result_map||!ready||!st_fixture_create_events(&events))goto fail;
 trace_view=(unsigned char*)MapViewOfFile(trace_map,FILE_MAP_READ|FILE_MAP_WRITE,0,0,ST_TRACE_SIZE);
 observed=(st_result*)MapViewOfFile(result_map,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(st_result));
 if(!trace_view||!observed)goto fail;
 memcpy(trace_view,"STCMB002",8);st_u32(trace_view,8,2);st_u32(trace_view,12,64);st_u32(trace_view,16,128);st_u32(trace_view,20,16384);st_u32(trace_view,40,1);st_u64(trace_view,48,generation);
 trace_file=CreateFileA(argv[3],GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
 if(trace_file==INVALID_HANDLE_VALUE)goto fail;
 job=CreateJobObjectW(NULL,NULL);port=CreateIoCompletionPort(INVALID_HANDLE_VALUE,NULL,0,1);
 if(!job||!port)goto fail;
 limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
 association.CompletionKey=job;association.CompletionPort=port;
 if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits)||
    !SetInformationJobObject(job,JobObjectAssociateCompletionPortInformation,&association,sizeof association))goto fail;
 InitializeProcThreadAttributeList(NULL,2,0,&attr_size);attrs=HeapAlloc(GetProcessHeap(),0,attr_size);
 if(!attrs)goto fail;
 si.lpAttributeList=(LPPROC_THREAD_ATTRIBUTE_LIST)attrs;
 if(!InitializeProcThreadAttributeList(si.lpAttributeList,2,0,&attr_size)){si.lpAttributeList=NULL;goto fail;}
 inherited[0]=input;inherited[1]=output;inherited[2]=trace_map;inherited[3]=result_map;inherited[4]=ready;
 inherited[5]=events.arrived;inherited[6]=events.resume;inherited[7]=events.wait_notice;
 if(!UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof inherited,NULL,NULL))goto fail;
 creation_jobs[0]=job;
 if(!UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,creation_jobs,sizeof creation_jobs,NULL,NULL))goto fail;
 si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;si.StartupInfo.hStdInput=input;si.StartupInfo.hStdOutput=output;si.StartupInfo.hStdError=output;
 if(_snprintf_s(command,sizeof command,_TRUNCATE,"\"%s\" %s %llu %llu %llu %llu %llu %llu %llu",argv[1],c->name,
    (unsigned long long)(uintptr_t)trace_map,(unsigned long long)(uintptr_t)result_map,(unsigned long long)(uintptr_t)ready,
    (unsigned long long)(uintptr_t)events.arrived,(unsigned long long)(uintptr_t)events.resume,(unsigned long long)(uintptr_t)events.wait_notice,
    (unsigned long long)generation)<0)goto fail;
 /* Windows 10+ atomic job assignment closes cancellation between creation
    and user-space bookkeeping. There is no unowned-child fallback. Suspended
    creation is cold setup, never suspension of a running runtime/master. */
 if(!CreateProcessA(argv[1],command,NULL,NULL,TRUE,CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&si.StartupInfo,&pi))goto fail;
 assigned=1; /* CreateProcess succeeded with the mandatory job-list attribute. */
 if(ResumeThread(pi.hThread)==(DWORD)-1)goto fail;
 resumed=1;
 {HANDLE waits[2]={ready,pi.hProcess};DWORD wait=WaitForMultipleObjects(2,waits,FALSE,4000);if(wait!=WAIT_OBJECT_0){timed_out=wait==WAIT_TIMEOUT;goto fail;}}
 /* No queue draining or rewriting. Ambient records invalidate this exposure. */
 if(!GetNumberOfConsoleInputEvents(input,&queue_before)||queue_before||!GetConsoleScreenBufferInfo(output,&geometry))goto fail;
 if(c->event==0){records[input_count++]=st_key(L'a');records[input_count++]=st_key(L'b');if(c->canonical)records[input_count++]=st_key(L'\r');}
 else {
  if(c->gate_index)records[input_count++]=st_key(L'a');
  INPUT_RECORD *event=&records[input_count++];
  if(c->event==1){event->EventType=MOUSE_EVENT;event->Event.MouseEvent.dwMousePosition.X=(SHORT)(geometry.srWindow.Left+1);event->Event.MouseEvent.dwMousePosition.Y=(SHORT)(geometry.srWindow.Top+1);event->Event.MouseEvent.dwEventFlags=MOUSE_WHEELED;event->Event.MouseEvent.dwButtonState=(DWORD)WHEEL_DELTA<<16;}
  else if(c->event==2){event->EventType=FOCUS_EVENT;event->Event.FocusEvent.bSetFocus=TRUE;}
  else {event->EventType=WINDOW_BUFFER_SIZE_EVENT;event->Event.WindowBufferSizeEvent.dwSize=geometry.dwSize;}
  records[input_count++]=st_key(L'b');
 }
 if(!WriteConsoleInputW(input,records,input_count,&accepted)||accepted!=input_count)goto fail;
 for(unsigned i=0;i<c->gates;++i){int delta=i==1?1:-1;gate_count=i+1;if(!st_fixture_service_gate(pi.hProcess,output,&events,delta,&gates[i])){timed_out=gates[i].wait_result==WAIT_TIMEOUT;goto fail;}}
 {DWORD wait=WaitForSingleObject(pi.hProcess,10000);timed_out=wait==WAIT_TIMEOUT;if(wait!=WAIT_OBJECT_0)goto fail;reaped=1;}
 if(!GetExitCodeProcess(pi.hProcess,&exit_code)||!GetNumberOfConsoleInputEvents(input,&queue_after))goto fail;
 queue_after_valid=1;success=exit_code==0&&queue_after==0;goto cleanup;
fail:
 error=GetLastError();
cleanup:
 if(pi.hProcess&&WaitForSingleObject(pi.hProcess,0)!=WAIT_OBJECT_0){
  if(!TerminateJobObject(job,125))++cleanup_errors;
  if(WaitForSingleObject(pi.hProcess,2000)!=WAIT_OBJECT_0)++cleanup_errors;else reaped=1;
 }else if(pi.hProcess)reaped=1;
 if(assigned){
  if(!TerminateJobObject(job,125))++cleanup_errors;
  cleanup_deadline=GetTickCount64()+2000;
  for(;;){
   JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting;ZeroMemory(&accounting,sizeof accounting);
   if(!QueryInformationJobObject(job,JobObjectBasicAccountingInformation,&accounting,sizeof accounting,NULL)){++cleanup_errors;break;}
   if(!accounting.ActiveProcesses){all_writers_stopped=1;break;}
   ULONGLONG now=GetTickCount64();if(now>=cleanup_deadline){++cleanup_errors;break;}
   DWORD message=0;ULONG_PTR key=0;LPOVERLAPPED overlapped=NULL;
   if(!GetQueuedCompletionStatus(port,&message,&key,&overlapped,(DWORD)(cleanup_deadline-now))&&GetLastError()!=WAIT_TIMEOUT){++cleanup_errors;break;}
  }
 }
 if(pi.hProcess&&!GetExitCodeProcess(pi.hProcess,&exit_code))++cleanup_errors;
 /* All child writers stopped before either mapping is inspected. */
 if(trace_view&&trace_file!=INVALID_HANDLE_VALUE&&all_writers_stopped&&reaped){
  DWORD bytes=0;if(!cleanup_errors&&assigned&&resumed&&reaped&&exit_code!=STILL_ACTIVE)st_u32(trace_view,44,1);
  if(!WriteFile(trace_file,trace_view,ST_TRACE_SIZE,&bytes,NULL)||bytes!=ST_TRACE_SIZE||!FlushFileBuffers(trace_file))++cleanup_errors;else trace_written=1;
 }
 if(!all_writers_stopped||!reaped||!observed||observed->magic!=ST_RESULT_MAGIC||
    observed->configured!=1||observed->finished!=1||observed->result||!observed->termios_restored||
    observed->read_count>ST_MAX_READS||observed->output_count!=strlen(c->expected)||
    observed->output_count>ST_MAX_OUTPUT||memcmp(observed->output,c->expected,observed->output_count))success=0;
 fprintf(log,"{\"schema\":2,\"stage\":\"console-records\",\"case\":\"%s\",\"generation\":%llu,\"input_batches\":[{\"requested\":%u,\"accepted\":%lu,\"records\":[",c->name,(unsigned long long)generation,input_count,(unsigned long)accepted);
 for(unsigned i=0;i<input_count;++i){INPUT_RECORD *r=&records[i];if(i)fputc(',',log);fprintf(log,"{\"type\":%u",r->EventType);
  if(r->EventType==KEY_EVENT)fprintf(log,",\"unicode\":%u,\"down\":true,\"repeat\":1,\"virtual_key\":%u,\"scan_code\":%u,\"controls\":%lu",(unsigned)r->Event.KeyEvent.uChar.UnicodeChar,(unsigned)r->Event.KeyEvent.wVirtualKeyCode,(unsigned)r->Event.KeyEvent.wVirtualScanCode,(unsigned long)r->Event.KeyEvent.dwControlKeyState);
  else if(r->EventType==MOUSE_EVENT)fprintf(log,",\"x\":%d,\"y\":%d,\"flags\":%lu,\"buttons\":%lu,\"controls\":%lu",r->Event.MouseEvent.dwMousePosition.X,r->Event.MouseEvent.dwMousePosition.Y,(unsigned long)r->Event.MouseEvent.dwEventFlags,(unsigned long)r->Event.MouseEvent.dwButtonState,(unsigned long)r->Event.MouseEvent.dwControlKeyState);
  else if(r->EventType==FOCUS_EVENT)fputs(",\"focused\":true",log);
  else fprintf(log,",\"width\":%d,\"height\":%d",r->Event.WindowBufferSizeEvent.dwSize.X,r->Event.WindowBufferSizeEvent.dwSize.Y);
  fputc('}',log);
 }
 fprintf(log,"]}],\"queue_before\":%lu,\"queue_remaining\":%lu,\"queue_remaining_valid\":%s,\"child_exit\":%lu,\"timed_out\":%s,\"reaped\":%s,\"assigned\":%s,\"resumed\":%s,\"cleanup_errors\":%d,\"fixture_error\":%lu,\"initial_input_mode\":%lu,\"initial_output_mode\":%lu,\"gates\":[",
 (unsigned long)queue_before,(unsigned long)queue_after,queue_after_valid?"true":"false",(unsigned long)exit_code,timed_out?"true":"false",reaped?"true":"false",assigned?"true":"false",resumed?"true":"false",cleanup_errors,(unsigned long)error,(unsigned long)initial_input_mode,(unsigned long)initial_output_mode);
 for(unsigned i=0;i<gate_count;++i){st_fixture_gate_result *g=&gates[i];if(i)fputc(',',log);fprintf(log,"{\"visit\":%u,\"wait_result\":%lu,\"wait_error\":%lu,\"geometry_error\":%lu,\"release_error\":%lu,\"arrived\":%u,\"queried\":%u,\"applied\":%u,\"released\":%u,\"width_before\":%d,\"width_after\":%d,\"height_before\":%d,\"height_after\":%d}",i+1,(unsigned long)g->wait_result,(unsigned long)g->wait_error,(unsigned long)g->geometry_error,(unsigned long)g->release_error,g->arrived,g->queried,g->applied,g->released,g->width_before,g->width_after,g->height_before,g->height_after);}
 fprintf(log,"],\"all_writers_stopped\":%s,\"trace_written\":%s,\"initial_left\":%d,\"initial_top\":%d,\"initial_width\":%d,\"initial_height\":%d,\"buffer_width\":%d,\"buffer_height\":%d,\"reader\":",all_writers_stopped?"true":"false",trace_written?"true":"false",geometry.srWindow.Left,geometry.srWindow.Top,geometry.srWindow.Right-geometry.srWindow.Left+1,geometry.srWindow.Bottom-geometry.srWindow.Top+1,geometry.dwSize.X,geometry.dwSize.Y);
 if(reaped&&all_writers_stopped&&observed&&observed->magic==ST_RESULT_MAGIC&&observed->read_count<=ST_MAX_READS&&observed->output_count<=ST_MAX_OUTPUT){
  fprintf(log,"{\"configured\":%u,\"finished\":%u,\"result\":%u,\"input_mode\":%u,\"output_mode\":%u,\"canonical\":%u,\"vmin\":%u,\"vtime\":%u,\"mouse\":%u,\"focus\":%u,\"select_called\":%u,\"select_result\":%u,\"select_error\":%u,\"termios_restored\":%u,\"reads\":[",observed->configured,observed->finished,observed->result,observed->input_mode,observed->output_mode,observed->canonical,observed->vmin,observed->vtime,observed->mouse,observed->focus,observed->select_called,observed->select_result,observed->select_error,observed->termios_restored);
  for(unsigned i=0;i<observed->read_count;++i){if(i)fputc(',',log);fprintf(log,"{\"count\":%lld,\"requested\":%u,\"errno\":%u}",(long long)observed->reads[i].count,observed->reads[i].requested,observed->reads[i].error);}
  fputs("],\"output_hex\":\"",log);for(unsigned i=0;i<observed->output_count;++i)fprintf(log,"%02x",observed->output[i]);fputs("\"}",log);
 }else fputs("null",log);
 fprintf(log,",\"scenario_ok\":%s,\"native_qualified\":false}\n",success&&!cleanup_errors&&all_writers_stopped&&trace_written?"true":"false");
 if(ferror(log))success=0;
 if(fclose(log))success=0;
 if(si.lpAttributeList)DeleteProcThreadAttributeList(si.lpAttributeList);
 if(attrs)HeapFree(GetProcessHeap(),0,attrs);
 if(pi.hThread)CloseHandle(pi.hThread);
 if(pi.hProcess)CloseHandle(pi.hProcess);
 if(job)CloseHandle(job);
 if(port)CloseHandle(port);
 if(trace_view)UnmapViewOfFile(trace_view);
 if(observed)UnmapViewOfFile(observed);
 if(trace_map)CloseHandle(trace_map);
 if(result_map)CloseHandle(result_map);
 if(ready)CloseHandle(ready);
 st_fixture_close_events(&events);
 if(trace_file!=INVALID_HANDLE_VALUE)CloseHandle(trace_file);
 if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);
 if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);
 FreeConsole();
 return success&&!cleanup_errors&&all_writers_stopped&&trace_written?0:1;
}
