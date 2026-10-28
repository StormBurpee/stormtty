#include "stormtty/process.hpp"
#include "stormtty/terminal.hpp"
#include "../src/platform/windows/process_control.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
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
#ifdef _WIN32
enum class ConsoleProbeExit : int {
    success = 0, stdin_not_console = 40, stdout_not_console = 41,
    stderr_not_console = 42, input_route_failed = 43
};
#endif
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
        if (!input_console) return static_cast<int>(ConsoleProbeExit::stdin_not_console);
        if (!output_console) return static_cast<int>(ConsoleProbeExit::stdout_not_console);
        if (!error_console) return static_cast<int>(ConsoleProbeExit::stderr_not_console);
        std::fputs("STDOUT-VIA-CONPTY\n", stdout);
        std::fputs("STDERR-VIA-CONPTY\n", stderr);
        if (std::getchar() != 'R') return static_cast<int>(ConsoleProbeExit::input_route_failed);
        std::fputs("STDIN-VIA-CONPTY\n", stdout);
        return static_cast<int>(ConsoleProbeExit::success);
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
            if (c == EOF || c == 'x') { std::puts("RESIZE-FINAL-TAIL"); return 0; }
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
    stormtty::ProcessGridSize initial_grid{};
    struct ResizeEvent { std::size_t offset; stormtty::ProcessGridSize grid; };
    std::vector<ResizeEvent> resizes;
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
    void set_grid(stormtty::ProcessGridSize size) {
        {
            std::lock_guard lock(mutex);
            if (output.empty() && resizes.empty()) initial_grid = size;
            else {
                require(resizes.size()<4096, "test resize-event budget exceeded");
                resizes.push_back({output.size(), size});
            }
        }
        cv.notify_all();
    }
    bool contains_terminal_text(std::string_view text, std::chrono::milliseconds timeout = 5s) {
        // Construct, mutate, select and destroy on this calling test thread.
        // The process output worker only copies bytes into the bounded capture.
        std::unique_lock lock(mutex);
        stormtty::TerminalOptions options;
        options.size = {initial_grid.columns, initial_grid.rows, 8, 16};
        stormtty::TerminalCore terminal(options);
        std::size_t parsed{}, next_resize{};
        return cv.wait_for(lock, timeout, [&] {
            // Replay each geometry change at its captured byte boundary rather
            // than interpreting all historical bytes at the newest width.
            while (next_resize < resizes.size()) {
                const auto& event=resizes[next_resize++];
                if (parsed<event.offset) terminal.feed(std::string_view(output).substr(parsed,event.offset-parsed));
                parsed=event.offset;
                terminal.resize({event.grid.columns,event.grid.rows,8,16});
            }
            if (parsed < output.size()) {
                terminal.feed(std::string_view(output).substr(parsed));
                parsed = output.size();
            }
            const auto size=terminal.size();
            terminal.select(0, 0, static_cast<std::uint16_t>(size.columns-1), static_cast<std::uint16_t>(size.rows-1));
            // Selection formatting unwraps soft wraps but preserves hard line
            // breaks; it does not invent text by stripping control sequences.
            const auto visible = terminal.selection_text(2u * 1024u * 1024u);
            terminal.clear_selection();
            return visible.find(text) != std::string::npos;
        });
    }
    bool contains(std::string_view text, std::chrono::milliseconds timeout = 5s) {
#ifdef _WIN32
        return contains_terminal_text(text, timeout);
#else
        // A POSIX PTY is a byte stream, unlike ConPTY's console repaint stream.
        // Preserve the existing byte-level assertions on Linux and macOS.
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return output.find(text) != std::string::npos; });
#endif
    }
};
// Keep only the latest bounded synthetic test capture for failure diagnostics.
// Raw ConPTY streams contain legitimate cursor/repaint sequences; an escaped
// dump distinguishes transport loss from a too-strict textual assertion.
std::shared_ptr<Capture> diagnostic_capture;
struct Session {
    std::shared_ptr<Capture> capture{std::make_shared<Capture>()};
    std::unique_ptr<Process> process;
    Session(ProcessOptions options, bool slow = false, bool throw_output = false) {
        diagnostic_capture = capture;
        capture->set_grid(options.grid);
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
void semantic_capture_tests() {
    // These synthetic VT tests run portably; only native Windows process
    // assertions use this semantic path. POSIX process assertions remain raw.
    {
        Capture capture;
        // Captured Windows ConPTY failure: it painted "A", emitted a title and
        // mode updates, then painted "RG0=[]". The visible argument was intact.
        capture.append("\x1b[?9001h\x1b[?1004h\x1b[?25l\x1b[2J\x1b[m\x1b[HA"
                       "\x1b]0;C:\\stormtty\\process_tests.exe\x07"
                       "\x1b[?25h\x1b[?9001l\x1b[?1004lRG0=[]\r\n");
        require(capture.contains_terminal_text("ARG0=[]", 0ms), "semantic capture lost argument across OSC/mode updates");
        require(!capture.contains_terminal_text("process_tests.exe", 0ms), "semantic capture exposed an OSC title as terminal text");
    }
    {
        Capture capture;
        capture.set_grid({16, 4});
        capture.append("ARG\x1b["); // Deliberately split a CSI across callbacks.
        std::jthread writer([&] {
            std::this_thread::sleep_for(10ms);
            capture.append("31m0\x1b[0m=\x1b[1;6H[value]\r\n");
        });
        require(capture.contains_terminal_text("ARG0=[value]", 1s), "semantic capture lost split CSI/SGR/cursor text");
    }
    {
        Capture capture;
        capture.append("WRONG-VALUE\x1b[1G\x1b[2KRIGHT-VALUE\r\n");
        require(capture.contains_terminal_text("RIGHT-VALUE", 0ms), "semantic capture lost replacement text");
        require(!capture.contains_terminal_text("WRONG-VALUE", 0ms), "semantic capture matched erased raw bytes");
    }
    {
        Capture capture;
        capture.set_grid({8, 4});
        capture.append("CWD=[C:/long/path]\r\n");
        require(capture.contains_terminal_text("CWD=[C:/long/path]", 0ms), "semantic capture did not unwrap soft wrap");
        Capture hard_break;
        hard_break.append("left\r\nright");
        require(!hard_break.contains_terminal_text("leftright", 0ms), "semantic capture erased a hard line break");
        Capture history;
        history.set_grid({4, 4});
        history.append("ABCDEFGH\x1b[2;1HX");
        history.set_grid({8, 4});
        require(history.contains_terminal_text("ABCDXFGH", 0ms), "semantic capture replayed historical bytes at the newest width");
    }
    {
        Capture capture;
        capture.set_grid({8, 4});
        capture.append("SIZE:");
        std::jthread writer([&] {
            std::this_thread::sleep_for(10ms);
            capture.set_grid({12, 4});
            capture.append("\x1b[1;6H120\x1b[0m:\x1b[1;10H40");
        });
        require(capture.contains_terminal_text("SIZE:120:40", 1s), "semantic capture did not apply updated terminal geometry");
    }
}
void idle_writer_shutdown_stress(const std::string& executable, unsigned rounds) {
    // No input is ever enqueued: normal exit and explicit close must wake a
    // writer parked in its CV wait. Concurrent small batches vary scheduling
    // without unbounded process/thread creation. This is bounded stress, not a
    // deterministic scheduler test for the predicate-to-wait lost-wakeup window.
    constexpr unsigned batch_size = 4;
    for (unsigned round = 0; round < rounds; ++round) {
        for (bool requested : {false, true}) {
            std::vector<std::unique_ptr<Session>> sessions;
            try {
                for (unsigned i = 0; i < batch_size; ++i) {
                    ProcessOptions options;
                    options.executable = executable;
                    options.arguments = {requested ? "--child-slow" : "--child-exit"};
                    sessions.push_back(std::make_unique<Session>(std::move(options)));
                }
                if (requested) {
                    for (auto& session : sessions) {
                        diagnostic_capture = session->capture;
                        require(session->capture->contains("READY"), "idle-writer close child not ready");
                    }
                    for (auto& session : sessions) session->process->request_close();
                }
                for (auto& session : sessions) {
                    diagnostic_capture = session->capture;
                    const auto result = session->finish();
                    require(result.error.empty(), "idle-writer shutdown reported a transport failure");
                    require(result.close_requested == requested, "idle-writer shutdown reason changed");
                    require(session->process->diagnostics().queued_input_bytes == 0, "idle-writer shutdown retained input");
                    if (!requested) {
                        require(result.exit_code == 23, "idle-writer natural exit code lost");
                        require(session->capture->contains("STORM-TAIL-\xe2\x98\x83-\xf0\x9f\x8c\xa9", 0ms),
                                "idle-writer natural exit lost trailing UTF-8 output");
                    }
                }
            } catch (...) {
                // Re-notify even a regressed parked writer and reap owned children
                // before reporting the original failure. Never leave slow test
                // children behind merely because a sibling assertion failed.
                for (auto& session : sessions) session->process->request_close();
                for (auto& session : sessions) {
                    if (!session->process->wait_for_exit(8s))
                        std::cerr << "idle-writer cleanup also exceeded its deadline\n";
                }
                throw;
            }
        }
    }
}
void control_bookkeeping_tests() {
    // Portable bookkeeping only: this does not exercise a failing native call.
    // Never induce native allocation failure with enormous grids or expose an
    // invalid HPCON/test-only failure switch through the public process API.
    stormtty::WindowsProcessControlDiagnostics state;
    require(!state.applied_grid, "uncreated console has an applied grid");
    state.applied_grid = stormtty::ProcessGridSize{80,24};
    state.requested_grid = {100,30}; state.requested_resize_generation = 7;
    stormtty::windows::detail::record_resize_result(state, 7, {100,30}, false);
    require(state.resize_failures == 1 && state.applied_resize_generation == 0 &&
            state.applied_grid == stormtty::ProcessGridSize{80,24}, "failed resize changed applied state");
    stormtty::windows::detail::record_resize_result(state, 7, {100,30}, true);
    require(state.applied_resize_generation == 7 && state.applied_grid == state.requested_grid,
            "successful coalesced resize did not advance applied state");
    state.requested_resize_generation=8; state.requested_grid={110,35};
    stormtty::windows::detail::record_resize_result(state, 7, {100,30}, true);
    require(state.requested_resize_generation==8 && state.requested_grid==stormtty::ProcessGridSize{110,35} &&
            state.applied_resize_generation==7 && state.applied_grid==stormtty::ProcessGridSize{100,30},
            "older native completion acknowledged or erased a newer request");
    stormtty::windows::detail::record_resize_result(state, 8, {110,35}, false);
    require(state.resize_failures==2 && state.applied_resize_generation==7 &&
            state.applied_grid==stormtty::ProcessGridSize{100,30}, "failed newer resize replaced successful applied state");
    state.control_wakes = std::numeric_limits<std::uint64_t>::max();
    stormtty::windows::detail::count(state.control_wakes);
    require(state.control_wakes == std::numeric_limits<std::uint64_t>::max(), "diagnostic counter wrapped");
}
#ifdef _WIN32
stormtty::WindowsProcessControlDiagnostics control_state(const Process& process) {
    const auto state = process.diagnostics().windows_control;
    require(state.has_value(), "Windows control diagnostics are unavailable");
    return *state;
}
void print_control(std::ostream& out, const stormtty::WindowsProcessControlDiagnostics& d) {
    out << "requested=" << d.requested_resize_generation << ':' << d.requested_grid.columns << 'x' << d.requested_grid.rows
        << " applied=" << d.applied_resize_generation << ':';
    if (d.applied_grid) out << d.applied_grid->columns << 'x' << d.applied_grid->rows; else out << "uncreated";
    out << " waits=" << d.wait_calls << " control_wakes=" << d.control_wakes << " exit_wakes=" << d.child_exit_wakes
        << " resize_calls=" << d.resize_calls << " resize_failures=" << d.resize_failures << '\n';
}
template<class Predicate> void await_control(const Process& process, Predicate predicate, const char* error) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        if (predicate(control_state(process))) return;
        require(process.state() != ProcessState::failed && process.state() != ProcessState::exited,
                "process exited before controller checkpoint");
        std::this_thread::sleep_for(1ms); // Bounded test observation, not production polling.
    } while (std::chrono::steady_clock::now() < deadline);
    std::cerr << "Controller checkpoint timed out: "; print_control(std::cerr,control_state(process));
    throw std::runtime_error(error);
}
void event_driven_control_tests(const std::string& executable) {
    const auto options = [&](const char* mode) { ProcessOptions o; o.executable=executable; o.arguments={mode}; return o; };
    {
        Session session(options("--child-resize"));
        require(session.capture->contains("READY"), "idle controller child not ready");
        await_control(*session.process, [](const auto& d) { return d.wait_calls > 0; }, "controller did not enter wait");
        const auto before = control_state(*session.process);
        require(before.requested_resize_generation == 0 && before.applied_resize_generation == 0 &&
                before.applied_grid == before.requested_grid, "initial controller geometry is not the launch grid");
        std::this_thread::sleep_for(200ms);
        const auto idle = control_state(*session.process);
        require(idle.wait_calls == before.wait_calls && idle.control_wakes == before.control_wakes &&
                idle.child_exit_wakes == before.child_exit_wakes, "idle controller woke periodically without work");

        require(!session.process->request_resize({0,24}), "invalid resize accepted");
        require(control_state(*session.process).requested_resize_generation == 0,
                "rejected resize advanced its generation");
        // Repeated request/apply/repark transitions exercise signalling on both
        // sides of wait entry; timeouts catch work lost by an explicit reset race.
        for (unsigned generation = 1; generation <= 32; ++generation) {
            const stormtty::ProcessGridSize grid{static_cast<std::uint16_t>(80+generation),30};
            session.capture->set_grid(grid);
            require(session.process->request_resize(grid), "sequential control resize rejected");
            await_control(*session.process, [&](const auto& d) {
                return d.applied_resize_generation == generation && d.applied_grid == grid;
            }, "request/apply/repark lost a resize");
            if (generation % 2) std::this_thread::yield(); else std::this_thread::sleep_for(1ms);
        }
        std::atomic<unsigned> accepted{};
        {
            std::vector<std::jthread> producers;
            for (unsigned producer=0; producer<4; ++producer) producers.emplace_back([&,producer] {
                for (unsigned i=0; i<64; ++i) {
                    if (session.process->request_resize({static_cast<std::uint16_t>(80+(i+producer)%40),30})) ++accepted;
                    if (i%4==0) std::this_thread::yield();
                }
            });
        } // Join every producer before sending the unambiguous newest request.
        require(accepted == 256, "concurrent control resize rejected");
        const stormtty::ProcessGridSize final_grid{120,40};
        session.capture->set_grid(final_grid);
        require(session.process->request_resize(final_grid), "final controller resize rejected");
        constexpr std::uint64_t final_generation = 32+256+1;
        await_control(*session.process, [&](const auto& d) {
            return d.requested_resize_generation == final_generation && d.applied_resize_generation == final_generation &&
                   d.requested_grid == final_grid && d.applied_grid == final_grid;
        }, "latest resize generation did not converge");
        // The applied generation proves only the native call. Ask the real child
        // separately; no ACK is treated as an output boundary or shell evidence.
        bool observed=false;
        for (unsigned attempt=0;attempt<50 && !observed;++attempt) {
            send(*session.process,"q"); observed=session.capture->contains("SIZE:120:40",50ms);
        }
        require(observed, "child did not observe the final native grid");
        send(*session.process,"x");
        const auto result=session.finish();
        require(result.error.empty() && result.exit_code==0, "resize controller natural exit failed");
        require(session.capture->contains("RESIZE-FINAL-TAIL",0ms), "resize controller lost final output");
        const auto final=control_state(*session.process);
        require(final.child_exit_wakes==1 && final.resize_failures==0 && final.resize_calls>0 && final.resize_calls<=final_generation &&
                final.wait_calls==final.control_wakes+final.child_exit_wakes,
                "controller final counters are inconsistent");
        require(!session.process->request_resize({90,30}) &&
                control_state(*session.process).requested_resize_generation==final_generation,
                "post-exit resize changed controller diagnostics");
        std::cout << "Native controller convergence: "; print_control(std::cout,final);
    }
    for (unsigned round=0; round<4; ++round) for (const bool flood : {false,true}) {
        Session session(options(flood ? "--child-flood-forever" : "--child-resize"),flood);
        require(session.capture->contains(flood ? "FLOOD-LIVE" : "READY"), "resize/close child not ready");
        std::atomic<bool> started{};
        std::jthread producer([&] {
            for (unsigned i=0; i<256; ++i) {
                const bool accepted=session.process->request_resize({static_cast<std::uint16_t>(80+i%40),30});
                started.store(true);
                if (!accepted) break;
                std::this_thread::yield();
            }
        });
        const auto ready_deadline=std::chrono::steady_clock::now()+5s;
        while (!started.load() && std::chrono::steady_clock::now()<ready_deadline) std::this_thread::sleep_for(1ms);
        require(started.load(), "resize producer did not start");
        const auto start=std::chrono::steady_clock::now();
        session.process->request_close();
        require(std::chrono::steady_clock::now()-start<100ms, "close during resize blocked caller");
        producer.join();
        const auto result=session.finish();
        require(result.close_requested && result.error.empty(), "close during resize/flood failed");
        const auto final=control_state(*session.process);
        require(final.applied_resize_generation<=final.requested_resize_generation && final.resize_failures==0,
                "close during resize corrupted generation state");
    }
    std::cout << "PASS: Windows event-driven controller idle, resize generations, signal/repark stress, resize/flood close and final drain\n";
}
#endif
void run_tests(const std::string& executable) {
    control_bookkeeping_tests();
    semantic_capture_tests();
    idle_writer_shutdown_stress(executable, 8);
    const auto options = [&](std::string mode) { ProcessOptions o; o.executable = executable; o.arguments = {std::move(mode)}; return o; };
#ifdef _WIN32
    event_driven_control_tests(executable);
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
        // Exit 0 is possible only after all three GetConsoleMode probes and the
        // input roundtrip succeed. ConPTY can insert repaint/control sequences
        // between printf fragments, so the diagnostic is not a raw-byte oracle.
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
#ifndef _WIN32
        require(!s.process->diagnostics().windows_control, "POSIX fabricated Windows control diagnostics");
#endif
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
        for (unsigned i = 0; i < 100; ++i) {
            const stormtty::ProcessGridSize size{static_cast<std::uint16_t>(80+i),30};
            s.capture->set_grid(size);
            require(s.process->request_resize(size), "resize rejected");
        }
        s.capture->set_grid({120, 40});
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
        const auto executable = executable_path(argv[0]);
        if (argc > 1 && std::string_view(argv[1]) == "--idle-writer-stress")
            idle_writer_shutdown_stress(executable, 64); // 512 launches; opt-in longer run.
        else if (argc > 1 && std::string_view(argv[1]) == "--event-control-tests") {
            control_bookkeeping_tests();
#ifdef _WIN32
            event_driven_control_tests(executable);
#else
            throw std::runtime_error("--event-control-tests requires native Windows ConPTY; not a portable pass");
#endif
        }
        else run_tests(executable);
#ifdef _WIN32
        std::cout << "PASS: native Windows system ConPTY process lifecycle tests (not graphics/Ink/physical UI qualification)\n";
#elif defined(__APPLE__)
        std::cout << "PASS: native macOS forkpty process lifecycle tests (not AppKit/UI qualification)\n";
#else
        std::cout << "PASS: Linux forkpty process lifecycle tests (not Windows/macOS runtime qualification)\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        if (diagnostic_capture) {
            std::lock_guard lock(diagnostic_capture->mutex);
            const auto& bytes = diagnostic_capture->output;
            const auto first = bytes.size() > 4096 ? bytes.size() - 4096 : 0;
            std::cerr << "Captured PTY bytes (last " << bytes.size()-first << " of " << bytes.size() << "): ";
            constexpr char hex[] = "0123456789ABCDEF";
            for (std::size_t i=first;i<bytes.size();++i) {
                const auto c=static_cast<unsigned char>(bytes[i]);
                if (c>=0x20 && c<0x7f && c!='\\') std::cerr << static_cast<char>(c);
                else std::cerr << "\\x" << hex[c>>4] << hex[c&15];
            }
            std::cerr << '\n';
        }
        return 1;
    }
}
