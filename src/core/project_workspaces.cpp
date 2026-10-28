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

std::vector<SessionId> sessions_in(const ProjectWorkspace& project) {
    std::vector<SessionId> sessions;
    sessions.reserve(project.workspace.state().panes.size() + project.parked_sessions.size());
    for (const auto& [id, pane] : project.workspace.state().panes) {
        (void)id;
        if (pane.session) sessions.push_back(pane.session);
    }
    for (const auto& parked : project.parked_sessions) sessions.push_back(parked.session);
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
    entries_.push_back({id, std::move(metadata), std::move(workspace), {}});
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
    return project ? sessions_in(*project) : std::vector<SessionId>{};
}

std::vector<SessionId> ProjectWorkspaceCatalog::all_session_ids() const {
    std::set<SessionId> sessions;
    for (const auto& project : entries_) {
        for (const auto& [id, pane] : project.workspace.state().panes) {
            (void)id;
            if (pane.session) sessions.insert(pane.session);
        }
        for (const auto& parked : project.parked_sessions) sessions.insert(parked.session);
    }
    return {sessions.begin(), sessions.end()};
}

bool ProjectWorkspaceCatalog::contains_session(SessionId session) const noexcept {
    if (!session) return false;
    for (const auto& project : entries_) {
        for (const auto& [id, pane] : project.workspace.state().panes) {
            (void)id;
            if (pane.session == session) return true;
        }
        for (const auto& parked : project.parked_sessions)
            if (parked.session == session) return true;
    }
    return false;
}

Result<RemovedProjectWorkspace> ProjectWorkspaceCatalog::remove(ProjectWorkspaceId id) {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
        [id](const ProjectWorkspace& project) { return project.id == id; });
    if (it == entries_.end()) return {{}, "Project workspace no longer exists"};
    // Allocate the report before mutating catalog state.
    auto sessions = sessions_in(*it);
    const auto index = static_cast<std::size_t>(it - entries_.begin());
    RemovedProjectWorkspace removed{std::move(*it), std::move(sessions)};
    entries_.erase(it);
    if (active_ == id)
        active_ = entries_.empty() ? ProjectWorkspaceId{} : entries_[std::min(index, entries_.size() - 1)].id;
    return {std::move(removed), {}};
}

ProjectWorkspace* ProjectWorkspaceCatalog::mutable_project(ProjectWorkspaceId id) noexcept {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
        [id](const ProjectWorkspace& project) { return project.id == id; });
    return it == entries_.end() ? nullptr : &*it;
}

Result<SessionId> ProjectWorkspaceCatalog::park(ProjectPaneReference source) {
    if (auto status = validate(); !status) return {{}, std::move(status.error)};
    auto* project = mutable_project(source.project);
    if (!project) return {{}, "Project workspace no longer exists"};
    const auto* pane = project->workspace.pane(source.local);
    if (!pane || !pane->session) return {{}, "Pane has no session to park"};
    if (project->parked_sessions.size() >= maximum_parked_sessions)
        return {{}, "Parked session limit exceeded"};
    const auto session = pane->session;
    auto staged = *project;
    staged.parked_sessions.push_back({session, pane->profile, pane->title});
    if (auto status = staged.workspace.close_pane(source.local); !status)
        return {{}, std::move(status.error)};
    *project = std::move(staged);
    return {session, {}};
}

Result<TabId> ProjectWorkspaceCatalog::reattach_to_new_tab(ProjectSessionReference source) {
    if (auto status = validate(); !status) return {{}, std::move(status.error)};
    auto* project = mutable_project(source.project);
    if (!project) return {{}, "Project workspace no longer exists"};
    auto staged = *project;
    const auto parked = std::find_if(staged.parked_sessions.begin(), staged.parked_sessions.end(),
        [&](const ParkedSession& entry) { return entry.session == source.local; });
    if (parked == staged.parked_sessions.end()) return {{}, "Parked session no longer exists"};
    auto result = staged.workspace.create_tab(parked->session, parked->profile);
    if (!result) return result;
    if (auto status = staged.workspace.set_pane_title(staged.workspace.active_pane(), parked->title); !status)
        return {{}, std::move(status.error)};
    staged.parked_sessions.erase(parked);
    *project = std::move(staged);
    return result;
}

Result<PaneId> ProjectWorkspaceCatalog::reattach_to_split(ProjectSessionReference source,
        ProjectPaneReference target, Direction direction, double fraction) {
    if (auto status = validate(); !status) return {{}, std::move(status.error)};
    if (source.project != target.project) return {{}, "Cannot reattach across project workspaces"};
    auto* project = mutable_project(source.project);
    if (!project) return {{}, "Project workspace no longer exists"};
    auto staged = *project;
    const auto parked = std::find_if(staged.parked_sessions.begin(), staged.parked_sessions.end(),
        [&](const ParkedSession& entry) { return entry.session == source.local; });
    if (parked == staged.parked_sessions.end()) return {{}, "Parked session no longer exists"};
    auto result = staged.workspace.split(target.local, direction, parked->session, parked->profile, fraction);
    if (!result) return result;
    if (auto status = staged.workspace.set_pane_title(*result.value, parked->title); !status)
        return {{}, std::move(status.error)};
    staged.parked_sessions.erase(parked);
    *project = std::move(staged);
    return result;
}

Result<ParkedSession> ProjectWorkspaceCatalog::discard_parked(ProjectSessionReference source) {
    if (auto status = validate(); !status) return {{}, std::move(status.error)};
    auto* project = mutable_project(source.project);
    if (!project) return {{}, "Project workspace no longer exists"};
    const auto parked = std::find_if(project->parked_sessions.begin(), project->parked_sessions.end(),
        [&](const ParkedSession& entry) { return entry.session == source.local; });
    if (parked == project->parked_sessions.end()) return {{}, "Parked session no longer exists"};
    auto removed = *parked;
    project->parked_sessions.erase(parked);
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
        if (project.parked_sessions.size() > maximum_parked_sessions)
            return Status::failure("Parked session limit exceeded");
        for (const auto& parked : project.parked_sessions)
            if (!parked.session || !sessions.insert(parked.session).second)
                return Status::failure("Invalid or duplicate parked session");

        for (const auto& [id, pane] : project.workspace.state().panes) {
            (void)id;
            if (pane.session && !sessions.insert(pane.session).second)
                return Status::failure("Session belongs to more than one project workspace");
        }
    }
    return {};
}

} // namespace stormtty
