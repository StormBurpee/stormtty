#pragma once

#include "stormtty/settings_services.hpp"
#include <algorithm>

namespace stormtty::shell_discovery_policy {

// The resolver is the one bounded native candidate probe. Keeping profile
// selection here lets characterization supply paths without consulting the host.
template<class ResolveExecutable>
void add_first(ShellDiscoveryResult& result, const char* id, const char* label,
               const std::vector<std::filesystem::path>& candidates, bool bash,
               ResolveExecutable&& resolve) {
    for (const auto& candidate : candidates) {
        auto executable = resolve(candidate);
        if (executable.empty() || executable.size() > 4096) continue;
        Profile profile;
        profile.id = id;
        profile.label = label;
        profile.executable = std::move(executable);
        if (bash) {
            profile.arguments = {"--login", "-i"};
            profile.environment.emplace("CHERE_INVOKING", "1");
        } else if (std::string_view(id) == "windows-cmd") {
            profile.arguments = {"/d"};
        } else {
            profile.arguments = {"-NoLogo"};
        }
        // Blank cwd inherits the host-selected project/current directory.
        result.profiles.push_back(std::move(profile));
        return;
    }
}

inline void hide_represented_cmd(ShellDiscoveryResult& result,
                                 std::span<const Profile> configured) {
    std::erase_if(result.profiles, [&](const Profile& candidate) {
        if (candidate.id != "windows-cmd") return false;
        return std::any_of(configured.begin(), configured.end(), [&](const Profile& existing) {
            auto alias = existing.executable;
            for (auto& c : alias) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            if (alias != "cmd" && alias != "cmd.exe") return false;
            auto arguments = existing.arguments;
            if (std::find(arguments.begin(), arguments.end(), "/d") == arguments.end()) {
                arguments.insert(arguments.begin(), "/d");
            }
            return arguments == candidate.arguments && existing.environment == candidate.environment &&
                   existing.working_directory == candidate.working_directory;
        });
    });
}

} // namespace stormtty::shell_discovery_policy
