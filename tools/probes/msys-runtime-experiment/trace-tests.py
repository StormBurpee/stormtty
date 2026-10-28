#!/usr/bin/env python3
"""Portable validation only: generated patch and mocked Win32 trace sink; not MSYS ABI qualification."""
import argparse, hashlib, importlib.util, pathlib, re, subprocess, tempfile
HERE=pathlib.Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('trace_generator',HERE/'make-trace-patch.py')
gen=importlib.util.module_from_spec(spec);spec.loader.exec_module(gen)
STUB=r'''
#include <cstdint>
#include <cwchar>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <cassert>
using DWORD=uint32_t; using LONG=int32_t; using WCHAR=wchar_t; using HANDLE=int;
#define NO_COPY
#define INVALID_FILE_ATTRIBUTES 0xffffffffu
#define FILE_ATTRIBUTE_DIRECTORY 16
#define FILE_ATTRIBUTE_REPARSE_POINT 1024
#define FILE_ATTRIBUTE_NORMAL 128
#define FILE_FLAG_OPEN_REPARSE_POINT 0x200000
#define DRIVE_FIXED 3
#define FILE_APPEND_DATA 4
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define OPEN_ALWAYS 4
#define INVALID_HANDLE_VALUE -1
struct BY_HANDLE_FILE_INFORMATION { DWORD dwFileAttributes; DWORD nNumberOfLinks; };
struct pinfo_stub { int pid=99; int pgid=99; };
static pinfo_stub process;
[[maybe_unused]] static pinfo_stub* myself=&process;
static std::wstring directory=L"C:\\evidence";
static bool reparse=false, failopen=false, remote=false, hardlink=false;
static std::vector<std::string> records;
static std::mutex mutex;
static thread_local DWORD error=31;
DWORD GetLastError(){return error;}
void SetLastError(DWORD v){error=v;}
DWORD GetEnvironmentVariableW(const WCHAR*,WCHAR* dst,DWORD size){if(directory.size()>=size)return directory.size()+1;wcscpy(dst,directory.c_str());return directory.size();}
DWORD GetDriveTypeW(const WCHAR*){return remote?4:DRIVE_FIXED;}
DWORD GetFileAttributesW(const WCHAR*){error=99;return FILE_ATTRIBUTE_DIRECTORY|(reparse?FILE_ATTRIBUTE_REPARSE_POINT:0);}
LONG InterlockedCompareExchange(LONG* p,LONG v,LONG expected){return __sync_val_compare_and_swap(p,expected,v);}
DWORD GetCurrentProcessId(){return 42;}
DWORD GetCurrentThreadId(){return 7;}
int __small_swprintf(WCHAR* dst,const WCHAR*,...){va_list ap;va_start(ap,dst);return 0;}
'''
# Stub formatting signature must model Cygwin %W rather than libc wide formatting.
STUB=STUB.replace('int __small_swprintf(WCHAR* dst,const WCHAR*,...){va_list ap;va_start(ap,dst);return 0;}',r'''int __small_swprintf(WCHAR* dst,const WCHAR* fmt,...){va_list ap;va_start(ap,fmt);auto d=va_arg(ap,WCHAR*);auto pid=va_arg(ap,DWORD);int n=swprintf(dst,240,L"%ls\\resize-%u.log",d,pid);va_end(ap);return n;}
int __small_sprintf(char* dst,const char* fmt,...){va_list ap;va_start(ap,fmt);int n=vsnprintf(dst,256,fmt,ap);va_end(ap);return n;}
HANDLE CreateFileW(const WCHAR*,DWORD,DWORD,void*,DWORD,DWORD,void*){error=88;return failopen?-1:1;}
bool GetFileInformationByHandle(HANDLE,BY_HANDLE_FILE_INFORMATION* i){i->nNumberOfLinks=hardlink?2:1;i->dwFileAttributes=reparse?FILE_ATTRIBUTE_REPARSE_POINT:0;return true;}
bool WriteFile(HANDLE,const char* p,int n,DWORD* written,void*){std::lock_guard<std::mutex> l(mutex);std::string block(p,n);size_t begin=0,end;while((end=block.find('\n',begin))!=std::string::npos){records.push_back(block.substr(begin,end-begin+1));begin=end+1;}assert(begin==block.size());*written=n;return true;}
bool CloseHandle(HANDLE){return true;}
''')
MAIN=r'''
int main(int argc,char**argv){
 assert(argc==2);std::string mode=argv[1];
 if(mode=="off")directory=L"";
 if(mode=="relative")directory=L"relative";
 if(mode=="traversal")directory=L"C:\\evidence\\..\\elsewhere";
 if(mode=="alias")directory=L"C:\\evidence.\\elsewhere";
 if(mode=="reparse")reparse=true;
 if(mode=="remote")remote=true;
 if(mode=="hardlink")hardlink=true;
 if(mode=="failopen")failopen=true;
 errno=17;error=31;
 stormtty_resize_trace(1191,1,0,100,30,111,35,0,0);
 assert(errno==17 && error==31);
#ifdef TRACE_EXPECT_DISABLED
 assert(records.empty());return 0;
#endif
 if(mode!="on" && mode!="cap"){assert(records.empty());return 0;}
 assert(records.size()==2);
 if(mode=="cap"){
 std::vector<std::thread> threads;
 for(int i=0;i<4;i++)threads.emplace_back([]{for(int j=0;j<3000;j++)stormtty_resize_trace(1191,1,0,100,30,111,35,0,0);});
 for(auto&t:threads)t.join();assert(records.size()==8192);
 }
 bool seen[8193]={};
 for(auto&r:records){int v[12];assert(r.size()<256);assert(sscanf(r.c_str(),"%d %d %d %d %d %d %d %d %d %d %d %d",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7],&v[8],&v[9],&v[10],&v[11])==12);assert(v[0]>0&&v[0]<=8192&&!seen[v[0]]);seen[v[0]]=true;if(v[0]==1)assert(v[4]==11&&v[10]==99&&v[11]==99);assert(v[1]==42&&r.back()=='\n');}
}
'''
def main():
 p=argparse.ArgumentParser();p.add_argument('--source',type=pathlib.Path);a=p.parse_args()
 patch=(HERE/'0002-resize-observation-trace.patch').read_text()
 assert 'retire' not in patch and 'discard_key_events (i + 1);\n+' not in patch
 for code in (1,2,3,4,5,6,7,8,10):
  assert re.search(r'[,?] '+str(code)+r'[, :]',patch),code
 if a.source:
  expected,after=gen.generate(a.source)
  assert patch==expected
  subprocess.run(['git','apply','--check',str(HERE/'0002-resize-observation-trace.patch')],cwd=a.source,check=True)
  spec=importlib.util.spec_from_file_location('stage_trace',HERE/'stage-trace.py');stage=importlib.util.module_from_spec(spec);spec.loader.exec_module(stage)
  for f,s in after.items():assert hashlib.sha256(s.encode()).hexdigest()==stage.SOURCE_FILES_SHA256['instrumented'][pathlib.Path(f).name]
 with tempfile.TemporaryDirectory(prefix='stormtty-trace-test-') as tmp:
  cpp=pathlib.Path(tmp)/'test.cpp';exe=pathlib.Path(tmp)/'test'
  cpp.write_text(STUB+gen.IMPL+MAIN)
  subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-DSTORMTTY_MSYS_RESIZE_TRACE=1','-pthread',str(cpp),'-o',str(exe)],check=True)
  for mode in ('off','relative','traversal','alias','reparse','remote','hardlink','failopen','on','cap'):
   subprocess.run([str(exe),mode],check=True);print('PASS mocked sink:',mode)
  subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-DTRACE_EXPECT_DISABLED=1','-pthread',str(cpp),'-o',str(exe)],check=True)
  subprocess.run([str(exe),'on'],check=True);print('PASS compile-time disabled sink despite opt-in')
 print('PASS patch source pins, application, staged hashes' if a.source else 'SKIP source pins/application: provide --source PREPARED_SOURCE')
 print('NOT VERIFIED: native MSYS compilation, loader, signal scheduling, Windows filesystem behavior')
if __name__=='__main__':main()
