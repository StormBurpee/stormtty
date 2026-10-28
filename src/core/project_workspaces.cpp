#include "stormtty/project_workspaces.hpp"
#include "text.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <set>
#include <utility>

namespace stormtty {
namespace {
// Shared by every catalog linked to this core. Saturate rather than wrapping
// and reissuing IDs. Catalog mutation itself remains UI-thread only.
ProjectWorkspaceId allocate_project_id() noexcept {
    static std::atomic<std::uint64_t> next{1};
    auto candidate = next.load(std::memory_order_relaxed);
    while (candidate) {
        const auto successor = candidate == std::numeric_limits<std::uint64_t>::max() ? 0 : candidate + 1;
        if (next.compare_exchange_weak(candidate, successor, std::memory_order_relaxed))
            return {candidate};
    }
    return {};
}

bool safe_text(std::string_view text, std::size_t maximum) {
    return text.size() <= maximum && detail::valid_utf8(text) &&
        std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

Status validate_metadata(const ProjectWorkspaceMetadata& metadata) {
    if (!safe_text(metadata.name, ProjectWorkspaceCatalog::maximum_name_bytes) ||
        metadata.name.find_first_not_of(' ') == std::string::npos)
        return Status::failure("Project name must be nonblank, valid UTF-8 and at most 256 bytes without control characters");
    if (!safe_text(metadata.working_directory, ProjectWorkspaceCatalog::maximum_directory_bytes))
        return Status::failure("Invalid project working directory");
    if (!safe_text(metadata.environment_label, ProjectWorkspaceCatalog::maximum_environment_bytes))
        return Status::failure("Invalid project environment label");
    return {};
}

std::vector<SessionId> sessions_in(const Workspace& workspace) {
    std::vector<SessionId> sessions;
    sessions.reserve(workspace.state().panes.size());
    for (const auto& [id, pane] : workspace.state().panes) {
        (void)id;
        if (pane.session) sessions.push_back(pane.session);
    }
    std::sort(sessions.begin(), sessions.end());
    return sessions;
}
} // namespace

const ProjectWorkspace* ProjectWorkspaceCatalog::find(ProjectWorkspaceId id) const noexcept {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
        [id](const ProjectWorkspace& project) { return project.id == id; });
    return it == entries_.end() ? nullptr : &*it;
}

Workspace* ProjectWorkspaceCatalog::workspace(ProjectWorkspaceId id) noexcept {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
        [id](const ProjectWorkspace& project) { return project.id == id; });
    return it == entries_.end() ? nullptr : &it->workspace;
}

const Workspace* ProjectWorkspaceCatalog::workspace(ProjectWorkspaceId id) const noexcept {
    const auto* project = find(id);
    return project ? &project->workspace : nullptr;
}

Result<ProjectWorkspaceId> ProjectWorkspaceCatalog::create(ProjectWorkspaceMetadata metadata, Workspace workspace) {
    if (entries_.size() >= maximum_projects) return {{}, "Project workspace limit exceeded"};
    if (auto status = validate_metadata(metadata); !status) return {{}, std::move(status.error)};
    if (auto status = workspace.validate(); !status) return {{}, std::move(status.error)};
    for (const auto& [id, pane] : workspace.state().panes) {
        (void)id;
        if (contains_session(pane.session)) return {{}, "Session already belongs to another project workspace"};
    }
    const auto id = allocate_project_id();
    if (!id) return {{}, "Project workspace ID space exhausted"};
    entries_.push_back({id, std::move(metadata), std::move(workspace)});
    if (!active_) active_ = id;
    return {id, {}};
}

Status ProjectWorkspaceCatalog::activate(ProjectWorkspaceId id) {
    if (!find(id)) return Status::failure("Project workspace no longer exists");
    active_ = id;
    return {};
}

Status ProjectWorkspaceCatalog::rename(ProjectWorkspaceId id, std::string name) {
    const auto* project = find(id);
    if (!project) return Status::failure("Project workspace no longer exists");
    auto metadata = project->metadata;
    metadata.name = std::move(name);
    return update_metadata(id, std::move(metadata));
}

Status ProjectWorkspaceCatalog::update_metadata(ProjectWorkspaceId id, ProjectWorkspaceMetadata metadata) {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
        [id](const ProjectWorkspace& project) { return project.id == id; });
    if (it == entries_.end()) return Status::failure("Project workspace no longer exists");
    if (auto status = validate_metadata(metadata); !status) return status;
    it->metadata = std::move(metadata);
    return {};
}

std::vector<SessionId> ProjectWorkspaceCatalog::session_ids(ProjectWorkspaceId id) const {
    const auto* project = find(id);
    return project ? sessions_in(project->workspace) : std::vector<SessionId>{};
}

std::vector<SessionId> ProjectWorkspaceCatalog::all_session_ids() const {
    std::set<SessionId> sessions;
    for (const auto& project : entries_) {
        for (const auto& [id, pane] : project.workspace.state().panes) {
            (void)id;
            if (pane.session) sessions.insert(pane.session);
        }
    }
    return {sessions.begin(), sessions.end()};
}

bool ProjectWorkspaceCatalog::contains_session(SessionId session) const noexcept {
    if (!session) return false;
    for (const auto& project : entries_)
        for (const auto& [id, pane] : project.workspace.state().panes) {
            (void)id;
            if (pane.session == session) return true;
        }
    return false;
}

Result<RemovedProjectWorkspace> ProjectWorkspaceCatalog::remove(ProjectWorkspaceId id) {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
        [id](const ProjectWorkspace& project) { return project.id == id; });
    if (it == entries_.end()) return {{}, "Project workspace no longer exists"};
    // Allocate the report before mutating catalog state.
    auto sessions = sessions_in(it->workspace);
    const auto index = static_cast<std::size_t>(it - entries_.begin());
    RemovedProjectWorkspace removed{std::move(*it), std::move(sessions)};
    entries_.erase(it);
    if (active_ == id)
        active_ = entries_.empty() ? ProjectWorkspaceId{} : entries_[std::min(index, entries_.size() - 1)].id;
    return {std::move(removed), {}};
}

Status ProjectWorkspaceCatalog::validate() const {
    if (entries_.size() > maximum_projects) return Status::failure("Project workspace limit exceeded");
    if (entries_.empty()) {
        if (active_) return Status::failure("Empty catalog has an active project workspace");
    } else if (!find(active_)) return Status::failure("Active project workspace is stale");
    std::set<ProjectWorkspaceId> ids;
    std::set<SessionId> sessions;
    for (const auto& project : entries_) {
        if (!project.id || !ids.insert(project.id).second)
            return Status::failure("Invalid or duplicate project workspace ID");
        if (auto status = validate_metadata(project.metadata); !status) return status;
        if (auto status = project.workspace.validate(); !status) return status;
        for (const auto& [id, pane] : project.workspace.state().panes) {
            (void)id;
            if (pane.session && !sessions.insert(pane.session).second)
                return Status::failure("Session belongs to more than one project workspace");
        }
    }
    return {};
}

} // namespace stormtty
