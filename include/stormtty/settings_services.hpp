#pragma once

#include "stormtty/config.hpp"
#include "stormtty/agent_provider_settings.hpp"
#include "stormtty/workspace_presets.hpp"
#include <cstdint>
#include <optional>

namespace stormtty {

// Windows services are synchronous and BACKGROUND-THREAD ONLY. They never
// launch a shell, touch a window, install software, or change the active Config.
struct ShellDiscoveryResult {
    // Candidates, not a replacement for Config::profiles. Preserve all existing
    // profiles and the current default until the user explicitly selects one.
    std::vector<Profile> profiles;
    std::vector<std::string> warnings;
};
// Configured profiles are never changed. Only the verified builtin Cmd
// candidate is hidden when its effective launch policy is already represented.
ShellDiscoveryResult discover_windows_shells(std::span<const Profile> configured = {});

// An owned optimistic-concurrency token. Keep the token from the SAME read that
// supplied the editor's Config; never refresh it silently before saving edits.
// bytes are bounded to 1 MiB and must not be logged (environment may be private).
struct ConfigFileSnapshot {
    std::filesystem::path path;
    bool existed{};
    std::string bytes;
    std::uint64_t file_id{}, volume_id{}, modified_time{};
    auto operator<=>(const ConfigFileSnapshot&) const = default;
};
struct SettingsSnapshotResult {
    std::optional<ConfigFileSnapshot> snapshot;
    std::optional<Config> config;
    std::string error;
    explicit operator bool() const noexcept { return snapshot && config && error.empty(); }
};
// Missing file is a successful snapshot with default_config(windows). Reads
// never create directories/files. Invalid/inaccessible configs are errors.
SettingsSnapshotResult load_settings_snapshot(const std::filesystem::path&);

struct ConfigSaveResult {
    std::optional<ConfigFileSnapshot> snapshot;
    std::string error;
    bool conflict{};
    explicit operator bool() const noexcept { return snapshot && error.empty(); }
};
// Explicit Save only. Exact host-selected absolute local path, validated UTF-8
// declarative config, same-directory atomic rename. Creates missing directories
// only along that path on Save. Refuses links/reparse points and stale snapshots.
// Success supplies the next token. On failure the host must not apply the draft.
// Hosts serialize saves; no HWND/callback is retained. See docs/settings-services.md
// for optimistic-concurrency and supported-path limitations.
ConfigSaveResult save_settings_config(const std::filesystem::path&, const Config&,
                                      const ConfigFileSnapshot& loaded);

// Uses the same guarded atomic local-file service, separate from config.toml.
// Missing file returns an empty library; malformed files never become defaults.
struct WorkspaceLibrarySnapshotResult {
    std::optional<ConfigFileSnapshot> snapshot;
    std::optional<WorkspaceLibrary> library;
    std::string error;
    explicit operator bool() const noexcept { return snapshot && library && error.empty(); }
};
WorkspaceLibrarySnapshotResult load_workspace_library_snapshot(const std::filesystem::path&);
ConfigSaveResult save_workspace_library(const std::filesystem::path&, const WorkspaceLibrary&,
                                       const ConfigFileSnapshot& loaded);

struct AgentSettingsSnapshotResult {
    std::optional<ConfigFileSnapshot> snapshot;
    std::optional<AgentProviderSettings> settings;
    std::string error;
    explicit operator bool() const noexcept {return snapshot&&settings&&error.empty();}
};
// Same guarded, atomic service; dedicated LOCALAPPDATA path, never workspace data.
AgentSettingsSnapshotResult load_agent_provider_settings(const std::filesystem::path&);
ConfigSaveResult save_agent_provider_settings(const std::filesystem::path&, const AgentProviderSettings&, const ConfigFileSnapshot&);
} // namespace stormtty
