// Opt-in native Windows startup measurement. Synthetic private state only.
// Never records command history, user paths, terminal frames or environment.
#include "stormtty/session.hpp"
#include "stormtty/windows_shell_state.hpp"
#include "stormtty/windows_managed_shell.hpp"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <regex>
#include <set>
#include <vector>
namespace fs = std::filesystem;
using namespace stormtty;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
unsigned stage{}, sample{};
void require(bool ok) { if (!ok) throw std::runtime_error("startup measurement failed"); }
std::string utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void write(const fs::path& path, std::string_view data) {
    std::ofstream file(path, std::ios::binary); file << data; file.close(); require(file.good());
}
struct Fixture {
    fs::path root, home, cwd;
    Fixture() {
        PWSTR raw{};
        require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)));
        struct Owned { PWSTR p; ~Owned() { CoTaskMemFree(p); } } owned{raw};
        root = fs::path(raw) / (L"StormTTY startup fixture \u00e9 \u8def\u5f84 " +
            std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(GetTickCount64()) + L" " + std::to_wstring(sample));
        require(fs::create_directory(root));
        home = root / L"home"; cwd = root / L"cwd";
        require(fs::create_directory(home)); require(fs::create_directory(cwd));
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
template<class F> void until(F predicate) {
    const auto end = Clock::now() + 30s;
    do { if (predicate()) return; std::this_thread::sleep_for(5ms); } while (Clock::now() < end);
    require(predicate());
}
struct OwnedSession {
    std::shared_ptr<Session> value;
    ~OwnedSession() { if (value) { value->request_close(); (void)value->wait_closed(8s); } }
};
// Keep the real managed prompt and all modules. The only instrumentation is a
// fixed ZLE status message, after line editor entry. No delayed loading or fake
// prompt. Its draw plus 5ms observation cadence is included in the measurement.
constexpr char config[] = R"ZSH(autoload -Uz add-zle-hook-widget
_stormtty_measure_ready() { zle -M 'STORMTTY_STARTUP_READY'; }
add-zle-hook-widget line-init _stormtty_measure_ready
)ZSH";
void source_profile(const fs::path& state) {
    // Parse only a fixed numeric/name whitelist. Never print raw zprof, paths,
    // configuration, history, exceptions or terminal content.
    const std::vector<std::string> stages{"bootstrap", "private_state", "fpath", "omz", "defaults_config", "tools", "autosuggestions", "context_hooks", "highlighting"};
    const std::set<std::string> names{"compinit", "compaudit", "compdump", "compdef", "zrecompile", "_omz_compdump_has_metadata", "_stormtty_private_state", "_zsh_highlight_load_highlighters", "_omz_source"};
    const std::string number=R"([0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)";
    const std::regex stage_line("([a-z_]+) ("+number+")");
    require(fs::file_size(state / L"startup-source-stages") <= 8192);
    std::ifstream intervals(state / L"startup-source-stages");
    std::string line; std::smatch match; std::size_t index{};
    while (std::getline(intervals,line)) {
        require(index<stages.size() && std::regex_match(line,match,stage_line) && match[1].str()==stages[index]);
        std::cout << "SOURCE_STAGE sample=" << sample << " name=" << stages[index++] << " elapsed_ms=" << match[2].str() << '\n';
    }
    require(index==stages.size());
    require(fs::file_size(state / L"startup-source-functions") <= 1024*1024);
    std::ifstream functions(state / L"startup-source-functions");
    const std::regex row(" *[0-9]+[)] +([0-9]+) +("+number+") +"+number+" +"+number+"% +("+number+") +"+number+" +"+number+"% +([A-Za-z_][A-Za-z_0-9]*) *");
    bool table{}; std::set<std::string> seen;
    while (std::getline(functions,line)) {
        if (table && line.empty()) break; // Header table only; not repeated call graph rows.
        if (!std::regex_match(line,match,row)) continue;
        table=true;
        const auto name=match[4].str();
        if (!names.contains(name)) continue;
        require(seen.insert(name).second);
        std::cout << "SOURCE_FUNCTION sample=" << sample << " name=" << name << " calls=" << match[1].str()
                  << " total_ms=" << match[2].str() << " self_ms=" << match[3].str() << '\n';
    }
    require(seen.contains("compinit") && seen.contains("compaudit"));
}
void measure(const fs::path& runtime, const fs::path& provider, Fixture& fixture, bool fresh, bool profiling) {
    stage = 2;
    const auto prepared = Clock::now();
    const auto state = prepare_managed_shell_state_at(fixture.root, false);
    const auto state_ms = milliseconds(prepared);
    require(static_cast<bool>(state));
    if (fresh) write(state.directory / L"config.zsh", config);
    fs::path existing_dump;
    fs::file_time_type dump_time{};
    if (!fresh) {
        for (const auto& entry : fs::directory_iterator(state.directory)) {
            const auto name = entry.path().filename().wstring();
            if (name.starts_with(L"zcompdump-") && !name.ends_with(L".zwc") && !name.ends_with(L".old") && !name.ends_with(L".lock"))
                existing_dump = entry.path();
        }
        require(!existing_dump.empty());
        dump_time = fs::last_write_time(existing_dump);
    }
    if (profiling) {
        // Require fresh diagnostics even on reused state; never accept a stale file.
        fs::remove(state.directory / L"startup-source-stages");
        fs::remove(state.directory / L"startup-source-functions");
    }
    // Fixture setup/config I/O are deliberately outside the two measured stages.
    ProcessOptions process;
    process.executable = utf8(runtime / L"usr/bin/zsh.exe");
    process.arguments = {"-d", "-i"}; process.working_directory = utf8(fixture.cwd); process.grid = {160, 40};
    process.provider = provider.empty() ? ProcessProvider::windows_inbox_basic : ProcessProvider::windows_packaged_experimental;
    process.provider_directory = utf8(provider);
    process.environment = {{"HOME", utf8(fixture.home)}, {"ZDOTDIR", utf8(runtime / L"stormtty/zsh")},
        {"STORMTTY_SESSION_ID", "startup-measurement"}, {"STORMTTY_EMPTY_HOME", ""},
        {"STORMTTY_NATIVE_ZSH_STATE", utf8(state.directory)},
        {"PATH", utf8(runtime / L"usr/bin")}, {"TERM", "xterm-256color"}, {"COLORTERM", "truecolor"},
        {"LANG", "C.UTF-8"}, {"LC_ALL", "C.UTF-8"}, {"MSYSTEM", "MSYS"},
        {"MSYS", std::nullopt}, {"ENV", std::nullopt}, {"BASH_ENV", std::nullopt},
        {"STORMTTY_MSYS_RESIZE_TRACE_DIR", std::nullopt},
        {"STORMTTY_SHELL_TOOLS", std::nullopt}, {"STORMTTY_FZF", std::nullopt},
        {"STORMTTY_FZF_COMPLETION", std::nullopt}, {"STORMTTY_FZF_KEYS", std::nullopt},
        {"STORMTTY_ZOXIDE", std::nullopt}, {"_ZO_DATA_DIR", std::nullopt},
        {"STORMTTY_AUTOSUGGESTIONS", std::nullopt}, {"STORMTTY_HIGHLIGHTING", std::nullopt}, {"STORMTTY_PROMPT", std::nullopt}};
    process.environment.push_back(managed_shell_console_environment());
    TerminalOptions terminal; terminal.size = {160, 40, 8, 16};
    stage = 3;
    const auto started = Clock::now();
    OwnedSession owned{Session::start(std::move(process), terminal)};
    until([&] { return screen(owned.value).find("STORMTTY_STARTUP_READY") != std::string::npos; });
    const auto ready_ms = milliseconds(started);
    stage = 4;
    if (profiling) source_profile(state.directory);
    // A quick prompt with missing modules is not an acceptable sample.
    require(owned.value->text("[[ ${+_comps[ls]} == 1 && $STORMTTY_CONSOLE_UTF8 == 1 && ${+functions[fzf-file-widget]} == 1 && ${+functions[z]} == 1 && ${+functions[zi]} == 1 && $STORMTTY_OMZ_LOADED == 1 && $STORMTTY_EPHEMERAL == 0 && ${+functions[_zsh_autosuggest_start]} == 1 && ${+functions[_zsh_highlight]} == 1 ]] && print -r -- STARTUP_VALID$((60+4))") == SessionSendResult::accepted);
    KeyInput enter; enter.key = Key::enter;
    require(owned.value->key(enter) == SessionSendResult::accepted);
    until([&] { return screen(owned.value).find("STARTUP_VALID64") != std::string::npos; });
    require(owned.value->text("exit") == SessionSendResult::accepted);
    require(owned.value->key(enter) == SessionSendResult::accepted);
    require(owned.value->wait_for_process_exit(15s));
    const auto info = owned.value->info();
    require(info.exit_code && *info.exit_code == 0 && info.error.empty());
    owned.value->request_close(); require(owned.value->wait_closed(8s));
    stage = 5;
    require(static_cast<bool>(prepare_managed_shell_state_at(fixture.root, false)));
    require(fs::is_regular_file(state.directory / L"history"));
    if (!fresh) require(fs::last_write_time(existing_dump) == dump_time);
    std::cout << "STARTUP_SAMPLE sample=" << sample << " fresh_state=" << fresh
              << " state_prepare_ms=" << state_ms << " launch_to_zle_ms=" << ready_ms << '\n';
}
}
int main() {
    try {
        stage = 1;
        int count{}; LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
        struct Arguments { LPWSTR* p; ~Arguments() { if (p) LocalFree(p); } } owned{args};
        require(args && count>=2);
        const bool profiling=std::wstring_view(args[1])==L"--source-profile";
        const int first=profiling?2:1;
        require(count==first+1 || count==first+2);
        const fs::path runtime = fs::absolute(args[first]);
        const fs::path provider = count==first+2 ? fs::absolute(args[first+1]) : fs::path{};
        require(fs::is_regular_file(runtime / L"stormtty-source-profile.json")==profiling);
        require(fs::is_regular_file(runtime / L"usr/bin/zsh.exe"));
        require(fs::is_regular_file(runtime / L"stormtty/vendor/oh-my-zsh/oh-my-zsh.sh"));
        std::unique_ptr<Fixture> fixture;
        for (sample = 1; sample <= (profiling?6u:10u); ++sample) {
            const bool fresh = sample <= 3;
            if (fresh) fixture = std::make_unique<Fixture>();
            measure(runtime, provider, *fixture, fresh, profiling);
        }
        std::cout << "STARTUP_RESULT code=0 fresh_samples=3 warm_samples=" << (profiling?3:7) << '\n';
        return 0;
    } catch (...) {
        std::cout << "STARTUP_RESULT code=1 sample=" << sample << " stage=" << stage << '\n';
        return 1;
    }
}
