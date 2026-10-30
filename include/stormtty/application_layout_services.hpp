#pragma once
#include "stormtty/application_layout_codec.hpp"
#include "stormtty/settings_services.hpp"
namespace stormtty {
struct ApplicationLayoutSnapshotResult {
    std::optional<ConfigFileSnapshot> snapshot;
    std::optional<DecodedApplicationLayout> layout;
    std::string error;
    explicit operator bool() const noexcept {return snapshot && layout && error.empty();}
};
// Windows background-thread-only services. Reads never create or migrate files.
ApplicationLayoutSnapshotResult load_application_layout_snapshot(const std::filesystem::path&);
// Same exact-baseline atomic-save contract as settings_services. Existing v1
// bytes require a verified sibling <filename>.v1-backup before replacement.
// Backup creation is create-new, preserves DACL, and never overwrites a differing
// existing backup. Invalid/unknown loaded documents cannot be overwritten.
ConfigSaveResult save_application_layout(const std::filesystem::path&,const ApplicationLayoutDocument&,const ConfigFileSnapshot&);
} // namespace stormtty
