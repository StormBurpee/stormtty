#include "stormtty/process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <csignal>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;
using stormtty::Process;
using stormtty::ProcessExit;
using stormtty::ProcessOptions;
using stormtty::ProcessSendResult;
using stormtty::ProcessState;

namespace {
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void child_terminal_mode() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    SetConsoleCP(CP_UTF8); SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY);
    DWORD mode{};
    if (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode))
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT));
    if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode))
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
    termios mode{};
    if (tcgetattr(STDIN_FILENO, &mode) == 0) { cfmakeraw(&mode); tcsetattr(STDIN_FILENO, TCSANOW, &mode); }
#endif
}
std::pair<unsigned, unsigned> child_size() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        return {static_cast<unsigned>(info.srWindow.Right - info.srWindow.Left + 1), static_cast<unsigned>(info.srWindow.Bottom - info.srWindow.Top + 1)};
#else
    winsize size{};
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0) return {size.ws_col, size.ws_row};
#endif
    return {};
}
std::string child_environment() {
#ifdef _WIN32
    DWORD length = GetEnvironmentVariableW(L"STORMTTY_PROCESS_TEST", nullptr, 0);
    if (!length) return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? "<unset>" : "";
    std::vector<wchar_t> wide(length);
    DWORD count = GetEnvironmentVariableW(L"STORMTTY_PROCESS_TEST", wide.data(), length);
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(count), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(count), result.data(), size, nullptr, nullptr);
    return result;
#else
    const char* value = std::getenv("STORMTTY_PROCESS_TEST");
    return value ? value : "<unset>";
#endif
}
int child_main(int argc, char** argv) {
    child_terminal_mode();
    std::string mode(argv[1]);
#ifdef _WIN32
    if (mode == "--child-console-handles") {
        DWORD input_mode{}, output_mode{}, error_mode{};
        const bool input_console = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &input_mode) != FALSE;
        const bool output_console = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &output_mode) != FALSE;
        const bool error_console = GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &error_mode) != FALSE;
        std::printf("CONSOLE-HANDLES:%d:%d:%d\n", input_console, output_console, error_console);
        if (!input_console || !output_console || !error_console) return 42;
        std::fputs("STDOUT-VIA-CONPTY\n", stdout);
        std::fputs("STDERR-VIA-CONPTY\n", stderr);
        if (std::getchar() != 'R') return 43;
        std::fputs("STDIN-VIA-CONPTY\n", stdout);
        return 0;
    }
#endif
    if (mode == "--child-exit") { std::fputs("STORM-TAIL-\xe2\x98\x83-\xf0\x9f\x8c\xa9\n", stdout); return 23; }
    if (mode == "--child-args") {
        for (int i = 2; i < argc; ++i) std::printf("ARG%d=[%s]\n", i - 2, argv[i]);
        std::printf("ENV=[%s]\n", child_environment().c_str());
        const auto cwd = std::filesystem::current_path().generic_string();
        std::printf("CWD=[%s]\n", cwd.c_str());
        return 0;
    }
    if (mode == "--child-flood") {
        for (int i = 0; i < 5000; ++i) std::printf("FLOOD-%05d:abcdefghijklmnopqrstuvwxyz0123456789\n", i);
        std::puts("FLOOD-DONE"); return 0;
    }
    if (mode == "--child-flood-forever") {
        for (;;) std::fputs("FLOOD-LIVE:abcdefghijklmnopqrstuvwxyz0123456789\n", stdout);
    }
    if (mode == "--child-slow") {
        std::puts("READY");
        std::this_thread::sleep_for(30s); return 0;
    }
    if (mode == "--child-resize") {
        std::puts("READY");
        for (;;) {
            const int c = std::getchar();
            if (c == EOF || c == 'x') return 0;
            const auto [columns, rows] = child_size(); std::printf("SIZE:%u:%u\n", columns, rows);
        }
    }
    std::puts("READY");
    std::string accumulated;
    for (;;) {
        int c = std::getchar();
        if (c == EOF) return 0;
        accumulated += static_cast<char>(c);
        std::putchar(c);
        if (accumulated.ends_with(":done:")) return 7;
    }
}
std::string executable_path(const char* argv0) {
#ifdef _WIN32
    std::vector<wchar_t> path(32768);
    DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    require(count > 0 && count < path.size(), "GetModuleFileNameW failed");
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(count), nullptr, 0, nullptr, nullptr);
    std::string value(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(count), value.data(), size, nullptr, nullptr);
    (void)argv0; return value;
#else
    return std::filesystem::canonical(argv0).string();
#endif
}
struct Capture {
    std::mutex mutex;
    std::condition_variable cv;
    std::string output;
    std::optional<ProcessExit> exit;
    std::size_t count{};
    bool slow{};
    void append(std::string_view bytes) {
        {
            std::lock_guard lock(mutex);
            // Test capture itself is bounded; flood-close only needs a prefix.
            count += bytes.size();
            output.append(bytes.substr(0, std::min<std::size_t>(bytes.size(), 2 * 1024 * 1024 - output.size())));
        }
        cv.notify_all();
        if (slow) std::this_thread::sleep_for(1ms);
    }
    bool contains(std::string_view text, std::chrono::milliseconds timeout = 5s) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return output.find(text) != std::string::npos; });
    }
};
struct Session {
    std::shared_ptr<Capture> capture{std::make_shared<Capture>()};
    std::unique_ptr<Process> process;
    Session(ProcessOptions options, bool slow = false, bool throw_output = false) {
        capture->slow = slow;
        auto c = capture;
        process = Process::start(std::move(options), {
            [c, throw_output](std::string_view bytes, std::stop_token) { if (throw_output) throw std::runtime_error("test callback failure"); c->append(bytes); },
            [c](ProcessExit result) { { std::lock_guard lock(c->mutex); c->exit = std::move(result); } c->cv.notify_all(); }
        });
    }
    ProcessExit finish(std::chrono::milliseconds timeout = 8s) {
        require(process->wait_for_exit(timeout), "process failed to complete before deadline");
        std::lock_guard lock(capture->mutex); require(capture->exit.has_value(), "exit callback missing"); return *capture->exit;
    }
};
void send(Process& process, std::string_view bytes) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = process.try_send(bytes);
        if (result == ProcessSendResult::accepted) return;
        require(result == ProcessSendResult::would_block, "unexpected input refusal");
        std::this_thread::sleep_for(2ms);
    }
    throw std::runtime_error("input backpressure deadline");
}
#ifdef _WIN32
// Only this serial native regression changes the test host's Win32 handle table;
// no application launch code mutates global std handles. The original handles
// remain owned by the test host and are restored even when an assertion throws.
class RedirectedParentStandardHandles final {
    struct OwnedHandle {
        HANDLE value{};
        ~OwnedHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } input_read_, input_write_, output_read_, output_write_;
    std::array<HANDLE, 3> original_{
        GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)};
    void restore() noexcept {
        SetStdHandle(STD_INPUT_HANDLE, original_[0]);
        SetStdHandle(STD_OUTPUT_HANDLE, original_[1]);
        SetStdHandle(STD_ERROR_HANDLE, original_[2]);
    }
public:
    RedirectedParentStandardHandles() {
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        require(CreatePipe(&input_read_.value, &input_write_.value, &attributes, 0) != FALSE, "redirected-parent input pipe creation failed");
        require(CreatePipe(&output_read_.value, &output_write_.value, &attributes, 0) != FALSE, "redirected-parent output pipe creation failed");
        require(SetHandleInformation(input_write_.value, HANDLE_FLAG_INHERIT, 0) != FALSE, "input peer inheritance reset failed");
        require(SetHandleInformation(output_read_.value, HANDLE_FLAG_INHERIT, 0) != FALSE, "output peer inheritance reset failed");
        try {
            require(SetStdHandle(STD_INPUT_HANDLE, input_read_.value) != FALSE, "parent stdin redirect failed");
            require(SetStdHandle(STD_OUTPUT_HANDLE, output_write_.value) != FALSE, "parent stdout redirect failed");
            require(SetStdHandle(STD_ERROR_HANDLE, output_write_.value) != FALSE, "parent stderr redirect failed");
        } catch (...) { restore(); throw; }
    }
    ~RedirectedParentStandardHandles() { restore(); }
    RedirectedParentStandardHandles(const RedirectedParentStandardHandles&) = delete;
    RedirectedParentStandardHandles& operator=(const RedirectedParentStandardHandles&) = delete;
    bool output_empty() const {
        DWORD available{};
        require(PeekNamedPipe(output_read_.value, nullptr, 0, nullptr, &available, nullptr) != FALSE, "redirected-parent output probe failed");
        return available == 0;
    }
};
#endif
void run_tests(const std::string& executable) {
    const auto options = [&](std::string mode) { ProcessOptions o; o.executable = executable; o.arguments = {std::move(mode)}; return o; };
#ifdef _WIN32
    {
        // Regression for native CTest/CI launches: bInheritHandles=FALSE alone
        // must not leave the child's stdio pointing at the parent's redirected
        // pipes. Exercise console API identity and all three actual I/O routes.
        RedirectedParentStandardHandles parent;
        Session s(options("--child-console-handles"));
        const bool ready = s.capture->contains("STDOUT-VIA-CONPTY");
        if (ready) send(*s.process, "R"); else s.process->request_close();
        const auto result = s.finish();
        require(parent.output_empty(), "ConPTY child output leaked into redirected parent stdout/stderr");
        require(ready && result.error.empty() && result.exit_code == 0, "redirected-parent ConPTY stdio regression failed");
        require(s.capture->contains("CONSOLE-HANDLES:1:1:1", 0ms), "child standard handles are not all console handles");
        require(s.capture->contains("STDERR-VIA-CONPTY", 0ms), "child stderr bypassed ConPTY");
        require(s.capture->contains("STDIN-VIA-CONPTY", 0ms), "child stdin bypassed ConPTY");
    }
#endif
    {
        Session s(options("--child-exit")); auto result = s.finish();
        require(result.error.empty() && result.exit_code == 23, "child exit code/tail launch failed");
        require(s.capture->contains("STORM-TAIL-\xe2\x98\x83-\xf0\x9f\x8c\xa9", 0ms), "UTF-8/trailing output missing");
        require(s.process->state() == ProcessState::exited, "wrong exit state");
        require(s.process->try_send("x") == ProcessSendResult::closed, "input accepted after exit");
        require(!s.process->request_resize({90, 30}), "resize accepted after exit");
        require(!s.process->diagnostics().advanced_protocols_verified, "unqualified protocol claim");
    }
    {
        auto o = options("--child-args");
        o.arguments.insert(o.arguments.end(), {"", "with spaces", "a\"b", "trailing\\", "two\\\\\"quotes"});
        o.environment = {{"STORMTTY_PROCESS_TEST", "env-value-\xe2\x98\x83"}};
        o.working_directory = std::filesystem::current_path().string();
        Session s(std::move(o)); auto result = s.finish();
        require(result.error.empty() && result.exit_code == 0, "argument child failed");
        require(s.capture->contains("ARG0=[]", 0ms), "empty argument lost");
        require(s.capture->contains("ARG1=[with spaces]", 0ms), "spaced argument lost");
        require(s.capture->contains("ARG2=[a\"b]", 0ms), "quoted argument lost");
        require(s.capture->contains("ARG3=[trailing\\]", 0ms), "trailing backslash lost");
        require(s.capture->contains("ARG4=[two\\\\\"quotes]", 0ms), "slash/quote argument lost");
        require(s.capture->contains("ENV=[env-value-\xe2\x98\x83]", 0ms), "UTF-8 environment override failed");
        require(s.capture->contains("CWD=[" + std::filesystem::current_path().generic_string() + "]", 0ms), "working directory was not applied");
    }
    {
        auto o = options("--child-args");
        o.environment = {{"STORMTTY_PROCESS_TEST", "delete-me"}, {"STORMTTY_PROCESS_TEST", std::nullopt}};
        Session s(std::move(o)); require(s.finish().error.empty(), "environment removal child failed");
        require(s.capture->contains("ENV=[<unset>]", 0ms), "environment removal failed");
    }
    {
        Session s(options("--child-echo")); require(s.capture->contains("READY"), "echo child not ready");
        send(*s.process, "ordered-"); send(*s.process, "\xe2\x98\x83-"); send(*s.process, ":done:");
        auto result = s.finish();
        require(result.error.empty() && result.exit_code == 7, "echo child exit failed");
        require(s.capture->contains("ordered-\xe2\x98\x83-:done:", 0ms), "ordered/UTF-8 input corrupted");
    }
    {
        Session s(options("--child-flood"), true); auto result = s.finish(20s);
        require(result.error.empty() && result.exit_code == 0, "flood child failed");
        require(s.capture->contains("FLOOD-DONE", 0ms), "flood final marker lost");
#ifndef _WIN32
        std::lock_guard lock(s.capture->mutex);
        for (int i = 0; i < 5000; ++i) { char marker[32]; std::snprintf(marker, sizeof(marker), "FLOOD-%05d:", i); require(s.capture->output.find(marker) != std::string::npos, "flood bytes lost"); }
#endif
    }
    {
        Session s(options("--child-resize")); require(s.capture->contains("READY"), "resize child not ready");
        require(!s.process->request_resize({0, 24}), "invalid resize accepted");
        for (unsigned i = 0; i < 100; ++i) require(s.process->request_resize({static_cast<std::uint16_t>(80 + i), 30}), "resize rejected");
        require(s.process->request_resize({120, 40}), "final resize rejected");
        bool resized = false;
        for (int i = 0; i < 50 && !resized; ++i) { send(*s.process, "q"); resized = s.capture->contains("SIZE:120:40", 50ms); }
        require(resized, "final coalesced resize not observed by child");
        send(*s.process, "x"); require(s.finish().error.empty(), "resize session failed");
    }
    {
        auto o = options("--child-slow"); o.input_capacity_bytes = 8192;
        Session s(std::move(o)); require(s.capture->contains("READY"), "slow child not ready");
        require(s.process->try_send(std::string(8193, 'x')) == ProcessSendResult::too_large, "oversized write accepted");
        bool blocked = false;
        for (int i = 0; i < 10000; ++i) {
            auto r = s.process->try_send(std::string(1024, 'x'));
            require(s.process->diagnostics().queued_input_bytes <= 8192, "input queue exceeded bound");
            if (r == ProcessSendResult::would_block) { blocked = true; break; }
            require(r == ProcessSendResult::accepted, "unexpected slow-reader result");
        }
        require(blocked, "input backpressure not observed");
        auto start = std::chrono::steady_clock::now(); s.process->request_close(); s.process->request_close();
        require(std::chrono::steady_clock::now() - start < 100ms, "request_close blocked caller");
        require(s.finish().close_requested, "close request not recorded");
    }
    {
        Session s(options("--child-flood-forever"), true); require(s.capture->contains("FLOOD-LIVE"), "live flood not ready");
        s.process->request_close(); require(s.finish().close_requested, "close-under-flood did not finish");
    }
    {
        auto o = options("--child-exit"); o.executable += ".definitely-missing";
        Session s(std::move(o)); require(!s.finish().error.empty(), "missing child reported success");
        require(s.process->state() == ProcessState::failed, "launch failure state missing");
    }
    {
        auto o = options("--child-exit"); o.environment = {{"bad=name", "x"}};
        Session s(std::move(o)); require(!s.finish().error.empty(), "invalid environment accepted");
    }
    {
        Session s(options("--child-flood-forever"), false, true);
        require(!s.finish().error.empty(), "callback exception not contained");
    }
    {
        // A downstream bounded queue may wait until close. Its stop callback is
        // deliberately slow: request_close must still never run it on the caller.
        auto capture = std::make_shared<Capture>();
        struct Downstream { std::mutex mutex; std::condition_variable_any changed; };
        auto downstream = std::make_shared<Downstream>();
        auto process = Process::start(options("--child-flood-forever"), {
            [capture, downstream](std::string_view, std::stop_token token) {
                std::stop_callback slow_stop(token, [] { std::this_thread::sleep_for(200ms); });
                capture->append("BLOCKED");
                std::unique_lock lock(downstream->mutex);
                downstream->changed.wait(lock, token, [] { return false; });
            },
            [capture](ProcessExit result) { std::lock_guard lock(capture->mutex); capture->exit = std::move(result); }
        });
        require(capture->contains("BLOCKED"), "cancellable output callback not entered");
        auto start = std::chrono::steady_clock::now(); process->request_close();
        require(std::chrono::steady_clock::now() - start < 100ms, "stop callback ran on requesting thread");
        require(process->wait_for_exit(8s), "cancellable output callback shutdown hung");
    }
    for (int i = 0; i < 12; ++i) {
        Session s(options("--child-slow")); s.process->request_close(); s.process->request_close();
        require(s.process->wait_for_exit(8s), "immediate repeated cancellation hung");
    }
    {
        Session s(options("--child-slow")); require(s.capture->contains("READY"), "destruction child not ready");
        const auto start = std::chrono::steady_clock::now(); s.process.reset();
        require(std::chrono::steady_clock::now() - start < 100ms, "destructor joined a worker");
        std::unique_lock lock(s.capture->mutex);
        require(s.capture->cv.wait_for(lock, 8s, [&] { return s.capture->exit.has_value(); }), "destroyed Process never reaped");
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]).starts_with("--child-")) return child_main(argc, argv);
    try {
        run_tests(executable_path(argv[0]));
#ifdef _WIN32
        std::cout << "PASS: native Windows system ConPTY process lifecycle tests (not graphics/Ink/physical UI qualification)\n";
#elif defined(__APPLE__)
        std::cout << "PASS: native macOS forkpty process lifecycle tests (not AppKit/UI qualification)\n";
#else
        std::cout << "PASS: Linux forkpty process lifecycle tests (not Windows/macOS runtime qualification)\n";
#endif
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
