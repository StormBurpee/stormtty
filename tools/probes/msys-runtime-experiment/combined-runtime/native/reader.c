/* Dedicated MSYS fixture. No automatic retry of a case and no synthetic signal
   delivery. Controlled EINTR continuation is recorded, never erased. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <locale.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include "cases.h"
#include "../reader-protocol.h"
int main(int argc,char **argv){
 /* CASE TRACE_MAP RESULT_MAP READY ARRIVED RESUME WAIT_NOTICE GENERATION */
 if(argc!=9)return 2;
 const st_case *c=st_find_case(argv[1]);uint64_t n[7];
 if(!c)return 2;
 for(unsigned i=0;i<7;++i)if(!st_decimal(argv[i+2],&n[i])||n[i]>UINTPTR_MAX)return 2;
 st_result *r=(st_result*)MapViewOfFile((HANDLE)(uintptr_t)n[1],FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof *r);
 if(!r)return 3;
 r->magic=ST_RESULT_MAGIC;int status=1,configured=0;struct termios saved,mode,actual;st_reader_api api;
 if(!setlocale(LC_ALL,"C.UTF-8")||!isatty(STDIN_FILENO)||tcgetattr(STDIN_FILENO,&saved))goto done;
 mode=saved;cfmakeraw(&mode);mode.c_cc[VMIN]=1;mode.c_cc[VTIME]=0;
 if(c->canonical){mode.c_lflag|=ICANON;mode.c_iflag|=ICRNL;}
 if(tcsetattr(STDIN_FILENO,TCSANOW,&mode))goto done;
 configured=1;
 if(tcgetattr(STDIN_FILENO,&actual)||!!(actual.c_lflag&ICANON)!=!!c->canonical||actual.c_cc[VMIN]!=1||actual.c_cc[VTIME]!=0)goto done;
 /* These output controls configure the actual MSYS console input translator. */
 const char *controls=c->event==1&&c->reporting?"\033[?1000h\033[?1006h\033[?1004l":
     c->event==2&&c->reporting?"\033[?1000l\033[?1006l\033[?1004h":
     "\033[?1000l\033[?1006l\033[?1004l";
 if(write(STDOUT_FILENO,controls,strlen(controls))!=(ssize_t)strlen(controls))goto done;
 DWORD input_mode=0,output_mode=0;
 if(!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&input_mode)||!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE),&output_mode))goto done;
 r->input_mode=input_mode;r->output_mode=output_mode;r->canonical=!!(actual.c_lflag&ICANON);
 r->vmin=actual.c_cc[VMIN];r->vtime=actual.c_cc[VTIME];r->mouse=c->event==1&&c->reporting;r->focus=c->event==2&&c->reporting;
 if(!st_reader_resolve(&api)||!st_reader_install_handlers(&api,0)||api.init((uintptr_t)n[0],n[6])||
    api.configure(c->gates?(uintptr_t)n[3]:0,c->gates?(uintptr_t)n[4]:0,0,c->gate_index,c->gates,0))goto done;
 r->configured=1;if(!SetEvent((HANDLE)(uintptr_t)n[2]))goto done;
 if(c->select_first){
  fd_set fds;FD_ZERO(&fds);FD_SET(STDIN_FILENO,&fds);struct timeval timeout={5,0};
  errno=0;int ready=select(STDIN_FILENO+1,&fds,NULL,NULL,&timeout);
  r->select_called=1;r->select_result=(uint32_t)ready;r->select_error=ready<0?(uint32_t)errno:0;
  if(ready!=1||!FD_ISSET(STDIN_FILENO,&fds))goto done;
 }
 while(r->output_count<strlen(c->expected)&&r->read_count<ST_MAX_READS){
  unsigned char bytes[64];errno=0;ssize_t got=read(STDIN_FILENO,bytes,c->requested);int error=got<0?errno:0;
  st_read_result *entry=&r->reads[r->read_count++];entry->count=got;entry->requested=c->requested;entry->error=(uint32_t)error;
  if(got>0){
   if((size_t)got>ST_MAX_OUTPUT-r->output_count)goto done;
   memcpy(r->output+r->output_count,bytes,(size_t)got);r->output_count+=(uint32_t)got;
   if(r->output_count>strlen(c->expected)||memcmp(r->output,c->expected,r->output_count))goto done;
  }else if(got==0||error!=EINTR||!c->gates)goto done;
 }
 status=r->output_count==strlen(c->expected)&&!memcmp(r->output,c->expected,r->output_count)?0:1;
done:
 if(configured){r->termios_restored=tcsetattr(STDIN_FILENO,TCSANOW,&saved)==0;if(!r->termios_restored)status=1;}
 r->result=(uint32_t)status;r->finished=1;UnmapViewOfFile(r);return status;
}
