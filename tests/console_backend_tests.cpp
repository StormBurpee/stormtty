// Native AMD64 acceptance only. This is not an Ink/graphics compatibility test.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "core_test.hpp"
#include "stormtty/config.hpp"
#include "stormtty/process.hpp"
#include "stormtty/settings_services.hpp"
#include "stormtty/terminal.hpp"
#include "stormtty/windows_console_runtime.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#include <fcntl.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

using namespace stormtty;
using namespace std::chrono_literals;
namespace {
struct Handle {
    HANDLE value{};
    ~Handle() { if (value && value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle()=default;
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};
struct Arguments {
    int count{};
    LPWSTR* values=CommandLineToArgvW(GetCommandLineW(),&count);
    ~Arguments() { if (values) LocalFree(values); }
};
std::filesystem::path executable() {
    std::wstring value(32768,L'\0');
    const auto count=GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size()));
    CHECK(count && count<value.size()); value.resize(count); return value;
}
std::string utf8(const std::wstring& value) {
    const auto count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    CHECK(count>0); std::string result(static_cast<std::size_t>(count),'\0');
    CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count,nullptr,nullptr)==count);
    return result;
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        std::array<wchar_t,32768> base{};
        const auto count=GetTempPathW(static_cast<DWORD>(base.size()),base.data());
        CHECK(count && count<base.size());
        path=std::filesystem::path(base.data())/(L"StormTTY console \u00e9 "+std::to_wstring(GetCurrentProcessId())+L" "+std::to_wstring(GetTickCount64()));
        CHECK(std::filesystem::create_directory(path));
    }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path,ignored); }
};
struct Capture {
    std::mutex mutex;
    std::condition_variable changed;
    std::string bytes;
    std::optional<ProcessExit> exit;
    bool overflow{};
    std::string text_locked() const {
        TerminalOptions options; options.size={120,40,8,16};
        TerminalCore terminal(options); terminal.feed(bytes);
        terminal.select(0,0,119,39);
        return terminal.selection_text(65536);
    }
    std::string actor_pid() {
        std::lock_guard lock(mutex);
        const auto text=text_locked(); const auto begin=text.find("PID-"); CHECK(begin!=std::string::npos);
        const auto end=text.find_first_not_of("0123456789",begin+4);
        CHECK(end!=begin+4); return text.substr(begin,end-begin);
    }
    bool contains(std::string_view expected, std::chrono::milliseconds timeout=8s) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock,timeout,[&] {
            return !overflow && text_locked().find(expected)!=std::string::npos;
        });
    }
};
struct Actor {
    std::shared_ptr<Capture> capture=std::make_shared<Capture>();
    std::unique_ptr<Process> process;
    Actor(WindowsConsoleBackend backend, const std::filesystem::path& override_directory={}) {
        ProcessOptions options; options.executable=utf8(executable().wstring());
        options.arguments={"--console-child"}; options.grid={120,40};
        select_windows_console_provider(options,backend,override_directory);
        auto state=capture;
        process=Process::start(std::move(options),{
            [state](std::string_view bytes,std::stop_token) {
                { std::lock_guard lock(state->mutex);
                  if (state->bytes.size()+bytes.size()>65536) state->overflow=true;
                  else state->bytes.append(bytes); }
                state->changed.notify_all();
            },
            [state](ProcessExit exit) {
                { std::lock_guard lock(state->mutex); state->exit=std::move(exit); }
                state->changed.notify_all();
            }});
    }
    ~Actor() { if (process) { process->request_close(); (void)process->wait_for_exit(8s); } }
    void send(std::string_view value) {
        const auto deadline=std::chrono::steady_clock::now()+5s;
        do {
            const auto result=process->try_send(value);
            if (result==ProcessSendResult::accepted) return;
            CHECK(result==ProcessSendResult::would_block); std::this_thread::sleep_for(2ms);
        } while (std::chrono::steady_clock::now()<deadline);
        throw std::runtime_error("native console input deadline");
    }
    void ready(bool bundled) {
        CHECK(capture->contains("CONSOLE-READY"));
        CHECK(process->state()==ProcessState::running);
        const auto diagnostic=process->diagnostics();
        CHECK(diagnostic.bundled_provider==bundled);
        CHECK(!diagnostic.advanced_protocols_verified);
        if (bundled) CHECK(diagnostic.provider.find("identity verified")!=std::string::npos);
    }
    void finish() {
        send("x"); CHECK(process->wait_for_exit(8s));
        CHECK(capture->contains("CONSOLE-FINAL"));
        std::lock_guard lock(capture->mutex);
        CHECK(capture->exit && capture->exit->exit_code==0 && capture->exit->error.empty());
        CHECK(!capture->overflow);
    }
    void failed() {
        CHECK(process->wait_for_exit(8s));
        CHECK(process->state()==ProcessState::failed);
        std::lock_guard lock(capture->mutex);
        CHECK(capture->exit && !capture->exit->error.empty());
        CHECK(!process->diagnostics().bundled_provider);
        CHECK(capture->bytes.find("CONSOLE-READY")==std::string::npos);
    }
};
int console_child() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    SetConsoleCP(CP_UTF8); SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdin),_O_BINARY); _setmode(_fileno(stdout),_O_BINARY);
    DWORD mode{};
    CHECK(GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&mode));
    CHECK(SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),mode&~(ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT|ENABLE_PROCESSED_INPUT)));
    CHECK(GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE),&mode));
    CHECK(SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE),mode|ENABLE_VIRTUAL_TERMINAL_PROCESSING));
    std::printf("CONSOLE-READY PID-%lu\r\n",static_cast<unsigned long>(GetCurrentProcessId()));
    for (;;) {
        const auto value=std::getchar();
        if (value==EOF) return 2;
        if (value=='x') { std::puts("CONSOLE-FINAL"); return 0; }
        if (value=='p') std::printf("SAME-ACTOR PID-%lu\r\n",static_cast<unsigned long>(GetCurrentProcessId()));
        if (value=='r') {
            CONSOLE_SCREEN_BUFFER_INFO info{}; CHECK(GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE),&info));
            std::printf("GRID-%d-%d\r\n",info.srWindow.Right-info.srWindow.Left+1,info.srWindow.Bottom-info.srWindow.Top+1);
        }
    }
}
void save_backend(const std::filesystem::path& path,WindowsConsoleBackend backend) {
    auto snapshot=load_settings_snapshot(path); CHECK(snapshot);
    auto config=*snapshot.config; config.windows_console_backend=backend;
    const auto saved=save_settings_config(path,config,*snapshot.snapshot); CHECK(saved);
    const auto loaded=load_settings_snapshot(path); CHECK(loaded);
    CHECK(loaded.config->windows_console_backend==backend);
    // The persisted field carries an engine identity, never the staging path.
    CHECK(loaded.snapshot->bytes.find("runtimes/conpty")==std::string::npos);
    CHECK(loaded.snapshot->bytes.find("runtimes\\conpty")==std::string::npos);
}
void restart(const std::filesystem::path& binary,const std::filesystem::path& config,bool failure=false) {
    std::wstring command=L"\""+binary.wstring()+L"\" "+(failure?L"--restart-failure ":L"--restart-child ")+L"\""+config.wstring()+L"\"";
    STARTUPINFOW startup{}; startup.cb=sizeof(startup);
    PROCESS_INFORMATION info{};
    CHECK(CreateProcessW(binary.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,binary.parent_path().c_str(),&startup,&info));
    Handle process,thread; process.value=info.hProcess; thread.value=info.hThread;
    const auto wait=WaitForSingleObject(process.value,30000);
    if (wait!=WAIT_OBJECT_0) { TerminateProcess(process.value,99); WaitForSingleObject(process.value,5000); }
    CHECK(wait==WAIT_OBJECT_0); DWORD exit{};
    CHECK(GetExitCodeProcess(process.value,&exit)); CHECK(exit==0);
}
void restart_child(const std::filesystem::path& path,bool failure) {
    auto loaded=load_settings_snapshot(path); CHECK(loaded);
    CHECK(loaded.config->windows_console_backend==WindowsConsoleBackend::bundled_preview);
    Actor bundled(loaded.config->windows_console_backend);
    if (!failure) { bundled.ready(true); bundled.finish(); return; }
    bundled.failed();
    // No fallback occurred. Explicit persisted System selection remains usable.
    save_backend(path,WindowsConsoleBackend::system);
    loaded=load_settings_snapshot(path); CHECK(loaded);
    Actor recovery(loaded.config->windows_console_backend); recovery.ready(false); recovery.finish();
}
void relocated_failure(const std::filesystem::path& root,bool corrupt) {
    CHECK(std::filesystem::create_directory(root));
    const auto binary=root/executable().filename();
    std::filesystem::copy_file(executable(),binary);
    // Shared Ghostty builds need this adjacent runtime in the relocated test.
    const auto ghostty=executable().parent_path()/L"ghostty-vt.dll";
    if (std::filesystem::exists(ghostty)) std::filesystem::copy_file(ghostty,root/ghostty.filename());
    const auto runtime=root/L"runtimes"/L"conpty"/L"1.25.260930003"/L"x64";
    std::filesystem::create_directories(runtime);
    std::filesystem::copy_file(bundled_console_runtime_directory()/L"conpty.dll",runtime/L"conpty.dll");
    if (corrupt) {
        std::filesystem::copy_file(bundled_console_runtime_directory()/L"OpenConsole.exe",runtime/L"OpenConsole.exe");
        std::fstream dll(runtime/L"conpty.dll",std::ios::binary|std::ios::in|std::ios::out); CHECK(dll.good());
        dll.seekp(0); dll.put('\0'); dll.close(); CHECK(dll.good());
    }
    const auto path=root/L"config.stormtty"; save_backend(path,WindowsConsoleBackend::bundled_preview);
    restart(binary,path,true);
    const auto recovered=load_settings_snapshot(path); CHECK(recovered);
    CHECK(recovered.config->windows_console_backend==WindowsConsoleBackend::system);
}
void acceptance() {
    CHECK(native_amd64_console_supported());
    CHECK(std::filesystem::is_regular_file(bundled_console_runtime_directory()/L"conpty.dll"));
    CHECK(std::filesystem::is_regular_file(bundled_console_runtime_directory()/L"OpenConsole.exe"));
    Directory directory; const auto path=directory.path/L"config.stormtty";
    auto initial=load_settings_snapshot(path); CHECK(initial);
    CHECK(initial.config->windows_console_backend==WindowsConsoleBackend::system);
    Actor original(initial.config->windows_console_backend); original.ready(false);
    const auto* identity=original.process.get();
    const auto original_pid=original.capture->actor_pid();
    save_backend(path,WindowsConsoleBackend::bundled_preview);
    auto loaded=load_settings_snapshot(path); CHECK(loaded);
    Actor bundled(loaded.config->windows_console_backend); bundled.ready(true);
    CHECK(original.process.get()==identity && original.process->state()==ProcessState::running);
    CHECK(!original.process->diagnostics().bundled_provider);
    original.send("p"); CHECK(original.capture->contains("SAME-ACTOR "+original_pid));
    CHECK(bundled.process->request_resize({100,30}));
    const auto deadline=std::chrono::steady_clock::now()+5s;
    while (std::chrono::steady_clock::now()<deadline) {
        const auto diagnostic=bundled.process->diagnostics();
        if (diagnostic.windows_control && diagnostic.windows_control->applied_grid==ProcessGridSize{100,30}) break;
        std::this_thread::sleep_for(5ms);
    }
    CHECK(bundled.process->diagnostics().windows_control->applied_grid==ProcessGridSize{100,30});
    bundled.send("r"); CHECK(bundled.capture->contains("GRID-100-30"));
    restart(executable(),path); // A separate process reloads config, no runtime CLI.
    save_backend(path,WindowsConsoleBackend::system);
    loaded=load_settings_snapshot(path); CHECK(loaded);
    Actor system(loaded.config->windows_console_backend); system.ready(false);
    CHECK(bundled.process->state()==ProcessState::running && bundled.process->diagnostics().bundled_provider);
    // Explicit CLI override wins even over a persisted System choice.
    Actor override_actor(WindowsConsoleBackend::system,bundled_console_runtime_directory()); override_actor.ready(true); override_actor.finish();
    original.finish(); bundled.finish(); system.finish();
    Actor canceled(WindowsConsoleBackend::bundled_preview); canceled.ready(true);
    canceled.process->request_close(); CHECK(canceled.process->wait_for_exit(8s));
    { std::lock_guard lock(canceled.capture->mutex); CHECK(canceled.capture->exit && canceled.capture->exit->close_requested); }
    relocated_failure(directory.path/L"missing host \u00e9",false);
    relocated_failure(directory.path/L"corrupt dll \u00e9",true);
    std::puts("Native console backend: persisted System/Bundled/System, process restart and no-CLI discovery, unchanged live actors, CLI precedence, resize/exit/drain, relocated missing/corrupt bundle and explicit System recovery passed. Not physical Settings UI, Ink resize, or graphics qualification.");
}
}
int main() {
    return run_core_tests("console_backend (Windows native AMD64)",[] {
        Arguments args; CHECK(args.values);
        if (args.count==2 && std::wstring_view(args.values[1])==L"--console-child") {
            CHECK(console_child()==0); return;
        }
        if (args.count==3 && (std::wstring_view(args.values[1])==L"--restart-child" || std::wstring_view(args.values[1])==L"--restart-failure")) {
            restart_child(args.values[2],std::wstring_view(args.values[1])==L"--restart-failure"); return;
        }
        CHECK(args.count==1); acceptance();
    });
}
