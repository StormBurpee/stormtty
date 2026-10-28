// Native Windows, real bundled Zsh -> ConPTY -> Session/libghostty. All fixture
// data is synthetic. Emit fixed result codes/counts/timings only, never frames.
#include "stormtty/session.hpp"
#include "stormtty/windows_shell_state.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
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
        check(fs::create_directory(cwd / L"navigation space é"));
        check(fs::create_directories(cwd / L"walker space é" / L"nested 路径"));
        write(cwd / L"walker space é" / L"nested 路径" / L"walker_unique_file.txt", "synthetic\n");
        check(fs::create_directory(cwd / L"literal $(not_a_command) `not_a_command`"));
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
            {"STORMTTY_MSYS_RESIZE_TRACE_DIR", std::nullopt}};
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
    void absent(std::string_view token) { check(until([&] { return screen(session).find(token) == std::string::npos; })); }
    void selected(std::string_view path) {
        // The walker and query matcher run asynchronously. Prompt visibility
        // alone does not mean Enter can accept a candidate. Observe the actual
        // selected row, with a fixed fixture-only ASCII pointer, before accepting.
        const bool found = until([&] {
            const auto value = screen(session);
            if (value.find("QFZF_SELECT:") == std::string::npos) return false;
            std::size_t start{};
            while (start < value.size()) {
                const auto end = value.find('\n', start);
                auto line = value.substr(start, end == std::string::npos ? end : end - start);
                const auto first = line.find_first_not_of(' ');
                if (first != std::string::npos && line[first] == '@') {
                    line.erase(0, first + 1);
                    const auto item = line.find_first_not_of(' ');
                    if (item != std::string::npos) {
                        line.erase(0, item);
                        line.erase(line.find_last_not_of(' ') + 1);
                        if (line == path || line == std::string(path) + "/") return true;
                    }
                }
                if (end == std::string::npos) break;
                start = end + 1;
            }
            return false;
        });
        if (!found) {
            // Only the authored fixture's fzf query and selected rows, never a
            // general frame/history dump. Hex preserves exact Unicode bytes.
            const auto value = screen(session);
            std::size_t start{}; unsigned emitted{};
            while (start < value.size() && emitted < 3) {
                const auto end = value.find('\n', start);
                auto line = value.substr(start, end == std::string::npos ? end : end - start);
                const auto first = line.find_first_not_of(' ');
                if (first != std::string::npos &&
                    (line[first] == '@' || line.compare(first, 12, "QFZF_SELECT:") == 0)) {
                    line.erase(0, first); line.erase(line.find_last_not_of(' ') + 1);
                    if (line.size() > 240) line.resize(240);
                    constexpr char hex[] = "0123456789abcdef";
                    std::string encoded;
                    for (const unsigned char byte : line) { encoded += hex[byte >> 4]; encoded += hex[byte & 15]; }
                    std::cout << "SHELL_WALKER_ROW index=" << emitted++ << " hex=" << encoded << std::endl;
                }
                if (end == std::string::npos) break;
                start = end + 1;
            }
            std::cout << "SHELL_WALKER_READY phase=" << phase << " selected=0 rows=" << emitted << std::endl;
        }
        check(found);
    }
    void text(std::string value) { check(session->text(std::move(value)) == SessionSendResult::accepted); }
    void key(Key value, std::uint16_t mods = 0) { KeyInput input; input.key = value; input.modifiers = mods; if (value == Key::t) input.unshifted_codepoint = 't'; if (value == Key::r) input.unshifted_codepoint = 'r'; if (value == Key::c) input.unshifted_codepoint = 'c'; if (value == Key::u) input.unshifted_codepoint = 'u'; check(session->key(input) == SessionSendResult::accepted); }
    void command(std::string value) { text(std::move(value)); key(Key::enter); ++sequence; }
    void ready(unsigned status) {
        expect("QREADY_" + std::to_string(sequence) + "_" + std::to_string(status));
        expect("QZLE_" + std::to_string(sequence) + " >");
    }
    void finish() {
        command("exit"); check(session->wait_for_process_exit(15s));
        const auto info = session->info(); check(info.exit_code && *info.exit_code == 0 && info.error.empty());
        session->request_close(); check(session->wait_closed(8s));
    }
};
const char config[] = R"ZSH(bindkey -e
export FZF_DEFAULT_OPTS='--height=12 --layout=reverse --no-mouse --no-scrollbar --pointer=@ --prompt=QFZF_SELECT:'
export FZF_CTRL_T_COMMAND='printf "%s\n" "qualification_unique_completion.txt"'
export FZF_ALT_C_COMMAND='printf "%s\n" "navigation space é"'
typeset -g _qual_config=73
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
        Fixture fixture;
        const auto state = prepare_managed_shell_state_at(fixture.root, false); check(static_cast<bool>(state));
        write(state.directory / L"config.zsh", config);
        ManagedSessionProbe shell(runtime, provider, fixture); cold_ms = shell.startup_ms;
        auto command = [&](std::string code, std::string_view marker) { shell.command(std::move(code)); shell.expect(marker); };
        phase = 2;
        command("[[ $STORMTTY_EPHEMERAL == 0 && ${+functions[fzf-file-widget]} == 1 && ${+functions[z]} == 1 && ${+functions[zi]} == 1 ]] && print -r -- QTOOLS$((20+2))", "QTOOLS22");
        command("[[ $(fzf --version) == 0.74.4* && $(zoxide --version) == 'zoxide 0.10.0' ]] && print -r -- QVERS$((20+3))", "QVERS23");
        command("(( ! ${+commands[perl]} )) && [[ -x $_stormtty_tools_root/usr/bin/awk.exe ]] && [[ $(awk 'BEGIN {print 712+1}') == 713 ]] && print -r -- QAWK$((20+4))", "QAWK24");
        // A fresh prompt must not create the database or invoke a navigation tool.
        check(!fs::exists(state.directory / L"db.zo"));
        phase = 3;
        // Actual upstream Ctrl-T widget selection, then ordinary ZLE input.
        shell.text("[[ -f "); shell.key(Key::t, control); shell.expect("QFZF_SELECT:");
        shell.key(Key::enter); shell.absent("QFZF_SELECT:"); shell.expect("qualification_unique_completion.txt");
        shell.text(" ]] && print -r -- QFILE$((30+4))"); shell.key(Key::enter); shell.expect("QFILE34");
        phase = 4;
        // Cancel must return to the original editable buffer without executing it.
        shell.text("print -r -- QCANCEL$((30+5))"); shell.key(Key::t, control);
        shell.expect("QFZF_SELECT:"); shell.key(Key::escape); shell.absent("QFZF_SELECT:"); shell.key(Key::enter); shell.expect("QCANCEL35");
        phase = 5;
        command("typeset -gi _qhistory=0; print -r -- QHISTSET$((60+1))", "QHISTSET61");
        command("(( ++_qhistory )); print -r -- QHISTORY$((600+17))", "QHISTORY617");
        shell.key(Key::r, control); shell.expect("QFZF_SELECT:"); shell.text("QHISTORY");
        shell.key(Key::enter); shell.absent("QFZF_SELECT:"); shell.expect("600+17"); shell.key(Key::enter);
        command("(( _qhistory == 2 )) && print -r -- QHISTORY_DONE$((40+6))", "QHISTORY_DONE46");
        phase = 6;
        // Alt-C is an upstream accept-line widget and changes prompt sequence.
        shell.key(Key::c, alt); shell.expect("QFZF_SELECT:"); shell.key(Key::enter); shell.absent("QFZF_SELECT:");
        command("[[ $PWD == */'navigation space é' ]] && print -r -- QDIR$((40+7))", "QDIR47");
        phase = 7;
        command("builtin cd -- ..; z 'navigation space'; [[ $PWD == */'navigation space é' ]] && print -r -- QJUMP$((40+8))", "QJUMP48");
        command("z -; [[ $PWD == $QUAL_EXPECT_CWD ]] && print -r -- QPREV$((40+9))", "QPREV49");
        command("z -- 'literal $(not_a_command) `not_a_command`'; [[ $PWD == *'literal $(not_a_command) `not_a_command`' ]] && print -r -- QLITERAL$((40+10))", "QLITERAL50");
        command("z -; print -r -- QRETURN$((50+1))", "QRETURN51");
        phase = 8;
        check(fs::is_regular_file(state.directory / L"db.zo"));
        check(!fs::exists(fixture.cwd / L"db.zo") && !fs::exists(fixture.home / L"db.zo"));
        // Capture unmodified native-generated init for source/template review.
        command("zoxide init zsh --hook pwd > \"$_stormtty_state/native-zoxide-init.zsh\"; [[ -s $_stormtty_state/native-zoxide-init.zsh ]] && print -r -- QINIT$((50+2))", "QINIT52");
        std::ifstream input(state.directory / L"native-zoxide-init.zsh", std::ios::binary);
        const std::string init{std::istreambuf_iterator<char>(input), {}};
        check(init.size() > 100 && init.size() < 65536);
        fs::create_directories(L"build/artifacts"); write(L"build/artifacts/shell-tools-zoxide-init.zsh", init);
        phase = 9;
        shell.text("cat qualification_unique**"); shell.key(Key::tab); shell.expect("QFZF_SELECT:");
        shell.key(Key::enter); shell.absent("QFZF_SELECT:");
        shell.text(" >/dev/null && print -r -- QCOMPLETE$((60+4))"); shell.key(Key::enter); shell.expect("QCOMPLETE64");
        phase = 10;
        shell.command("zi navigation"); shell.expect("QFZF_SELECT:"); shell.key(Key::enter); shell.absent("QFZF_SELECT:");
        command("[[ $PWD == */'navigation space é' ]] && print -r -- QZI$((60+5))", "QZI65");
        command("z -; print -r -- QZIBACK$((60+6))", "QZIBACK66");
        phase = 11;
        shell.key(Key::r, control); shell.expect("QFZF_SELECT:"); shell.key(Key::escape); shell.absent("QFZF_SELECT:");
        shell.key(Key::c, alt); shell.expect("QFZF_SELECT:"); shell.key(Key::escape); shell.absent("QFZF_SELECT:");
        shell.key(Key::t, control); shell.expect("QFZF_SELECT:"); shell.text("QNO_MATCH_837192"); shell.key(Key::escape); shell.absent("QFZF_SELECT:");
        command("[[ $PWD == $QUAL_EXPECT_CWD ]] && print -r -- QCANCELS$((60+7))", "QCANCELS67");
        shell.command("zi navigation"); shell.expect("QFZF_SELECT:"); shell.key(Key::escape); shell.absent("QFZF_SELECT:");
        command("[[ $PWD == $QUAL_EXPECT_CWD ]] && print -r -- QZICANCEL$((60+8))", "QZICANCEL68");
        phase = 12;
        command("unset FZF_CTRL_T_COMMAND FZF_ALT_C_COMMAND FZF_DEFAULT_COMMAND; print -r -- QDEFAULTS$((70+1))", "QDEFAULTS71");
        shell.text("_qwalker="); shell.key(Key::t, control); shell.expect("QFZF_SELECT:");
        shell.text("walker_unique_file"); shell.selected("walker space é/nested 路径/walker_unique_file.txt"); shell.key(Key::enter); shell.absent("QFZF_SELECT:"); shell.key(Key::enter);
        command("[[ $_qwalker == 'walker space é/nested 路径/walker_unique_file.txt' && -f $_qwalker ]] && print -r -- QWALKER$((70+2))", "QWALKER72");
        phase = 131;
        shell.key(Key::c, alt); shell.expect("QFZF_SELECT:");
        phase = 132;
        shell.text("nested 路径"); shell.selected("walker space é/nested 路径"); shell.key(Key::enter); shell.absent("QFZF_SELECT:");
        phase = 133;
        // Numeric-only diagnostics distinguish a selection/path contract failure
        // from a final prompt or process teardown failure. Never print frames.
        const char* walker_contracts[] = {
            "$PWD == $QUAL_EXPECT_CWD/'walker space é/nested 路径'",
            "$PWD == $QUAL_EXPECT_CWD",
            "$PWD == $QUAL_EXPECT_CWD/'walker space é'",
            "$PWD == */'nested 路径'"
        };
        for (unsigned i = 0; i < 4; ++i) {
            const auto marker = "QWDIAG" + std::to_string(i) + "_";
            shell.command("if [[ " + std::string(walker_contracts[i]) + " ]]; then print -r -- " + marker + "$((0+1)); else print -r -- " + marker + "$((0+0)); fi");
            check(until([&] { const auto value = screen(shell.session); return value.find(marker + "0") != std::string::npos || value.find(marker + "1") != std::string::npos; }));
            std::cout << "SHELL_WALKER_CONTRACT index=" << i << " value=" << (screen(shell.session).find(marker + "1") != std::string::npos ? 1 : 0) << std::endl;
        }
        phase = 134;
        command("[[ $PWD == $QUAL_EXPECT_CWD/'walker space é/nested 路径' ]] && print -r -- QWALKDIR$((70+3))", "QWALKDIR73");
        phase = 135;
        command("print -r -- QEND$((50+3))", "QEND53");
        phase = 136;
        shell.finish();
        std::cout << "SHELL_NAVIGATION_RESULT code=0 phase=13 checks=" << checks << " cold_samples=1 cold_ms=" << cold_ms << " warm_samples=0 warm_ms=" << warm_ms << '\n';
        return 0;
    } catch (...) {
        std::cout << "SHELL_NAVIGATION_RESULT code=1 phase=" << phase << " checks=" << checks << " cold_ms=" << cold_ms << '\n';
        return 1;
    }
}
