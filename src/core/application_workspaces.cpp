#include "stormtty/application_workspaces.hpp"
#include <algorithm>
#include <limits>
#include <set>

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
Result<WindowId> ApplicationWorkspaces::create_window() {
    if(windows_.size()>=maximum_windows || !next_window_)return {{},"Window limit reached"};
    WindowId id{next_window_};
    windows_.push_back({id,{},{}});
    next_window_=next_window_==std::numeric_limits<std::uint64_t>::max()?0:next_window_+1;
    return {id,{}};
}
Result<ProjectWorkspaceId> ApplicationWorkspaces::create_workspace(WindowId target, ProjectWorkspaceMetadata metadata, Workspace workspace) {
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
    if(auto valid=validate();!valid)return {{},valid.error};
    if(!preview.window || owner(preview.project)!=preview.window || preview.placement_revision!=revision_ ||
       preview.sessions!=catalog_.session_ids(preview.project))return {{},"Close target changed; request authorization again"};
    if(revision_==std::numeric_limits<std::uint64_t>::max())return {{},"Placement revision exhausted"};
    auto removed=catalog_.remove(preview.project);if(!removed)return removed;
    for(auto& w:windows_)if(w.id==preview.window)erase_project(w,preview.project);
    ++revision_;return removed;
}
Status ApplicationWorkspaces::close_empty_window(WindowId id) {
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
