/* Cold reader setup plus minimal no-throw C signal handler. Native PE import
   and linked-handler inspection are REQUIRED before any fixture execution. */
#ifndef STORMTTY_COMBINED_READER_PROTOCOL
#define STORMTTY_COMBINED_READER_PROTOCOL
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
typedef int (*st_init_fn)(uintptr_t,uint64_t);
typedef int (*st_configure_fn)(uintptr_t,uintptr_t,uintptr_t,unsigned,unsigned,unsigned);
typedef int (*st_arm_fn)(int,uint64_t);
typedef void (*st_handler_fn)(int,int,unsigned);
typedef struct {st_init_fn init;st_configure_fn configure;st_arm_fn arm;st_handler_fn handler;} st_reader_api;
static st_handler_fn st_reader_handler;
static volatile sig_atomic_t st_reader_winch_count,st_reader_external_count;
static void st_reader_signal(int number) {
 int saved_errno=errno;DWORD saved_win32=GetLastError();
 volatile sig_atomic_t *counter=number==SIGWINCH?&st_reader_winch_count:&st_reader_external_count;
 if(*counter<SIG_ATOMIC_MAX)++*counter;
 if(st_reader_handler){st_reader_handler(1,number,(unsigned)*counter);st_reader_handler(0,number,(unsigned)*counter);}
 errno=saved_errno;SetLastError(saved_win32);
}
static int st_reader_resolve(st_reader_api *api) {
 HMODULE module=GetModuleHandleW(L"msys-2.0.dll");
 const char *names[4]={"stormtty_combined_init_v2","stormtty_combined_configure_v2",
     "stormtty_combined_external_arm_v2","stormtty_combined_handler_v2"};
 FARPROC symbols[4];
 if(!module)return 0;
 for(unsigned i=0;i<4;++i){symbols[i]=GetProcAddress(module,names[i]);if(!symbols[i])return 0;}
 /* No incompatible function-pointer casts; exact x86_64 ABI is a native gate. */
 _Static_assert(sizeof api->init==sizeof symbols[0],"init ABI");
 _Static_assert(sizeof api->configure==sizeof symbols[0],"configure ABI");
 _Static_assert(sizeof api->arm==sizeof symbols[0],"arm ABI");
 _Static_assert(sizeof api->handler==sizeof symbols[0],"handler ABI");
 memcpy(&api->init,&symbols[0],sizeof api->init);
 memcpy(&api->configure,&symbols[1],sizeof api->configure);
 memcpy(&api->arm,&symbols[2],sizeof api->arm);
 memcpy(&api->handler,&symbols[3],sizeof api->handler);return 1;
}
static int st_reader_install_handlers(const st_reader_api *api,int restart) {
 struct sigaction action;memset(&action,0,sizeof action);
 /* No added mask/signal suppression. The normal POSIX self-signal mask applies;
    unexpected overlapping signal sources make the evidence inconclusive. */
 if(sigemptyset(&action.sa_mask))return 0;
 action.sa_handler=st_reader_signal;action.sa_flags=restart?SA_RESTART:0;
 st_reader_handler=api->handler;
 return !sigaction(SIGWINCH,&action,NULL)&&!sigaction(SIGUSR1,&action,NULL);
}
#endif
