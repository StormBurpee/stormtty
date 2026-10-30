#pragma once
#include "stormtty/application_workspaces.hpp"

namespace stormtty {
// One bounded envelope, not process/session persistence. The original library
// carries existing named presets unchanged. Window records carry launch layouts.
struct ApplicationLayoutDocument {
    static constexpr std::size_t maximum_bytes=WorkspaceLibrary::maximum_bytes;
    WorkspaceLibrary library;
    WindowRestoreHistory history;
    bool clean_shutdown{true};
};
struct DecodedApplicationLayout {
    ApplicationLayoutDocument document;
    // Read-only import. Caller MUST preserve original bytes with a safe backup
    // before replacing a legacy file; parsing does not authorize disk migration.
    bool requires_legacy_backup{};
};
Result<std::string> serialize_application_layout(const ApplicationLayoutDocument&);
Result<DecodedApplicationLayout> deserialize_application_layout(std::string_view);
} // namespace stormtty
