// Portable API-double regression, NOT native runtime or linked-handler proof.
#define STORMTTY_COMBINED_CONSOLE_TRACE 1
#define _WIN32 1
#include "../observer.hpp"
#include "../controller-protocol.h"
#include <cassert>
#include <string>
static DWORD last_error=77,thread_id=10,wait_result=WAIT_OBJECT_0,multi_result=WAIT_OBJECT_0;
static bool set_ok=true,query_ok=true,geometry_ok=true;
static unsigned waits,sets,created,closed,geometry_calls;
static CONSOLE_SCREEN_BUFFER_INFO screen={{200,100},{0,0},0,{0,0,79,23},{200,100}};
extern "C" DWORD GetLastError(){return last_error;}
extern "C" void SetLastError(DWORD x){last_error=x;}
extern "C" DWORD GetCurrentProcessId(){return 9;}
extern "C" DWORD GetCurrentThreadId(){return thread_id;}
extern "C" void* MapViewOfFile(HANDLE h,DWORD,DWORD,DWORD,size_t){return h;}
extern "C" BOOL UnmapViewOfFile(const void*){return TRUE;}
extern "C" DWORD WaitForSingleObject(HANDLE,DWORD timeout){assert(timeout==1500);++waits;errno=EIO;last_error=123;return wait_result;}
extern "C" DWORD WaitForMultipleObjects(DWORD n,const HANDLE*,BOOL all,DWORD timeout){assert(n==2&&!all&&timeout==2000);return multi_result;}
extern "C" BOOL ReleaseMutex(HANDLE){return TRUE;}
extern "C" BOOL SetEvent(HANDLE){++sets;errno=EIO;last_error=456;return set_ok;}
extern "C" HANDLE CreateEventW(SECURITY_ATTRIBUTES *s,BOOL manual,BOOL initial,const wchar_t *name){assert(s->bInheritHandle&&!manual&&!initial&&!name);return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(++created));}
extern "C" BOOL CloseHandle(HANDLE){++closed;return TRUE;}
extern "C" BOOL GetConsoleScreenBufferInfo(HANDLE,CONSOLE_SCREEN_BUFFER_INFO *info){*info=screen;return query_ok;}
extern "C" BOOL SetConsoleWindowInfo(HANDLE,BOOL absolute,const SMALL_RECT *rect){assert(absolute);++geometry_calls;if(geometry_ok)screen.srWindow=*rect;return geometry_ok;}
static st_path::Mapping mapping{};
static void init(){
 memcpy(mapping.header.magic,"STCMB002",8);mapping.header.version=2;
 mapping.header.header_size=64;mapping.header.record_size=128;mapping.header.capacity=16384;
 mapping.header.initialized=1;mapping.header.generation=99;
 assert(stormtty_combined_init_v2(reinterpret_cast<uintptr_t>(&mapping),99)==0);
 assert(errno==E2BIG&&last_error==77);
}
static const st_path::Record& last(){assert(mapping.header.reserved);return mapping.records[mapping.header.reserved-1];}
int main(int argc,char **argv){
 assert(argc==2);const std::string mode=argv[1];errno=E2BIG;init();
 if(mode=="invalid-config"){
  assert(stormtty_combined_configure_v2(1,1,3,0,1,0)==1);
  assert(stormtty_combined_configure_v2(1,2,1,0,1,0)==1);
  assert(stormtty_combined_configure_v2(1,2,3,2,1,0)==1);
  assert(stormtty_combined_configure_v2(1,2,3,0,9,0)==1);
  assert(stormtty_combined_configure_v2(1,2,3,0,0,0)==1);
  assert(stormtty_combined_configure_v2(0,0,0,0,0,0)==0);return 0;
 }
 if(mode=="controller"){
  st_fixture_events events{};assert(st_fixture_create_events(&events));
  st_fixture_gate_result result{};
  assert(st_fixture_service_gate((HANDLE)5,(HANDLE)6,&events,-1,&result));
  assert(result.width_before==80&&result.width_after==79&&result.released);
  assert(st_fixture_service_gate((HANDLE)5,(HANDLE)6,&events,0,&result));
  assert(result.width_before==79&&result.width_after==79&&geometry_calls==1);
  query_ok=false;assert(!st_fixture_service_gate((HANDLE)5,(HANDLE)6,&events,1,&result));assert(result.released);
  query_ok=true;geometry_ok=false;
  assert(!st_fixture_service_gate((HANDLE)5,(HANDLE)6,&events,1,&result));assert(result.released&&!result.applied);
  geometry_ok=true;set_ok=false;
  assert(!st_fixture_service_gate((HANDLE)5,(HANDLE)6,&events,1,&result));assert(!result.released&&result.release_error);
  set_ok=true;
  multi_result=WAIT_TIMEOUT;auto previous=sets;
  assert(!st_fixture_service_gate((HANDLE)5,(HANDLE)6,&events,1,&result));assert(sets==previous);
  st_fixture_close_events(&events);assert(closed==3&&!events.arrived);return 0;
 }
 unsigned index=mode=="prefix"?1:0,visits=mode=="disabled"?0:3;
 assert(stormtty_combined_configure_v2(visits?1:0,visits?2:0,3,index,visits,1)==0);
 assert(stormtty_combined_configure_v2(1,2,3,0,1,1)==1);
 if(mode=="external")assert(stormtty_combined_external_arm_v2(10,42)==0);
 if(mode=="no-read"){
  st_path::peek(1,0,3,0,0,false);st_path::dispatch(MOUSE_EVENT,0,3,0,0,0);
  st_path::mouse_rendezvous();assert(!sets&&!waits);return 0;
 }
 st_path::read_begin(64,0,2,0,0);
 if(mode=="zero-return"){
  st_path::read_end(UINT64_MAX,0,EINTR,st_path::SIGNAL_EMPTY);
  assert(last().data[0]==UINT64_MAX&&last().data[1]==0&&last().data[2]==EINTR&&last().data[3]==9);
  assert(errno==E2BIG&&last_error==77);return 0;
 }
 st_path::copy(1,0,0,63);
 if(mode=="wait"){
  unsigned calls=0;
  auto original=[&](){++calls;assert(errno==E2BIG&&last_error==77);errno=EINTR;SetLastError(91);return 1u;};
  assert(st_path::read_wait(100,original)==1&&calls==1&&sets==1);
  assert(errno==EINTR&&last_error==91);assert(last().event==st_path::READ_WAIT_END&&last().data[0]==1);
  st_path::ingress(st_path::WAIT_SIGNAL,1);assert(last().data[0]==1);
  assert(st_path::restart_decision(5,28,true));assert(last().data[2]==1);
  st_path::read_end(1,1,99,st_path::SIGNAL_COPIED);assert(last().data[2]==0&&last().data[3]==8);return 0;
 }
 if(mode=="external"){
  stormtty_combined_handler_v2(1,10,1);assert(last().event==st_path::HANDLER_ENTER);
  assert(last().data[2]==2&&last().data[3]==42&&last().read&&last().operation==0);
  stormtty_combined_handler_v2(0,10,1);assert(last().event==st_path::HANDLER_RETURN);
  assert(errno==E2BIG&&last_error==77);stormtty_combined_handler_v2(1,11,1);
  assert(last().event==st_path::CONTEXT_ERROR);return 0;
 }
 st_path::peek(1,0,3,63,0,false);st_path::dispatch(MOUSE_EVENT,index,3,0,0,0);
 if(mode=="wrong-index")st_path::dispatch(MOUSE_EVENT,1,3,0,0,0);
 if(mode=="master")st_path::context(2);
 if(mode=="timeout")wait_result=WAIT_TIMEOUT;
 if(mode=="set-failed")set_ok=false;
 if(mode=="wait-failed")wait_result=WAIT_FAILED;
 st_path::mouse_rendezvous();assert(errno==E2BIG&&last_error==77);
 if(mode=="disabled"||mode=="wrong-index"||mode=="master"){assert(!sets&&!waits);return 0;}
 assert(last().event==st_path::GATE_END&&last().data[0]==1&&last().data[1]==index);
 if(mode=="timeout"||mode=="set-failed"||mode=="wait-failed"){
  assert(last().data[6]==1);auto previous=sets;st_path::mouse_rendezvous();assert(sets==previous);
  if(mode=="set-failed")assert(waits==0&&last().data[4]==456);
  if(mode=="wait-failed")assert(last().data[5]==123);
 }else{
  assert(last().data[2]==WAIT_OBJECT_0&&last().data[6]==0);
  st_path::mouse_rendezvous();st_path::mouse_rendezvous();st_path::mouse_rendezvous();assert(waits==3);
 }
 return 0;
}
