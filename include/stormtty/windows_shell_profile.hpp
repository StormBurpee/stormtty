#pragma once

#include "stormtty/config.hpp"

#include <string_view>
#include <utility>

namespace stormtty {

// Stable managed reference: persists no installation path or inherited PATH.
// Windows resolves it only against an explicitly configured/CLI runtime.
inline constexpr std::string_view managed_zsh_profile_id = "stormtty-managed-zsh";
inline constexpr std::string_view managed_zsh_profile_executable = "@stormtty/managed-zsh";

inline Profile managed_zsh_profile_reference() {
    Profile profile;
    profile.id = managed_zsh_profile_id;
    profile.label = "Zsh · managed";
    profile.executable = managed_zsh_profile_executable;
    return profile;
}

inline bool has_managed_zsh_selection(const Config& config) {
    if (config.default_profile != managed_zsh_profile_id) {
        return false;
    }

    for (const auto& profile : config.profiles) {
        if (profile.id == config.default_profile) {
            return profile.executable == managed_zsh_profile_executable;
        }
    }

    return false;
}

// Caller invokes only when the app includes a bundled/explicit runtime.
// An existing saved choice (including old Cmd defaults) is authoritative.
inline void apply_developer_shell_default(Config& config, bool saved_config) {
    if (saved_config) {
        return;
    }

    auto managed = managed_zsh_profile_reference();
    config.default_profile = managed.id;

    for (auto& profile : config.profiles) {
        if (profile.id == managed.id) {
            profile = std::move(managed);
            return;
        }
    }

    config.profiles.push_back(std::move(managed));
}

} // namespace stormtty
