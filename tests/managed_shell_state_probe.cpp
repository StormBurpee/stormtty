// Native Windows, real bundled Zsh -> ConPTY -> Session/libghostty. All fixture
// data is synthetic. Emit fixed result codes/counts/timings only, never frames.
#include "stormtty/session.hpp"
#include "stormtty/windows_shell_state.hpp"
#include "stormtty/windows_managed_shell.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace fs = std::filesystem;
using namespace stormtty;
using namespace std::chrono_literals;
namespace {
unsigned phase{}, checks{};
struct TypingEvidence {
    unsigned characters{}, frames{}, retreats{};
    long long echo_total_us{}, echo_max_us{};
} typing;
void check(bool ok) { ++checks; if (!ok) throw std::runtime_error("qualification failed"); }
std::string utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
std::string posix(const fs::path& path) {
    auto value = utf8(path);
    check(value.size() > 2 && value[1] == ':');
    char drive = value[0]; if (drive >= 'A' && drive <= 'Z') drive += 'a' - 'A';
    return "/" + std::string(1, drive) + value.substr(2);
}
void write(const fs::path& path, std::string_view data) {
    std::ofstream stream(path, std::ios::binary); stream << data; stream.close(); check(stream.good());
}
struct Fixture {
    fs::path root, home, cwd;
    Fixture() {
        PWSTR raw{};
        check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)));
        struct Owned { PWSTR p; ~Owned() { CoTaskMemFree(p); } } owned{raw};
        root = fs::path(raw) / (L"StormTTY managed qualification \u00e9 \u8def\u5f84 " +
            std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(GetTickCount64()));
        home = root / L"home \u00e9"; cwd = root / L"cwd \u8def\u5f84";
        check(fs::create_directories(home)); check(fs::create_directory(cwd));
        write(home / L".zshrc", "typeset -g QUAL_GLOBAL_RC_READ=1\n");
        write(cwd / L"qualification_unique_completion.txt", "synthetic\n");
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};
std::string screen(const std::shared_ptr<Session>& session) {
    std::string result;
    if (const auto frame = session->frame()) for (const auto& line : frame->lines) {
        for (const auto& cell : line->cells) if (cell.span) result += cell.grapheme.empty() ? " " : cell.grapheme;
        result += '\n';
    }
    return result;
}
template<class F> bool until(F predicate, std::chrono::milliseconds timeout = 15s) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    do { if (predicate()) return true; std::this_thread::sleep_for(20ms); } while (std::chrono::steady_clock::now() < end);
    return predicate();
}
struct ManagedSessionProbe {
    std::shared_ptr<Session> session;
    unsigned sequence{};
    long long startup_ms{};
    ManagedSessionProbe(const fs::path& runtime, const fs::path& provider, const Fixture& fixture) {
        // Every fresh launch calls the real native validator. No spoofed trust
        // assertion, ambient path, PowerShell subprocess, or POSIX mode bypass.
        const auto state = prepare_managed_shell_state_at(fixture.root, false);
        check(static_cast<bool>(state));
        ProcessOptions process;
        process.executable = utf8(runtime / L"usr" / L"bin" / L"zsh.exe");
        process.arguments = {"-d", "-i"}; process.working_directory = utf8(fixture.cwd); process.grid = {160, 40};
        process.provider = provider.empty() ? ProcessProvider::windows_inbox_basic : ProcessProvider::windows_packaged_experimental;
        process.provider_directory = utf8(provider);
        process.environment = {{"HOME", utf8(fixture.home)}, {"ZDOTDIR", utf8(runtime / L"stormtty" / L"zsh")},
            {"STORMTTY_SESSION_ID", "native-qualification"}, {"STORMTTY_EMPTY_HOME", ""},
            {"STORMTTY_NATIVE_ZSH_STATE", utf8(state.directory)},
            {"QUAL_EXPECT_HOME", posix(fixture.home)}, {"QUAL_EXPECT_CWD", posix(fixture.cwd)},
            {"PATH", utf8(runtime / L"usr" / L"bin")}, {"TERM", "xterm-256color"}, {"COLORTERM", "truecolor"},
            {"LANG", "C.UTF-8"}, {"LC_ALL", "C.UTF-8"}, {"MSYSTEM", "MSYS"},
            {"MSYS", std::nullopt}, {"ENV", std::nullopt}, {"BASH_ENV", std::nullopt},
            {"STORMTTY_MSYS_RESIZE_TRACE_DIR", std::nullopt},
            {"ZSH_AUTOSUGGEST_USE_ASYNC", std::nullopt}, {"ZSH_AUTOSUGGEST_STRATEGY", std::nullopt}};
        process.environment.push_back(managed_shell_console_environment());
        TerminalOptions terminal; terminal.size = {160, 40, 8, 16};
        const auto started = std::chrono::steady_clock::now();
        session = Session::start(std::move(process), terminal);
        expect("QZLE_0 >");
        startup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        // Install downstream of _stormtty_precmd, not before it via config.
        command("add-zsh-hook precmd _qual_next"); ready(0);
    }
    ~ManagedSessionProbe() { if (session) { session->request_close(); (void)session->wait_closed(8s); } }
    void expect(std::string_view token) { check(until([&] { return screen(session).find(token) != std::string::npos; })); }
    void text(std::string value) { check(session->text(std::move(value)) == SessionSendResult::accepted); }
    void key(Key value) { KeyInput input; input.key = value; check(session->key(input) == SessionSendResult::accepted); }
    void command(std::string value) { text(std::move(value)); key(Key::enter); ++sequence; }
    void ready(unsigned status) {
        expect("QREADY_" + std::to_string(sequence) + "_" + std::to_string(status));
        expect("QZLE_" + std::to_string(sequence) + " >");
    }
    void quoted_typing() {
        command("[[ ${+ZSH_AUTOSUGGEST_USE_ASYNC} == 0 && $ZSH_AUTOSUGGEST_STRATEGY == history ]] && print -r -- QPOLICY$((61+2))");
        expect("QPOLICY63"); ready(0);
        command("typeset -gi _qual_async_calls=0; _zsh_autosuggest_async_request() { (( ++_qual_async_calls )); return 0; }"); ready(0);
        const auto prompt="QZLE_"+std::to_string(sequence)+" > ";
        auto row_text=[](const FramePtr& frame, unsigned row) {
            std::string text;
            if (frame && row<frame->lines.size() && frame->lines[row])
                for (const auto& cell:frame->lines[row]->cells)
                    if (cell.span) text+=cell.grapheme.empty()?" ":cell.grapheme;
            return text;
        };
        check(until([&] {
            const auto f=session->frame();
            return f && f->cursor.visible && f->cursor.column==prompt.size() &&
                row_text(f,f->cursor.row).starts_with(prompt);
        }));
        const auto origin=session->frame()->cursor;
        std::string entered;
        const auto end=std::chrono::steady_clock::now()+15s;
        std::uint64_t generation{};
        unsigned last_column=origin.column;
        auto observe=[&] {
            const auto f=session->frame();
            if (f && f->generation!=generation) {
                generation=f->generation; ++typing.frames;
                if (f->cursor.row==origin.row) {
                    if (f->cursor.column<last_column) ++typing.retreats;
                    last_column=f->cursor.column;
                }
            }
            return f;
        };
        auto exact=[&](const FramePtr& f) {
            const auto expected=prompt+entered;
            const auto row=row_text(f,origin.row);
            return f && f->cursor.visible && f->cursor.row==origin.row &&
                f->cursor.column==expected.size() && row.starts_with(expected) &&
                row.find_first_not_of(' ',expected.size())==std::string::npos;
        };
        auto wait_echo=[&] {
            while (std::chrono::steady_clock::now()<end) {
                if (exact(observe())) return;
                std::this_thread::sleep_for(2ms);
            }
            check(false);
        };
        // An authored, unique colon command cannot match earlier fixture
        // history. Normal per-character input, not paste or injected frames.
        for (const char character:std::string(": \"QTYPING quoted sample\"")) {
            const auto start=std::chrono::steady_clock::now();
            entered+=character; text(std::string(1,character)); wait_echo();
            const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
            ++typing.characters; typing.echo_total_us+=elapsed;
            typing.echo_max_us=std::max(typing.echo_max_us,static_cast<long long>(elapsed));
            const auto observe_until=std::chrono::steady_clock::now()+150ms;
            while (std::chrono::steady_clock::now()<observe_until) {
                check(std::chrono::steady_clock::now()<end);
                (void)observe(); std::this_thread::sleep_for(2ms);
            }
            // Retreats during unsynchronized ZLE repaint are diagnostics, not
            // failures. Require eventual authoritative text/cursor only.
            wait_echo();
        }
        entered+='x'; text("x"); wait_echo();
        entered.pop_back(); last_column=origin.column+static_cast<unsigned>(entered.size());
        key(Key::backspace); wait_echo(); // Deliberate erase is not a repaint retreat.
        key(Key::enter); ++sequence; ready(0);
        command("(( _qual_async_calls == 0 )) && print -r -- QNOFORK$((70+3))");
        expect("QNOFORK73"); ready(0);
    }
    void finish() {
        command("exit"); check(session->wait_for_process_exit(15s));
        const auto info = session->info(); check(info.exit_code && *info.exit_code == 0 && info.error.empty());
        session->request_close(); check(session->wait_closed(8s));
    }
};
const char config[] = R"ZSH(typeset -g _qual_config=73
STORMTTY_PROMPT=custom
PROMPT='QBOOT > '
typeset -gi _qual_seq=0
autoload -Uz add-zle-hook-widget
_qual_line_init() { PROMPT="QZLE_${_qual_seq} > "; zle reset-prompt; }
add-zle-hook-widget line-init _qual_line_init
_qual_next() { local last=$?; (( ++_qual_seq )); print -r -- "QREADY_${_qual_seq}_${last}"; return 0; }
)ZSH";
}
int main() {
    long long cold_ms{}, warm_ms{};
    try {
        phase = 1;
        int count{}; LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
        struct Arguments { LPWSTR* p; ~Arguments() { if (p) LocalFree(p); } } owned{args};
        check(args && (count == 2 || count == 3));
        const fs::path runtime = fs::absolute(args[1]);
        const fs::path provider = count == 3 ? fs::absolute(args[2]) : fs::path{};
        check(fs::is_regular_file(runtime / L"usr" / L"bin" / L"zsh.exe"));
        check(fs::is_regular_file(runtime / L"stormtty" / L"zsh" / L".zshrc"));
        check(fs::is_regular_file(runtime / L"stormtty" / L"vendor" / L"oh-my-zsh" / L"oh-my-zsh.sh"));
        Fixture fixture;
        const auto state = prepare_managed_shell_state_at(fixture.root, false); check(static_cast<bool>(state));
        write(state.directory / L"config.zsh", config);
        fs::path dump; fs::file_time_type dump_time{};
        phase = 2;
        {
            ManagedSessionProbe shell(runtime, provider, fixture); cold_ms = shell.startup_ms;
            phase = 3;
            // Keep each contract independently observable without logging paths,
            // shell contents or history. A failed bit identifies the invariant;
            // the same required predicates still block qualification.
            const std::array<const char*, 10> contracts = {
                "-r $_stormtty_vendor/oh-my-zsh/oh-my-zsh.sh",
                "$STORMTTY_OMZ_LOADED == 1", "$STORMTTY_EPHEMERAL == 0", "$_qual_config == 73",
                "${+functions[_zsh_autosuggest_start]} == 1", "${+functions[_zsh_highlight]} == 1",
                "${+STORMTTY_NATIVE_ZSH_STATE} == 0", "${+QUAL_GLOBAL_RC_READ} == 0",
                "$HOME == $QUAL_EXPECT_HOME", "$PWD == $QUAL_EXPECT_CWD"};
            for (std::size_t index = 0; index < contracts.size(); ++index) {
                const std::string marker = "QCONTRACT" + std::to_string(index + 1);
                shell.command("[[ " + std::string(contracts[index]) + " ]]; print -r -- " + marker + "_$?");
                shell.ready(0);
                const bool passed = screen(shell.session).find(marker + "_0") != std::string::npos;
                std::cout << "MANAGED_STATE_CONTRACT index=" << index + 1 << " passed=" << passed << '\n';
                check(passed);
            }
            phase = 4;
            shell.command("false"); shell.ready(1);
            shell.command("(( _stormtty_exit == 1 )) && [[ ${STORMTTY_CONTEXT[exit]} == 1 ]] && print -r -- QHOOK$((40+3))");
            shell.expect("QHOOK43"); shell.ready(0);
            phase = 5;
            shell.text("[[ -f qualification_unique_comp"); shell.key(Key::tab);
            shell.expect("qualification_unique_completion.txt");
            shell.text(" ]] && print -r -- QTAB$((80+1))"); shell.key(Key::enter); ++shell.sequence;
            shell.expect("QTAB81"); shell.ready(0);
            phase = 6;
            shell.command("print -r -- QSUGGEST$((700+31))"); shell.expect("QSUGGEST731"); shell.ready(0);
            shell.finish();
        }
        phase = 7;
        check(fs::is_regular_file(state.directory / L"history"));
        check(fs::file_size(state.directory / L"history") > 0);
        for (const auto& entry : fs::directory_iterator(state.directory)) {
            const auto name = entry.path().filename().wstring();
            if (name.starts_with(L"zcompdump-") && !name.ends_with(L".zwc") && !name.ends_with(L".old") && !name.ends_with(L".lock")) {
                check(fs::is_regular_file(entry.path())); dump = entry.path();
            }
        }
        check(!dump.empty()); dump_time = fs::last_write_time(dump);
        phase = 8;
        {
            ManagedSessionProbe shell(runtime, provider, fixture); warm_ms = shell.startup_ms;
            phase = 9;
            shell.command("[[ $_qual_config == 73 && $STORMTTY_EPHEMERAL == 0 ]] && print -r -- QWARM$((20+3))");
            shell.expect("QWARM23"); shell.ready(0);
            phase = 10;
            shell.text("print -r -- QSUGGEST");
            shell.expect("700+31"); // History-backed live autosuggestion, before acceptance.
            shell.key(Key::right); shell.key(Key::enter); ++shell.sequence;
            shell.expect("QSUGGEST731"); shell.ready(0);
            shell.quoted_typing();
            const auto d=shell.session->info().deadline;
            std::cout<<"SHIPPING_DEADLINE requested="<<d.requested<<" available="<<d.available<<" waits="<<d.waits<<" timers="<<d.timer_wakes<<" notified="<<d.notification_wakes<<" spurious="<<d.spurious_wakes<<" failures="<<d.failures<<" overshoot_total_us="<<d.overshoot_total_us<<" overshoot_max_us="<<d.overshoot_max_us<<'\n';
            shell.finish();
        }
        phase = 11;
        check(fs::last_write_time(dump) == dump_time); // Cache reused, not regenerated.
        check(static_cast<bool>(prepare_managed_shell_state_at(fixture.root, false)));
        std::ifstream input(state.directory / L"config.zsh", std::ios::binary);
        const std::string persisted{std::istreambuf_iterator<char>(input), {}};
        check(persisted == config);
        std::cout << "MANAGED_TYPING_RESULT code=0 characters=" << typing.characters << " frames=" << typing.frames << " retreats=" << typing.retreats << " echo_total_us=" << typing.echo_total_us << " echo_max_us=" << typing.echo_max_us << '\n';
        std::cout << "MANAGED_STATE_RESULT code=0 phase=11 cold_samples=1 warm_samples=1 checks=" << checks << " cold_ms=" << cold_ms << " warm_ms=" << warm_ms << '\n';
        return 0;
    } catch (...) {
        std::cout << "MANAGED_TYPING_RESULT code=1 characters=" << typing.characters << " frames=" << typing.frames << " retreats=" << typing.retreats << " echo_total_us=" << typing.echo_total_us << " echo_max_us=" << typing.echo_max_us << '\n';
        std::cout << "MANAGED_STATE_RESULT code=1 phase=" << phase << " checks=" << checks << " cold_ms=" << cold_ms << " warm_ms=" << warm_ms << '\n';
        return 1;
    }
}
