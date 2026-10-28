// Real Zsh -> StormTTY Process -> Session/libghostty qualification; no fake shell.
#include "stormtty/session.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "stormtty/windows_managed_shell.hpp"
#else
#include <unistd.h>
#include <sys/ioctl.h>
#endif

namespace fs = std::filesystem;
using namespace stormtty;
using namespace std::chrono_literals;
namespace {
std::string utf8(const fs::path& p) {
    auto text = p.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
fs::path path_utf8(std::string_view text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
std::string screen(const std::shared_ptr<Session>& session) {
    std::string value;
    if (const auto frame = session->frame()) for (const auto& row : frame->lines) {
        for (const auto& cell : row->cells) if (cell.span != 0)
            value += cell.grapheme.empty() ? " " : cell.grapheme;
        value += '\n';
    }
    return value;
}
template<class F> bool until(F predicate, std::chrono::milliseconds timeout = 10s) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    do { if (predicate()) return true; std::this_thread::sleep_for(20ms); }
    while (std::chrono::steady_clock::now() < end);
    return predicate();
}
void expect(const std::shared_ptr<Session>& session, std::string_view token) {
    if (!until([&] { return screen(session).find(token) != std::string::npos; })) {
        std::cerr << "Expected " << token << "\n" << screen(session)
                  << "\nSession error: " << session->info().error << '\n';
        throw std::runtime_error("shell marker not observed in real terminal frame");
    }
    std::cout << "[pass] " << token << '\n';
}
void key(const std::shared_ptr<Session>& s, Key k, std::uint16_t mods = 0) {
    KeyInput input; input.key = k; input.modifiers = mods;
    require(s->key(input) == SessionSendResult::accepted, "key rejected");
}
void command(const std::shared_ptr<Session>& s, std::string text) {
    std::cout << "[submit] bytes=" << text.size() << " text=" << text << '\n';
    require(s->text(std::move(text)) == SessionSendResult::accepted, "command rejected");
    key(s, Key::enter);
}
// One fresh ZLE input-ready cycle per submitted command. The fixture preserves
// the preceding command status before any prompt bookkeeping can overwrite it.
struct PromptProtocol {
    std::shared_ptr<Session> session;
    unsigned sequence{1};
    void ready(int status = -1) {
        const auto prefix = "__STORMTTY_PROMPT:" + std::to_string(sequence) + ":";
        expect(session, status < 0 ? prefix : prefix + std::to_string(status) + "__");
    }
    void send(std::string text) {
        ready();
        command(session, std::move(text));
        ++sequence;
    }
};
void dump_journal(const fs::path& path) {
    if (path.empty()) return;
    std::ifstream input(path, std::ios::binary);
    // The fixture contains no user commands or secrets. Never read beyond the
    // diagnostic bound, even if a broken fixture ignores its own record cap.
    std::string bytes(128 * 1024, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    std::cout << "ZLE journal (controlled fixture; max128KiB; last successful flush, possibly before failure):\n" << bytes
              << "\nEnd ZLE journal\n";
}
// Snapshot controller completion separately from shell/native observations.
// A successful native resize API call does not establish Zsh observation.
void dump_process_diagnostics(const std::shared_ptr<Session>& session, std::string_view label) {
    const auto d = session->diagnostics();
    std::cout << "Process diagnostics " << label << ": provider=" << d.provider
              << " queued_input_bytes=" << d.queued_input_bytes;
    if (d.windows_control) {
        const auto& c = *d.windows_control;
        std::cout << " requested_generation=" << c.requested_resize_generation
                  << " applied_generation=" << c.applied_resize_generation
                  << " requested_grid=" << c.requested_grid.columns << 'x' << c.requested_grid.rows
                  << " applied_grid=";
        if (c.applied_grid) std::cout << c.applied_grid->columns << 'x' << c.applied_grid->rows;
        else std::cout << "unavailable";
        std::cout << " resize_calls=" << c.resize_calls << " resize_failures=" << c.resize_failures
                  << " wait_calls=" << c.wait_calls << " control_wakes=" << c.control_wakes
                  << " child_exit_wakes=" << c.child_exit_wakes;
    } else std::cout << " windows_control=not-applicable";
    std::cout << '\n';
}
std::string inherited_path() {
#ifdef _WIN32
    const auto count = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (!count) return {};
    std::wstring wide(count, L'\0');
    const auto length = GetEnvironmentVariableW(L"PATH", wide.data(), count);
    if (!length || length >= count) throw std::runtime_error("PATH changed while reading");
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                         static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (!bytes) throw std::runtime_error("PATH UTF-8 conversion failed");
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(length),
                        result.data(), bytes, nullptr, nullptr);
    return result;
#else
    const auto path = std::getenv("PATH"); return path ? path : "";
#endif
}
int native_size(std::string_view tag = {}) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) return 43;
    const auto columns = info.srWindow.Right - info.srWindow.Left + 1;
    const auto rows = info.srWindow.Bottom - info.srWindow.Top + 1;
#else
    winsize info{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &info) != 0) return 43;
    const auto columns = info.ws_col;
    const auto rows = info.ws_row;
#endif
    std::cout << "__STORMTTY_NATIVE_SIZE:" << columns << 'x' << rows << "__\n" << std::flush;
    if (!tag.empty()) std::cout << "__STORMTTY_NATIVE_SIZE_ACK:" << tag << ':' << columns << 'x' << rows << "__\n" << std::flush;
    return 0;
}
int native_handles() {
#ifdef _WIN32
    DWORD in{}, out{}, err{};
    const bool good = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &in) &&
        GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &out) &&
        GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &err);
    SetConsoleOutputCP(CP_UTF8);
#else
    const bool good = isatty(0) && isatty(1) && isatty(2);
#endif
    std::cout << (good ? "__STORMTTY_NATIVE_TTY_OK__\n" : "__STORMTTY_NATIVE_TTY_FAIL__\n") << std::flush;
    return good ? 0 : 42;
}
}
int main(int argc, char** argv) {
#ifdef _WIN32
    if (argc==2 && std::string_view(argv[1])=="--managed-home-environment") {
        try {
            // ASCII hex protocol keeps Python/console codepages out of the
            // resolver test. These are real host-helper UTF-8 child overrides.
            constexpr char hex[]="0123456789abcdef";
            for (const auto& entry:managed_shell_home_environment()) {
                std::cout << entry.name << '=';
                for (const unsigned char byte:*entry.value) std::cout << hex[byte>>4] << hex[byte&15];
                std::cout << '\n';
            }
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "managed home resolver: " << error.what() << '\n';
            return 1;
        }
    }
#endif
    if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--native-size")
        return native_size(argc==3?std::string_view(argv[2]):std::string_view{});
    if (argc == 2 && std::string_view(argv[1]) == "--native-handles") return native_handles();
    unsigned resize_repeats=1;
    if (argc >= 3 && std::string_view(argv[argc-2]) == "--resize-repeats") {
        const std::string count=argv[argc-1];
        if (count.empty() || count.size()>2 || count.find_first_not_of("0123456789")!=std::string::npos ||
            (resize_repeats=static_cast<unsigned>(std::strtoul(count.c_str(),nullptr,10)))<1 || resize_repeats>50) {
            std::cerr << "--resize-repeats must be 1..50\n"; return 2;
        }
        argc-=2;
    }
    if (argc < 4 || argc > 6) {
        std::cerr << "usage: bundled_shell_probe SHELL_ABSOLUTE FIXTURE_ABSOLUTE SCRATCH_DIR [inbox | --packaged-provider ABSOLUTE_DIRECTORY] [--resize-repeats 1..50]\n";
        return 2;
    }
    std::cout << std::unitbuf;
    std::shared_ptr<Session> session;
    fs::path journal;
    try {
        const auto shell = fs::absolute(path_utf8(argv[1]));
        const auto fixture = fs::absolute(path_utf8(argv[2]));
        const auto scratch = fs::absolute(path_utf8(argv[3]));
        // Require a new directory: never overwrite a user's files.
        require(!fs::exists(scratch), "scratch directory must not exist");
        journal = scratch / "zle-input.log";
        const auto dotdir = scratch / "dotdir";
        const auto home = scratch / "home";
        const auto cwd = scratch / path_utf8("workspace caf\xc3\xa9 \xe8\xb7\xaf\xe5\xbe\x84");
        fs::create_directories(dotdir); fs::create_directories(home); fs::create_directories(cwd);
        std::ofstream(dotdir / ".zshrc", std::ios::binary)
            << "source -- \"$STORMTTY_PROBE_FIXTURE\"\n";
        // A tripwire: HOME/.zshrc must not run in the managed private profile.
        std::ofstream(home / ".zshrc", std::ios::binary) << "print GLOBAL_DOTFILE_WAS_READ\n";
        ProcessOptions p; p.executable = utf8(shell); p.arguments = {"-d", "-i"};
        p.working_directory = utf8(cwd); p.grid = {100, 30};
        if (argc == 5) {
            require(std::string_view(argv[4]) == "inbox", "unknown provider");
            p.provider = ProcessProvider::windows_inbox_basic;
        } else if (argc == 6) {
            require(std::string_view(argv[4]) == "--packaged-provider", "unknown provider option");
            const auto provider = path_utf8(argv[5]);
            require(provider.is_absolute() && fs::is_directory(provider), "packaged provider directory must be absolute and exist");
            p.provider = ProcessProvider::windows_packaged_experimental;
            p.provider_directory = utf8(provider);
        }
#ifdef _WIN32
        const auto path = utf8(shell.parent_path()) + ";" + inherited_path();
        const auto sleep = shell.parent_path() / "sleep.exe";
        const auto stty = shell.parent_path() / "stty.exe";
#else
        const auto path = utf8(shell.parent_path()) + ":" + inherited_path();
        const auto sleep = fs::path("/usr/bin/sleep");
        const auto stty = fs::path("/usr/bin/stty");
#endif
        require(fs::is_regular_file(sleep), "candidate sleep executable missing");
        require(fs::is_regular_file(stty), "candidate stty executable missing");
        p.environment = {{"STORMTTY_PROBE_SLEEP", utf8(sleep)}, {"ZDOTDIR", utf8(dotdir)}, {"HOME", utf8(home)},
            {"STORMTTY_PROBE_FIXTURE", utf8(fixture)},
            {"STORMTTY_PROBE_JOURNAL", utf8(journal)},
            {"STORMTTY_PROBE_STTY", utf8(stty)},
            {"STORMTTY_PROBE_NATIVE", utf8(fs::absolute(path_utf8(argv[0])))},
            {"PATH", path}, {"TERM", "xterm-256color"}, {"COLORTERM", "truecolor"},
            {"LANG", "C.UTF-8"}, {"LC_ALL", "C.UTF-8"},
            {"MSYSTEM", "MSYS"}, {"MSYS", std::nullopt}, {"ENV", std::nullopt}, {"BASH_ENV", std::nullopt}};
        TerminalOptions terminal; terminal.size = {100, 30, 8, 16};
        const auto started = std::chrono::steady_clock::now();
        session = Session::start(std::move(p), terminal);
        expect(session, "__STORMTTY_READY__");
        PromptProtocol prompt{session};
        prompt.ready(0);
        std::cout << "provider=" << session->diagnostics().provider << '\n';
        std::cout << "startup_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count() << '\n';
        require(screen(session).find("GLOBAL_DOTFILE_WAS_READ") == std::string::npos,
                "managed profile sourced HOME/.zshrc");
        prompt.send("stormtty_probe_tty"); expect(session, "__STORMTTY_TTY_OK__");
        prompt.send("stormtty_probe_native"); expect(session, "__STORMTTY_NATIVE_TTY_OK__");
        prompt.send("stormtty_probe_unicode 'caf\xc3\xa9 \xe8\xb7\xaf\xe5\xbe\x84 \xe2\x98\x83'");
        expect(session, "__STORMTTY_UNICODE:caf\xc3\xa9 \xe8\xb7\xaf\xe5\xbe\x84 \xe2\x98\x83__");
        prompt.send("stormtty_probe_cwd");
        expect(session, "workspace caf\xc3\xa9 \xe8\xb7\xaf\xe5\xbe\x84__");
        require(until([&] { return session->info().working_directory.find("workspace") != std::string::npos; }),
                "OSC 7 did not reach Session cwd metadata");
        for (unsigned phase=0; phase<resize_repeats; ++phase) {
            const bool enlarged=phase%2==0;
            const std::string dimensions=enlarged?"111x35":"100x30";
            std::cout << "[resize phase " << phase+1 << '/' << resize_repeats << "] target=" << dimensions << '\n';
            prompt.send("stormtty_probe_journal_reset"); prompt.ready(0);
            auto flush = [&] {
                prompt.send("stormtty_probe_journal_flush");
                prompt.ready(0);
            };
            try {
                require(session->request_resize(enlarged?TerminalSize{111,35,8,16}:TerminalSize{100,30,8,16})
                        == SessionSendResult::accepted, "resize request rejected");
                const bool shell_resized = until([&] {
                    const auto sequence=prompt.sequence;
                    prompt.send("stormtty_probe_size");
                    prompt.ready(0); // Any lost/altered command fails immediately, never retried.
                    const auto frame=screen(session);
                    return frame.find("__STORMTTY_SIZE:"+dimensions+"__") != std::string::npos &&
                        frame.find("__STORMTTY_SIZE_ACK:"+std::to_string(sequence)+":"+dimensions+"__") != std::string::npos;
                });
                std::cout << "shell_resize_observed=" << shell_resized << '\n';
                require(shell_resized, "shell did not observe terminal resize");
                // Independent native dimensions never substitute for shell evidence.
                const auto native_sequence=prompt.sequence;
                prompt.send("stormtty_probe_native_size"); prompt.ready(0);
                expect(session, "__STORMTTY_NATIVE_SIZE:"+dimensions+"__");
                expect(session, "__STORMTTY_NATIVE_SIZE_ACK:"+std::to_string(native_sequence)+":"+dimensions+"__");
                dump_process_diagnostics(session, "phase-complete");
                flush();
                std::cout << "[resize phase " << phase+1 << "] PASS\n";
            } catch (...) {
                // Preserve evidence before flushing or running any child, which
                // may redraw the terminal or incidentally refresh shell state.
                std::cerr << "Original resize failure frame (before diagnostics):\n" << screen(session);
                dump_process_diagnostics(session, "original-failure-before-diagnostics");
                // Diagnostics only after the original failure. Neither a
                // successful flush nor new observations can change that result.
                try {
                    prompt.ready(); // Wait for this exact cycle, allowing nonzero status.
                    flush();
                } catch (const std::exception& error) {
                    std::cerr << "Post-failure journal flush unavailable: " << error.what() << '\n';
                }
                try {
                    std::cout << "Post-failure dimensions (original test remains FAILED; no forced WINCH):\n";
                    const auto sequence=prompt.sequence;
                    prompt.send("stormtty_probe_resize_diagnostic");
                    expect(session, "__STORMTTY_RESIZE_DIAG_END:"+std::to_string(sequence)+"__");
                    prompt.ready(0);
                    std::cout << screen(session);
                } catch (const std::exception& error) {
                    std::cerr << "Post-failure size diagnostic unavailable: " << error.what() << '\n';
                }
                throw;
            }
        }
        prompt.send("stormtty_probe_wait first"); expect(session, "__STORMTTY_WAITING:first__");
        key(session, Key::c, Modifier::control);
        prompt.ready(130);
        prompt.send("stormtty_probe_after_interrupt"); expect(session, "__STORMTTY_INTERRUPTED:130__");
        // Clear old markers before another long-running real foreground job.
        prompt.send("printf '\\033[2J\\033[H'; stormtty_probe_wait second");
        expect(session, "__STORMTTY_WAITING:second__");
        key(session, Key::z, Modifier::control);
        prompt.send("stormtty_probe_job_stopped"); expect(session, "__STORMTTY_JOB_STOPPED__");
        prompt.send("bg; stormtty_probe_job_running"); expect(session, "__STORMTTY_JOB_RUNNING__");
        // fg reports "running" (bg reports "continued"). This is an
        // observed command transition, not a timed guess. The later jobs-empty
        // assertion also rejects interrupting only the shell with a live job.
        prompt.send("fg"); expect(session, "+ running");
        key(session, Key::c, Modifier::control);
        prompt.ready(130);
        prompt.send("stormtty_probe_after_interrupt"); expect(session, "__STORMTTY_INTERRUPTED:130__");
        prompt.send("stormtty_probe_jobs_empty"); expect(session, "__STORMTTY_JOBS_EMPTY__");
        std::cout << "provider=" << session->diagnostics().provider << "\n" << screen(session);
        prompt.send("stormtty_probe_exit");
        require(session->wait_closed(10s), "zsh natural exit did not close Session");
        require(session->info().state == SessionState::exited, "zsh exited in failed state");
        require(session->info().exit_code == 37, "zsh exit status lost");
        dump_journal(journal);
#ifdef _WIN32
        std::cout << "PASS native Windows bundled-shell candidate through Process/Session\n";
#else
        std::cout << "PASS POSIX probe mechanics only; NOT Windows bundled-shell qualification\n";
#endif
        return 0;
    } catch (const std::exception& e) {
        if (session) { std::cerr << "Failure frame:\n" << screen(session); session->request_close(); (void)session->wait_closed(10s); }
        dump_journal(journal);
        std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
}
