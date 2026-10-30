#pragma once
#include "stormtty/workspace_presets.hpp"

namespace stormtty {
using WindowId = Id<struct WindowTag>;
struct WorkspaceWindow {
    WindowId id;
    std::vector<ProjectWorkspaceId> projects;
    ProjectWorkspaceId active;
    bool operator==(const WorkspaceWindow&) const = default;
};
struct WorkspaceClosePreview {
    WindowId window;
    ProjectWorkspaceId project;
    std::uint64_t placement_revision{};
    std::vector<SessionId> sessions;
};
struct WindowClosePreview {
    WindowId window;
    std::uint64_t placement_revision{};
    std::vector<ProjectWorkspaceId> projects;
    std::vector<SessionId> sessions;
    bool operator==(const WindowClosePreview&) const = default;
};
struct ClosedWorkspaceWindow {
    WindowId window;
    std::vector<RemovedProjectWorkspace> projects;
};
// UI-thread application placement owner. This does not own or launch processes.
// The catalog's legacy active selection is NOT a window's selection.
class ApplicationWorkspaces {
public:
    static constexpr std::size_t maximum_windows = 64;
    const ProjectWorkspaceCatalog& catalog() const noexcept { return catalog_; }
    const std::vector<WorkspaceWindow>& windows() const noexcept { return windows_; }
    const WorkspaceWindow* window(WindowId) const noexcept;
    WindowId owner(ProjectWorkspaceId) const noexcept;
    Workspace* workspace(ProjectWorkspaceId id) noexcept { return catalog_.workspace(id); }
    Status rename(ProjectWorkspaceId id, std::string name) { return catalog_.rename(id, std::move(name)); }
    Status update_metadata(ProjectWorkspaceId id, ProjectWorkspaceMetadata m) { return catalog_.update_metadata(id, std::move(m)); }
    Result<SessionId> park(ProjectPaneReference ref) { return catalog_.park(ref); }
    Result<TabId> reattach_to_new_tab(ProjectSessionReference ref) { return catalog_.reattach_to_new_tab(ref); }
    Result<PaneId> reattach_to_split(ProjectSessionReference ref, ProjectPaneReference target, Direction d, double fraction=0.5) {
        return catalog_.reattach_to_split(ref,target,d,fraction);
    }
    Result<ParkedSession> discard_parked(ProjectSessionReference ref) { return catalog_.discard_parked(ref); }
    // Serialized unpublished construction. Abort is allocation-free and never
    // closes a process; host retires candidate actors after clearing their view.
    Result<WindowId> begin_window_creation();
    Status commit_window_creation(WindowId);
    void abort_window_creation(WindowId) noexcept;
    Result<WindowId> create_window(); // Empty until host explicitly creates/restores a workspace.
    Result<ProjectWorkspaceId> create_workspace(WindowId, ProjectWorkspaceMetadata, Workspace = {});
    Status activate(WindowId, ProjectWorkspaceId);
    // Expected source protects stale menu targets. Same-window move is a no-op.
    // Destination becomes active; source falls back to next, then previous row.
    Status move(ProjectWorkspaceId, WindowId expected_source, WindowId destination);
    Result<WorkspaceClosePreview> preview_close(ProjectWorkspaceId) const;
    // Host obtains authorization first. Recheck placement revision + actual sessions.
    // Returned detached data is for explicit host retirement; this is NOT move.
    Result<RemovedProjectWorkspace> close_workspace(const WorkspaceClosePreview&);
    Result<WindowClosePreview> preview_close_window(WindowId) const;
    Result<ClosedWorkspaceWindow> close_window(const WindowClosePreview&);
    Status close_empty_window(WindowId); // Never closes a nonempty window implicitly.
    // Bootstrap-only replacement: one existing window and no live bindings.
    // Prepares a complete catalog/placement before commit; never used for a move.
    Status restore_single_window(WindowId, const WorkspaceSnapshot&);
    Status validate() const;
private:
    ProjectWorkspaceCatalog catalog_;
    std::vector<WorkspaceWindow> windows_;
    std::uint64_t next_window_{1}, revision_{1};
    WindowId creating_;
};

// Declarative snapshots only: no live SessionIds, parked jobs or process output.
struct ApplicationLayout {
    std::vector<WorkspaceSnapshot> windows;
    std::size_t active_window{};
};
Status validate_application_layout(const ApplicationLayout&);
Result<ApplicationLayout> capture_application_layout(const ApplicationWorkspaces&, WindowId active);
enum class RestoreEvent { cold_launch, new_window, launch_while_running, crash_recovery };
struct SequencedLayout { std::uint64_t sequence{}; ApplicationLayout layout; };
// In-memory policy, deliberately not a file format or an I/O implementation.
// Host records closure ONLY after authorization, and quit BEFORE removing windows.
class WindowRestoreHistory {
public:
    Status record_closed(WorkspaceSnapshot);
    Status record_quit(ApplicationLayout);
    Status checkpoint(ApplicationLayout);
    const SequencedLayout* choose(RestoreEvent) const noexcept;
    const std::optional<SequencedLayout>& last_closed() const noexcept {return closed_;}
    const std::optional<SequencedLayout>& last_quit() const noexcept {return quit_;}
    const std::optional<SequencedLayout>& last_checkpoint() const noexcept {return checkpoint_;}
    static Result<WindowRestoreHistory> from_records(std::optional<SequencedLayout> closed,
        std::optional<SequencedLayout> quit, std::optional<SequencedLayout> checkpoint);
private:
    std::uint64_t next_{1};
    std::optional<SequencedLayout> closed_, quit_, checkpoint_;
    Status record(std::optional<SequencedLayout>&, ApplicationLayout);
};
} // namespace stormtty
