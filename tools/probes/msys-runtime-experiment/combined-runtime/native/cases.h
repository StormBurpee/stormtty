#ifndef ST_COMBINED_CASES_H
#define ST_COMBINED_CASES_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define ST_RESULT_MAGIC 0x32524353u
#define ST_MAX_READS 16u
#define ST_MAX_OUTPUT 128u
#define ST_TRACE_SIZE (64u+16384u*128u)
typedef struct {
 const char *name;unsigned event,reporting,gate_index,gates,canonical,select_first,requested;
 const char *expected;
} st_case;
/* Native event codes: 0 keys only, 1 mouse, 2 focus, 3 unchanged resize.
   SGR wheel-up at visible cell (2,2), focus gained, and ASCII keys only. */
static const st_case st_cases[]={
 {"raw-1",0,0,0,0,0,0,1,"ab"},
 {"raw-64",0,0,0,0,0,0,64,"ab"},
 {"canonical",0,0,0,0,1,0,64,"ab\n"},
 {"select-raw",0,0,0,0,0,1,64,"ab"},
 {"stable-mouse-off",1,0,0,0,0,0,64,"b"},
 {"stable-mouse-on",1,1,0,0,0,0,64,"\033[<64;2;2Mb"},
 {"stable-focus-off",2,0,0,0,0,0,64,"b"},
 {"stable-focus-on",2,1,0,0,0,0,64,"\033[Ib"},
 {"stable-resize",3,0,0,0,0,0,64,"b"},
 {"changed-mouse-off",1,0,0,1,0,0,64,"b"},
 {"changed-mouse-on",1,1,0,1,0,0,64,"\033[<64;2;2Mb"},
 {"prefix-mouse-off",1,0,1,1,0,0,64,"ab"},
 {"prefix-mouse-on",1,1,1,1,0,0,64,"a\033[<64;2;2Mb"},
 {"storm-mouse-off",1,0,0,3,0,0,64,"b"},
 {"storm-mouse-on",1,1,0,3,0,0,64,"\033[<64;2;2Mb"}
};
static const st_case *st_find_case(const char *name){
 for(size_t i=0;i<sizeof st_cases/sizeof st_cases[0];++i)if(!strcmp(name,st_cases[i].name))return &st_cases[i];
 return NULL;
}
typedef struct {int64_t count;uint32_t requested,error;} st_read_result;
typedef struct {
 uint32_t magic,configured,finished,result,input_mode,output_mode;
 uint32_t canonical,vmin,vtime,mouse,focus,select_called,select_result,select_error;
 uint32_t read_count,output_count,termios_restored;
 st_read_result reads[ST_MAX_READS];unsigned char output[ST_MAX_OUTPUT];
} st_result;
_Static_assert(offsetof(st_result,reads)==72,"shared result read offset");
_Static_assert(sizeof(st_read_result)==16 && sizeof(st_result)==456,"shared result wire size");
static int st_decimal(const char *s,uint64_t *value){
 uint64_t n=0;if(!s||!*s)return 0;
 for(;*s;++s){if(*s<'0'||*s>'9')return 0;unsigned d=(unsigned)(*s-'0');if(n>(UINT64_MAX-d)/10)return 0;n=n*10+d;}
 if(!n)return 0;
 *value=n;return 1;
}
#endif
