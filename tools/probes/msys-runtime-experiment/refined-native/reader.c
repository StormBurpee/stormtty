/* Diagnostic MSYS-native reader. No runtime hooks; copied_chars is unobservable. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
static volatile sig_atomic_t wins, jump_enabled;
static sigjmp_buf restart;
static void winch(int n) { (void)n; ++wins; if (jump_enabled) siglongjmp(restart,1); }
static void timeout(int n) { (void)n; _exit(124); }
static int ready(int millis) {
  fd_set fds; struct timeval tv={millis/1000,(millis%1000)*1000};
  FD_ZERO(&fds); FD_SET(0,&fds); return select(1,&fds,NULL,NULL,&tv);
}
int main(int argc,char **argv) {
  if(argc!=4) return 2; /* scenario, log, marker; all private paths */
  if(!setlocale(LC_ALL,"C.UTF-8")) return 9;
  const char *mode=argv[1];
  int canon=!strcmp(mode,"canonical") || !strcmp(mode,"longjmp");
  int mouse=!strcmp(mode,"mouse-on");
  size_t expected=canon?3:!strcmp(mode,"surrogate")?6:mouse?12:2;
  FILE *log=fopen(argv[2],"wx"); if(!log) return 3;
  setvbuf(log,NULL,_IONBF,0);
  /* t changes after sigsetjmp; static storage remains defined after a jump. */
  static struct termios saved, t;
  if(!isatty(0)||tcgetattr(0,&saved)) { fprintf(log,"{\"error\":\"not-console-tty\"}\n"); return 4; }
  t=saved; cfmakeraw(&t); if(canon) t.c_lflag |= ICANON;
  t.c_cc[VMIN]=2; t.c_cc[VTIME]=0;
  if(tcsetattr(0,TCSANOW,&t)) return 5;
  struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=winch; sigemptyset(&sa.sa_mask);
  if(sigaction(SIGWINCH,&sa,NULL)) return 6; /* deliberately no SA_RESTART */
  signal(SIGALRM,timeout); alarm(8);
  if(mouse && write(1,"\033[?1000h\033[?1006h",16)!=16) return 7;
  fprintf(log,"{\"scenario\":\"%s\",\"vmin\":2,\"canonical\":%d,\"path_observed\":false}\n",mode,canon);
  /* Keep modified state out of automatic variables across siglongjmp.
     jump_enabled bounds a call-attempt window, not exact read/select entry:
     a post-return jump can skip successful bytes before logging them. */
  static unsigned char collected[256]; static size_t count; static int attempts;
  if(sigsetjmp(restart,1)) { jump_enabled=0; fprintf(log,"{\"longjmp\":true,\"signals\":%d}\n",(int)wins); }
  else {
    FILE *marker=fopen(argv[3],"wx"); if(!marker) return 8;
    fputs("configured; about to enter read/select (not proof of entry)\n",marker); fclose(marker);
  }
  while(count<expected && attempts++<12) {
    if(attempts>1 || !strcmp(mode,"select")) {
      errno=0; jump_enabled=!strcmp(mode,"longjmp");
      int rc=ready(1000); int err=errno; jump_enabled=0;
      fprintf(log,"{\"select\":%d,\"errno\":%d,\"signals\":%d}\n",rc,err,(int)wins);
      if(rc<0 && err==EINTR) continue;
      if(rc<=0) break;
    }
    unsigned char bytes[64]; errno=0;
    jump_enabled=!strcmp(mode,"longjmp");
    ssize_t n=read(0,bytes,sizeof bytes); int err=errno; jump_enabled=0;
    fprintf(log,"{\"read\":%ld,\"errno\":%d,\"signals\":%d,\"hex\":\"",(long)n,err,(int)wins);
    if(n>0) for(ssize_t j=0;j<n;++j) fprintf(log,"%02x",bytes[j]);
    fputs("\"}\n",log);
    if(n>0 && count+(size_t)n<=sizeof collected) { memcpy(collected+count,bytes,(size_t)n); count+=(size_t)n; }
    else if(n<0 && err==EINTR) continue;
    else break;
    /* Once first read returned, tail drain must not block on lone final byte. */
    t.c_cc[VMIN]=1; if(tcsetattr(0,TCSANOW,&t)) break;
  }
  jump_enabled=0;
  if(ready(150)>0) {
    unsigned char extra[64]; ssize_t n=read(0,extra,sizeof extra);
    if(n>0 && count+(size_t)n<=sizeof collected) { memcpy(collected+count,extra,(size_t)n); count+=(size_t)n; }
  }
  fprintf(log,"{\"final_hex\":\""); for(size_t i=0;i<count;++i) fprintf(log,"%02x",collected[i]);
  fprintf(log,"\",\"signals\":%d,\"exact_path\":\"inconclusive-no-hook\"}\n",(int)wins);
  if(mouse) (void)write(1,"\033[?1000l\033[?1006l",16);
  (void)tcsetattr(0,TCSANOW,&saved); alarm(0); fclose(log); return 0;
}
