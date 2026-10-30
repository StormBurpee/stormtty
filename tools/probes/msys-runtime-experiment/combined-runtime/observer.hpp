/* Diagnostic-only bounded numeric recorder plus explicit scheduling rendezvous.
   NOT PASSIVE. Included by a separately reviewed source patch, never the app.
   No input bytes, paths, pointers or handles are serialized. */
#ifndef STORMTTY_COMBINED_CONSOLE_TRACE
#error Explicit diagnostic build required
#endif
#if !defined(_WIN32) && !defined(__MSYS__) && !defined(__CYGWIN__)
#error Native Windows/MSYS diagnostic target required
#endif
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
namespace st_path {
constexpr uint32_t capacity=16384;
struct alignas(8) Header {
  char magic[8]; uint32_t version,header_size,record_size,capacity;
  uint64_t reserved,overflow; uint32_t initialized,finalized;
  uint64_t generation,flags;
};
struct alignas(8) Record {
  uint32_t committed; uint16_t version,event; uint64_t sequence,generation;
  uint32_t tid; uint16_t role,flags; uint64_t read,peek,operation,parent;
  uint64_t data[8];
};
struct Mapping {Header header; Record records[capacity];};
static_assert(sizeof(Header)==64 && sizeof(Record)==128,"wire size");
static_assert(offsetof(Record,data)==64 && offsetof(Header,reserved)==24,"wire offset");
static_assert(__atomic_always_lock_free(8,nullptr)&&__atomic_always_lock_free(4,nullptr),"native lock-free instructions required");
enum Event : uint16_t {READ_BEGIN=1,COPY,READ_MORE,PEEK,DISPATCH,GEOMETRY,
 RETIRE_BEGIN,RETIRE_API,RETIRE_END,LOCK_BEGIN,LOCK_END,SIGNAL_BEGIN,SIGNAL_END,
 PROCESS_RETURN,READ_RETURN,MASTER_QUEUE,MASTER_EVENT,HANDLER_ENTER,HANDLER_RETURN,CONTEXT_ERROR,
 READ_WAIT_BEGIN,READ_WAIT_END,INGRESS,RESTART_DECISION,GATE_BEGIN,GATE_END,WAIT_NOTICE,EXTERNAL_ARM};
enum ReturnReason : unsigned {BACKGROUND=1,TIMEOUT_COPIED,TIMEOUT_EMPTY,NONBLOCK_COPIED,
 NONBLOCK_EMPTY,COMPLETE,ERROR_RETURN,SIGNAL_COPIED,SIGNAL_EMPTY};
enum Ingress : unsigned {WAIT_SIGNAL=1,INPUT_WINCH,INPUT_SIGNAL};
struct Frame {uint64_t id,copied,peek;};
struct Context {
 uint32_t tid; uint16_t role,depth; Frame reads[4]; uint64_t orphan_peek;
 uint64_t operations[8]; uint16_t operation_depth; uint16_t event_type;
 uint32_t index; uint64_t retired;
};
static Mapping* mapping;
static DWORD process_id;
static uint64_t next_id;
static Context contexts[8];
static uint64_t active_signal;
static uint64_t signal_read,signal_peek;
static uint32_t signal_tid;
static int signal_number;
// Single reader fixture only. Handler snapshots use atomics, never Context.
static uint64_t reader_read,reader_peek;
static uint32_t reader_tid;
static uint64_t external_token;
static int external_signal;
struct Gates {
 HANDLE arrived,resume,wait_notice;
 unsigned index,limit,visits,wait_copied;
 bool configured,failed,notified;
};
static Gates gates;
constexpr DWORD rendezvous_timeout_ms=1500;
struct Errors {
 DWORD win; int posix;
 Errors():win(GetLastError()),posix(errno){}
 ~Errors(){errno=posix;SetLastError(win);}
};
inline Mapping* active(){return __atomic_load_n(&mapping,__ATOMIC_ACQUIRE);}
inline uint64_t id(){return __atomic_add_fetch(&next_id,1,__ATOMIC_RELAXED);}
inline Context* context(uint16_t role=1) {
 Mapping* m=active();if(!m)return nullptr;
 if(GetCurrentProcessId()!=process_id){__atomic_store_n(&m->header.overflow,1,__ATOMIC_RELEASE);return nullptr;}
 const DWORD tid=GetCurrentThreadId();
 for(auto& c:contexts) {
  uint32_t owner=__atomic_load_n(&c.tid,__ATOMIC_ACQUIRE);
  if(owner==tid){if(role==2)c.role=2;return &c;}
  if(!owner&&__atomic_compare_exchange_n(&c.tid,&owner,tid,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
   c.role=role;return &c;
  }
 }
 __atomic_store_n(&m->header.overflow,1,__ATOMIC_RELEASE);return nullptr;
}
inline uint64_t read_id(Context* c){return c&&c->depth?c->reads[c->depth-1].id:0;}
inline uint64_t peek_id(Context* c){return !c?0:c->depth?c->reads[c->depth-1].peek:c->orphan_peek;}
inline uint64_t copied(Context* c){return c&&c->depth?c->reads[c->depth-1].copied:0;}
inline uint64_t operation(Context* c){return c&&c->operation_depth?c->operations[c->operation_depth-1]:0;}
inline void emit_context(Context* c,Event event,uint64_t a=0,uint64_t b=0,uint64_t d=0,uint64_t e=0,
 uint64_t f=0,uint64_t g=0,uint64_t h=0,uint64_t i=0,uint64_t op=0,uint64_t parent=0,uint16_t role=0,uint16_t flags=1) {
 Errors saved;Mapping* m=active();if(!m||!c||__atomic_load_n(&m->header.overflow,__ATOMIC_ACQUIRE))return;
 uint64_t slot=__atomic_load_n(&m->header.reserved,__ATOMIC_RELAXED);bool got=false;
 for(unsigned attempt=0;attempt<32;++attempt) {
  if(slot>=capacity)break;
  if(__atomic_compare_exchange_n(&m->header.reserved,&slot,slot+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){got=true;break;}
 }
 if(!got){__atomic_store_n(&m->header.overflow,1,__ATOMIC_RELEASE);return;}
 Record& r=m->records[slot];r.version=2;r.event=event;r.sequence=slot;r.generation=m->header.generation;
 r.tid=c->tid;r.role=role?role:c->role;r.flags=flags;r.read=read_id(c);r.peek=peek_id(c);
 r.operation=op?op:operation(c);r.parent=parent;
 r.data[0]=a;r.data[1]=b;r.data[2]=d;r.data[3]=e;r.data[4]=f;r.data[5]=g;r.data[6]=h;r.data[7]=i;
 __atomic_store_n(&r.committed,1,__ATOMIC_RELEASE);
}
inline void event(Event kind,uint64_t a=0,uint64_t b=0,uint64_t d=0,uint64_t e=0,
 uint64_t f=0,uint64_t g=0,uint64_t h=0,uint64_t i=0) {
 Errors saved;emit_context(context(),kind,a,b,d,e,f,g,h,i);
}
inline uint64_t begin(Event kind,uint64_t a=0,uint64_t b=0,uint64_t d=0,uint64_t e=0,uint64_t f=0) {
 Errors saved;auto* c=context();if(!c)return 0;
 if(c->operation_depth>=8){emit_context(c,CONTEXT_ERROR,1);return 0;}
 const auto parent=operation(c),current=id();c->operations[c->operation_depth++]=current;
 emit_context(c,kind,a,b,d,e,f,0,0,0,current,parent);return current;
}
inline void end(Event kind,uint64_t a=0,uint64_t b=0,uint64_t d=0,uint64_t e=0,uint64_t f=0) {
 Errors saved;auto* c=context();if(!c)return;
 emit_context(c,kind,a,b,d,e,f);if(c->operation_depth)--c->operation_depth;else emit_context(c,CONTEXT_ERROR,2);
}
inline void read_begin(uint64_t requested,uint64_t canonical,uint64_t vmin,uint64_t vtime,uint64_t nonblock) {
 Errors saved;auto* c=context();if(!c)return;
 if(c->depth>=4){emit_context(c,CONTEXT_ERROR,3);return;}
 c->reads[c->depth++]={id(),0,0};
 if(c->depth!=1||c->tid!=reader_tid){emit_context(c,CONTEXT_ERROR,8);return;}
 __atomic_store_n(&reader_peek,0,__ATOMIC_RELAXED);
 __atomic_store_n(&reader_read,read_id(c),__ATOMIC_RELEASE);
 emit_context(c,READ_BEGIN,requested,canonical,vmin,vtime,nonblock);
}
inline void read_end(uint64_t result,uint64_t count,int error,unsigned reason) {
 Errors saved;auto* c=context();if(!c)return;emit_context(c,READ_RETURN,result,count,result==UINT64_MAX?(uint64_t)error:0,reason);
 __atomic_store_n(&reader_read,0,__ATOMIC_RELEASE);
 if(c->depth)--c->depth;else emit_context(c,CONTEXT_ERROR,4);
}
inline void copy(uint64_t count,uint64_t ra,uint64_t ready,uint64_t remaining) {
 Errors saved;auto* c=context();if(!c||!c->depth)return;
 auto& f=c->reads[c->depth-1];const auto previous=f.copied;f.copied=count;
 emit_context(c,COPY,previous,count-previous,count,ra,ready,remaining);
}
inline void peek(BOOL ok,DWORD error,DWORD count,size_t remaining,size_t ra,bool ready) {
 Errors saved;auto* c=context();if(!c)return;
 if(c->depth)c->reads[c->depth-1].peek=id();else c->orphan_peek=id();
 if(c->tid==reader_tid&&c->depth)__atomic_store_n(&reader_peek,peek_id(c),__ATOMIC_RELEASE);
 emit_context(c,PEEK,ok,ok?0:error,ok?count:0,remaining,remaining?1:2,ra,ready);
}
inline void dispatch(unsigned type,unsigned index,unsigned total,unsigned stat,unsigned mouse,size_t ra,bool focus=false) {
 Errors saved;auto* c=context();if(!c)return;c->event_type=(uint16_t)type;c->index=index;
 emit_context(c,DISPATCH,type,index,total,stat,mouse,ra,copied(c),focus);
}
inline void geometry(int x,int y,int nx,int ny,bool ok) {
 Errors saved;auto* c=context();if(!c)return;
 emit_context(c,GEOMETRY,c->event_type,c->index,(uint64_t)x,(uint64_t)y,(uint64_t)nx,(uint64_t)ny,x!=nx||y!=ny,ok);
}
inline void process_return(unsigned stat,size_t ra,bool ready) {
 Errors saved;auto* c=context();if(c)emit_context(c,PROCESS_RETURN,stat,c->index,ra,ready,copied(c));
}
template<class T> inline T process_result(T stat,size_t ra,bool ready) {
 process_return((unsigned)stat,ra,ready);return stat;
}
inline void retire_begin(size_t explicit_n,size_t effective,size_t processed,size_t con_processed) {
 Errors saved;auto* c=context();if(!c)return;c->retired=0;
 begin(RETIRE_BEGIN,explicit_n,effective,processed,con_processed,0);
}
inline void retire_api(DWORD request,BOOL ok,DWORD actual,DWORD error) {
 Errors saved;auto* c=context();if(!c)return;
 emit_context(c,RETIRE_API,request,ok,ok?actual:0,!ok,ok?0:error,c->retired);
 if(ok)c->retired+=actual;
}
inline void signal_begin(int sig) {
 Errors saved;auto* c=context();if(!c)return;
 const auto op=begin(SIGNAL_BEGIN,(uint64_t)sig,c->event_type,c->index);
 uint64_t expected=0;
 if(!op||!__atomic_compare_exchange_n(&active_signal,&expected,UINT64_MAX,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {emit_context(c,CONTEXT_ERROR,5);return;}
 __atomic_store_n(&signal_read,read_id(c),__ATOMIC_RELAXED);
 __atomic_store_n(&signal_peek,peek_id(c),__ATOMIC_RELAXED);
 __atomic_store_n(&signal_tid,c->tid,__ATOMIC_RELAXED);
 __atomic_store_n(&signal_number,sig,__ATOMIC_RELAXED);
 __atomic_store_n(&active_signal,op,__ATOMIC_RELEASE);
}
inline void signal_end(int sig) {
 Errors saved;auto* c=context();if(!c)return;
 uint64_t expected=operation(c);__atomic_compare_exchange_n(&active_signal,&expected,0,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
 end(SIGNAL_END,(uint64_t)sig,c->event_type,c->index);
}
inline DWORD wait(HANDLE handle,DWORD timeout,unsigned kind,unsigned role=1) {
 {Errors saved;context((uint16_t)role);begin(LOCK_BEGIN,kind,1);}
 const DWORD result=WaitForSingleObject(handle,timeout);
 {Errors saved;end(LOCK_END,kind,1,result,result!=WAIT_OBJECT_0,result!=WAIT_OBJECT_0?GetLastError():0);}
 return result;
}
inline BOOL release(HANDLE handle,unsigned kind,unsigned role=1) {
 {Errors saved;context((uint16_t)role);begin(LOCK_BEGIN,kind,2);}
 const BOOL result=ReleaseMutex(handle);
 {Errors saved;end(LOCK_END,kind,2,(uint64_t)result,!result,!result?GetLastError():0);}
 return result;
}
inline void master_event(unsigned type,unsigned index,unsigned total,bool processed) {
 Errors saved;auto* c=context(2);if(!c)return;c->event_type=(uint16_t)type;c->index=index;
 emit_context(c,MASTER_EVENT,type,index,total,processed);
}
inline void master_queue(unsigned kind,DWORD requested,BOOL ok,DWORD actual,DWORD error,DWORD total,DWORD processed) {
 Errors saved;emit_context(context(2),MASTER_QUEUE,kind,requested,ok,ok?actual:0,!ok,ok?0:error,total,processed);
}
inline void ingress(unsigned kind,unsigned result) {
 Errors saved;auto* c=context();if(c)emit_context(c,INGRESS,kind,result,copied(c));
}
inline bool restart_decision(unsigned result,int sig,bool restart) {
 Errors saved;auto* c=context();if(c)emit_context(c,RESTART_DECISION,result,(uint64_t)sig,restart,copied(c));
 return restart;
}
template<class F> inline auto read_wait(DWORD timeout,F original_wait) -> decltype(original_wait()) {
 {
  Errors saved;auto* c=context();
  if(c)emit_context(c,READ_WAIT_BEGIN,timeout,copied(c));
  if(c&&c->role==1&&c->tid==reader_tid&&gates.configured&&gates.wait_notice
     &&!gates.notified&&copied(c)==gates.wait_copied) {
   gates.notified=true;const BOOL ok=SetEvent(gates.wait_notice);const DWORD error=GetLastError();
   emit_context(c,WAIT_NOTICE,ok,ok?0:error,copied(c));
  }
 }
 // Exactly the original wait, same arguments. Notice is not read-entry proof.
 const auto result=original_wait();
 {Errors saved;auto* c=context();if(c)emit_context(c,READ_WAIT_END,(uint64_t)result,copied(c),result==WAIT_FAILED?GetLastError():0);}
 return result;
}
inline void mouse_rendezvous() {
 Errors saved;auto* c=context();
 // No select/master gate. No lock, queue, input-mode or signal mutation here.
 if(!c||c->role!=1||c->tid!=reader_tid||!read_id(c)||c->event_type!=MOUSE_EVENT
    ||!gates.configured||gates.failed||c->index!=gates.index||gates.visits>=gates.limit)return;
 const auto visit=++gates.visits;
 emit_context(c,GATE_BEGIN,visit,c->index,rendezvous_timeout_ms,copied(c));
 const BOOL notified=SetEvent(gates.arrived);const DWORD notify_error=GetLastError();
 const DWORD result=notified?WaitForSingleObject(gates.resume,rendezvous_timeout_ms):WAIT_FAILED;
 const DWORD wait_error=GetLastError();
 gates.failed=!notified||result!=WAIT_OBJECT_0;
 emit_context(c,GATE_END,visit,c->index,result,notified,notified?0:notify_error,
              result==WAIT_FAILED&&notified?wait_error:0,gates.failed);
 // Timeout disables further gates, never changes the runtime return decision.
}
} // namespace st_path
extern "C" __declspec(dllexport) int stormtty_combined_init_v2(uintptr_t handle,uint64_t generation) {
 st_path::Errors saved;
 if(st_path::active()||!handle||!generation)return 1;
 auto* m=(st_path::Mapping*)MapViewOfFile((HANDLE)handle,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(st_path::Mapping));
 if(!m)return 2;
 const char magic[8]={'S','T','C','M','B','0','0','2'};bool ok=true;
 for(unsigned i=0;i<8;++i)ok=ok&&m->header.magic[i]==magic[i];
 ok=ok&&m->header.version==2&&m->header.header_size==64&&m->header.record_size==128&&m->header.capacity==st_path::capacity;
 ok=ok&&m->header.initialized==1&&m->header.finalized==0&&m->header.generation==generation;
 ok=ok&&!m->header.reserved&&!m->header.overflow&&!m->header.flags;
 if(!ok){UnmapViewOfFile(m);return 3;}
 st_path::process_id=GetCurrentProcessId();st_path::reader_tid=GetCurrentThreadId();
 __atomic_store_n(&st_path::mapping,m,__ATOMIC_RELEASE);return 0;
}
extern "C" __declspec(dllexport) int stormtty_combined_configure_v2(uintptr_t arrived,uintptr_t resume,
 uintptr_t wait_notice,unsigned index,unsigned visits,unsigned wait_copied) {
 st_path::Errors saved;
 if(!st_path::active()||st_path::gates.configured||GetCurrentThreadId()!=st_path::reader_tid
    ||__atomic_load_n(&st_path::reader_read,__ATOMIC_ACQUIRE)||visits>8||index>1||wait_copied>1
    ||(visits&&(!arrived||!resume||arrived==resume))||(!visits&&(arrived||resume))
    ||(wait_notice&&(wait_notice==arrived||wait_notice==resume)))return 1;
 st_path::gates={(HANDLE)arrived,(HANDLE)resume,(HANDLE)wait_notice,index,visits,0,wait_copied,true,false,false};
 return 0;
}
extern "C" __declspec(dllexport) int stormtty_combined_external_arm_v2(int sig,uint64_t token) {
 st_path::Errors saved;
 if(!st_path::active()||sig<=0||!token||GetCurrentThreadId()!=st_path::reader_tid
    ||__atomic_load_n(&st_path::reader_read,__ATOMIC_ACQUIRE)||st_path::external_token)return 1;
 __atomic_store_n(&st_path::external_signal,sig,__ATOMIC_RELAXED);
 __atomic_store_n(&st_path::external_token,token,__ATOMIC_RELEASE);
 st_path::event(st_path::EXTERNAL_ARM,(uint64_t)sig,token);return 0;
}
extern "C" __declspec(dllexport) void stormtty_combined_handler_v2(int entering,int sig,unsigned counter) {
 // Handler owns its stack context; never reads or initializes thread-context
 // tables interrupted halfway through an update. Shared signal metadata is atomic.
 st_path::Errors saved;if(!st_path::active())return;
 st_path::Context local{};local.tid=GetCurrentThreadId();local.role=3;local.depth=1;
 auto op=__atomic_load_n(&st_path::active_signal,__ATOMIC_ACQUIRE);
 unsigned source=0;uint64_t token=0;
 if(op&&op!=UINT64_MAX&&sig==__atomic_load_n(&st_path::signal_number,__ATOMIC_RELAXED)
    &&local.tid==__atomic_load_n(&st_path::signal_tid,__ATOMIC_RELAXED)) {
  source=1;local.reads[0].id=__atomic_load_n(&st_path::signal_read,__ATOMIC_RELAXED);
  local.reads[0].peek=__atomic_load_n(&st_path::signal_peek,__ATOMIC_RELAXED);
 } else {
  token=__atomic_load_n(&st_path::external_token,__ATOMIC_ACQUIRE);
  if(token&&sig==__atomic_load_n(&st_path::external_signal,__ATOMIC_RELAXED)
     &&local.tid==st_path::reader_tid) {
   source=2;op=0;local.reads[0].id=__atomic_load_n(&st_path::reader_read,__ATOMIC_ACQUIRE);
   local.reads[0].peek=__atomic_load_n(&st_path::reader_peek,__ATOMIC_ACQUIRE);
  }
 }
 if(!source||!local.reads[0].id){st_path::emit_context(&local,st_path::CONTEXT_ERROR,6);return;}
 st_path::emit_context(&local,entering?st_path::HANDLER_ENTER:st_path::HANDLER_RETURN,(uint64_t)sig,counter,source,token,0,0,0,0,op,0,3);
 if(source==1&&__atomic_load_n(&st_path::active_signal,__ATOMIC_ACQUIRE)!=op)
  st_path::emit_context(&local,st_path::CONTEXT_ERROR,7);
}
