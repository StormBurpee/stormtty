// Real bundled Zsh -> nano -> ConPTY -> Ghostty editing, with synthetic data only.
#include "stormtty/session.hpp"
#include "stormtty/windows_managed_shell.hpp"
#include "stormtty/windows_shell_state.hpp"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
namespace fs = std::filesystem;
using namespace stormtty;
using namespace std::chrono_literals;
namespace {
unsigned phase{}, checks{};
void check(bool value) { ++checks; if (!value) throw std::runtime_error("nano qualification failed"); }
std::string utf8(const fs::path& path) { const auto s = path.generic_u8string(); return {reinterpret_cast<const char*>(s.data()), s.size()}; }
void write(const fs::path& path, std::string_view value) { std::ofstream f(path, std::ios::binary); f << value; f.close(); check(f.good()); }
std::string read(const fs::path& path) { std::ifstream f(path, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
struct Fixture {
    fs::path root, home, file;
    Fixture() {
        PWSTR raw{}; check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)));
        struct Owned { PWSTR p; ~Owned() { CoTaskMemFree(p); } } owned{raw};
        root = fs::path(raw) / (L"StormTTY nano qualification " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(GetTickCount64()));
        home = root / L"home \u00e9"; file = home / L"nano fixture \u8def\u5f84.txt";
        check(fs::create_directories(home)); write(file, "original\n");
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};
struct Probe {
    std::shared_ptr<Session> session;
    ~Probe() { if (session) { session->request_close(); (void)session->wait_closed(8s); } }
    void text(std::string value) { check(session->text(std::move(value)) == SessionSendResult::accepted); }
    void key(Key value, bool ctrl = false) { KeyInput k; k.key = value; k.modifiers = ctrl ? Modifier::control : 0; check(session->key(k) == SessionSendResult::accepted); }
    void expect(std::string_view token) {
        const auto end = std::chrono::steady_clock::now() + 20s;
        do {
            std::string screen;
            if (const auto frame = session->frame()) for (const auto& line : frame->lines) {
                for (const auto& cell : line->cells) if (cell.span) screen += cell.grapheme.empty() ? " " : cell.grapheme;
                screen += '\n';
            }
            if (screen.find(token) != std::string::npos) { check(true); return; }
            std::this_thread::sleep_for(20ms);
        } while (std::chrono::steady_clock::now() < end);
        check(false);
    }
};
}
int main() {
    try {
        phase = 1;
        int count{}; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        struct Args { LPWSTR* p; ~Args() { if (p) LocalFree(p); } } owned{args};
        check(args && (count == 2 || count == 3));
        const auto runtime = fs::absolute(args[1]);
        const auto provider = count == 3 ? fs::absolute(args[2]) : fs::path{};
        Fixture fixture;
        const auto state = prepare_managed_shell_state_at(fixture.root, false); check(static_cast<bool>(state));
        write(state.directory / L"config.zsh", "STORMTTY_PROMPT=custom\nPROMPT='QNANO_BOOT > '\nautoload -Uz add-zle-hook-widget\n_qual_nano_ready() { PROMPT='QNANO_READY > '; zle reset-prompt; }\nadd-zle-hook-widget line-init _qual_nano_ready\n");
        ProcessOptions process;
        process.executable = utf8(runtime / L"usr/bin/zsh.exe"); process.arguments = {"-d", "-i"};
        process.working_directory = utf8(fixture.home); process.grid = {120, 32};
        process.provider = provider.empty() ? ProcessProvider::windows_inbox_basic : ProcessProvider::windows_packaged_experimental;
        process.provider_directory = utf8(provider);
        process.environment = {{"HOME", utf8(fixture.home)}, {"ZDOTDIR", utf8(runtime / L"stormtty/zsh")},
            {"STORMTTY_NATIVE_ZSH_STATE", utf8(state.directory)}, {"STORMTTY_SESSION_ID", "nano-qualification"},
            {"STORMTTY_EMPTY_HOME", ""}, {"PATH", utf8(runtime / L"usr/bin")},
            {"TERM", "xterm-256color"}, {"LANG", "C.UTF-8"}, {"LC_ALL", "C.UTF-8"}, {"MSYSTEM", "MSYS"},
            {"MSYS", std::nullopt}, {"ENV", std::nullopt}, {"BASH_ENV", std::nullopt},
            {"STORMTTY_MSYS_RESIZE_TRACE_DIR", std::nullopt}};
        process.environment.push_back(managed_shell_console_environment());
        TerminalOptions terminal; terminal.size = {120, 32, 8, 16};
        Probe probe; probe.session = Session::start(std::move(process), terminal); probe.expect("QNANO_READY >");
        phase = 2;
        // Resolve nano through the managed shell PATH; ignore all user nanorc files.
        probe.text("nano --ignorercfiles -- 'nano fixture \xe8\xb7\xaf\xe5\xbe\x84.txt'; print -r -- QNANO_EXIT_${?}"); probe.key(Key::enter);
        probe.expect("GNU nano"); probe.expect("original");
        phase = 3;
        probe.key(Key::a, true); probe.key(Key::k, true);
        probe.text("edited caf\xc3\xa9 \xe8\xb7\xaf\xe5\xbe\x84"); probe.key(Key::enter);
        probe.expect("edited caf");
        phase = 4;
        probe.key(Key::o, true); probe.expect("File Name to Write"); probe.key(Key::enter);
        probe.expect("Wrote 1 line"); probe.key(Key::x, true);
        probe.expect("QNANO_EXIT_0");
        check(read(fixture.file) == "edited caf\xc3\xa9 \xe8\xb7\xaf\xe5\xbe\x84\n");
        phase = 5;
        probe.text("exit"); probe.key(Key::enter); check(probe.session->wait_for_process_exit(15s));
        const auto info = probe.session->info(); check(info.exit_code && *info.exit_code == 0 && info.error.empty());
        probe.session->request_close(); check(probe.session->wait_closed(8s));
        std::cout << "MANAGED_NANO_RESULT code=0 phase=5 checks=" << checks << '\n'; return 0;
    } catch (...) { std::cout << "MANAGED_NANO_RESULT code=1 phase=" << phase << " checks=" << checks << '\n'; return 1; }
}
