/* Native, synthetic-only qualification launcher; never used by StormTTY.
 * No ConPTY API. Inheritance allowlist + suspended assignment to owned Job.
 * Build with a Windows C compiler, not MSYS GCC.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
static void shut(HANDLE *h) { if (*h && *h != INVALID_HANDLE_VALUE) CloseHandle(*h); *h = NULL; }
static int pipe_pair(HANDLE *read_end, HANDLE *write_end, int parent_reads) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    return CreatePipe(read_end, write_end, &sa, 0) &&
        SetHandleInformation(parent_reads ? *read_end : *write_end, HANDLE_FLAG_INHERIT, 0);
}
static int send_bytes(HANDLE h, const unsigned char *p, DWORD n) {
    DWORD done = 0; return WriteFile(h, p, n, &done, NULL) && done == n;
}
static int drain(HANDLE h, unsigned char *buffer, size_t *size, size_t capacity) {
    DWORD available = 0, got = 0;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &available, NULL)) return GetLastError() == ERROR_BROKEN_PIPE;
    if (!available) return 1;
    if (available > capacity - *size) return 0;
    if (!ReadFile(h, buffer + *size, available, &got, NULL)) return 0;
    *size += got; return 1;
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 4 || wcschr(argv[1], L'"') || wcschr(argv[2], L'"') || argv[2][0] != L'/') return 125;
    const wchar_t *mode = argv[3];
    if (wcscmp(mode,L"raw") && wcscmp(mode,L"bytes") && wcscmp(mode,L"resize") &&
        wcscmp(mode,L"signal") && wcscmp(mode,L"eof") && wcscmp(mode,L"flood")) return 125;
    static wchar_t full[32768];
    DWORD chars = GetFullPathNameW(argv[1], 32768, full, NULL);
    if (!chars || chars >= 32768 || wcscmp(full, argv[1])) return 125;
    HANDLE in_r=NULL,in_w=NULL,out_r=NULL,out_w=NULL,err_r=NULL,err_w=NULL,ctl_r=NULL,ctl_w=NULL,job=NULL;
    PROCESS_INFORMATION pi = {0}; STARTUPINFOEXW si; ZeroMemory(&si,sizeof si);
    SIZE_T attr_size=0; int ok=0, attributes_initialized=0;
    static unsigned char output[1024*1024], errors[65536]; size_t output_size=0,error_size=0;
    if (!pipe_pair(&in_r,&in_w,0) || !pipe_pair(&out_r,&out_w,1) ||
        !pipe_pair(&err_r,&err_w,1) || !pipe_pair(&ctl_r,&ctl_w,0)) goto end;
    job=CreateJobObjectW(NULL,NULL); if (!job) goto end;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits; ZeroMemory(&limits,sizeof limits);
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits)) goto end;
    InitializeProcThreadAttributeList(NULL,1,0,&attr_size);
    si.lpAttributeList=HeapAlloc(GetProcessHeap(),0,attr_size); if (!si.lpAttributeList) goto end;
    if (!InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&attr_size)) goto end;
    attributes_initialized=1;
    HANDLE handles[]={in_r,out_w,err_w,ctl_r};
    if (!UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof handles,NULL,NULL)) goto end;
    si.StartupInfo.cb=sizeof si; si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput=in_r; si.StartupInfo.hStdOutput=out_w; si.StartupInfo.hStdError=err_w;
    static wchar_t command[32768];
    int n=swprintf(command,32768,L"\"%ls\" --control-handle %llu -- \"%ls\" %ls",argv[1],(unsigned long long)(uintptr_t)ctl_r,argv[2],mode);
    if (n<0 || n>=32768) goto end;
    if (!CreateProcessW(argv[1],command,NULL,NULL,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&si.StartupInfo,&pi)) goto end;
    if (!AssignProcessToJobObject(job,pi.hProcess)) { TerminateProcess(pi.hProcess,125); goto end; }
    if (ResumeThread(pi.hThread)==(DWORD)-1) goto end;
    shut(&in_r);shut(&out_w);shut(&err_w);shut(&ctl_r);shut(&pi.hThread);
    ULONGLONG start=GetTickCount64(), resized_at=0; int sent=0, resized=0;
    const unsigned char payload[]={27,'[','D',27,'[','2','0','0','~',0xe4,0xb8,0xad,0xf0,0x9f,0x99,0x82,0,255,13,10,27,'[','2','0','1','~'};
    for (;;) {
        if (GetTickCount64()-start>10000) goto end;
        if (!drain(out_r,output,&output_size,sizeof output) || !drain(err_r,errors,&error_size,sizeof errors)) goto end;
        if (!sent && output_size>=6 && !memcmp(output,"READY\n",6)) {
            if (!wcscmp(mode,L"raw")) {
                unsigned char p[4+sizeof payload]={sizeof payload,0,0,0}; memcpy(p+4,payload,sizeof payload);
                if (!send_bytes(in_w,p,(DWORD)sizeof p)) goto end; sent=1;
            } else if (!wcscmp(mode,L"signal") || !wcscmp(mode,L"eof")) {
                unsigned char c=!wcscmp(mode,L"signal")?3:4;
                if (!send_bytes(in_w,&c,1)) goto end; sent=1;
            } else if (!wcscmp(mode,L"resize")) {
                if (!resized) {
                    const unsigned char frame[]={'S','T',1,1,132,0,43,0};
                    if (!send_bytes(ctl_w,frame,(DWORD)sizeof frame)) goto end;
                    resized=1;resized_at=GetTickCount64();
                } else if (GetTickCount64()-resized_at>=100) {
                    const unsigned char c='x'; if (!send_bytes(in_w,&c,1)) goto end;sent=1;
                }
            } else if (!wcscmp(mode,L"flood")) {
                const unsigned char frame[]={'S','T',1,2,0,0,0,0};
                if (!send_bytes(ctl_w,frame,(DWORD)sizeof frame)) goto end;sent=1;
            }
        }
        DWORD result=WaitForSingleObject(pi.hProcess,10);
        if (result==WAIT_FAILED) goto end;
        if (result==WAIT_OBJECT_0) break;
    }
    if (!drain(out_r,output,&output_size,sizeof output) || !drain(err_r,errors,&error_size,sizeof errors)) goto end;
    DWORD code; if (!GetExitCodeProcess(pi.hProcess,&code)) goto end;
    if (!wcscmp(mode,L"bytes")) {
        ok=code==0 && output_size==256;
        for (size_t i=0;i<output_size && ok;i++) if (output[i]!=(unsigned char)i) ok=0;
    } else if (!wcscmp(mode,L"raw")) ok=code==0 && output_size==6+sizeof payload && !memcmp(output+6,payload,sizeof payload);
    else if (!wcscmp(mode,L"resize")) ok=code==0 && output_size==13 && !memcmp(output+6,"132,43\n",7);
    else if (!wcscmp(mode,L"signal")) ok=code==42 && output_size==10 && !memcmp(output+6,"INT\n",4);
    else if (!wcscmp(mode,L"eof")) ok=code==0 && output_size==10 && !memcmp(output+6,"EOF\n",4);
    else ok=sent && (code==0 || code==129 || code==137);
    fprintf(stderr,"bridge mode=%ls code=%lu bytes=%zu stderr_bytes=%zu result=%s\n",mode,(unsigned long)code,output_size,error_size,ok?"PASS":"FAIL");
end:
    /* Closing owned Job kills remaining descendants even after broker exit. */
    if (job) {
        if (!TerminateJobObject(job,125)) ok=0;
        ULONGLONG until=GetTickCount64()+3000;
        for (;;) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting;
            if (!QueryInformationJobObject(job,JobObjectBasicAccountingInformation,&accounting,sizeof accounting,NULL)) { ok=0;break; }
            if (!accounting.ActiveProcesses) break;
            if (GetTickCount64()>=until) { ok=0;break; }
            Sleep(10);
        }
    }
    shut(&job);
    if (pi.hProcess && WaitForSingleObject(pi.hProcess,3000)!=WAIT_OBJECT_0) ok=0;
    shut(&pi.hProcess);shut(&pi.hThread);
    if (attributes_initialized) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (si.lpAttributeList) HeapFree(GetProcessHeap(),0,si.lpAttributeList);
    shut(&in_r);shut(&in_w);shut(&out_r);shut(&out_w);shut(&err_r);shut(&err_w);shut(&ctl_r);shut(&ctl_w);
    return ok?0:1;
}
