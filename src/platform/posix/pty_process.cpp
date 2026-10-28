#include "stormtty/process.hpp"

#if !defined(_WIN32)
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <utility>
#include <csignal>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

extern char** environ;

namespace stormtty {
namespace {
using namespace std::chrono_literals;
struct Fd {
    int value{-1};
    Fd() = default;
    explicit Fd(int v) : value(v) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    void reset(int v = -1) noexcept { if (value >= 0) ::close(value); value = v; }
};
bool valid_grid(ProcessGridSize g) { return g.columns && g.rows && g.columns <= 32767 && g.rows <= 32767; }
void validate_text(std::string_view s) {
    // UTF-8, excluding NUL, overlong forms, surrogates and out-of-range scalars.
    for (std::size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i++]);
        if (!c) throw std::invalid_argument("process strings must not contain NUL");
        if (c < 0x80) continue;
        unsigned n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (!n || s.size() - i < n) throw std::invalid_argument("invalid UTF-8 process string");
        auto first = static_cast<unsigned char>(s[i]);
        if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
            (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90))
            throw std::invalid_argument("invalid UTF-8 process string");
        while (n--) if ((static_cast<unsigned char>(s[i++]) & 0xc0) != 0x80)
            throw std::invalid_argument("invalid UTF-8 process string");
    }
}
std::runtime_error os_error(const char* operation) { return std::runtime_error(std::string(operation) + ": " + std::strerror(errno)); }
void set_cloexec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) throw os_error("fcntl(FD_CLOEXEC)");
}
} // namespace

struct Process::Impl : std::enable_shared_from_this<Impl> {
    ProcessOptions options;
    ProcessCallbacks callbacks;
    std::atomic<ProcessState> phase{ProcessState::starting};
    std::atomic<bool> closing{false};
    std::atomic<bool> cancel_output_requested{false};
    std::atomic<bool> stop_reader{false};
    std::atomic<bool> reader_done{false};
    std::stop_source output_stop;
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    std::deque<std::string> input;
    std::size_t queued{};
    std::optional<ProcessGridSize> resize;
    bool done{};
    std::string failure;
    ProcessDiagnostics diagnostic{"POSIX forkpty", "Native PTY; protocol/rendering support requires separate qualification.", false, false, 0};
    Fd master;
    pid_t child{-1};

    Impl(ProcessOptions o, ProcessCallbacks c) : options(std::move(o)), callbacks(std::move(c)) {}
    void close() noexcept {
        cancel_output_requested.store(true);
        closing.store(true);
        auto p = phase.load();
        while (p == ProcessState::starting || p == ProcessState::running)
            if (phase.compare_exchange_weak(p, ProcessState::close_requested)) break;
        // stop callbacks can execute synchronously, so only the controller
        // requests the stop token; this caller-facing method never runs them.
        changed.notify_all();
    }
    void fail(std::string message) noexcept {
        try { std::lock_guard lock(mutex); if (failure.empty()) failure = std::move(message); } catch (...) {}
        close();
    }
    void launch() {
        validate_text(options.executable);
        validate_text(options.working_directory);
        if (options.executable.empty() || options.executable.front() != '/')
            throw std::invalid_argument("executable must be an absolute UTF-8 path");
        if (!options.working_directory.empty() && options.working_directory.front() != '/')
            throw std::invalid_argument("working_directory must be absolute");
        if (!valid_grid(options.grid) || !options.input_capacity_bytes)
            throw std::invalid_argument("invalid process grid/input capacity");
        if (options.provider == ProcessProvider::windows_inbox_basic)
            throw std::invalid_argument("Windows inbox provider is unavailable on POSIX");
        std::vector<std::string> args{options.executable};
        for (const auto& arg : options.arguments) { validate_text(arg); args.push_back(arg); }
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        std::map<std::string, std::string> env;
        for (char** p = environ; p && *p; ++p) {
            std::string_view entry(*p);
            auto equal = entry.find('=');
            if (equal != std::string_view::npos) env.emplace(entry.substr(0, equal), entry.substr(equal + 1));
        }
        // A terminal session always has a conventional TERM unless overridden.
        env["TERM"] = "xterm-256color";
        for (const auto& e : options.environment) {
            validate_text(e.name);
            if (e.name.empty() || e.name.find('=') != std::string::npos) throw std::invalid_argument("invalid environment name");
            if (e.value) { validate_text(*e.value); env[e.name] = *e.value; } else env.erase(e.name);
        }
        std::vector<std::string> entries;
        entries.reserve(env.size());
        for (auto& [name, value] : env) entries.push_back(name + "=" + value);
        std::vector<char*> envp;
        for (auto& e : entries) envp.push_back(e.data());
        envp.push_back(nullptr);
        int error_pipe[2];
        if (pipe(error_pipe) < 0) throw os_error("pipe(exec status)");
        Fd error_read(error_pipe[0]), error_write(error_pipe[1]);
        set_cloexec(error_read.value); set_cloexec(error_write.value);
        winsize size{};
        size.ws_col = options.grid.columns; size.ws_row = options.grid.rows;
        int master_fd = -1;
        // Prepare all strings before fork. Child does only async-signal-safe work
        // before execve; do not add C++ allocation, locks, callbacks or setenv here.
        const char* executable = options.executable.c_str();
        const char* cwd = options.working_directory.empty() ? nullptr : options.working_directory.c_str();
        if (closing.load()) return;
        child = forkpty(&master_fd, nullptr, nullptr, &size);
        if (child < 0) throw os_error("forkpty");
        if (child == 0) {
            ::close(error_read.value);
            int error = 0;
            if (cwd && chdir(cwd) < 0) error = errno;
            if (!error) { execve(executable, argv.data(), envp.data()); error = errno; }
            // A single int is atomic in a pipe; retry EINTR without allocation.
            while (::write(error_write.value, &error, sizeof(error)) < 0 && errno == EINTR) {}
            _exit(127);
        }
        master.reset(master_fd);
        error_write.reset();
        set_cloexec(master.value);
        int flags = fcntl(master.value, F_GETFL);
        if (flags < 0 || fcntl(master.value, F_SETFL, flags | O_NONBLOCK) < 0) throw os_error("fcntl(O_NONBLOCK)");
        int exec_error = 0;
        ssize_t result;
        do { result = ::read(error_read.value, &exec_error, sizeof(exec_error)); } while (result < 0 && errno == EINTR);
        if (result < 0) throw os_error("read(exec status)");
        if (result != 0) throw std::runtime_error("child exec/chdir: " + std::string(std::strerror(exec_error)));
    }
    void read_loop() noexcept {
        std::array<char, 64 * 1024> buffer{};
        while (!stop_reader.load()) {
            pollfd p{master.value, POLLIN, 0};
            int r = poll(&p, 1, 50);
            if (r < 0) { if (errno == EINTR) continue; fail("poll(PTY output) failed"); break; }
            if (!r) continue;
            ssize_t count = ::read(master.value, buffer.data(), buffer.size());
            if (count > 0) {
                try { if (callbacks.output) callbacks.output({buffer.data(), static_cast<std::size_t>(count)}, output_stop.get_token()); }
                catch (...) { fail("output callback threw"); break; }
            } else if (!count || (count < 0 && errno == EIO)) break; // Linux PTY EOF is EIO.
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { fail("read(PTY output) failed"); break; }
        }
        reader_done.store(true);
        changed.notify_all();
    }
    void write_loop() noexcept {
        try {
            for (;;) {
                std::string bytes;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] { return closing.load() || !input.empty(); });
                    if (closing.load()) break;
                    bytes = std::move(input.front()); input.pop_front();
                }
                std::size_t offset = 0;
                while (offset < bytes.size() && !closing.load()) {
                    pollfd p{master.value, POLLOUT, 0};
                    int r = poll(&p, 1, 50);
                    if (r < 0) { if (errno == EINTR) continue; fail("poll(PTY input) failed"); return; }
                    if (!r) continue;
                    ssize_t count = ::write(master.value, bytes.data() + offset, bytes.size() - offset);
                    if (count > 0) {
                        offset += static_cast<std::size_t>(count);
                        std::lock_guard lock(mutex); queued -= static_cast<std::size_t>(count);
                    } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        if (!closing.load() && !reader_done.load()) fail("write(PTY input) failed");
                        return;
                    }
                }
            }
        } catch (...) { fail("PTY writer failed"); }
    }
    void signal_owned_groups(int signal) noexcept {
        if (child <= 0) return;
        // forkpty makes child a new session leader. A live or unreaped leader pins
        // its PID/session identity; never signal after waitpid has reaped it.
        pid_t foreground = master.value >= 0 ? tcgetpgrp(master.value) : -1;
        if (foreground > 0 && foreground != child && getsid(foreground) == child)
            ::kill(-foreground, signal);
        ::kill(-child, signal);
        // Covers setup failure before forkpty has completed setsid in the child.
        ::kill(child, signal);
    }
    void run() noexcept {
        std::thread reader, writer;
        std::optional<std::uint32_t> code;
        bool requested = false;
        try {
            if (!closing.load()) launch();
            if (child > 0) {
                reader = std::thread([this] { read_loop(); });
                writer = std::thread([this] { write_loop(); });
                auto expected = ProcessState::starting;
                phase.compare_exchange_strong(expected, ProcessState::running);
                bool child_exited = false;
                auto close_time = std::chrono::steady_clock::time_point::max();
                while (!child_exited) {
                    siginfo_t info{};
                    if (waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
                        if (errno == EINTR) continue;
                        throw os_error("waitid");
                    }
                    if (info.si_pid == child) { child_exited = true; break; }
                    if (closing.load()) {
                        if (close_time == std::chrono::steady_clock::time_point::max()) {
                            requested = true; close_time = std::chrono::steady_clock::now();
                            output_stop.request_stop(); signal_owned_groups(SIGHUP);
                        } else if (std::chrono::steady_clock::now() - close_time >= 500ms) signal_owned_groups(SIGKILL);
                    }
                    std::optional<ProcessGridSize> next;
                    {
                        std::unique_lock lock(mutex);
                        if (!closing.load()) next = std::exchange(resize, std::nullopt);
                        if (!next) changed.wait_for(lock, 20ms);
                    }
                    if (next) {
                        winsize size{}; size.ws_col = next->columns; size.ws_row = next->rows;
                        if (ioctl(master.value, TIOCSWINSZ, &size) < 0) throw os_error("ioctl(TIOCSWINSZ)");
                    }
                }
                requested = requested || closing.load();
                phase.store(ProcessState::draining);
                // Session leader remains unreaped while cleaning up its groups.
                signal_owned_groups(SIGHUP);
                closing.store(true); changed.notify_all();
                if (writer.joinable()) writer.join();
                // Allow remaining PTY output to drain. A descendant that keeps a
                // slave fd open must not retain the session forever after exit.
                {
                    std::unique_lock lock(mutex);
                    changed.wait_for(lock, 500ms, [&] { return reader_done.load(); });
                }
                if (!reader_done.load()) {
                    signal_owned_groups(SIGKILL);
                    if (!cancel_output_requested.load())
                        fail("PTY output drain exceeded grace period after child exit; cancelling downstream output wait");
                    output_stop.request_stop();
                    std::unique_lock lock(mutex);
                    changed.wait_for(lock, 500ms, [&] { return reader_done.load(); });
                }
                stop_reader.store(true); output_stop.request_stop();
                if (reader.joinable()) reader.join();
                int status = 0;
                pid_t result;
                do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
                if (result < 0) throw os_error("waitpid");
                child = -1;
                if (WIFEXITED(status)) code = static_cast<std::uint32_t>(WEXITSTATUS(status));
                else if (WIFSIGNALED(status)) code = static_cast<std::uint32_t>(128 + WTERMSIG(status));
            } else requested = closing.load();
        } catch (const std::exception& e) { fail(e.what()); }
        catch (...) { fail("unknown PTY lifecycle failure"); }
        if (child > 0) {
            signal_owned_groups(SIGKILL);
            closing.store(true); stop_reader.store(true); output_stop.request_stop(); changed.notify_all();
            if (writer.joinable()) writer.join();
            if (reader.joinable()) reader.join();
            int status{};
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            child = -1;
        }
        master.reset();
        ProcessExit result;
        {
            std::lock_guard lock(mutex);
            input.clear(); queued = 0;
            result = {code, requested || cancel_output_requested.load(), failure};
            phase.store(failure.empty() ? ProcessState::exited : ProcessState::failed);
        }
        try { if (callbacks.exited) callbacks.exited(std::move(result)); } catch (...) { /* never escape worker */ }
        {
            std::lock_guard lock(mutex); done = true;
        }
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
