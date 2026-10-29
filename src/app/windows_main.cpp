#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include "stormtty/native_window.hpp"
#include "../platform/windows/startup_notice.hpp"
#include "../platform/windows/smoke_teardown_trace.hpp"
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
struct ComApartment {
    HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool smoke{};
    ~ComApartment() {
        stormtty::windows::smoke_teardown_trace(smoke,"UI CoUninitialize begin");
        if (SUCCEEDED(status)) CoUninitialize();
        stormtty::windows::smoke_teardown_trace(smoke,"UI CoUninitialize end");
    }
};
std::filesystem::path configured_shell_runtime() {
#ifdef STORMTTY_HAS_BUNDLED_SHELL_RUNTIME
    std::wstring module(32768,L'\0');
    const DWORD count=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
    if (!count || count>=module.size()) throw std::runtime_error("Could not locate the app-owned bundled shell runtime.");
    module.resize(count);
    return std::filesystem::path(module).parent_path()/L"runtimes"/L"zsh";
#else
    return {};
#endif
}
void check_shell_runtime(const std::filesystem::path& root, bool configured) {
    if (!root.empty() && (!std::filesystem::is_regular_file(root/L"usr"/L"bin"/L"zsh.exe") ||
        !std::filesystem::is_regular_file(root/L"usr"/L"bin"/L"msys-2.0.dll") ||
        !std::filesystem::is_regular_file(root/L"stormtty"/L"zsh"/L".zshrc") ||
        !std::filesystem::is_regular_file(root/L"etc"/L"fstab") ||
        !std::filesystem::is_regular_file(root/L"etc"/L"nsswitch.conf") ||
        (configured && !std::filesystem::is_regular_file(root/L"stormtty-shell-manifest.json"))))
        throw std::runtime_error(configured?
            "The app-owned bundled Zsh runtime is missing or incomplete. Close StormTTY and rebuild the stormtty target; keep runtimes/zsh beside StormTTY.exe when copying/installing. No fallback shell was launched.":
            "The shell runtime needs usr/bin/zsh.exe, usr/bin/msys-2.0.dll, stormtty/zsh/.zshrc, etc/fstab and etc/nsswitch.conf. Restage with shell-bootstrap.py. No fallback shell was launched.");
}
struct Arguments {
    int count{};
    LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count);
    ~Arguments() { if (values) LocalFree(values); }
};
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    bool smoke=false;
    stormtty::windows::SmokeTeardownEnd completed{false,"wWinMain locals destroyed; returning to CRT"};
    try {
        Arguments args;
        if (!args.values) throw std::runtime_error("Could not read command-line arguments.");
        for (int i=1;i<args.count;++i) if (std::wstring_view(args.values[i])==L"--smoke" || std::wstring_view(args.values[i])==L"--smoke-layout" || std::wstring_view(args.values[i])==L"--smoke-modal" || std::wstring_view(args.values[i])==L"--smoke-projects" || std::wstring_view(args.values[i])==L"--smoke-agent" || std::wstring_view(args.values[i])==L"--smoke-settings" || std::wstring_view(args.values[i])==L"--smoke-workspaces") smoke=true;
        completed.enabled=smoke;
        stormtty::NativeRunOptions options; options.smoke_test=smoke;
        // Must precede creation of any HWND, including startup error dialogs.
        // ERROR_ACCESS_DENIED means the executable manifest already set it.
        if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) && GetLastError() != ERROR_ACCESS_DENIED)
            throw std::runtime_error("Per-monitor-v2 DPI awareness requires Windows 10 version 1703 or newer.");
        ComApartment apartment; apartment.smoke=smoke;
        if (FAILED(apartment.status)) throw std::runtime_error("Could not initialize the UI COM apartment.");
        auto config = stormtty::default_config(stormtty::Platform::windows);
        std::filesystem::path config_path;
        std::filesystem::path workspace_fixture;
        bool required = false;
        unsigned smoke_modes=0;
        for (int i = 1; i < args.count; ++i) {
            const std::wstring argument(args.values[i]);
            if (argument == L"--config" && i + 1 < args.count) { config_path = args.values[++i]; required = true; }
            else if (argument == L"--smoke-workspaces" && i + 1 < args.count) {
                const std::wstring stage(args.values[++i]);
                if(stage!=L"seed" && stage!=L"restore" && stage!=L"corrupt") throw std::runtime_error("Invalid workspace smoke stage.");
                options.smoke_workspaces=std::string(stage.begin(),stage.end()); ++smoke_modes;
            }
            else if (argument == L"--workspace-fixture" && i + 1 < args.count) workspace_fixture=args.values[++i];
            else if (argument == L"--smoke") { ++smoke_modes; }
            else if (argument == L"--smoke-layout") { options.smoke_layout=true; ++smoke_modes; }
            else if (argument == L"--smoke-modal") { options.smoke_modal=true; ++smoke_modes; }
            else if (argument == L"--smoke-projects") { options.smoke_projects=true; ++smoke_modes; }
            else if (argument == L"--codex-executable" && i+1<args.count) {
                options.codex_executable=args.values[++i];
                if(!options.codex_executable.is_absolute()) throw std::invalid_argument("Codex executable must be an absolute path.");
            }
            else if ((argument==L"--codex-account-reference" || argument==L"--codex-runtime-version") && i+1<args.count) {
                const std::wstring value=args.values[++i];
                if(value.empty() || value.size()>256 || std::any_of(value.begin(),value.end(),[](wchar_t c){return c<32||c>126;})) throw std::invalid_argument("Codex reference/version must be printable ASCII, at most 256 characters; never supply a token.");
                auto& target=argument==L"--codex-account-reference"?options.codex_account_reference:options.codex_runtime_version;
                target.assign(value.begin(),value.end());
            }
            else if (argument == L"--smoke-agent") { options.smoke_agent=true; ++smoke_modes; }
            else if (argument == L"--smoke-settings") { options.smoke_settings=true; ++smoke_modes; }
            else if (argument == L"--capture" && i+1<args.count) options.capture_path=args.values[++i];
            else if ((argument == L"--console-runtime" || argument == L"--shell-runtime") && i+1<args.count) {
                const bool shell_runtime=argument==L"--shell-runtime";
                auto& selected=shell_runtime?options.shell_runtime_directory:options.console_runtime_directory;
                if (!selected.empty()) throw std::invalid_argument(shell_runtime?"Specify --shell-runtime only once.":"Specify --console-runtime only once.");
                const std::filesystem::path directory(args.values[++i]);
                const auto& path=directory.native();
                // Match the provider's local-drive trust boundary; never resolve
                // a relative runtime through cwd, PATH, environment or config.
                const bool local_drive=path.size()>=3 &&
                    ((path[0]>=L'A' && path[0]<=L'Z') || (path[0]>=L'a' && path[0]<=L'z')) &&
                    path[1]==L':' && (path[2]==L'\\' || path[2]==L'/');
                if (!directory.is_absolute() || !local_drive || path.size()>=32767)
                    throw std::invalid_argument(shell_runtime?
                        "--shell-runtime requires an absolute local drive root. UNC/device/relative paths are not accepted.":
                        "--console-runtime requires an absolute local drive directory. UNC/device/relative paths are not accepted.");
                selected=directory.lexically_normal();
            }
            else if (argument == L"--help") {
                constexpr std::string_view help="StormTTY [--config path] [--console-runtime absolute-directory]\n         [--shell-runtime absolute-root]\n\n"
                    "Verification modes (choose one):\n"
                    "--smoke [--capture frame.png]\n"
                    "--smoke-layout --capture layout.png\n"
                    "--smoke-modal --capture modal.png\n"
                    "--smoke-projects --capture projects.png\n"
                    "--smoke-settings --capture settings.png\n"
                    "--smoke-workspaces seed|restore|corrupt --workspace-fixture isolated-directory\n\n"
                    "--console-runtime explicitly opts this launch into the pinned experimental\n"
                    "Microsoft ConPTY runtime (1.25.260930003). No system fallback on failure.\n"
                    "It can also be combined with any verification mode above.\n"
                    "Without it, Settings selects System (default) or Bundled (preview) for new sessions.\n"
                    "The bundle is private to the app; no launch-time downloads or system changes.\n\n"
                    "--shell-runtime opts this launch into staged upstream Zsh 5.9.2 with\n"
                    "StormTTY's managed preset (root/usr/bin/zsh.exe and root/stormtty/zsh/.zshrc).\n"
                    "It keeps HOME/project folders, writes no user dotfiles or history, and\n"
                    "does not change the saved default shell. Smoke uses real Zsh, not cmd.\n"
                    "Windows app builds bundle managed Zsh and find\n"
                    "runtimes/zsh beside the executable; existing saved shell choices win.\n\n"
                    "Ctrl+Shift+P opens commands.\nCtrl+Shift+T creates a tab.\n"
                    "Ctrl+Tab changes tabs.\nCtrl+Shift+V pastes.\nAlt+Space opens the system menu.";
                if (smoke) { std::fprintf(stdout,"%.*s\n",static_cast<int>(help.size()),help.data()); return 0; }
                return stormtty::show_startup_notice("StormTTY controls",help,stormtty::StartupNoticeKind::information)==0?0:1;
            } else throw std::runtime_error("Unknown or incomplete option. Use --help for supported options.");
        }
        // Only builds including a validated runtime discover
        // this fixed executable-relative directory. Never search cwd or PATH.
        if (options.shell_runtime_directory.empty()) {
            options.shell_runtime_directory=configured_shell_runtime();
            options.shell_runtime_override=false;
        }

        if(smoke && !options.codex_executable.empty()) throw std::invalid_argument("Native smoke cannot configure a live agent provider.");
        if (smoke_modes>1) throw std::invalid_argument("Choose only one native smoke scenario.");
        if (smoke && required) throw std::runtime_error("Smoke mode does not load user configuration or profile scripts.");
        if (!smoke && !options.capture_path.empty()) throw std::runtime_error("--capture is available only with explicit --smoke mode.");
        if (config_path.empty() && !smoke) {
            const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
            if (count) {
                std::wstring directory(count, L'\0');
                const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", directory.data(), count);
                if (written && written < count) { directory.resize(written); config_path = std::filesystem::path(directory) / L"StormTTY" / L"config.stormtty"; }
            }
        }
        if (!config_path.empty()) { config_path=std::filesystem::absolute(config_path).lexically_normal(); options.config_path=config_path; }
        if (!options.smoke_workspaces.empty()) {
            if (!workspace_fixture.is_absolute() || !workspace_fixture.filename().wstring().starts_with(L"StormTTY.WorkspaceSmoke.") ||
                (GetFileAttributesW(workspace_fixture.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Workspace smoke requires an explicit isolated absolute fixture directory.");
            std::ifstream marker(workspace_fixture / L"fixture.marker",std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(marker)),{});
            if(bytes!="StormTTY isolated workspace qualification v1\n") throw std::runtime_error("Missing workspace fixture marker.");
            options.config_path=workspace_fixture/L"config.stormtty";
            if(std::filesystem::exists(options.config_path) || std::filesystem::is_symlink(workspace_fixture/L"workspaces.stormtty"))
                throw std::runtime_error("Unsafe workspace fixture contents.");
            options.shell_runtime_directory.clear();
            config=stormtty::default_config(stormtty::Platform::windows);
        } else if (!workspace_fixture.empty()) throw std::runtime_error("Workspace fixture requires dedicated smoke mode.");
        bool config_loaded=false;
        if (!config_path.empty() && (required || std::filesystem::exists(config_path))) {
            auto loaded = stormtty::load_config(config_path, stormtty::Platform::windows);
            if (!loaded) {
                std::string error = "Configuration rejected; no partial settings were applied.\n";
                for (const auto& item : loaded.errors) error += "Line " + std::to_string(item.line) + ": " + item.message + "\n";
                throw std::runtime_error(error);
            }
            config = std::move(*loaded.config); config_loaded=true;
        }
        if (!options.shell_runtime_directory.empty() && !options.shell_runtime_override) {
            // Bundled Settings smoke starts with an old saved Cmd choice,
            // then selects/saves the discovered managed profile through the UI.
            stormtty::apply_developer_shell_default(config,config_loaded || options.smoke_settings);
        }
        try { check_shell_runtime(options.shell_runtime_directory,!options.shell_runtime_override); }
        catch (const std::exception& error) {
            if (options.shell_runtime_override || stormtty::has_managed_zsh_selection(config)) throw;
            options.shell_runtime_error=error.what();
        }
        if (stormtty::has_managed_zsh_selection(config) && options.shell_runtime_directory.empty())
            throw std::runtime_error("Your saved default is managed Zsh, but this build has no bundled shell runtime. Build with STORMTTY_BUNDLE_ZSH=ON or launch with --shell-runtime <absolute staged root>; no fallback shell was launched.");
        const int result=stormtty::run_native_application(std::move(config),std::move(options));
        stormtty::windows::smoke_teardown_trace(smoke,"native application returned to wWinMain");
        return result;
    } catch (const std::exception& exception) {
        if (smoke) std::fprintf(stderr,"Native smoke startup error: %s\n",exception.what());
        else (void)stormtty::show_startup_notice("StormTTY startup error",exception.what(),stormtty::StartupNoticeKind::error);
    } catch (...) {
        if (smoke) std::fprintf(stderr,"Native smoke startup failed unexpectedly.\n");
        else (void)stormtty::show_startup_notice("StormTTY startup error","An unexpected startup error occurred.",stormtty::StartupNoticeKind::error);
    }
    return 1;
}
