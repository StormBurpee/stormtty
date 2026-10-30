#pragma once
#ifdef _WIN32
#include "stormtty/config.hpp"
#include "stormtty/process.hpp"
#include "stormtty/terminal.hpp"
#include <filesystem>
#include <optional>

namespace stormtty {
// Immutable per-host launch capability, not live Settings or a window context.
struct WindowsSessionLaunchOptions {
    std::filesystem::path shell_runtime_directory;
    std::string shell_runtime_error;
    bool shell_runtime_override{true};
    std::filesystem::path console_runtime_directory;
    bool smoke_test{}, smoke_projects{}, smoke_windows{};
};
struct PreparedWindowsSession {
    ProcessOptions process;
    TerminalOptions terminal;
    bool managed{};
};
// UI-thread confined. Owns the resolved runtime profile and launch overrides;
// preparation returns independent values and never allocates/starts/registers an
// actor, writes saved config, changes ambient environment, or retains a caller.
// Native launch-scoped HOME/state validation stays in the existing services.
class WindowsSessionLauncher final {
public:
    explicit WindowsSessionLauncher(WindowsSessionLaunchOptions);
    const std::optional<Profile>& managed_profile() const noexcept { return managed_profile_; }
    bool using_managed_shell(const Config&) const;
    const std::string& default_profile_id(const Config&) const;
    PreparedWindowsSession prepare(const Config&, std::string profile_id,
        std::string_view preferred_profile, std::string_view project_directory,
        const std::filesystem::path& smoke_state_root = {}) const;
private:
    const WindowsSessionLaunchOptions options_;
    std::optional<Profile> managed_profile_;
};
} // namespace stormtty
#endif
