#include "stormtty/windows_session_launcher.hpp"
#include "stormtty/windows_shell_profile.hpp"
#include "stormtty/windows_console_runtime.hpp"
#include "stormtty/windows_managed_shell.hpp"
#include "stormtty/windows_shell_state.hpp"

#ifdef STORMTTY_EXPERIMENTAL_COMPLETION_SEED
#include "stormtty/windows_completion_seed.hpp"
#endif

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace stormtty {
namespace {

#ifdef STORMTTY_EXPERIMENTAL_COMPLETION_SEED
std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return L"\ufffd";
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
#endif

std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("Invalid UTF-16 text received from Windows.");
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

Color color_hex(std::string_view text, Color fallback) {
    if (text.size() != 7 || text[0] != '#') {
        return fallback;
    }

    unsigned value{};
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        const int n = c >= '0' && c <= '9' ? c - '0'
            : c >= 'A' && c <= 'F' ? c - 'A' + 10
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : -1;
        if (n < 0) {
            return fallback;
        }
        value = value * 16 + static_cast<unsigned>(n);
    }

    return {
        static_cast<std::uint8_t>(value >> 16),
        static_cast<std::uint8_t>(value >> 8),
        static_cast<std::uint8_t>(value),
        255
    };
}

} // namespace

WindowsSessionLauncher::WindowsSessionLauncher(WindowsSessionLaunchOptions options)
    : options_(std::move(options)) {
    if (!options_.shell_runtime_directory.empty() && options_.shell_runtime_error.empty()) {
        const auto& root = options_.shell_runtime_directory;

        Profile managed;
        managed.id = managed_zsh_profile_id;
        managed.label = "Zsh · managed";
        managed.executable = narrow((root / L"usr" / L"bin" / L"zsh.exe").wstring());
        managed.arguments = {"-d", "-i"}; // Skip optional global rc files; actual interactive Zsh.

        std::string child_path = narrow((root / L"usr" / L"bin").wstring());
        const auto count = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (count) {
            if (count > 32767) {
                throw std::runtime_error("Inherited PATH exceeds the supported child environment bound.");
            }

            std::wstring inherited(count, L'\0');
            const auto written = GetEnvironmentVariableW(L"PATH", inherited.data(), count);
            if (written >= count) {
                throw std::runtime_error("Inherited PATH changed while preparing the managed shell.");
            }

            inherited.resize(written);
            if (!inherited.empty()) {
                child_path += ';' + narrow(inherited);
            }
        }

        managed.environment = {
            {"ZDOTDIR", narrow((root / L"stormtty" / L"zsh").generic_wstring())},
            {"STORMTTY_SESSION_ID", "managed"},
            {"TERM", "xterm-256color"},
            {"COLORTERM", "truecolor"},
            {"LANG", "C.UTF-8"},
            {"MSYSTEM", "MSYS"},
            {"PATH", std::move(child_path)}
        };
        managed_profile_ = std::move(managed);
    }
}

bool WindowsSessionLauncher::using_managed_shell(const Config& config) const {
    return managed_profile_ && (options_.shell_runtime_override || has_managed_zsh_selection(config));
}

const std::string& WindowsSessionLauncher::default_profile_id(const Config& config) const {
    return using_managed_shell(config) ? managed_profile_->id : config.default_profile;
}

PreparedWindowsSession WindowsSessionLauncher::prepare(
    const Config& config,
    std::string profile_id,
    std::string_view preferred_profile,
    std::string_view project_directory,
    const std::filesystem::path& smoke_state_root
) const {
    if (profile_id.empty()) {
        profile_id = preferred_profile;
    }
    if (profile_id == "@default") {
        profile_id = default_profile_id(config);
    }
    const bool managed = profile_id == managed_zsh_profile_id;

    ProcessOptions process;
    select_windows_console_provider(process, config.windows_console_backend, options_.console_runtime_directory);

    // One factory covers tabs, splits and projects. Each Process owns its chosen
    // provider for life; only a successfully saved choice affects new sessions.
    if (managed && !managed_profile_) {
        throw std::runtime_error(
            "Managed Zsh is selected but no app-owned runtime is available. Build with "
            "STORMTTY_BUNDLE_ZSH=ON or use --shell-runtime; no fallback was launched.");
    }

    const Profile* selected = managed ? &*managed_profile_ : nullptr;
    if (!selected) {
        for (const auto& profile : config.profiles) {
            if (profile.id == profile_id) {
                selected = &profile;
                break;
            }
        }
    }
    if (!selected) {
        throw std::runtime_error("Saved shell profile is unavailable. Choose a shell in the workspace menu.");
    }

    if (selected) {
        process.executable = selected->executable;
        process.arguments = selected->arguments;
        process.working_directory = selected->working_directory;
        for (const auto& [name, value] : selected->environment) {
            process.environment.push_back({name, value});
        }
    }

    if (managed) {
        // Resolve only an absent HOME through wide APIs; explicit/empty HOME wins.
        process.environment.push_back(managed_shell_console_environment());
        const auto home_environment = managed_shell_home_environment();
        bool empty_home = false;
        for (const auto& entry : home_environment) {
            if (entry.name == "STORMTTY_EMPTY_HOME") {
                empty_home = entry.value == "1";
            }
            process.environment.push_back(entry);
        }

        // Launch-scoped native DACL validation, never an ambient trust switch.
        // State validation overrides inherited data without a subprocess, HOME
        // replacement, or changes to an existing user's files/permissions.
        const auto state = options_.smoke_test
            ? prepare_managed_shell_state_at(smoke_state_root, empty_home)
            : prepare_managed_shell_state(empty_home);
        if (options_.smoke_test && !empty_home && !state) {
            throw std::runtime_error("Private smoke shell state failed revalidation.");
        }
        process.environment.push_back({
            "STORMTTY_NATIVE_ZSH_STATE",
            state ? std::optional<std::string>(narrow(state.directory.generic_wstring())) : std::nullopt
        });

#ifdef STORMTTY_EXPERIMENTAL_COMPLETION_SEED
        if (state) {
            const auto runtime = std::filesystem::path(widen(process.executable))
                .parent_path().parent_path().parent_path();
            // Every opted-in managed launch participates, even warm/custom state.
            // Heavy integrity work, if fresh, runs on the controller before child launch.
            process.initialization = completion_seed_launch(
                state.directory.parent_path().parent_path(), runtime).initialization;
        }
#endif

        // Child environment only: user startup hooks must not override this preset.
        for (const char* name : {"MSYS", "ENV", "BASH_ENV"}) {
            process.environment.push_back({name, std::nullopt});
        }
    }

    if (!project_directory.empty()) {
        process.working_directory = project_directory;
    }

    // The built-in alias is resolved only against the trusted system directory,
    // never the current directory/PATH. Other configured executables must be
    // absolute and are validated by the process transport.
    std::string alias = process.executable;
    for (auto& character : alias) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    if (!managed && (options_.smoke_test || alias.empty() || alias == "cmd.exe" || alias == "cmd")) {
        std::array<wchar_t, MAX_PATH + 1> directory{};
        const UINT length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        if (!length || length >= directory.size()) {
            throw std::runtime_error("Could not discover the Windows system shell.");
        }
        process.executable = narrow(std::wstring(directory.data(), length) + L"\\cmd.exe");

        if (options_.smoke_test) {
            process.arguments = {"/d", "/q"};
            process.environment.clear();
            if (!options_.smoke_projects && !options_.smoke_windows) {
                process.working_directory.clear();
            }
        } else if (std::find(process.arguments.begin(), process.arguments.end(), "/d") == process.arguments.end()) {
            // Do not unexpectedly run cmd AutoRun registry code.
            process.arguments.insert(process.arguments.begin(), "/d");
        }
    }

    TerminalOptions terminal;
    terminal.scrollback_lines = config.scrollback_lines;

    // VT default colors belong to the engine, including OSC queries/resets.
    // Translucency is exclusively a renderer policy; terminal colors stay opaque.
    terminal.default_foreground = color_hex(config.theme.foreground, {236, 238, 242, 255});
    terminal.ansi.emplace();
    for (std::size_t i = 0; i < 16; ++i) {
        (*terminal.ansi)[i] = color_hex(config.theme.ansi[i], {0, 0, 0, 255});
    }
    terminal.default_background = color_hex(config.theme.background, {23, 25, 29, 255});

    return {std::move(process), std::move(terminal), managed};
}

} // namespace stormtty
