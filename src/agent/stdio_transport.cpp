#include "stormtty/agent_stdio_transport.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
namespace stormtty {
namespace {
#ifdef _WIN32
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle()=default; explicit Handle(HANDLE h):value(h){}
    ~Handle(){reset();} Handle(const Handle&)=delete; Handle& operator=(const Handle&)=delete;
    void reset(HANDLE h=INVALID_HANDLE_VALUE) { if(value!=INVALID_HANDLE_VALUE && value!=nullptr) CloseHandle(value); value=h; }
};
std::wstring wide(const std::string& s) {
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    if(!n) throw std::runtime_error("Invalid UTF-8 process path");
    std::wstring out(n,L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),out.data(),n); return out;
}
#else
struct Fd {
    int value{-1}; Fd()=default; explicit Fd(int v):value(v){} ~Fd(){reset();}
    Fd(const Fd&)=delete; Fd& operator=(const Fd&)=delete;
    void reset(int v=-1){if(value>=0) close(value); value=v;}
};
void make_pipe(Fd& read,Fd& write) {
    int p[2]; if(pipe(p)!=0) throw std::runtime_error("Cannot create provider pipes");
    read.reset(p[0]); write.reset(p[1]);
    if(fcntl(p[0],F_SETFD,FD_CLOEXEC)<0 || fcntl(p[1],F_SETFD,FD_CLOEXEC)<0) throw std::runtime_error("Cannot protect provider pipe inheritance");
}
#endif
}
struct AgentStdioTransport::Impl {
    std::shared_ptr<AgentService> service;
    std::function<void()> state_changed;
    void notify() noexcept { try { if(state_changed) state_changed(); } catch(...) {} }
    std::atomic_bool stopping{};
    std::mutex stop_mutex, process_mutex;
    std::thread worker;
#ifdef _WIN32
    Handle process, job, input, output, errors;
    std::thread writer;
    void write_pump() noexcept {
        try {
            while(!stopping) {
                auto state=service->snapshot();
                if(state.connection==AgentConnection::failed || state.connection==AgentConnection::disconnected) break;
                for(const auto& frame:service->take_outbound()) {
                    std::size_t offset{};
                    while(offset<frame.size() && !stopping) {
                        DWORD sent{};
                        if(!WriteFile(input.value,frame.data()+offset,static_cast<DWORD>(std::min<std::size_t>(frame.size()-offset,4096)),&sent,nullptr) || !sent) throw std::runtime_error("Provider stdin closed");
                        offset+=sent;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } catch(...) { service->disconnect("Provider stdin failed; no requests replayed"); notify(); TerminateJobObject(job.value,1); }
    }
    void spawn(const AgentStdioOptions& o) {
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE}; Handle child_in,child_out,child_err;
        HANDLE a{},b{};
        if(!CreatePipe(&a,&b,&sa,0)) throw std::runtime_error("Cannot create provider stdin");
        child_in.reset(a); input.reset(b);
        if(!CreatePipe(&a,&b,&sa,0)) throw std::runtime_error("Cannot create provider stdout");
        output.reset(a); child_out.reset(b);
        if(!CreatePipe(&a,&b,&sa,0)) throw std::runtime_error("Cannot create provider stderr");
        errors.reset(a); child_err.reset(b);
        for(auto h:{input.value,output.value,errors.value}) if(!SetHandleInformation(h,HANDLE_FLAG_INHERIT,0)) throw std::runtime_error("Cannot protect provider handles");
        SIZE_T size{}; InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<unsigned char> storage(size);
        auto* attrs=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if(!InitializeProcThreadAttributeList(attrs,1,0,&size)) throw std::runtime_error("Cannot initialize provider handle list");
        struct AttrGuard { LPPROC_THREAD_ATTRIBUTE_LIST p; ~AttrGuard(){DeleteProcThreadAttributeList(p);} } guard{attrs};
        HANDLE inherited[]{child_in.value,child_out.value,child_err.value};
        if(!UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)) throw std::runtime_error("Cannot constrain provider handle inheritance");
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb=sizeof(startup); startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput=child_in.value; startup.StartupInfo.hStdOutput=child_out.value; startup.StartupInfo.hStdError=child_err.value; startup.lpAttributeList=attrs;
        auto exe=wide(o.executable),cwd=wide(o.working_directory);
        if(exe.find(L'"')!=std::wstring::npos) throw std::runtime_error("Invalid executable path");
        auto command=L"\""+exe+L"\" app-server";
        job.reset(CreateJobObjectW(nullptr,nullptr)); if(job.value==nullptr) throw std::runtime_error("Cannot create provider process job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) throw std::runtime_error("Cannot constrain provider process lifetime");
        PROCESS_INFORMATION pi{};
        if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,cwd.c_str(),&startup.StartupInfo,&pi)) throw std::runtime_error("Cannot launch selected provider executable");
        process.reset(pi.hProcess); Handle thread(pi.hThread);
        if(!AssignProcessToJobObject(job.value,process.value)) { TerminateProcess(process.value,1); throw std::runtime_error("Cannot assign provider process job"); }
        if(ResumeThread(thread.value)==static_cast<DWORD>(-1)) { TerminateJobObject(job.value,1); throw std::runtime_error("Cannot resume provider process"); }
    }
#else
    Fd input,output,errors;
    pid_t pid{-1};
    void spawn(const AgentStdioOptions& o) {
        Fd child_in,child_out,child_err; make_pipe(child_in,input); make_pipe(output,child_out); make_pipe(errors,child_err);
#ifdef __APPLE__
        // Darwin sends pipe-write SIGPIPE to the process, not necessarily the
        // writing thread. Suppress it only on our stdin descriptor; never change
        // the application's signal disposition or unrelated PTY behavior.
        // Apple fcntl(2): F_SETNOSIGPIPE disables signal generation on EPIPE.
        if(fcntl(input.value,F_SETNOSIGPIPE,1)<0) throw std::runtime_error("Cannot protect provider stdin from SIGPIPE");
#endif
        posix_spawn_file_actions_t actions;
        if(posix_spawn_file_actions_init(&actions)!=0) throw std::runtime_error("Cannot prepare provider file actions");
        struct ActionsGuard { posix_spawn_file_actions_t* a; ~ActionsGuard(){posix_spawn_file_actions_destroy(a);} } guard{&actions};
        auto check=[](int code){if(code) throw std::runtime_error("Cannot prepare provider launch");};
        check(posix_spawn_file_actions_adddup2(&actions,child_in.value,STDIN_FILENO));
        check(posix_spawn_file_actions_adddup2(&actions,child_out.value,STDOUT_FILENO));
        check(posix_spawn_file_actions_adddup2(&actions,child_err.value,STDERR_FILENO));
        // cwd is sent explicitly in each protocol start; process working directory
        // remains the host cwd on platforms without spawn chdir. No global chdir.
#if defined(__GLIBC__) || defined(__APPLE__)
        check(posix_spawn_file_actions_addchdir_np(&actions,o.working_directory.c_str()));
#endif
        posix_spawnattr_t attr; check(posix_spawnattr_init(&attr));
        struct AttrGuard { posix_spawnattr_t* a; ~AttrGuard(){posix_spawnattr_destroy(a);} } attr_guard{&attr};
        check(posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP)); check(posix_spawnattr_setpgroup(&attr,0));
        char* argv[]{const_cast<char*>(o.executable.c_str()),const_cast<char*>("app-server"),nullptr};
        check(posix_spawn(&pid,o.executable.c_str(),&actions,&attr,argv,environ));
        for(int fd:{input.value,output.value,errors.value}) if(fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK)<0) { kill(-pid,SIGKILL); waitpid(pid,nullptr,0); pid=-1; throw std::runtime_error("Cannot make provider pipes nonblocking"); }
    }
#endif
    void pump() noexcept {
        try {
#if !defined(_WIN32) && !defined(__APPLE__)
            // Linux delivers pipe-write SIGPIPE to the writer. This worker-only
            // mask does not change the application's disposition or caller mask.
            sigset_t signals; sigemptyset(&signals); sigaddset(&signals,SIGPIPE);
            if(pthread_sigmask(SIG_BLOCK,&signals,nullptr)!=0) throw std::runtime_error("Cannot protect provider writer from SIGPIPE");
#endif
            std::array<char,16384> buffer{};
#ifndef _WIN32
            std::string writing; std::size_t offset{};
#endif
            bool eof=false;
#ifndef _WIN32
            bool stderr_eof=false;
#endif
            const auto handshake_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
            while(!stopping && !eof) {
                auto state=service->snapshot();
                if(state.connection==AgentConnection::failed || state.connection==AgentConnection::disconnected) break;
                if(state.connection==AgentConnection::connecting && std::chrono::steady_clock::now()>handshake_deadline) throw std::runtime_error("Provider handshake timed out");
#ifndef _WIN32
                if(writing.empty()) { for(auto& frame:service->take_outbound()) writing+=frame; offset=0; }
#endif
#ifdef _WIN32
                for(auto h:{output.value,errors.value}) {
                    DWORD available{};
                    if(!PeekNamedPipe(h,nullptr,0,nullptr,&available,nullptr)) { if(h==output.value) eof=true; continue; }
                    if(available) { DWORD got{}; if(!ReadFile(h,buffer.data(),static_cast<DWORD>(std::min<std::size_t>(available,buffer.size())),&got,nullptr)) { if(h==output.value) eof=true; }
                        else if(h==output.value) { service->feed_stdout({buffer.data(),got}); notify(); } }
                }
                if(WaitForSingleObject(process.value,0)==WAIT_OBJECT_0) {
                    DWORD available{}; if(!PeekNamedPipe(output.value,nullptr,0,nullptr,&available,nullptr) || !available) eof=true;
                }
                if(!eof) std::this_thread::sleep_for(std::chrono::milliseconds(10));
#else
                pollfd fds[]{{output.value,POLLIN,0},{stderr_eof?-1:errors.value,POLLIN,0},{input.value,static_cast<short>(writing.empty()?0:POLLOUT),0}};
                int ready=poll(fds,3,20); if(ready<0) { if(errno==EINTR) continue; throw std::runtime_error("Provider pipe poll failed"); }
                if(!writing.empty() && (fds[2].revents&(POLLOUT|POLLERR|POLLHUP))) {
                    auto n=write(input.value,writing.data()+offset,writing.size()-offset);
                    if(n>0) { offset+=static_cast<std::size_t>(n); if(offset==writing.size()) writing.clear(); }
                    else if(n<0 && errno!=EINTR && errno!=EAGAIN) throw std::runtime_error("Provider stdin closed");
                }
                for(int i=0;i<2;++i) if(fds[i].revents&(POLLIN|POLLHUP|POLLERR)) {
                    auto n=read(fds[i].fd,buffer.data(),buffer.size());
                    if(n>0 && i==0) { service->feed_stdout({buffer.data(),static_cast<std::size_t>(n)}); notify(); }
                    else if(n==0) { if(i==0) eof=true; else stderr_eof=true; }
                    else if(n<0 && errno!=EINTR && errno!=EAGAIN) throw std::runtime_error("Provider pipe read failed");
                }
#endif
            }
            if(service->snapshot().connection!=AgentConnection::failed) service->disconnect(stopping?"Provider stopped":"Provider stdout closed");
        } catch(const std::exception& e) { service->disconnect(e.what()); }
        catch(...) { service->disconnect("Provider transport failed; no requests replayed"); }
        notify();
        // Terminate descendants even when the provider exits/crashes first.
#ifdef _WIN32
        TerminateJobObject(job.value,1);
#else
        { std::lock_guard lock(process_mutex); if(pid>0) { kill(-pid,SIGKILL); while(waitpid(pid,nullptr,0)<0 && errno==EINTR) {} pid=-1; } }
#endif
    }
    void stop() {
        std::lock_guard lock(stop_mutex); stopping=true;
#ifdef _WIN32
        if(job.value!=INVALID_HANDLE_VALUE && job.value) TerminateJobObject(job.value,1);
        // Also cover a launch failure between CreateProcess and job assignment.
        if(process.value!=INVALID_HANDLE_VALUE && process.value && WaitForSingleObject(process.value,0)!=WAIT_OBJECT_0)
            TerminateProcess(process.value,1);
#else
        { std::lock_guard lock(process_mutex); if(pid>0) kill(-pid,SIGKILL); }
#endif
        if(worker.joinable()) worker.join();
#ifdef _WIN32
        if(writer.joinable()) writer.join();
        // TerminateJobObject/TerminateProcess initiate asynchronous termination.
        // Joining our pipe workers is NOT evidence the child released its CWD,
        // files or other process-owned resources. Observe the owned process exit
        // before stop/destruction returns, as the POSIX waitpid path already does.
        if(process.value!=INVALID_HANDLE_VALUE && process.value && WaitForSingleObject(process.value,5000)!=WAIT_OBJECT_0)
            std::fputs("StormTTY provider teardown: child exit was not observed after termination.\n",stderr);
#endif
#ifndef _WIN32
        if(pid>0) { while(waitpid(pid,nullptr,0)<0 && errno==EINTR) {} pid=-1; }
#endif
    }
    ~Impl(){stop();}
};
AgentStdioTransport::AgentStdioTransport(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
AgentStdioTransport::~AgentStdioTransport()=default;
void AgentStdioTransport::stop(){impl_->stop();}
std::unique_ptr<AgentStdioTransport> AgentStdioTransport::launch(const AgentStdioOptions& o,std::shared_ptr<AgentService> service,std::string& error) {
    bool connection_owned=false;
    try {
        if(!o.user_confirmed_launch || !service) throw std::runtime_error("Explicit provider launch confirmation required");
        if(o.executable.size()>32768 || o.working_directory.size()>32768 || o.executable.find('\0')!=std::string::npos || o.working_directory.find('\0')!=std::string::npos || !std::filesystem::path(o.executable).is_absolute() || !std::filesystem::path(o.working_directory).is_absolute()) throw std::runtime_error("Provider executable and working directory must be absolute paths");
        if(!service->connect(o.runtime_version,o.account_reference)) throw std::runtime_error("Provider connection rejected");
        connection_owned=true;
        auto impl=std::make_unique<Impl>(); impl->service=service; impl->state_changed=o.state_changed;
        impl->spawn(o); auto* ptr=impl.get(); impl->worker=std::thread([ptr]{ptr->pump();});
#ifdef _WIN32
        impl->writer=std::thread([ptr]{ptr->write_pump();});
#endif
        error.clear(); return std::unique_ptr<AgentStdioTransport>(new AgentStdioTransport(std::move(impl)));
    } catch(const std::exception& e) { error=e.what(); if(service && connection_owned) service->disconnect(error); return {}; }
}
} // namespace stormtty
