// Real Zsh -> StormTTY Process -> Session/libghostty qualification; no fake shell.
#include "stormtty/session.hpp"
#ifdef STORMTTY_QUALIFICATION_INPUT_EVIDENCE
#include "stormtty/qualification_input_evidence.hpp"
#endif
#include <chrono>
#include <charconv>
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
// Privacy-safe experimental result: a fixed code and bounded numeric fields only.
// Never serialize exception text, command input, or screen contents here.
struct ResizeSummary {
    bool enabled{}, emitted{};
    unsigned code{1}, phase{}, expected_cols{}, expected_rows{}, observed_cols{}, observed_rows{};
    void observe(const std::string& frame, std::uint64_t sequence) {
        observed_cols=observed_rows=0;
        const auto marker="__STORMTTY_SIZE_ACK:"+std::to_string(sequence)+":";
        const auto start=frame.rfind(marker);
        if (start==std::string::npos) return;
        const char* begin=frame.data()+start+marker.size();
        const char* end=frame.data()+frame.size();
        unsigned cols{}, rows{};
        const auto x=std::from_chars(begin,end,cols);
        if (x.ec!=std::errc{} || x.ptr==end || *x.ptr!='x') return;
        const auto y=std::from_chars(x.ptr+1,end,rows);
        if (y.ec!=std::errc{} || end-y.ptr<2 || y.ptr[0]!='_' || y.ptr[1]!='_' ||
            cols<1 || rows<1 || cols>32767 || rows>32767) return;
        observed_cols=cols; observed_rows=rows;
    }
    void emit() {
        if (!enabled || emitted) return;
        emitted=true;
        std::cout << "RESIZE_CASE_SUMMARY code=" << code << " phase=" << phase
            << " expected_cols=" << expected_cols << " expected_rows=" << expected_rows
            << " observed_cols=" << observed_cols << " observed_rows=" << observed_rows << '\n';
    }
};
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
// Copy only the experiment's bounded numeric trace files, never arbitrary files.
// A live writer may append after this boundary: snapshots are evidence prefixes,
// not an atomic global stop. No shell command or console query is used here.
void collect_resize_trace(const fs::path& source, const fs::path& destination) {
    require(fs::create_directory(destination), "trace snapshot directory already exists");
    std::size_t count=0, total=0;
    for (const auto& entry : fs::directory_iterator(source)) {
        const auto name=entry.path().filename().string();
        require(name.starts_with("resize-") && name.ends_with(".log") && name.size()>11 &&
                name.substr(7,name.size()-11).find_first_not_of("0123456789")==std::string::npos,
                "unexpected file in resize trace directory");
        require(fs::is_regular_file(entry.symlink_status()), "trace entry must be a regular file, not a link");
        const auto size=entry.file_size();
        require(++count<=32 && size<=2*1024*1024 && total+size<=16*1024*1024,
                "resize trace snapshot budget exceeded");
        total+=static_cast<std::size_t>(size);
        std::ifstream input(entry.path(),std::ios::binary);
        require(bool(input), "trace file could not be opened");
        std::string bytes(static_cast<std::size_t>(size),'\0');
        input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));
        require(input.gcount()==static_cast<std::streamsize>(bytes.size()), "trace file changed or could not be read");
        std::ofstream output(destination/entry.path().filename(),std::ios::binary);
        output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
        output.close();
        require(bool(output), "trace snapshot write failed");
    }
    std::cout << "RESIZE_TRACE_COLLECTED files=" << count << " bytes=" << total
              << " directory=" << utf8(destination) << " prefix_only=1\n";
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
    bool repeats_set=false, trace_idle=false, resize_one_shot=false, resize_summary=false;
    fs::path trace_directory;
    // Experiment options follow positional/provider arguments, in either order.
    while (argc>1) {
        const std::string_view last=argv[argc-1];
        if (last=="--resize-one-shot") {
            if (resize_one_shot) { std::cerr << "duplicate --resize-one-shot\n"; return 2; }
            resize_one_shot=true; --argc;
        } else if (last=="--resize-summary") {
            if (resize_summary) { std::cerr << "duplicate --resize-summary\n"; return 2; }
            resize_summary=true; --argc;
        } else if (last=="--resize-trace-idle") {
            if (trace_idle) { std::cerr << "duplicate --resize-trace-idle\n"; return 2; }
            trace_idle=true; --argc;
        } else if (argc>=3 && std::string_view(argv[argc-2])=="--resize-trace-directory") {
            if (!trace_directory.empty() || last.empty()) { std::cerr << "invalid trace directory option\n"; return 2; }
            trace_directory=path_utf8(last); argc-=2;
        } else if (argc>=3 && std::string_view(argv[argc-2])=="--resize-repeats") {
            const std::string count(last);
            if (repeats_set || count.empty() || count.size()>2 || count.find_first_not_of("0123456789")!=std::string::npos ||
                (resize_repeats=static_cast<unsigned>(std::strtoul(count.c_str(),nullptr,10)))<1 || resize_repeats>50) {
                std::cerr << "--resize-repeats must occur once and be 1..50\n"; return 2;
            }
            repeats_set=true; argc-=2;
        } else break;
    }
    if (argc < 4 || argc > 6 || (argc==5 && std::string_view(argv[4])!="inbox") ||
        (argc==6 && std::string_view(argv[4])!="--packaged-provider") || (trace_idle && (trace_directory.empty() || repeats_set || resize_one_shot)) ||
        (resize_one_shot && !repeats_set)) {
        std::cerr << "usage: bundled_shell_probe SHELL_ABSOLUTE FIXTURE_ABSOLUTE SCRATCH_DIR [inbox | --packaged-provider ABSOLUTE_DIRECTORY] [--resize-repeats 1..50] [--resize-trace-directory ABSOLUTE_EMPTY_DIRECTORY] [--resize-trace-idle] [--resize-one-shot] [--resize-summary]\n";
        return 2;
    }
    if (!trace_directory.empty()) {
        std::error_code error;
        const bool directory=fs::is_directory(trace_directory,error);
        const bool empty=directory && !error && fs::is_empty(trace_directory,error);
        if (!trace_directory.is_absolute() || error || !empty) {
            std::cerr << "trace directory must be absolute, existing and empty\n"; return 2;
        }
#ifdef _WIN32
        trace_directory.make_preferred();
        const auto native=trace_directory.native();
        if (native.size()<3 || native.size()>=180 || native[1]!=L':' || native[2]!=L'\\' ||
            !((native[0]>=L'A' && native[0]<=L'Z') || (native[0]>=L'a' && native[0]<=L'z')) ||
            trace_directory.lexically_normal()!=trace_directory) {
            std::cerr << "trace directory must be a normalized local drive path shorter than 180 characters\n"; return 2;
        }
        for (auto ancestor=trace_directory; !ancestor.empty();) {
            const auto attributes=GetFileAttributesW(ancestor.c_str());
            const auto component=ancestor.filename().native();
            if (attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
                (!component.empty() && (component.back()==L'.' || component.back()==L' '))) {
                std::cerr << "trace directory must not contain reparse points or ambiguous components\n"; return 2;
            }
            const auto parent=ancestor.parent_path();
            if (parent==ancestor) break;
            ancestor=parent;
        }
#endif
    }
    std::cout << std::unitbuf;
    ResizeSummary summary; summary.enabled=resize_summary;
    std::shared_ptr<Session> session;
    fs::path journal;
#ifdef STORMTTY_QUALIFICATION_INPUT_EVIDENCE
    auto input_evidence = std::make_shared<qualification::InputEvidence>();
    std::uint64_t last_input_sequence = 0;
    auto dump_input = [&](std::string_view label) {
        const auto snapshot = input_evidence->snapshot(last_input_sequence);
        qualification::InputEvidence::dump(std::cout, snapshot, label);
        last_input_sequence = snapshot.completed;
    };
#endif
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
        if (!trace_directory.empty()) {
            // Runtime trace validates a native drive path, not MSYS/generic slashes.
            const auto native=trace_directory.u8string();
            p.environment.push_back({"STORMTTY_MSYS_RESIZE_TRACE_DIR",
                std::string(reinterpret_cast<const char*>(native.data()),native.size())});
        } else p.environment.push_back({"STORMTTY_MSYS_RESIZE_TRACE_DIR",std::nullopt});
        TerminalOptions terminal; terminal.size = {100, 30, 8, 16};
        const auto started = std::chrono::steady_clock::now();
#ifdef STORMTTY_QUALIFICATION_INPUT_EVIDENCE
        // Only this explicit private synthetic fixture session can opt in. Do
        // not attach to the managed-home helper, user launch, or arbitrary app.
        p.qualification_input_evidence = input_evidence;
#endif
        session = Session::start(std::move(p), terminal);
        expect(session, "__STORMTTY_READY__");
        PromptProtocol prompt{session};
        prompt.ready(0);
        std::cout << "provider=" << session->diagnostics().provider << '\n';
        std::cout << "startup_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count() << '\n';
        require(screen(session).find("GLOBAL_DOTFILE_WAS_READ") == std::string::npos,
                "managed profile sourced HOME/.zshrc");
        if (!trace_idle && !resize_one_shot) {
            prompt.send("stormtty_probe_tty"); expect(session, "__STORMTTY_TTY_OK__");
            prompt.send("stormtty_probe_native"); expect(session, "__STORMTTY_NATIVE_TTY_OK__");
            prompt.send("stormtty_probe_unicode 'caf\xc3\xa9 \xe8\xb7\xaf\xe5\xbe\x84 \xe2\x98\x83'");
            expect(session, "__STORMTTY_UNICODE:caf\xc3\xa9 \xe8\xb7\xaf\xe5\xbe\x84 \xe2\x98\x83__");
            prompt.send("stormtty_probe_cwd");
            expect(session, "workspace caf\xc3\xa9 \xe8\xb7\xaf\xe5\xbe\x84__");
            require(until([&] { return session->info().working_directory.find("workspace") != std::string::npos; }),
                    "OSC 7 did not reach Session cwd metadata");
        }
        for (unsigned phase=0; phase<resize_repeats; ++phase) {
            const bool enlarged=phase%2==0;
            const std::string dimensions=enlarged?"111x35":"100x30";
            summary.phase=phase+1; summary.expected_cols=enlarged?111:100;
            summary.expected_rows=enlarged?35:30; summary.observed_cols=summary.observed_rows=0;
            summary.code=3;
            std::cout << "[resize phase " << phase+1 << '/' << resize_repeats << "] target=" << dimensions << '\n';
            if (!trace_idle && !resize_one_shot) { prompt.send("stormtty_probe_journal_reset"); prompt.ready(0); }
            else if (trace_idle) std::cout << "RESIZE_TRACE_IDLE prompt_sequence=" << prompt.sequence << " zle_initialized=1\n";
            bool trace_collected=false, trace_attempted=false;
            auto collect = [&] {
                if (trace_directory.empty() || trace_collected) return;
                trace_attempted=true;
                collect_resize_trace(trace_directory,scratch/("resize-trace-phase-"+std::to_string(phase+1)));
                trace_collected=true;
            };
            auto flush = [&] {
                prompt.send("stormtty_probe_journal_flush");
                prompt.ready(0);
            };
            try {
                summary.code=2;
                require(session->request_resize(enlarged?TerminalSize{111,35,8,16}:TerminalSize{100,30,8,16})
                        == SessionSendResult::accepted, "resize request rejected");
                auto observe_size = [&] {
                    summary.code=3;
                    summary.observed_cols=summary.observed_rows=0;
                    const auto sequence=prompt.sequence;
                    prompt.send("stormtty_probe_size");
                    prompt.ready(0); // Any lost/altered command fails immediately, never retried.
                    const auto frame=screen(session);
                    if (summary.enabled) summary.observe(frame,sequence);
                    return frame.find("__STORMTTY_SIZE:"+dimensions+"__") != std::string::npos &&
                        frame.find("__STORMTTY_SIZE_ACK:"+std::to_string(sequence)+":"+dimensions+"__") != std::string::npos;
                };
                const bool shell_resized = (trace_idle || resize_one_shot) ? observe_size() : until(observe_size);
                summary.code=5;
                collect(); // Before native helper, diagnostic commands, or teardown.
                std::cout << "shell_resize_observed=" << shell_resized << '\n';
                summary.code=4;
                require(shell_resized, "shell did not observe terminal resize");
                summary.code=6;
                if (trace_idle) {
                    dump_process_diagnostics(session,"trace-idle-complete");
#ifdef STORMTTY_QUALIFICATION_INPUT_EVIDENCE
                    dump_input("trace-idle-complete");
#endif
                    summary.code=7;
                    session->request_close();
                    require(session->wait_closed(10s), "trace experiment cleanup did not close Session");
                    summary.code=0; summary.emit();
                    std::cout << "PASS isolated idle-resize observation; NOT full bundled-shell qualification\n";
                    return 0;
                }
                if (resize_one_shot) {
                    std::cout << "[resize phase " << phase+1 << "] PASS\n";
                    continue; // No native jobs, journal commands or size-query polling.
                }
                // Independent native dimensions never substitute for shell evidence.
                const auto native_sequence=prompt.sequence;
                prompt.send("stormtty_probe_native_size"); prompt.ready(0);
                expect(session, "__STORMTTY_NATIVE_SIZE:"+dimensions+"__");
                expect(session, "__STORMTTY_NATIVE_SIZE_ACK:"+std::to_string(native_sequence)+":"+dimensions+"__");
                dump_process_diagnostics(session, "phase-complete");
                flush();
#ifdef STORMTTY_QUALIFICATION_INPUT_EVIDENCE
                dump_input("resize-phase-success");
#endif
                std::cout << "[resize phase " << phase+1 << "] PASS\n";
            } catch (...) {
                summary.emit(); // Before diagnostics can refresh shell state.
                // Preserve evidence before flushing or running any child, which
                // may redraw the terminal or incidentally refresh shell state.
                std::cerr << "Original resize failure frame (before diagnostics):\n" << screen(session);
                dump_process_diagnostics(session, "original-failure-before-diagnostics");
#ifdef STORMTTY_QUALIFICATION_INPUT_EVIDENCE
                dump_input("original-resize-failure-before-diagnostics");
#endif
                try {
                    if (!trace_attempted) collect();
                    else if (!trace_collected) throw; // Preserve original collection failure; never retry.
                }
                catch (const std::exception& error) {
                    std::cerr << "Resize trace collection failed: " << error.what() << '\n';
                    throw; // Do not run contaminating diagnostics without evidence.
                }
                if (trace_idle || resize_one_shot) {
                    std::cerr << "Trace idle experiment: post-failure shell diagnostics suppressed\n";
                    throw;
                }
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
        if (resize_one_shot) {
            summary.code=7;
            session->request_close();
            require(session->wait_closed(10s), "one-shot experiment cleanup did not close Session");
#ifdef STORMTTY_QUALIFICATION_INPUT_EVIDENCE
            dump_input("one-shot-complete");
#endif
            summary.code=0; summary.emit();
            std::cout << "PASS isolated builtin-only repeated one-shot resize; NOT full bundled-shell qualification\n";
            return 0;
        }
        summary.code=8;
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
#ifdef STORMTTY_QUALIFICATION_INPUT_EVIDENCE
        dump_input("final-result");
#endif
        dump_journal(journal);
#ifdef _WIN32
        std::cout << "PASS native Windows bundled-shell candidate through Process/Session\n";
#else
        std::cout << "PASS POSIX probe mechanics only; NOT Windows bundled-shell qualification\n";
#endif
        summary.code=0; summary.emit();
        return 0;
    } catch (const std::exception& e) {
        summary.emit();
        if (session) { std::cerr << "Failure frame:\n" << screen(session); session->request_close(); (void)session->wait_closed(10s); }
#ifdef STORMTTY_QUALIFICATION_INPUT_EVIDENCE
        dump_input("final-result");
#endif
        dump_journal(journal);
        std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
}
