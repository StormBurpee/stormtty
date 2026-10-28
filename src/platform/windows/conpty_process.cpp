#include "stormtty/process.hpp"
#include "process_control.hpp"

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
#include <bcrypt.h>
#include <tlhelp32.h>
#include <set>
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
using windows::detail::count;
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
// Serializes host creation/identity verification across our Process instances.
// Unrelated client launches are permitted; zero or multiple matching new hosts
// fail closed. This remains an explicit qualification path, not a default.
std::mutex provider_launch_mutex;
struct Module {
    HMODULE value{};
    ~Module() { if (value) FreeLibrary(value); }
    Module() = default;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
};
std::set<DWORD> direct_children() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) throw win_error("Process snapshot");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    std::set<DWORD> result;
    if (!Process32FirstW(snapshot.value, &entry)) throw win_error("Process32FirstW");
    do { if (entry.th32ParentProcessID == GetCurrentProcessId()) result.insert(entry.th32ProcessID); }
    while (Process32NextW(snapshot.value, &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) throw win_error("Process32NextW");
    return result;
}
std::wstring normalized_path(std::wstring path) {
    if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    for (auto& c : path) if (c == L'/') c = L'\\';
    return path;
}
void verify_pinned_file(const std::wstring& path, const char* expected, Handle& lock) {
    // Deny modification/deletion until the module and its host have closed.
    lock.reset(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (lock.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Bundled console runtime is missing or inaccessible. Restore runtimes/conpty beside StormTTY, or select System in Settings. No fallback was launched.");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(lock.value, &info)) throw win_error("Inspect pinned ConPTY file");
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("ConPTY file must be a regular non-reparse file");
    if (info.nFileSizeHigh || !info.nFileSizeLow || info.nFileSizeLow > 2 * 1024 * 1024)
        throw std::runtime_error("Invalid pinned ConPTY file size");
    std::vector<unsigned char> bytes(info.nFileSizeLow);
    DWORD read{};
    if (!ReadFile(lock.value, bytes.data(), info.nFileSizeLow, &read, nullptr) || read != info.nFileSizeLow)
        throw win_error("Read pinned ConPTY file");
    Module crypto;
    crypto.value = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!crypto.value) throw win_error("Load system bcrypt");
    const auto open = reinterpret_cast<decltype(&BCryptOpenAlgorithmProvider)>(GetProcAddress(crypto.value, "BCryptOpenAlgorithmProvider"));
    const auto hash = reinterpret_cast<decltype(&BCryptHash)>(GetProcAddress(crypto.value, "BCryptHash"));
    const auto close = reinterpret_cast<decltype(&BCryptCloseAlgorithmProvider)>(GetProcAddress(crypto.value, "BCryptCloseAlgorithmProvider"));
    if (!open || !hash || !close) throw std::runtime_error("System SHA256 APIs unavailable");
    BCRYPT_ALG_HANDLE algorithm{};
    if (open(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("Open SHA256 failed");
    std::array<unsigned char,32> digest{};
    const auto status = hash(algorithm, nullptr, 0, bytes.data(), read, digest.data(), static_cast<ULONG>(digest.size()));
    close(algorithm, 0);
    if (status < 0) throw std::runtime_error("SHA256 failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string actual;
    for (auto c : digest) { actual += hex[c >> 4]; actual += hex[c & 15]; }
    if (actual != expected) throw std::runtime_error("Bundled console runtime SHA256 mismatch. Restore the pinned bundle, or select System in Settings. No fallback was launched.");
}
struct Provider {
    using Create = HRESULT (WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
    using Resize = HRESULT (WINAPI*)(HPCON, COORD);
    using Close = void (WINAPI*)(HPCON);
    std::unique_lock<std::mutex> launch_lock{provider_launch_mutex};
    Handle dll_lock, host_lock, verified_host;
    Module module; // Released before pinned file locks, after HPCON lifetime.
    Create create{};
    Resize resize{};
    Close close{};
    bool packaged{};
    std::wstring host_path;
    std::set<DWORD> children_before;
    explicit Provider(const ProcessOptions& options) {
        packaged = options.provider == ProcessProvider::windows_packaged_experimental;
        if (packaged) {
#if !defined(_M_X64) && !defined(__x86_64__)
            throw std::runtime_error("Experimental pinned ConPTY package is x64-only");
#endif
            SYSTEM_INFO native{};
            GetNativeSystemInfo(&native);
            if (native.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64)
                throw std::runtime_error("Bundled ConPTY preview requires native AMD64 Windows; ARM64/emulation is not qualified. Select System in Settings.");
            auto directory = widen(options.provider_directory);
            if (!absolute_path(directory) || directory.starts_with(L"\\\\"))
                throw std::invalid_argument("Packaged ConPTY requires an absolute local drive directory");
            while (!directory.empty() && (directory.back() == L'\\' || directory.back() == L'/')) directory.pop_back();
            host_path = directory + L"\\OpenConsole.exe";
            const auto dll_path = directory + L"\\conpty.dll";
            // Host is checked first: missing packaging fails before any DLL code
            // executes or pseudoconsole/child is created. No fallback is allowed.
            verify_pinned_file(host_path, "3d66b23d0a71bb8eed2b77edc8b9df9bf54ce6c8fb74c863a30e760997f80586", host_lock);
            verify_pinned_file(dll_path, "feeef341d891643c62d30b6b07800bc70f0bc148f44cb8c3bee84aa557ae805a", dll_lock);
            std::array<wchar_t,32768> final_path{};
            const DWORD length = GetFinalPathNameByHandleW(host_lock.value, final_path.data(), static_cast<DWORD>(final_path.size()), FILE_NAME_NORMALIZED);
            if (!length || length >= final_path.size()) throw win_error("Resolve pinned OpenConsole path");
            host_path = normalized_path(std::wstring(final_path.data(), length));
            module.value = LoadLibraryExW(dll_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module.value) throw win_error("Load pinned ConPTY DLL");
            create = reinterpret_cast<Create>(GetProcAddress(module.value, "ConptyCreatePseudoConsole"));
            resize = reinterpret_cast<Resize>(GetProcAddress(module.value, "ConptyResizePseudoConsole"));
            close = reinterpret_cast<Close>(GetProcAddress(module.value, "ConptyClosePseudoConsole"));
            children_before = direct_children();
        } else {
            if (options.provider != ProcessProvider::windows_inbox_basic && options.provider != ProcessProvider::platform_default)
                throw std::invalid_argument("unsupported Windows process provider");
            if (!options.provider_directory.empty()) throw std::invalid_argument("Provider directory requires explicit experimental selection");
            HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
            if (!kernel) throw win_error("GetModuleHandleW(kernel32)");
            create = reinterpret_cast<Create>(GetProcAddress(kernel, "CreatePseudoConsole"));
            resize = reinterpret_cast<Resize>(GetProcAddress(kernel, "ResizePseudoConsole"));
            close = reinterpret_cast<Close>(GetProcAddress(kernel, "ClosePseudoConsole"));
        }
        if (!create || !resize || !close) throw std::runtime_error("Selected ConPTY provider APIs unavailable");
    }
    void verify_host() {
        if (packaged) {
            const auto after = direct_children();
            std::vector<DWORD> added;
            std::set_difference(after.begin(), after.end(), children_before.begin(), children_before.end(), std::back_inserter(added));
            std::size_t matching = 0;
            for (const auto pid : added) {
                Handle candidate(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
                if (!candidate.value) {
                    // An unrelated child can finish between snapshot and query.
                    // Any other uncertainty must not become identity success.
                    if (GetLastError() == ERROR_INVALID_PARAMETER) continue;
                    throw win_error("Open new child identity");
                }
                std::array<wchar_t,32768> image{}; DWORD length = static_cast<DWORD>(image.size());
                if (!QueryFullProcessImageNameW(candidate.value, 0, image.data(), &length))
                    throw win_error("Read new child image");
                const auto actual = normalized_path(std::wstring(image.data(), length));
                if (CompareStringOrdinal(actual.c_str(), -1, host_path.c_str(), -1, TRUE) == CSTR_EQUAL) {
                    ++matching;
                    verified_host.reset(std::exchange(candidate.value, nullptr));
                }
            }
            if (matching != 1)
                throw std::runtime_error("Experimental ConPTY requires exactly one matching new OpenConsole; fallback/ambiguous host rejected");
        }
        launch_lock.unlock();
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
    struct ResizeRequest { ProcessGridSize grid; std::uint64_t generation; };
    std::optional<ResizeRequest> resize;
    bool done{};
    std::string failure;
    ProcessDiagnostics diagnostic{"Windows ConPTY (not started)", "Provider selection pending", false, false, 0};
    Handle input_write, output_read, control;
    // Duplicated real Windows thread handles, not implementation-specific
    // std::thread::native_handle values. Kept until after workers are joined.
    Handle reader_cancel, writer_cancel;

    Impl(ProcessOptions o, ProcessCallbacks c) : options(std::move(o)), callbacks(std::move(c)) {
        // Auto-reset, initially nonsignalled, and owned until all workers release
        // Impl. No worker/caller can reset or close a handle another wait uses.
        control.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!control.value) throw win_error("CreateEvent(ConPTY control)");
        diagnostic.windows_control.emplace();
        diagnostic.windows_control->requested_grid = options.grid;
    }
    // Every flag read by a changed.wait predicate must be published while
    // holding this mutex. Atomic storage alone does not prevent a notification
    // falling between a predicate check and the wait's unlock-and-sleep step.
    // Keep callbacks, stop requests and joins outside this short critical section.
    void publish_flag(std::atomic<bool>& flag) noexcept {
        { std::lock_guard lock(mutex); flag.store(true); }
        changed.notify_all();
    }
    void close() noexcept {
        cancel_output_requested.store(true);
        publish_flag(closing);
        SetEvent(control.value);
        auto p = phase.load();
        while (p == ProcessState::starting || p == ProcessState::running)
            if (phase.compare_exchange_weak(p, ProcessState::close_requested)) break;
        // stop-token callbacks can run inline: keep them off the UI/caller.
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
        if (!register_cancel_handle(reader_cancel)) { publish_flag(reader_done); return; }
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
        publish_flag(reader_done);
    }
    void write_loop() noexcept {
        if (!register_cancel_handle(writer_cancel)) { publish_flag(writer_done); return; }
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
        publish_flag(writer_done);
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
                publish_flag(complete);
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
            Provider provider(options);
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
                    diagnostic.provider = provider.packaged ? "Windows ConPTY (packaged experimental, host verification pending)" : "Windows ConPTY (system)";
                    diagnostic.detail = provider.packaged ? "Experimental pinned provider loaded; host identity has not yet been verified." : "kernel32 Create/Resize/ClosePseudoConsole; OS-managed conhost. Bundled Microsoft.Windows.Console.ConPTY is NOT in use. Kitty/Ink/graphics transport remains unverified.";
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
                    { std::lock_guard lock(mutex); diagnostic.windows_control->applied_grid = options.grid; }
                    provider.verify_host();
                    if (provider.packaged) {
                        std::lock_guard lock(mutex);
                        diagnostic.provider = "Windows ConPTY (packaged experimental 1.25.260930003; OpenConsole identity verified)";
                        diagnostic.detail = "Pinned x64 DLL/host SHA256 verified; matching direct-child image verified; flags=0; isolated diagnostic only; advanced protocols unverified.";
                        diagnostic.bundled_provider = true;
                    }
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
                    // Explicit null standard-handle slots prevent a redirected
                    // parent stdin/stdout/stderr (for example CTest's pipes)
                    // from overriding the pseudoconsole's client handles.
                    // Console initialization fills null slots from the attached
                    // ConPTY. Keep bInheritHandles=FALSE; do NOT pass the host's
                    // transport pipe handles as the child's standard handles.
                    // This is also the launch pattern used by Microsoft Terminal:
                    // src/cascadia/TerminalConnection/ConptyConnection.cpp.
                    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                    startup.StartupInfo.hStdInput = nullptr;
                    startup.StartupInfo.hStdOutput = nullptr;
                    startup.StartupInfo.hStdError = nullptr;
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
                    const HANDLE events[]{process.value, control.value};
                    for (;;) {
                        { std::lock_guard lock(mutex); count(diagnostic.windows_control->wait_calls); }
                        // Child exit takes precedence if both handles are ready.
                        // No periodic polling while running; teardown below keeps
                        // its separate, finite drain/cancellation deadlines.
                        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                        if (wait == WAIT_FAILED) throw win_error("WaitForMultipleObjects(child/control)");
                        std::optional<ResizeRequest> next;
                        {
                            std::lock_guard lock(mutex);
                            if (wait == WAIT_OBJECT_0) {
                                count(diagnostic.windows_control->child_exit_wakes); break;
                            }
                            if (wait != WAIT_OBJECT_0 + 1) throw std::runtime_error("Unexpected ConPTY controller wait result");
                            count(diagnostic.windows_control->control_wakes);
                            if (closing.load()) { requested = true; break; }
                            next = std::exchange(resize, std::nullopt);
                            if (next) count(diagnostic.windows_control->resize_calls);
                        }
                        if (next) {
                            hr = provider.resize(console.value, {static_cast<SHORT>(next->grid.columns), static_cast<SHORT>(next->grid.rows)});
                            {
                                std::lock_guard lock(mutex);
                                windows::detail::record_resize_result(*diagnostic.windows_control,
                                    next->generation, next->grid, SUCCEEDED(hr));
                            }
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
            publish_flag(closing);
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
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->closing.load() || impl_->done) return false;
        auto& state = *impl_->diagnostic.windows_control;
        // Never wrap a generation and falsely acknowledge a newer request.
        if (state.requested_resize_generation == std::numeric_limits<std::uint64_t>::max()) return false;
        ++state.requested_resize_generation; state.requested_grid = size;
        impl_->resize = Impl::ResizeRequest{size, state.requested_resize_generation};
    }
    // Publish first, signal second, both without native calls under the mutex.
    // An auto-reset event retains a signal until a wait consumes it. If requests
    // coalesce before that wait, the newest grid remains in resize. If a producer
    // publishes while the controller applies an older resize, its signal remains
    // for the next wait. A delayed signal can cause a harmless empty wake, but
    // cannot erase newer work: there is no explicit ResetEvent race.
    SetEvent(impl_->control.value);
    return true;
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
