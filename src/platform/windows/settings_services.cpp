#include "stormtty/settings_services.hpp"
#include "windows_local_path.hpp"
#include "stormtty/application_layout_services.hpp"

#ifdef _WIN32
#include "guarded_document_store.hpp"
#include <stdexcept>
#include <utility>

namespace stormtty {
using windows_local_path::checked_path;

SettingsSnapshotResult load_settings_snapshot(const std::filesystem::path& input) {
    SettingsSnapshotResult result;
    try {
        GuardedDocumentRead read(input);
        const auto& snapshot = read.snapshot();
        if (snapshot.existed) {
            auto parsed = parse_config(snapshot.bytes, Platform::windows);
            if (!parsed) {
                const auto& error = parsed.errors.front();
                result.error = "Settings are invalid at line " + std::to_string(error.line) + ": " + error.message;
                return result;
            }
            result.config = std::move(parsed.config);
        } else result.config = default_config(Platform::windows);
        result.snapshot = read.take_snapshot();
    } catch (const std::exception& error) { result.error = error.what(); }
    return result;
}

ConfigSaveResult save_settings_config(const std::filesystem::path& input, const Config& config, const ConfigFileSnapshot& loaded) {
    try {
        if (auto status = validate_config(config, Platform::windows); !status) return {{}, status.error, false};
        const auto bytes = serialize_config(config, Platform::windows);
        const auto roundtrip = parse_config(bytes, Platform::windows);
        if (!roundtrip || serialize_config(*roundtrip.config, Platform::windows) != bytes)
            return {{}, "Settings could not be encoded as valid configuration.", false};
        return save_guarded_document(input, bytes, loaded);
    } catch (const std::exception& error) { return {{}, error.what(), false}; }
}

WorkspaceLibrarySnapshotResult load_workspace_library_snapshot(const std::filesystem::path& input) {
    WorkspaceLibrarySnapshotResult result;
    try {
        GuardedDocumentRead read(input);
        const auto& snapshot = read.snapshot();
        if (snapshot.existed) {
            auto parsed = deserialize_workspace_library(snapshot.bytes);
            if (!parsed) { result.error = parsed.error; return result; }
            result.library = std::move(parsed.value);
        } else result.library.emplace();
        result.snapshot = read.take_snapshot();
    } catch (const std::exception& error) { result.error = error.what(); }
    return result;
}

ConfigSaveResult save_workspace_library(const std::filesystem::path& input, const WorkspaceLibrary& library, const ConfigFileSnapshot& loaded) {
    try {
        auto bytes = serialize_workspace_library(library);
        if (!bytes) return {{}, bytes.error, false};
        return save_guarded_document(input, *bytes.value, loaded);
    } catch (const std::exception& error) { return {{}, error.what(), false}; }
}

ApplicationLayoutSnapshotResult load_application_layout_snapshot(const std::filesystem::path& input) {
    ApplicationLayoutSnapshotResult result;
    try {
        GuardedDocumentRead read(input);
        const auto& snapshot = read.snapshot();
        if (snapshot.existed) {
            auto parsed = deserialize_application_layout(snapshot.bytes);
            if (!parsed) { result.error = parsed.error; return result; }
            result.layout = std::move(parsed.value);
        } else result.layout.emplace();
        result.snapshot = read.take_snapshot();
    } catch (const std::exception& error) { result.error = error.what(); }
    return result;
}

ConfigSaveResult save_application_layout(const std::filesystem::path& input,
                                        const ApplicationLayoutDocument& document,
                                        const ConfigFileSnapshot& loaded) {
    try {
        // Preflight first: a valid large v1 file may not fit the envelope. Never
        // create a backup or touch the original if complete encoding fails.
        auto bytes = serialize_application_layout(document);
        if (!bytes) return {{}, bytes.error, false};
        const auto path = checked_path(input);
        if (loaded.path != path) return {{}, "Layout snapshot does not match its path.", false};
        bool legacy = false;
        if (loaded.existed) {
            auto parsed = deserialize_application_layout(loaded.bytes);
            if (!parsed) return {{}, "Invalid or unsupported original layout; nothing was saved.", false};
            legacy = parsed.value->requires_legacy_backup;
        }
        if (!legacy) return save_guarded_document(path, *bytes.value, loaded);
        return save_guarded_document_with_legacy_backup(path, *bytes.value, loaded);
    } catch (const std::exception& error) { return {{}, error.what(), false}; }
}

AgentSettingsSnapshotResult load_agent_provider_settings(const std::filesystem::path& input) {
    AgentSettingsSnapshotResult result;
    try {
        GuardedDocumentRead read(input);
        const auto& snapshot = read.snapshot();
        if (snapshot.existed) result.settings = parse_agent_provider_settings(snapshot.bytes, result.error);
        else result.settings.emplace();
        if (result.settings) result.snapshot = read.take_snapshot();
    } catch (const std::exception& error) { result.error = error.what(); }
    return result;
}

ConfigSaveResult save_agent_provider_settings(const std::filesystem::path& path,
                                             const AgentProviderSettings& settings,
                                             const ConfigFileSnapshot& baseline) {
    const auto error = validate_agent_provider_settings(settings);
    if (!error.empty()) return {{}, error, false};
    return save_guarded_document(path, serialize_agent_provider_settings(settings), baseline);
}
} // namespace stormtty

#else
namespace stormtty {
SettingsSnapshotResult load_settings_snapshot(const std::filesystem::path&) { return {{}, {}, "Windows settings persistence is unavailable on this platform."}; }
ConfigSaveResult save_settings_config(const std::filesystem::path&, const Config&, const ConfigFileSnapshot&) {
    return {{}, "Windows settings persistence is unavailable on this platform.", false};
}
WorkspaceLibrarySnapshotResult load_workspace_library_snapshot(const std::filesystem::path&) {
    return {{}, {}, "Windows workspace persistence is unavailable on this platform."};
}

ConfigSaveResult save_workspace_library(const std::filesystem::path&, const WorkspaceLibrary&, const ConfigFileSnapshot&) {
    return {{}, "Windows workspace persistence is unavailable on this platform.", false};
}

ApplicationLayoutSnapshotResult load_application_layout_snapshot(const std::filesystem::path&) {
    return {{},{},"Application layout persistence requires the Windows host."};
}

ConfigSaveResult save_application_layout(const std::filesystem::path&,const ApplicationLayoutDocument&,const ConfigFileSnapshot&) {
    return {{},"Application layout persistence requires the Windows host.",false};
}
AgentSettingsSnapshotResult load_agent_provider_settings(const std::filesystem::path&) {return {{},{},"Local agent settings require the Windows host."};}
ConfigSaveResult save_agent_provider_settings(const std::filesystem::path&,const AgentProviderSettings&,const ConfigFileSnapshot&) {return {{},"Local agent settings require the Windows host.",false};}
} // namespace stormtty
#endif
