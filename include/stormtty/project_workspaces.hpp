#pragma once

#include "stormtty/workspace.hpp"

namespace stormtty {

// Unique across catalogs in this process; never a PaneId, TabId or SessionId.
// Not a persistent identifier and not a promise of restoring a live session.
using ProjectWorkspaceId = Id<struct ProjectWorkspaceTag>;

// The existing Workspace's IDs remain local. Use qualified references in any
// UI/cache spanning projects, even when the local numeric IDs happen to match.
template <class LocalId> struct ProjectReference {
    ProjectWorkspaceId project;
    LocalId local;
    explicit constexpr operator bool() const noexcept { return project && local; }
    auto operator<=>(const ProjectReference&) const = default;
};
using ProjectPaneReference = ProjectReference<PaneId>;
using ProjectTabReference = ProjectReference<TabId>;
using ProjectNodeReference = ProjectReference<NodeId>;
using ProjectSessionReference = ProjectReference<SessionId>;

struct ProjectWorkspaceMetadata {
    std::string name;
    // UTF-8 host launch context, not a command or an instruction to chdir.
    // Empty means the host's default launch directory; not the current shell cwd.
    std::string working_directory;
    // Descriptive only. This does not establish SSH/WSL/container connectivity.
    std::string environment_label;
    auto operator<=>(const ProjectWorkspaceMetadata&) const = default;
};

struct ProjectWorkspace {
    ProjectWorkspaceId id;
    ProjectWorkspaceMetadata metadata;
    Workspace workspace;
};

struct RemovedProjectWorkspace {
    // Keeps all metadata, tabs, splits and opaque session associations available
    // after detachment. Destroying this result does not terminate a process.
    ProjectWorkspace project;
    std::vector<SessionId> sessions;
};

// UI-thread catalog, with no filesystem, process, terminal, or launch behavior.
// Hiding/activating projects preserves every Workspace and session association.
// Host owns session lifetime and must allocate SessionIds uniquely within this
// catalog. Mutable Workspace access retains the existing transactional API;
// callers must not bind an ID already used by another project (validate detects
// violations). Independent catalogs may use independent host session registries.
class ProjectWorkspaceCatalog {
public:
    static constexpr std::size_t maximum_projects = 256;
    static constexpr std::size_t maximum_name_bytes = 256;
    static constexpr std::size_t maximum_directory_bytes = 32768;
    static constexpr std::size_t maximum_environment_bytes = 256;

    ProjectWorkspaceCatalog() = default;
    // Do not silently duplicate catalog identities or overwrite session owners.
    ProjectWorkspaceCatalog(const ProjectWorkspaceCatalog&) = delete;
    ProjectWorkspaceCatalog& operator=(const ProjectWorkspaceCatalog&) = delete;
    ProjectWorkspaceCatalog(ProjectWorkspaceCatalog&&) = delete;
    ProjectWorkspaceCatalog& operator=(ProjectWorkspaceCatalog&&) = delete;

    const std::vector<ProjectWorkspace>& entries() const noexcept { return entries_; }
    ProjectWorkspaceId active_id() const noexcept { return active_; }
    const ProjectWorkspace* find(ProjectWorkspaceId) const noexcept;
    const ProjectWorkspace* active() const noexcept { return find(active_); }
    Workspace* workspace(ProjectWorkspaceId) noexcept;
    const Workspace* workspace(ProjectWorkspaceId) const noexcept;
    Workspace* active_workspace() noexcept { return workspace(active_); }
    const Workspace* active_workspace() const noexcept { return workspace(active_); }

    // First create activates automatically; later creates preserve activation.
    // Imported Workspace IDs/sessions are preserved, not reallocated or launched.
    Result<ProjectWorkspaceId> create(ProjectWorkspaceMetadata, Workspace = {});
    Status activate(ProjectWorkspaceId);
    Status rename(ProjectWorkspaceId, std::string name);
    // Only future host launches may use new context; no live session is changed.
    Status update_metadata(ProjectWorkspaceId, ProjectWorkspaceMetadata);

    // Preview sessions BEFORE obtaining close authorization. remove returns the
    // current associations as well; host must recheck if the model changed while
    // a confirmation UI was open. No API here closes a session or OS handle.
    std::vector<SessionId> session_ids(ProjectWorkspaceId) const;
    std::vector<SessionId> all_session_ids() const;
    bool contains_session(SessionId) const noexcept;
    // If active is removed, activate the next row, or previous at the end.
    // Removing the last row leaves a valid empty catalog; no implicit window exit.
    [[nodiscard]] Result<RemovedProjectWorkspace> remove(ProjectWorkspaceId);

    Status validate() const;

private:
    std::vector<ProjectWorkspace> entries_;
    ProjectWorkspaceId active_;
};

} // namespace stormtty
