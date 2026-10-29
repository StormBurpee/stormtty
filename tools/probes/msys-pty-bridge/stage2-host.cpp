// Qualification only: real pinned TerminalCore, plain pipes, MSYS forkpty.
// No outer ConPTY and no invented VT replies. All write_pty effects round-trip.
#define NOMINMAX
#include <windows.h>
#include "stormtty/terminal.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
using namespace std::chrono_literals;
using stormtty::TerminalCore;
namespace {
void need(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Handle {
    HANDLE h{};
    ~Handle() { reset(); }
    void reset(HANDLE v=nullptr) { if (h && h!=INVALID_HANDLE_VALUE) CloseHandle(h); h=v; }
    Handle()=default; Handle(const Handle&)=delete; Handle& operator=(const Handle&)=delete;
};
void pipe(Handle& r, Handle& w, bool parent_reads) {
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    need(CreatePipe(&r.h,&w.h,&sa,65536)!=0,"create pipe");
    need(SetHandleInformation(parent_reads?r.h:w.h,HANDLE_FLAG_INHERIT,0)!=0,"pipe inheritance");
}
struct Bridge {
    Handle in_r,in_w,out_r,out_w,err_r,err_w,ctl_r,ctl_w,job,process;
    std::mutex mutex; std::condition_variable cv; std::deque<std::string> queue;
    std::size_t queued{}; bool stop{}, writer_failed{}; std::thread writer;
    bool cleaned{}; std::uint64_t output_bytes{}, response_bytes{}, response_count{}, cursor_reports{};
    std::string errors; TerminalCore terminal;
    const std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    Bridge() { pipe(in_r,in_w,false);pipe(out_r,out_w,true);pipe(err_r,err_w,true);pipe(ctl_r,ctl_w,false); }
    void launch(const std::wstring& broker) {
        need(!broker.empty() && broker.find(L'"')==std::wstring::npos,"broker path");
        job.reset(CreateJobObjectW(nullptr,nullptr));need(job.h!=nullptr,"create job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        need(SetInformationJobObject(job.h,JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=0,"job policy");
        SIZE_T count=0;InitializeProcThreadAttributeList(nullptr,1,0,&count);
        std::vector<unsigned char> storage(count);
        auto* list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        need(InitializeProcThreadAttributeList(list,1,0,&count)!=0,"attribute list");
        struct AttrGuard { LPPROC_THREAD_ATTRIBUTE_LIST p; ~AttrGuard(){DeleteProcThreadAttributeList(p);} } guard{list};
        HANDLE handles[]{in_r.h,out_w.h,err_w.h,ctl_r.h};
        need(UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr)!=0,"handle allowlist");
        STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput=in_r.h;si.StartupInfo.hStdOutput=out_w.h;si.StartupInfo.hStdError=err_w.h;si.lpAttributeList=list;
        auto cmd=L"\""+broker+L"\" --control-handle "+std::to_wstring(reinterpret_cast<std::uintptr_t>(ctl_r.h))+L" -- /usr/bin/zsh.exe -d -i";
        PROCESS_INFORMATION pi{};
        need(CreateProcessW(broker.c_str(),cmd.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&pi)!=0,"launch broker");
        process.reset(pi.hProcess);Handle thread;thread.h=pi.hThread;
        if (!AssignProcessToJobObject(job.h,process.h)) { TerminateProcess(process.h,125);throw std::runtime_error("assign job"); }
        need(ResumeThread(thread.h)!=DWORD(-1),"resume broker");
        in_r.reset();out_w.reset();err_w.reset();ctl_r.reset();
        writer=std::thread([this] {
            for (;;) {
                std::string bytes;
                { std::unique_lock lock(mutex);cv.wait(lock,[this]{return stop||!queue.empty();});
                  if(stop) return;bytes=std::move(queue.front());queue.pop_front(); }
                std::size_t offset=0;
                while(offset<bytes.size()) {
                    DWORD written=0;
                    if(!WriteFile(in_w.h,bytes.data()+offset,static_cast<DWORD>(bytes.size()-offset),&written,nullptr)||!written) {
                        std::lock_guard lock(mutex);writer_failed=true;return;
                    }
                    offset+=written;
                }
                {std::lock_guard lock(mutex);queued-=bytes.size();}
            }
        });
    }
    void send(std::string bytes) {
        if(bytes.empty()) return;
        std::lock_guard lock(mutex);need(!writer_failed&&!stop,"writer closed");
        need(bytes.size()<=1048576-queued,"input bound");queued+=bytes.size();queue.push_back(std::move(bytes));cv.notify_one();
    }
    void drain(Handle& h,bool vt) {
        // At most 64 KiB per pump: control/state tests cannot starve on output.
        DWORD available=0;
        if(!PeekNamedPipe(h.h,nullptr,0,nullptr,&available,nullptr)) {need(GetLastError()==ERROR_BROKEN_PIPE,"peek pipe");return;}
        if(!available)return;
        std::array<char,65536> bytes{};DWORD read=0;
        need(ReadFile(h.h,bytes.data(),std::min<DWORD>(available,static_cast<DWORD>(bytes.size())),&read,nullptr)!=0,"read pipe");
        if(vt) {
            output_bytes+=read;need(output_bytes<=8*1024*1024,"output work bound");
            terminal.feed(std::string_view(bytes.data(),read));
            for(auto& effect:terminal.take_effects()) if(effect.kind==stormtty::EffectKind::write_pty) {
                response_count++;response_bytes+=effect.text.size();
                // Observe (never invent) the adapter's actual cursor report.
                const auto& reply=effect.text;
                if(reply.size()>=6 && reply.starts_with("\x1b[") && reply.back()=='R') {
                    bool valid=true;unsigned semicolons=0,digits=0;
                    for(std::size_t i=2;i+1<reply.size();i++) {
                        if(reply[i]>='0'&&reply[i]<='9')digits++;
                        else if(reply[i]==';'&&digits&&semicolons==0){semicolons++;digits=0;}
                        else valid=false;
                    }
                    if(valid&&semicolons==1&&digits)cursor_reports++;
                }need(response_bytes<=65536,"response bound");send(std::move(effect.text));
            }
        } else {need(errors.size()+read<=65536,"stderr bound");errors.append(bytes.data(),read);}
    }
    void pump() {need(std::chrono::steady_clock::now()-started<25s,"host deadline");drain(out_r,true);drain(err_r,false);Sleep(2);}
    std::string screen() {
        std::string value;
        const auto frame=terminal.snapshot();
        for(const auto& row:frame->lines) {for(const auto& cell:row->cells) if(cell.span) value+=cell.grapheme.empty()?" ":cell.grapheme;value+='\n';}
        return value;
    }
    void expect(const std::string& marker,std::chrono::milliseconds budget=4s) {
        auto until=std::chrono::steady_clock::now()+budget;
        do {pump();if(screen().find(marker)!=std::string::npos)return;}while(std::chrono::steady_clock::now()<until);
        throw std::runtime_error("missing fixed marker: "+marker);
    }
    void ready(unsigned sequence,int status=0) {expect("__BRIDGE_READY:"+std::to_string(sequence)+":"+std::to_string(status)+"__");}
    void key(stormtty::Key key,std::uint16_t mods=0) {stormtty::KeyInput i;i.key=key;i.modifiers=mods;send(terminal.encode_key(i));}
    void command(std::string text) {send(std::move(text));key(stormtty::Key::enter);}
    void resize(unsigned columns,unsigned rows) {
        terminal.resize({static_cast<std::uint16_t>(columns),static_cast<std::uint16_t>(rows),8,16});
        unsigned char frame[]{'S','T',1,1,static_cast<unsigned char>(columns),static_cast<unsigned char>(columns>>8),static_cast<unsigned char>(rows),static_cast<unsigned char>(rows>>8)};
        DWORD written=0;need(WriteFile(ctl_w.h,frame,sizeof(frame),&written,nullptr)&&written==sizeof(frame),"resize pipe");
    }
    void settle() {auto until=std::chrono::steady_clock::now()+200ms;do{pump();}while(std::chrono::steady_clock::now()<until);}
    DWORD await_exit() {
        auto until=std::chrono::steady_clock::now()+4s;
        while(WaitForSingleObject(process.h,0)==WAIT_TIMEOUT) {need(std::chrono::steady_clock::now()<until,"exit deadline");pump();}
        drain(out_r,true);DWORD code=0;need(GetExitCodeProcess(process.h,&code)!=0,"exit status");return code;
    }
    bool cleanup() noexcept {
        if(cleaned)return true;cleaned=true;bool ok=true;
        {std::lock_guard lock(mutex);stop=true;cv.notify_one();}
        if(job.h) {
            if(!TerminateJobObject(job.h,125))ok=false;
            auto until=GetTickCount64()+3000;
            for(;;) {JOBOBJECT_BASIC_ACCOUNTING_INFORMATION a{};
                if(!QueryInformationJobObject(job.h,JobObjectBasicAccountingInformation,&a,sizeof(a),nullptr)){ok=false;break;}
                if(!a.ActiveProcesses)break;
                if(GetTickCount64()>=until){ok=false;break;}Sleep(10);
            }
        }
        if(writer.joinable()) {
            CancelSynchronousIo(writer.native_handle());
            if(WaitForSingleObject(writer.native_handle(),2000)!=WAIT_OBJECT_0) {
                // Fail closed. Own process death closes its non-inherited Job.
                TerminateProcess(GetCurrentProcess(),125);
            }
            writer.join();
        }
        if(process.h&&WaitForSingleObject(process.h,2000)!=WAIT_OBJECT_0)ok=false;
        return ok;
    }
    ~Bridge(){cleanup();}
};
}
#ifndef STORMTTY_BRIDGE_NO_MAIN
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 125;
    unsigned phase=0;bool ok=false;Bridge b;
    try {
        b.launch(argv[1]);b.ready(1);phase=1;
        b.command("bridge_tty");b.expect("__BRIDGE_TTY_OK__");b.ready(2);phase=2;
        b.send(b.terminal.encode_paste("bridge_unicode '中🙂 pasted'"));b.key(stormtty::Key::enter);
        b.expect("__BRIDGE_UNICODE:中🙂 pasted__");b.ready(3);phase=3;
        b.send(b.terminal.encode_paste("bridge_length '"+std::string(4096,'x')+"'"));b.key(stormtty::Key::enter);
        b.expect("__BRIDGE_LENGTH:4096__");b.ready(4);
        b.send("bridge_unicode 'abXc'");b.resize(100,35);b.key(stormtty::Key::left);b.key(stormtty::Key::left);b.key(stormtty::Key::backspace);b.key(stormtty::Key::enter);
        b.expect("__BRIDGE_UNICODE:abc__");b.ready(5);phase=4;
        // Fixed schedule, not an ACK: actual shell/native size markers must match.
        for(unsigned i=0;i<10;i++)b.resize(90+i,30+i);
        b.resize(132,43);b.settle();b.command("bridge_size");b.expect("__BRIDGE_SIZE:132x43__");b.ready(6);phase=5;
        auto responses_before=b.response_count;auto cursor_before=b.cursor_reports;
        b.command("bridge_native");b.expect("__BRIDGE_NATIVE_READY:132:43__");phase=6;
        b.resize(148,47);b.settle();b.send("native-ok\r");
        b.expect("__BRIDGE_NATIVE_DONE:148:47__");b.expect("__BRIDGE_NATIVE_UNICODE:中🙂__");b.ready(7);phase=7;
        auto native_responses=b.response_count-responses_before;auto native_cursor_reports=b.cursor_reports-cursor_before;
        need(native_responses>0 && native_cursor_reports>0,"native phase emitted no real cursor report");
        b.command("bridge_size");b.expect("__BRIDGE_SIZE:148x47__");b.ready(8);phase=8;
        b.command("bridge_wait");b.expect("__BRIDGE_WAIT__");b.key(stormtty::Key::c,stormtty::control);b.ready(9,130);phase=9;
        b.command("exit 37");need(b.await_exit()==37,"shell exit code");phase=10;
        need(b.cleanup(),"owned cleanup");ok=true;
        std::cout<<"BRIDGE_STAGE2 native_response_count="<<native_responses<<" native_cursor_reports="<<native_cursor_reports<<" response_count="<<b.response_count<<" response_bytes="<<b.response_bytes<<" output_bytes="<<b.output_bytes<<" phases="<<phase<<" result=PASS\n";
    } catch(const std::exception& e) {std::cerr<<"BRIDGE_STAGE2 phase="<<phase<<" result=FAIL reason="<<e.what()<<"\n";
        std::cerr<<"FIXTURE_STDERR="<<b.errors<<"\n";
        try {std::cerr<<"FIXTURE_SCREEN="<<b.screen().substr(0,16384)<<"\n";} catch(...) {}}
    if(!b.cleanup())ok=false;
    return ok?0:1;
}

#endif
