#pragma once
#include "stormtty/project_workspaces.hpp"

namespace stormtty {
// Declarative local launch intent only. No processes, commands, environment,
// output, OSC-derived cwd, or parked live jobs survive a restart.
struct SavedProjectWorkspace {
    ProjectWorkspaceMetadata metadata;
    Workspace layout;
};
struct WorkspaceSnapshot {
    std::vector<SavedProjectWorkspace> projects;
    std::size_t active_index{}; // zero for an empty snapshot
};
struct WorkspacePreset {
    std::string name;
    WorkspaceSnapshot snapshot;
};
struct WorkspaceLibrary {
    static constexpr std::size_t maximum_presets = 32;
    static constexpr std::size_t maximum_bytes = 1024 * 1024;
    bool restore_on_launch{true};
    WorkspaceSnapshot last_session;
    std::vector<WorkspacePreset> presets;
};

Result<WorkspaceSnapshot> capture_workspace_snapshot(const ProjectWorkspaceCatalog&);
Status validate_workspace_snapshot(const WorkspaceSnapshot&);
Status restore_workspace_snapshot(ProjectWorkspaceCatalog&, const WorkspaceSnapshot&);
// Exact case-sensitive names; replacing is explicit and preserves preset order.
Status save_workspace_preset(WorkspaceLibrary&, std::string name, WorkspaceSnapshot, bool replace = false);
Status remove_workspace_preset(WorkspaceLibrary&, std::string_view name);
Result<std::string> serialize_workspace_library(const WorkspaceLibrary&);
Result<WorkspaceLibrary> deserialize_workspace_library(std::string_view);
} // namespace stormtty
