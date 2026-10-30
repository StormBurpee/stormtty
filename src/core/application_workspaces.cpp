#include "stormtty/application_workspaces.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <type_traits>

namespace stormtty {
namespace {
void erase_project(WorkspaceWindow& window, ProjectWorkspaceId project) {
    const auto found=std::find(window.projects.begin(),window.projects.end(),project);
    const auto index=static_cast<std::size_t>(found-window.projects.begin());
    window.projects.erase(found);
    if(window.active==project) window.active=window.projects.empty()?ProjectWorkspaceId{}:window.projects[std::min(index,window.projects.size()-1)];
}
Status valid_layout(const ApplicationLayout& layout) {
    if(layout.windows.size()>ApplicationWorkspaces::maximum_windows ||
       (layout.windows.empty()?layout.active_window!=0:layout.active_window>=layout.windows.size()))
        return Status::failure("Invalid application window selection or count");
    std::set<std::string> identities;
    std::size_t projects=0;
    for(const auto& window:layout.windows) {
        if(auto result=validate_workspace_snapshot(window);!result)return result;
        projects+=window.projects.size();
        for(const auto& project:window.projects)
            if(!identities.insert(project.metadata.persistent_id).second)return Status::failure("Workspace appears in multiple restored windows");
    }
    return projects<=ProjectWorkspaceCatalog::maximum_projects?Status{}:Status::failure("Application workspace limit reached");
}
}
const WorkspaceWindow* ApplicationWorkspaces::window(WindowId id) const noexcept {
    const auto it=std::find_if(windows_.begin(),windows_.end(),[id](const auto& w){return w.id==id;});
    return it==windows_.end()?nullptr:&*it;
}
WindowId ApplicationWorkspaces::owner(ProjectWorkspaceId id) const noexcept {
    for(const auto& w:windows_)if(std::find(w.projects.begin(),w.projects.end(),id)!=w.projects.end())return w.id;
    return {};
}
Result<WindowId> ApplicationWorkspaces::begin_window_creation() {
    auto result=create_window();if(result)creating_=*result.value;return result;
}
Status ApplicationWorkspaces::restore_creating_window(WindowId id, const WorkspaceSnapshot& snapshot) {
    if(!id || creating_!=id)return Status::failure("Restore requires an unpublished candidate window");
    if(revision_==std::numeric_limits<std::uint64_t>::max())return Status::failure("Placement revision exhausted");
    auto* candidate=&*std::find_if(windows_.begin(),windows_.end(),[&](const auto& w){return w.id==id;});
    for(const auto project:candidate->projects)if(!catalog_.session_ids(project).empty())return Status::failure("Candidate already owns live sessions");
    ProjectWorkspaceCatalog prepared;
    if(auto result=restore_workspace_snapshot(prepared,snapshot);!result)return result;
    const auto existing=catalog_.entries_.size()-candidate->projects.size();
    if(prepared.entries_.size()>ProjectWorkspaceCatalog::maximum_projects-existing)return Status::failure("Application workspace limit reached");
    for(const auto& restored:prepared.entries_)for(const auto& current:catalog_.entries_)
        if(owner(current.id)!=id && current.metadata.persistent_id==restored.metadata.persistent_id)return Status::failure("Restored workspace already belongs to another window");
    std::vector<ProjectWorkspaceId> placement;placement.reserve(prepared.entries_.size());
    for(const auto& restored:prepared.entries_)placement.push_back(restored.id);
    // MSVC's map move constructor may allocate sentinel nodes. Construct every
    // destination slot before mutation, then commit only through noexcept move
    // assignment and vector swap. Allocation failure leaves the old catalog intact.
    std::vector<ProjectWorkspace> replacement(existing+prepared.entries_.size());
    static_assert(std::is_nothrow_move_assignable_v<ProjectWorkspace>);
    const bool replacing_active=owner(catalog_.active_)==id;
    std::size_t next=0;
    for(auto& current:catalog_.entries_)
        if(std::find(candidate->projects.begin(),candidate->projects.end(),current.id)==candidate->projects.end())
            replacement[next++]=std::move(current);
    for(auto& restored:prepared.entries_)replacement[next++]=std::move(restored);
    catalog_.entries_.swap(replacement);
    candidate->projects.swap(placement);candidate->active=prepared.active_;
    if(replacing_active || !catalog_.active_)catalog_.active_=prepared.active_?prepared.active_:(catalog_.entries_.empty()?ProjectWorkspaceId{}:catalog_.entries_.front().id);
    ++revision_;return {};
}
Status ApplicationWorkspaces::commit_window_creation(WindowId id) {
    if(!id || creating_!=id)return Status::failure("Window is not being created");
    if(auto valid=validate();!valid)return valid;
    creating_={};return {};
}
void ApplicationWorkspaces::abort_window_creation(WindowId id) noexcept {
    if(!id || creating_!=id)return;
    const auto* candidate=window(id);
    if(candidate) {
        static_assert(std::is_nothrow_move_assignable_v<ProjectWorkspace>);
        std::erase_if(catalog_.entries_,[&](const auto& project){return std::find(candidate->projects.begin(),candidate->projects.end(),project.id)!=candidate->projects.end();});
        if(!catalog_.find(catalog_.active_))catalog_.active_=catalog_.entries_.empty()?ProjectWorkspaceId{}:catalog_.entries_.front().id;
        std::erase_if(windows_,[&](const auto& entry){return entry.id==id;});
    }
    creating_={}; // Counters remain monotonic; abandoned identities are not reused.
}
Result<WindowId> ApplicationWorkspaces::create_window() {
    if(creating_)return {{},"Window construction is already pending"};
    if(windows_.size()>=maximum_windows || !next_window_)return {{},"Window limit reached"};
    WindowId id{next_window_};
    windows_.push_back({id,{},{}});
    next_window_=next_window_==std::numeric_limits<std::uint64_t>::max()?0:next_window_+1;
    return {id,{}};
}
Result<ProjectWorkspaceId> ApplicationWorkspaces::create_workspace(WindowId target, ProjectWorkspaceMetadata metadata, Workspace workspace) {
    if(creating_ && creating_!=target)return {{},"Another window is being constructed"};
    if(!window(target))return {{},"Destination window no longer exists"};
    if(revision_==std::numeric_limits<std::uint64_t>::max())return {{},"Placement revision exhausted"};
    // Allocate placement capacity before catalog mutation; no partial ownership.
    auto& w=*std::find_if(windows_.begin(),windows_.end(),[target](const auto& entry){return entry.id==target;});
    w.projects.reserve(w.projects.size()+1);
    auto created=catalog_.create(std::move(metadata),std::move(workspace));
    if(!created)return created;
    w.projects.push_back(*created.value); if(!w.active)w.active=*created.value;
    ++revision_;return created;
}
Status ApplicationWorkspaces::activate(WindowId target, ProjectWorkspaceId project) {
    if(!target || owner(project)!=target)return Status::failure("Workspace is not in this window");
    for(auto& w:windows_)if(w.id==target)w.active=project;
    return {};
}
Status ApplicationWorkspaces::move(ProjectWorkspaceId project, WindowId source, WindowId destination) {
    if(creating_)return Status::failure("Cannot move workspaces during window construction");
    if(auto valid=validate();!valid)return valid;
    if(!source || owner(project)!=source || !window(destination))return Status::failure("Workspace move target changed");
    if(source==destination)return {};
    if(revision_==std::numeric_limits<std::uint64_t>::max())return Status::failure("Placement revision exhausted");
    // Prepare all allocating work before a single swap commit. Catalog untouched.
    auto prepared=windows_;
    for(auto& w:prepared)if(w.id==destination){w.projects.push_back(project);w.active=project;}
    for(auto& w:prepared)if(w.id==source)erase_project(w,project);
    windows_.swap(prepared);++revision_;return {};
}
Result<WorkspaceClosePreview> ApplicationWorkspaces::preview_close(ProjectWorkspaceId project) const {
    const auto w=owner(project);if(!w)return {{},"Workspace no longer exists"};
    return {WorkspaceClosePreview{w,project,revision_,catalog_.session_ids(project)}, {}};
}
Result<RemovedProjectWorkspace> ApplicationWorkspaces::close_workspace(const WorkspaceClosePreview& preview) {
    if(creating_)return {{},"Cannot close workspaces during window construction"};
    if(auto valid=validate();!valid)return {{},valid.error};
    if(!preview.window || owner(preview.project)!=preview.window || preview.placement_revision!=revision_ ||
       preview.sessions!=catalog_.session_ids(preview.project))return {{},"Close target changed; request authorization again"};
    if(revision_==std::numeric_limits<std::uint64_t>::max())return {{},"Placement revision exhausted"};
    auto removed=catalog_.remove(preview.project);if(!removed)return removed;
    for(auto& w:windows_)if(w.id==preview.window)erase_project(w,preview.project);
    ++revision_;return removed;
}
Result<WindowClosePreview> ApplicationWorkspaces::preview_close_window(WindowId id) const {
    const auto* w=window(id);if(!w)return {{},"Window no longer exists"};
    WindowClosePreview result{id,revision_,w->projects,{}};
    for(auto project:w->projects){const auto ids=catalog_.session_ids(project);result.sessions.insert(result.sessions.end(),ids.begin(),ids.end());}
    std::sort(result.sessions.begin(),result.sessions.end());return {std::move(result),{}};
}
Result<ClosedWorkspaceWindow> ApplicationWorkspaces::close_window(const WindowClosePreview& preview) {
    if(creating_)return {{},"Cannot close windows during window construction"};
    if(auto valid=validate();!valid)return {{},valid.error};
    auto current=preview_close_window(preview.window);
    if(!current||*current.value!=preview)return {{},"Window close target changed; request authorization again"};
    if(revision_==std::numeric_limits<std::uint64_t>::max())return {{},"Placement revision exhausted"};
    auto removed=catalog_.remove_many(preview.projects);if(!removed)return {{},removed.error};
    static_assert(std::is_nothrow_move_assignable_v<WorkspaceWindow>);
    std::erase_if(windows_,[&](const auto& w){return w.id==preview.window;});++revision_;
    return {ClosedWorkspaceWindow{preview.window,std::move(*removed.value)}, {}};
}
Status ApplicationWorkspaces::close_empty_window(WindowId id) {
    if(creating_)return Status::failure("Cannot close windows during window construction");
    const auto* w=window(id);if(!w)return Status::failure("Window no longer exists");
    if(!w->projects.empty())return Status::failure("Window still owns workspaces");
    std::erase_if(windows_,[id](const auto& entry){return entry.id==id;});return {};
}
Status ApplicationWorkspaces::restore_single_window(WindowId id, const WorkspaceSnapshot& snapshot) {
    if(windows_.size()!=1 || !window(id))return Status::failure("Bootstrap restoration requires exactly one window");
    if(revision_==std::numeric_limits<std::uint64_t>::max())return Status::failure("Placement revision exhausted");
    ProjectWorkspaceCatalog prepared;
    if(auto restored=restore_workspace_snapshot(prepared,snapshot);!restored)return restored;
    WorkspaceWindow placement{id,{},prepared.active_id()};
    for(const auto& project:prepared.entries())placement.projects.push_back(project.id);
    // replace_unbound validates and performs all allocations before swapping.
    if(auto replaced=catalog_.replace_unbound(prepared);!replaced)return replaced;
    windows_.front().projects.swap(placement.projects);
    windows_.front().active=placement.active;
    ++revision_;return {};
}
Status ApplicationWorkspaces::validate() const {
    if(auto valid=catalog_.validate();!valid)return valid;
    std::set<WindowId> windows;std::set<ProjectWorkspaceId> projects;
    for(const auto& w:windows_) {
        if(!w.id || !windows.insert(w.id).second)return Status::failure("Invalid window identity");
        if(w.projects.empty()?static_cast<bool>(w.active):std::find(w.projects.begin(),w.projects.end(),w.active)==w.projects.end())return Status::failure("Invalid window selection");
        for(auto id:w.projects)if(!catalog_.find(id)||!projects.insert(id).second)return Status::failure("Invalid workspace ownership");
    }
    if(projects.size()!=catalog_.entries().size())return Status::failure("Unowned workspace");
    return {};
}
Result<ApplicationLayout> capture_application_layout(const ApplicationWorkspaces& app, WindowId active) {
    if(auto valid=app.validate();!valid)return {{},valid.error};
    if(app.windows().empty()?static_cast<bool>(active):!app.window(active))return {{},"Invalid active window"};
    auto all=capture_workspace_snapshot(app.catalog());if(!all)return {{},all.error};
    ApplicationLayout result;
    for(const auto& window:app.windows()) {
        WorkspaceSnapshot snapshot;
        for(auto id:window.projects) {
            const auto& entries=app.catalog().entries();
            const auto found=std::find_if(entries.begin(),entries.end(),[id](const auto& p){return p.id==id;});
            if(id==window.active)snapshot.active_index=snapshot.projects.size();
            snapshot.projects.push_back(all.value->projects[static_cast<std::size_t>(found-entries.begin())]);
        }
        if(window.id==active)result.active_window=result.windows.size();
        result.windows.push_back(std::move(snapshot));
    }
    return {std::move(result),{}};
}
Status validate_application_layout(const ApplicationLayout& layout) {return valid_layout(layout);}
Result<WindowRestoreHistory> WindowRestoreHistory::from_records(std::optional<SequencedLayout> closed,
        std::optional<SequencedLayout> quit, std::optional<SequencedLayout> checkpoint) {
    std::set<std::uint64_t> sequences;
    std::uint64_t maximum=0;
    for(const auto* record:{&closed,&quit,&checkpoint})if(*record) {
        if(!(*record)->sequence || !sequences.insert((*record)->sequence).second)return {{},"Invalid restore sequence"};
        if(auto valid=valid_layout((*record)->layout);!valid)return {{},valid.error};
        maximum=std::max(maximum,(*record)->sequence);
    }
    if(closed && closed->layout.windows.size()!=1)return {{},"Individual-close record must contain exactly one window"};
    WindowRestoreHistory result;
    result.closed_=std::move(closed);result.quit_=std::move(quit);result.checkpoint_=std::move(checkpoint);
    result.next_=maximum==std::numeric_limits<std::uint64_t>::max()?0:maximum+1;
    return {std::move(result),{}};
}
Status WindowRestoreHistory::record(std::optional<SequencedLayout>& target, ApplicationLayout layout) {
    if(auto valid=valid_layout(layout);!valid)return valid;
    if(!next_)return Status::failure("Restore sequence exhausted");
    target=SequencedLayout{next_,std::move(layout)};
    next_=next_==std::numeric_limits<std::uint64_t>::max()?0:next_+1;return {};
}
Status WindowRestoreHistory::record_closed(WorkspaceSnapshot snapshot) {
    ApplicationLayout layout;layout.windows.push_back(std::move(snapshot));return record(closed_,std::move(layout));
}
Status WindowRestoreHistory::record_quit(ApplicationLayout layout) {return record(quit_,std::move(layout));}
Status WindowRestoreHistory::checkpoint(ApplicationLayout layout) {return record(checkpoint_,std::move(layout));}
const SequencedLayout* WindowRestoreHistory::choose(RestoreEvent event) const noexcept {
    if(event==RestoreEvent::new_window || event==RestoreEvent::launch_while_running)return nullptr;
    if(event==RestoreEvent::crash_recovery)return checkpoint_?&*checkpoint_:nullptr;
    if(quit_ && (!closed_ || quit_->sequence>closed_->sequence))return &*quit_;
    return closed_?&*closed_:nullptr;
}
} // namespace stormtty
