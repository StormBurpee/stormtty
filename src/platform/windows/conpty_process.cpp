#include "stormtty/process.hpp"

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER _WIN32_WINNT
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <climits>
#include <cwchar>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace stormtty {
namespace {
using namespace std::chrono_literals;
struct LaunchCancelled {};
struct Handle {
    HANDLE value{};
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE h = nullptr) noexcept {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = h;
    }
};
std::runtime_error win_error(const char* operation, DWORD code = GetLastError()) {
    return std::runtime_error(std::string(operation) + " failed (Windows error " + std::to_string(code) + ")");
}
std::wstring widen(std::string_view text) {
    if (text.find('\0') != std::string_view::npos || text.size() > static_cast<std::size_t>(INT_MAX))
        throw std::invalid_argument("invalid UTF-8 process string");
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw win_error("UTF-8 conversion");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size))
        throw win_error("UTF-8 conversion");
    return result;
}
bool absolute_path(std::wstring_view p) {
    return (p.size() >= 3 && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) &&
            p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) ||
           (p.size() >= 3 && p[0] == L'\\' && p[1] == L'\\');
}
// Microsoft CRT argv quoting, including empty arguments and trailing backslashes.
// lpApplicationName is separately specified; no executable PATH/CWD search occurs.
std::wstring quote_argument(std::wstring_view arg) {
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') { result.append(slashes * 2 + 1, L'\\'); result += c; }
        else { result.append(slashes, L'\\'); result += c; }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result += L'"';
    return result;
}
bool valid_grid(ProcessGridSize g) { return g.columns && g.rows && g.columns <= 32767 && g.rows <= 32767; }
struct EnvironmentLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
std::vector<wchar_t> make_environment(const ProcessOptions& options) {
    std::map<std::wstring, std::wstring, EnvironmentLess> env;
    struct EnvironmentBlock {
        wchar_t* value{GetEnvironmentStringsW()};
        ~EnvironmentBlock() { if (value) FreeEnvironmentStringsW(value); }
    } inherited;
    if (!inherited.value) throw win_error("GetEnvironmentStringsW");
    for (const wchar_t* p = inherited.value; *p; p += wcslen(p) + 1) {
        std::wstring entry(p);
        // Preserve hidden drive cwd entries such as '=C:=C:\\'.
        const auto equals = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
        if (equals != std::wstring::npos) env.emplace(entry.substr(0, equals), entry.substr(equals + 1));
    }
    for (const auto& entry : options.environment) {
        auto name = widen(entry.name);
        if (name.empty() || name.find(L'=') != std::wstring::npos) throw std::invalid_argument("invalid environment name");
        if (entry.value) env[name] = widen(*entry.value); else env.erase(name);
    }
    std::vector<wchar_t> block;
    for (const auto& [name, value] : env) {
        block.insert(block.end(), name.begin(), name.end()); block.push_back(L'=');
        block.insert(block.end(), value.begin(), value.end()); block.push_back(L'\0');
    }
    if (block.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}
struct Attributes {
    std::vector<std::byte> storage;
    PPROC_THREAD_ATTRIBUTE_LIST value{};
    Attributes() {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (!size) throw win_error("InitializeProcThreadAttributeList(size)");
        storage.resize(size);
        auto p = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(p, 1, 0, &size)) throw win_error("InitializeProcThreadAttributeList");
        value = p;
    }
    ~Attributes() { if (value) DeleteProcThreadAttributeList(value); }
    Attributes(const Attributes&) = delete;
    Attributes& operator=(const Attributes&) = delete;
};
struct Provider {
    using Create = HRESULT (WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
    using Resize = HRESULT (WINAPI*)(HPCON, COORD);
    using Close = void (WINAPI*)(HPCON);
    Create create{};
    Resize resize{};
    Close close{};
    explicit Provider(ProcessProvider selection) {
        if (selection != ProcessProvider::windows_inbox_basic && selection != ProcessProvider::platform_default)
            throw std::invalid_argument("unsupported Windows process provider");
        // System module only. Never search CWD/PATH for conpty.dll or substitute
        // a downloaded DLL for Windows components. Bundled support remains gated
        // on signature, host-image and runtime qualification (see baseline ADR).
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (!kernel) throw win_error("GetModuleHandleW(kernel32)");
        create = reinterpret_cast<Create>(GetProcAddress(kernel, "CreatePseudoConsole"));
        resize = reinterpret_cast<Resize>(GetProcAddress(kernel, "ResizePseudoConsole"));
        close = reinterpret_cast<Close>(GetProcAddress(kernel, "ClosePseudoConsole"));
        if (!create || !resize || !close) throw std::runtime_error("Inbox ConPTY APIs are unavailable on this Windows version");
    }
};
struct PseudoConsole {
    HPCON value{};
    Provider& provider;
    explicit PseudoConsole(Provider& p) : provider(p) {}
    ~PseudoConsole() { reset(); }
    void reset() noexcept { if (value) { auto old = std::exchange(value, nullptr); provider.close(old); } }
    PseudoConsole(const PseudoConsole&) = delete;
    PseudoConsole& operator=(const PseudoConsole&) = delete;
};
} // namespace

struct Process::Impl : std::enable_shared_from_this<Impl> {
    ProcessOptions options;
    ProcessCallbacks callbacks;
    std::atomic<ProcessState> phase{ProcessState::starting};
    std::atomic<bool> closing{false};
    std::atomic<bool> cancel_output_requested{false};
    std::atomic<bool> stop_reader{false};
    std::atomic<bool> reader_done{false}, writer_done{false};
    std::stop_source output_stop;
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    std::deque<std::string> input;
    std::size_t queued{};
    std::optional<ProcessGridSize> resize;
    bool done{};
    std::string failure;
    ProcessDiagnostics diagnostic{"Windows ConPTY (not started)", "Provider selection pending", false, false, 0};
    Handle input_write, output_read;
    // Duplicated real Windows thread handles, not implementation-specific
    // std::thread::native_handle values. Kept until after workers are joined.
    Handle reader_cancel, writer_cancel;

    Impl(ProcessOptions o, ProcessCallbacks c) : options(std::move(o)), callbacks(std::move(c)) {}
    void close() noexcept {
        cancel_output_requested.store(true);
        closing.store(true);
        auto p = phase.load();
        while (p == ProcessState::starting || p == ProcessState::running)
            if (phase.compare_exchange_weak(p, ProcessState::close_requested)) break;
        // stop-token callbacks can run inline: keep them off the UI/caller.
        changed.notify_all();
    }
    void fail(std::string message) noexcept {
        try { std::lock_guard lock(mutex); if (failure.empty()) failure = std::move(message); } catch (...) {}
        close();
    }
    bool register_cancel_handle(Handle& target) noexcept {
        HANDLE h{};
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            fail("DuplicateHandle(worker) failed"); return false;
        }
        { std::lock_guard lock(mutex); target.reset(h); }
        changed.notify_all(); return true;
    }
    void read_loop() noexcept {
        if (!register_cancel_handle(reader_cancel)) { reader_done.store(true); changed.notify_all(); return; }
        std::array<char, 64 * 1024> buffer{};
        bool discard_after_callback_failure = false;
        while (!stop_reader.load()) {
            DWORD count = 0;
            if (!ReadFile(output_read.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
                const DWORD error = GetLastError();
                if (error != ERROR_BROKEN_PIPE && error != ERROR_OPERATION_ABORTED && !closing.load()) fail("ReadFile(ConPTY output) failed");
                break;
            }
            if (!count) break;
            if (!discard_after_callback_failure) {
                try { if (callbacks.output) callbacks.output({buffer.data(), count}, output_stop.get_token()); }
                catch (...) {
                    fail("output callback threw");
                    // ClosePseudoConsole can emit final output. Continue draining
                    // on this explicit failure path rather than deadlocking close.
                    discard_after_callback_failure = true;
                }
            }
        }
        reader_done.store(true); changed.notify_all();
    }
    void write_loop() noexcept {
        if (!register_cancel_handle(writer_cancel)) { writer_done.store(true); changed.notify_all(); return; }
        try {
            while (!closing.load()) {
                std::string bytes;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] { return closing.load() || !input.empty(); });
                    if (closing.load()) break;
                    bytes = std::move(input.front()); input.pop_front();
                }
                std::size_t offset = 0;
                while (offset < bytes.size() && !closing.load()) {
                    DWORD count = 0;
                    const auto amount = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 64 * 1024));
                    if (!WriteFile(input_write.value, bytes.data() + offset, amount, &count, nullptr)) {
                        if (!closing.load() && GetLastError() != ERROR_BROKEN_PIPE) fail("WriteFile(ConPTY input) failed");
                        goto finished;
                    }
                    if (!count) { fail("zero-byte ConPTY input write"); goto finished; }
                    offset += count;
                    { std::lock_guard lock(mutex); queued -= count; }
                }
            }
        } catch (...) { fail("ConPTY writer failed"); }
finished:
        writer_done.store(true); changed.notify_all();
    }
    void cancel_and_join(std::thread& thread, Handle& cancel, const std::atomic<bool>& finished) noexcept {
        if (!thread.joinable()) return;
        // Cancel repeatedly to cover the documented race between a worker's stop
        // check and entry into synchronous I/O. Never close its pipe handle while
        // ReadFile/WriteFile may still be using it.
        while (!finished.load()) {
            std::unique_lock lock(mutex);
            if (cancel.value) CancelSynchronousIo(cancel.value);
            changed.wait_for(lock, 10ms, [&] { return finished.load(); });
        }
        thread.join(); cancel.reset();
    }
    void close_console_with_drain(PseudoConsole& console) noexcept {
        if (!console.value) return;
        std::atomic<bool> complete{false};
        const auto hpc = console.value;
        std::thread closer;
        try {
            // A transient closer lets the controller keep servicing cancellation
            // even if a normal child exit races a blocked downstream consumer.
            closer = std::thread([&console, &complete, hpc, this] {
                console.provider.close(hpc);
                complete.store(true); changed.notify_all();
            });
            console.value = nullptr; // transferred only after thread creation
        } catch (...) {
            fail("could not create ConPTY closer worker");
            output_stop.request_stop(); console.reset(); return;
        }
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        bool timed_out = false;
        while (!complete.load()) {
            if (cancel_output_requested.load()) output_stop.request_stop();
            if (!timed_out && std::chrono::steady_clock::now() >= deadline) {
                timed_out = true;
                fail("ConPTY close exceeded drain grace period; cancelling downstream output wait");
                output_stop.request_stop();
            }
            std::unique_lock lock(mutex);
            changed.wait_for(lock, 10ms, [&] { return complete.load(); });
        }
        closer.join();
    }
    void run() noexcept {
        std::thread reader, writer;
        Handle input_read, output_write, process, primary_thread, job;
        std::optional<std::uint32_t> code;
        bool requested = false;
        // Provider and HPCON remain paired for the entire lifetime. The nested
        // scope's catch always drains before HPCON destruction, including setup
        // failure after resource acquisition and suspended-child/job failures.
        try {
            Provider provider(options.provider);
            PseudoConsole console(provider);
            try {
                auto executable = widen(options.executable);
                auto cwd = widen(options.working_directory);
                if (!absolute_path(executable) || (!cwd.empty() && !absolute_path(cwd)))
                    throw std::invalid_argument("executable and nonempty working_directory must be absolute UTF-8 paths");
                if (!valid_grid(options.grid) || !options.input_capacity_bytes)
                    throw std::invalid_argument("invalid process grid/input capacity");
                std::wstring command = quote_argument(executable);
                for (const auto& argument : options.arguments) { command += L' '; command += quote_argument(widen(argument)); }
                if (command.size() >= 32767) throw std::invalid_argument("Windows command line exceeds 32766 UTF-16 code units");
                auto environment = make_environment(options);
                {
                    std::lock_guard lock(mutex);
                    diagnostic.provider = "Windows ConPTY (system)";
                    diagnostic.detail = "kernel32 Create/Resize/ClosePseudoConsole; OS-managed conhost. Bundled Microsoft.Windows.Console.ConPTY is NOT in use. Kitty/Ink/graphics transport remains unverified.";
                }
                if (!closing.load()) {
                    HANDLE r{}, w{};
                    if (!CreatePipe(&r, &w, nullptr, 0)) throw win_error("CreatePipe(input)");
                    input_read.reset(r); input_write.reset(w);
                    if (!CreatePipe(&r, &w, nullptr, 0)) throw win_error("CreatePipe(output)");
                    output_read.reset(r); output_write.reset(w);
                    reader = std::thread([this] { read_loop(); });
                    {
                        std::unique_lock lock(mutex);
                        changed.wait(lock, [&] { return reader_cancel.value || reader_done.load(); });
                        if (reader_done.load()) throw std::runtime_error("ConPTY output pump could not start");
                    }
                    // Do not use INHERIT_CURSOR or folklore numeric passthrough
                    // flags. The basic provider has no verified width/VT contract.
                    HRESULT hr = provider.create({static_cast<SHORT>(options.grid.columns), static_cast<SHORT>(options.grid.rows)},
                                                 input_read.value, output_write.value, 0, &console.value);
                    if (FAILED(hr)) throw win_error("CreatePseudoConsole", static_cast<DWORD>(hr));
                    Attributes attributes;
                    if (!UpdateProcThreadAttribute(attributes.value, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                                   console.value, sizeof(console.value), nullptr, nullptr))
                        throw win_error("UpdateProcThreadAttribute(PSEUDOCONSOLE)");
                    job.reset(CreateJobObjectW(nullptr, nullptr));
                    if (!job.value) throw win_error("CreateJobObjectW");
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
                    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limit, sizeof(limit)))
                        throw win_error("SetInformationJobObject");
                    STARTUPINFOEXW startup{};
                    startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes.value;
                    PROCESS_INFORMATION pi{};
                    if (closing.load()) throw LaunchCancelled{};
                    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                                        environment.data(), cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &pi))
                        throw win_error("CreateProcessW");
                    process.reset(pi.hProcess); primary_thread.reset(pi.hThread);
                    if (!AssignProcessToJobObject(job.value, process.value)) throw win_error("AssignProcessToJobObject");
                    if (closing.load()) throw LaunchCancelled{};
                    if (ResumeThread(primary_thread.value) == static_cast<DWORD>(-1)) throw win_error("ResumeThread");
                    primary_thread.reset();
                    // ConPTY owns its copies; retaining these suppresses pipe EOF.
                    input_read.reset(); output_write.reset();
                    writer = std::thread([this] { write_loop(); });
                    auto expected = ProcessState::starting;
                    phase.compare_exchange_strong(expected, ProcessState::running);
                    for (;;) {
                        DWORD wait = WaitForSingleObject(process.value, 20);
                        if (wait == WAIT_OBJECT_0) break;
                        if (wait == WAIT_FAILED) throw win_error("WaitForSingleObject(child)");
                        if (closing.load()) { requested = true; break; }
                        std::optional<ProcessGridSize> next;
                        { std::lock_guard lock(mutex); next = std::exchange(resize, std::nullopt); }
                        if (next) {
                            hr = provider.resize(console.value, {static_cast<SHORT>(next->columns), static_cast<SHORT>(next->rows)});
                            if (FAILED(hr)) throw win_error("ResizePseudoConsole", static_cast<DWORD>(hr));
                        }
                    }
                }
            } catch (const LaunchCancelled&) { /* requested during startup */ }
            catch (const std::exception& e) { fail(e.what()); }
            catch (...) { fail("unknown ConPTY startup/lifecycle failure"); }
            requested = requested || closing.load();
            if (requested) output_stop.request_stop();
            phase.store(ProcessState::draining);
            closing.store(true); changed.notify_all();
            cancel_and_join(writer, writer_cancel, writer_done);
            input_write.reset();
            input_read.reset(); output_write.reset();
            // Only our job and child are terminated. In particular, never invoke
            // wsl --shutdown/--terminate or signal an entire WSL distribution.
            if (process.value && WaitForSingleObject(process.value, 0) != WAIT_OBJECT_0) {
                if (job.value) TerminateJobObject(job.value, 1);
                TerminateProcess(process.value, 1); // also handles pre-job setup failure
            }
            // Close is off UI and reader stays alive to consume the final frame.
            // Callback cancellation was signaled on explicit close/failure only;
            // a normal child exit still delivers its complete trailing output.
            close_console_with_drain(console);
            if (process.value) {
                const DWORD wait = WaitForSingleObject(process.value, 5000);
                DWORD exit_code{};
                if (wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.value, &exit_code)) code = exit_code;
                else fail("child exit could not be observed after ConPTY closure");
            }
            // Closing the provider normally breaks output. Allow final data/EOF
            // before cancellation, and report a drain timeout rather than claim
            // silent success if a stuck host fails to close the pipe.
            if (reader.joinable()) {
                std::unique_lock lock(mutex);
                bool drained = changed.wait_for(lock, 1000ms, [&] { return reader_done.load(); });
                lock.unlock();
                if (!drained) fail("ConPTY output drain timed out during teardown");
            }
            stop_reader.store(true); output_stop.request_stop();
            cancel_and_join(reader, reader_cancel, reader_done);
        } catch (const std::exception& e) { fail(e.what()); }
        catch (...) { fail("unknown ConPTY provider failure"); }
        input_write.reset(); output_read.reset(); job.reset(); process.reset(); primary_thread.reset();
        ProcessExit result;
        {
            std::lock_guard lock(mutex);
            input.clear(); queued = 0;
            result = {code, requested || cancel_output_requested.load(), failure};
            phase.store(failure.empty() ? ProcessState::exited : ProcessState::failed);
        }
        try { if (callbacks.exited) callbacks.exited(std::move(result)); } catch (...) { /* no exception crosses worker */ }
        { std::lock_guard lock(mutex); done = true; }
        changed.notify_all();
    }
};

Process::Process(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
std::unique_ptr<Process> Process::start(ProcessOptions options, ProcessCallbacks callbacks) {
    auto impl = std::make_shared<Impl>(std::move(options), std::move(callbacks));
    auto process = std::unique_ptr<Process>(new Process(impl));
    std::thread([impl] { impl->run(); }).detach();
    return process;
}
Process::~Process() { request_close(); }
ProcessSendResult Process::try_send(std::string_view bytes) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing.load() || impl_->done) return ProcessSendResult::closed;
    if (bytes.size() > impl_->options.input_capacity_bytes) return ProcessSendResult::too_large;
    if (bytes.size() > impl_->options.input_capacity_bytes - impl_->queued) return ProcessSendResult::would_block;
    if (!bytes.empty()) { impl_->input.emplace_back(bytes); impl_->queued += bytes.size(); impl_->changed.notify_all(); }
    return ProcessSendResult::accepted;
}
bool Process::request_resize(ProcessGridSize size) {
    if (!valid_grid(size)) return false;
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing.load() || impl_->done) return false;
    impl_->resize = size; impl_->changed.notify_all(); return true;
}
void Process::request_close() noexcept { impl_->close(); }
ProcessState Process::state() const noexcept { return impl_->phase.load(); }
ProcessDiagnostics Process::diagnostics() const {
    std::lock_guard lock(impl_->mutex); auto d = impl_->diagnostic; d.queued_input_bytes = impl_->queued; return d;
}
bool Process::wait_for_exit(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(impl_->mutex); return impl_->changed.wait_for(lock, timeout, [&] { return impl_->done; });
}
} // namespace stormtty
#endif
