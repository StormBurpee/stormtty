// Real Windows managed Zsh -> ConPTY -> Ghostty -> SessionInfo context proof.
// Every path/branch is synthetic. Logs contain numeric phases/checks only.
#include "stormtty/session.hpp"
#include "stormtty/windows_shell_state.hpp"
#include "stormtty/windows_managed_shell.hpp"
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
void check(bool value) { ++checks; if (!value) throw std::runtime_error("context qualification failed"); }
std::string utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::string posix(const fs::path& path) {
    auto value = utf8(path); check(value.size() > 2 && value[1] == ':');
    char drive = value[0]; if (drive >= 'A' && drive <= 'Z') drive += 'a' - 'A';
    return "/" + std::string(1, drive) + value.substr(2);
}
std::string encoded(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char byte : value) {
        const bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '~' || byte == '-' || byte == '/' || byte == ':';
        if (plain) result += static_cast<char>(byte);
        else { result += '%'; result += hex[byte >> 4]; result += hex[byte & 15]; }
    }
    return result;
}
void write(const fs::path& path, std::string_view value) {
    std::ofstream stream(path, std::ios::binary); stream << value; stream.close(); check(stream.good());
}
struct Fixture {
    fs::path root, home, repo, nested, outside, linked, metadata;
    Fixture() {
        PWSTR raw{}; check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)));
        struct Owned { PWSTR value; ~Owned() { CoTaskMemFree(value); } } owned{raw};
        root = fs::path(raw) / (L"StormTTY context qualification " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(GetTickCount64()));
        home = root / L"home \u00e9"; repo = home / L"repo \u8def\u5f84 % ; $(literal)";
        nested = repo / L"nested space"; outside = home / L"outside";
        linked = home / L"linked"; metadata = home / L"metadata";
        check(fs::create_directories(repo / L".git")); check(fs::create_directory(nested));
        check(fs::create_directory(outside)); check(fs::create_directory(linked)); check(fs::create_directory(metadata));
        write(repo / L".git" / L"HEAD", "ref: refs/heads/feature/\xc3\xa9%;$(literal)\n");
        write(linked / L".git", "gitdir: ../metadata\n"); write(metadata / L"HEAD", "ref: refs/heads/linked-main\n");
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
template<class F> bool until(F predicate) {
    const auto end = std::chrono::steady_clock::now() + 15s;
    do { if (predicate()) return true; std::this_thread::sleep_for(20ms); } while (std::chrono::steady_clock::now() < end);
    return predicate();
}
const char config[] = R"ZSH(STORMTTY_PROMPT=custom
PROMPT='QCONTEXT_BOOT > '
typeset -gi _qual_seq=0 _qual_git_calls=0
git() { (( ++_qual_git_calls )); return 99; }
autoload -Uz add-zle-hook-widget
_qual_line_init() { PROMPT="QCONTEXT_ZLE_${_qual_seq} > "; zle reset-prompt; }
add-zle-hook-widget line-init _qual_line_init
_qual_next() { local last=$?; (( ++_qual_seq )); print -r -- "QCONTEXT_READY_${_qual_seq}_${last}"; return 0; }
)ZSH";
struct Probe {
    std::shared_ptr<Session> session;
    unsigned sequence{};
    Probe(const fs::path& runtime, const fs::path& provider, const Fixture& fixture) {
        const auto state = prepare_managed_shell_state_at(fixture.root, false); check(static_cast<bool>(state));
        write(state.directory / L"config.zsh", config);
        ProcessOptions process;
        process.executable = utf8(runtime / L"usr" / L"bin" / L"zsh.exe"); process.arguments = {"-d", "-i"};
        process.working_directory = utf8(fixture.outside); process.grid = {160, 40};
        process.provider = provider.empty() ? ProcessProvider::windows_inbox_basic : ProcessProvider::windows_packaged_experimental;
        process.provider_directory = utf8(provider);
        process.environment = {{"HOME", utf8(fixture.home)}, {"ZDOTDIR", utf8(runtime / L"stormtty" / L"zsh")},
            {"STORMTTY_SESSION_ID", "context-qualification"}, {"STORMTTY_EMPTY_HOME", ""},
            {"STORMTTY_NATIVE_ZSH_STATE", utf8(state.directory)}, {"STORMTTY_SHELL_CONTEXT", "1"},
            {"QUAL_REPO", posix(fixture.repo)}, {"QUAL_NESTED", posix(fixture.nested)},
            {"QUAL_OUTSIDE", posix(fixture.outside)}, {"QUAL_LINKED", posix(fixture.linked)},
            {"PATH", utf8(runtime / L"usr" / L"bin")}, {"TERM", "xterm-256color"}, {"COLORTERM", "truecolor"},
            {"LANG", "C.UTF-8"}, {"LC_ALL", "C.UTF-8"}, {"MSYSTEM", "MSYS"},
            {"MSYS", std::nullopt}, {"ENV", std::nullopt}, {"BASH_ENV", std::nullopt},
            {"STORMTTY_MSYS_RESIZE_TRACE_DIR", std::nullopt}};
        process.environment.push_back(managed_shell_console_environment());
        TerminalOptions terminal; terminal.size = {160, 40, 8, 16};
        session = Session::start(std::move(process), terminal);
        expect("QCONTEXT_ZLE_0 >"); command("add-zsh-hook precmd _qual_next");
    }
    ~Probe() { if (session) { session->request_close(); (void)session->wait_closed(8s); } }
    void expect(std::string_view token) { check(until([&] { return screen(session).find(token) != std::string::npos; })); }
    void command(std::string value, unsigned status = 0) {
        check(session->text(std::move(value)) == SessionSendResult::accepted);
        KeyInput enter; enter.key = Key::enter; check(session->key(enter) == SessionSendResult::accepted); ++sequence;
        expect("QCONTEXT_READY_" + std::to_string(sequence) + "_" + std::to_string(status));
        expect("QCONTEXT_ZLE_" + std::to_string(sequence) + " >");
    }
    void context(const fs::path& directory, std::string display, std::string root, GitHeadKind kind, std::string head) {
        const auto key = "file://" + encoded(posix(directory));
        check(until([&] {
            const auto info = session->info();
            return info.shell_context && info.working_directory == key && info.shell_context->directory_key == key &&
                info.shell_context->display_directory == display && info.shell_context->repository_root == root &&
                info.shell_context->git == kind && info.shell_context->head == head;
        }));
    }
};
}
int main() {
    try {
        phase = 1;
        int count{}; LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
        struct Arguments { LPWSTR* value; ~Arguments() { if (value) LocalFree(value); } } owned{args};
        check(args && (count == 2 || count == 3));
        const auto runtime = fs::absolute(args[1]);
        const auto provider = count == 3 ? fs::absolute(args[2]) : fs::path{};
        check(fs::is_regular_file(runtime / L"usr" / L"bin" / L"zsh.exe"));
        check(!fs::exists(runtime / L"usr" / L"bin" / L"git.exe"));
        check(!fs::exists(runtime / L"ucrt64" / L"bin" / L"git.exe"));
        Fixture fixture; Probe shell(runtime, provider, fixture);
        const auto* actor = shell.session.get();
        const auto root = "~/" + utf8(fixture.repo.filename());
        phase = 2; shell.context(fixture.outside, "~/outside", "", GitHeadKind::none, "");
        phase = 3; shell.command("builtin cd -- \"$QUAL_NESTED\"");
        shell.context(fixture.nested, root + "/nested space", root, GitHeadKind::branch, "feature/\xc3\xa9%;$(literal)");
        phase = 4; write(fixture.repo / L".git" / L"HEAD", "abcdef0123456789abcdef0123456789abcdef01\n");
        shell.command(":"); shell.context(fixture.nested, root + "/nested space", root, GitHeadKind::detached, "abcdef01");
        phase = 5; shell.command("builtin cd -- \"$QUAL_OUTSIDE\"");
        shell.context(fixture.outside, "~/outside", "", GitHeadKind::none, "");
        phase = 6; shell.command("builtin cd -- \"$QUAL_LINKED\"");
        shell.context(fixture.linked, "~/linked", "~/linked", GitHeadKind::branch, "linked-main");
        // Real Git-for-Windows drive-absolute .git indirection, not a mocked
        // resolver. Keep the same Session and refresh its actual Zsh prompt.
        const auto drive_metadata = utf8(fixture.metadata);
        check(drive_metadata.size() > 2 && drive_metadata[1] == ':' && drive_metadata[2] == '/');
        write(fixture.linked / L".git", "gitdir: " + drive_metadata + "\n");
        write(fixture.metadata / L"HEAD", "ref: refs/heads/drive-main\n");
        shell.command(":");
        shell.context(fixture.linked, "~/linked", "~/linked", GitHeadKind::branch, "drive-main");
        phase = 7; shell.command("STORMTTY_SHELL_CONTEXT=0");
        check(until([&] { return !shell.session->info().shell_context; }));
        shell.command("STORMTTY_SHELL_CONTEXT=1");
        shell.context(fixture.linked, "~/linked", "~/linked", GitHeadKind::branch, "drive-main");
        phase = 8; shell.command("[[ $_qual_git_calls == 0 && ${+commands[git]} == 0 ]] && print -r -- QNOGIT$((40+2))");
        shell.expect("QNOGIT42"); check(shell.session.get() == actor);
        check(!fs::exists(fixture.repo / L"PWNED"));
        check(shell.session->text("exit") == SessionSendResult::accepted);
        KeyInput enter; enter.key = Key::enter; check(shell.session->key(enter) == SessionSendResult::accepted);
        check(shell.session->wait_for_process_exit(15s));
        const auto info = shell.session->info(); check(info.exit_code && *info.exit_code == 0 && info.error.empty());
        shell.session->request_close(); check(shell.session->wait_closed(8s));
        std::cout << "MANAGED_CONTEXT_RESULT code=0 phase=8 checks=" << checks << '\n';
        return 0;
    } catch (...) {
        std::cout << "MANAGED_CONTEXT_RESULT code=1 phase=" << phase << " checks=" << checks << '\n';
        return 1;
    }
}
