#pragma once
#include "stormtty/application_workspaces.hpp"
#include <algorithm>
#include <ranges>
#include <span>

namespace stormtty {
// Non-owning UI-thread adapter: a window sees only its assigned catalog records.
// No copied ProjectWorkspace records; IDs/references retain the catalog's lifetime
// rules. Do not keep an entries() view across placement/catalog mutation.
class WindowWorkspaceView {
public:
    WindowWorkspaceView(ApplicationWorkspaces& app, WindowId id):app_(app),id_(id) {}
    WindowId id() const noexcept { return id_; }
    auto entries() const {
        const auto* window=app_.window(id_);
        const std::span<const ProjectWorkspaceId> ids=window?std::span<const ProjectWorkspaceId>(window->projects):std::span<const ProjectWorkspaceId>{};
        return ids | std::views::transform([app=&app_](ProjectWorkspaceId id)->const ProjectWorkspace& {return *app->catalog().find(id);});
    }
    ProjectWorkspaceId active_id() const noexcept { const auto* w=app_.window(id_);return w?w->active:ProjectWorkspaceId{}; }
    const ProjectWorkspace* find(ProjectWorkspaceId id) const noexcept { return app_.owner(id)==id_ && id_?app_.catalog().find(id):nullptr; }
    const ProjectWorkspace* active() const noexcept { return find(active_id()); }
    Workspace* workspace(ProjectWorkspaceId id) noexcept {return find(id)?app_.workspace(id):nullptr;}
    const Workspace* workspace(ProjectWorkspaceId id) const noexcept {const auto* p=find(id);return p?&p->workspace:nullptr;}
    Workspace* active_workspace() noexcept {return workspace(active_id());}
    const Workspace* active_workspace() const noexcept {return workspace(active_id());}
    Result<ProjectWorkspaceId> create(ProjectWorkspaceMetadata metadata, Workspace workspace={}) {return app_.create_workspace(id_,std::move(metadata),std::move(workspace));}
    Status activate(ProjectWorkspaceId id) {return app_.activate(id_,id);}
    Status rename(ProjectWorkspaceId id,std::string name) {return find(id)?app_.rename(id,std::move(name)):Status::failure("Workspace is not in this window");}
    Status update_metadata(ProjectWorkspaceId id,ProjectWorkspaceMetadata metadata) {return find(id)?app_.update_metadata(id,std::move(metadata)):Status::failure("Workspace is not in this window");}
    std::vector<SessionId> session_ids(ProjectWorkspaceId id) const {return find(id)?app_.catalog().session_ids(id):std::vector<SessionId>{};}
    std::vector<SessionId> all_session_ids() const {
        std::vector<SessionId> result;
        for(const auto& project:entries()){auto ids=session_ids(project.id);result.insert(result.end(),ids.begin(),ids.end());}
        std::sort(result.begin(),result.end());return result;
    }
    bool contains_session(SessionId id) const noexcept {
        if(!id)return false;
        for(const auto& project:entries()) {
            for(const auto& [pane_id,pane]:project.workspace.state().panes){(void)pane_id;if(pane.session==id)return true;}
            for(const auto& parked:project.parked_sessions)if(parked.session==id)return true;
        }
        return false;
    }
    // Explicit removal only, after host authorization. A live transfer uses app.move.
    Result<RemovedProjectWorkspace> remove(ProjectWorkspaceId id) {
        if(!find(id))return {{},"Workspace is not in this window"};
        auto preview=app_.preview_close(id);if(!preview)return {{},preview.error};
        return app_.close_workspace(*preview.value);
    }
    Result<SessionId> park(ProjectPaneReference ref) {return find(ref.project)?app_.park(ref):Result<SessionId>{{},"Workspace is not in this window"};}
    Result<TabId> reattach_to_new_tab(ProjectSessionReference ref) {return find(ref.project)?app_.reattach_to_new_tab(ref):Result<TabId>{{},"Workspace is not in this window"};}
    Result<PaneId> reattach_to_split(ProjectSessionReference ref,ProjectPaneReference target,Direction d,double fraction=0.5) {
        return find(ref.project)&&find(target.project)?app_.reattach_to_split(ref,target,d,fraction):Result<PaneId>{{},"Workspace is not in this window"};
    }
    Result<ParkedSession> discard_parked(ProjectSessionReference ref) {return find(ref.project)?app_.discard_parked(ref):Result<ParkedSession>{{},"Workspace is not in this window"};}
    Status validate() const {return app_.window(id_)?app_.validate():Status::failure("Window no longer exists");}
    Result<WorkspaceSnapshot> snapshot() const {
        if(!app_.window(id_))return {{},"Window no longer exists"};
        auto captured=capture_application_layout(app_,id_);if(!captured)return {{},captured.error};
        return {std::move(captured.value->windows[captured.value->active_window]),{}};
    }
private:
    ApplicationWorkspaces& app_;
    WindowId id_;
};
} // namespace stormtty
