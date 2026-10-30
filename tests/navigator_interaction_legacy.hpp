#pragma once

#include "stormtty/workspace_navigator.hpp"
#include "stormtty/terminal_find.hpp"

namespace stormtty::test {

class LegacyNavigatorModel {
public:
    void open(std::vector<NavigatorWorkspace> entries, ProjectWorkspaceId project, SessionId session) {
        entries_=std::move(entries);project_=project;session_=session;query_.clear();opened_=true;selection_stale_=false;filter();
    }
    // Activity refresh may update labels/order, but never authorizes a different
    // actor or placement. A stale selection remains blocked across later refreshes
    // until the user explicitly chooses a row or changes the search.
    void refresh(std::vector<NavigatorWorkspace> entries, ProjectWorkspaceId initial_project, SessionId initial_session) {
        if(!opened_){open(std::move(entries),initial_project,initial_session);return;}
        const auto previous=target();
        entries_=std::move(entries);filter();
        if(!selection_stale_ && previous!=target())selection_stale_=true;
    }
    bool selection_stale() const{return selection_stale_;}
    // Filtering preserves catalog order and the selected stable identity when
    // still present; it does not reorder workspaces on every keystroke.
    void search(std::string query) {if(query!=query_)selection_stale_=false;query_=std::move(query);filter();}
    const std::string& query() const{return query_;}
    const std::vector<NavigatorWorkspace>& entries() const{return entries_;}
    const std::vector<std::size_t>& workspaces() const{return workspaces_;}
    const std::vector<std::size_t>& terminals() const{return terminals_;}
    std::size_t workspace_index() const{return workspace_index_;}
    std::size_t terminal_index() const{return terminal_index_;}
    const NavigatorWorkspace* workspace() const{return workspaces_.empty()?nullptr:&entries_[workspaces_[workspace_index_]];}
    const NavigatorTerminal* terminal() const{const auto* w=workspace();return w&&!terminals_.empty()?&w->terminals[terminals_[terminal_index_]]:nullptr;}
    std::optional<NavigatorTarget> target() const {if(selection_stale_)return {};const auto* w=workspace();if(!w)return {};const auto* t=terminal();return NavigatorTarget{w->project,t?t->session:SessionId{},t&&t->parked,t?t->identity:WatchIdentity{}};}
    void select_workspace(std::size_t index){if(index>=workspaces_.size())return;selection_stale_=false;workspace_index_=index;project_=workspace()->project;session_={};filter_terminals();}
    void select_terminal(std::size_t index){if(index>=terminals_.size())return;selection_stale_=false;terminal_index_=index;session_=terminal()->session;}
    void move(bool workspaces,int direction){const auto count=workspaces?workspaces_.size():terminals_.size();if(!count)return;const auto i=workspaces?workspace_index_:terminal_index_;const auto next=direction<0?(i?i-1:count-1):(i+1)%count;if(workspaces)select_workspace(next);else select_terminal(next);}
private:
    bool workspace_matches(const NavigatorWorkspace& w) const{return bool(command_search_score(query_,w.name,w.directory));}
    bool terminal_matches(const NavigatorTerminal& t) const{return bool(command_search_score(query_,t.title,t.directory));}
    void filter(){
        workspaces_.clear();
        for(std::size_t i=0;i<entries_.size();++i){const auto& w=entries_[i];if(workspace_matches(w)||std::any_of(w.terminals.begin(),w.terminals.end(),[&](const auto& t){return terminal_matches(t);}))workspaces_.push_back(i);}
        workspace_index_=0;
        for(std::size_t i=0;i<workspaces_.size();++i)if(entries_[workspaces_[i]].project==project_)workspace_index_=i;
        if(const auto* w=workspace())project_=w->project;
        filter_terminals();
    }
    void filter_terminals(){
        terminals_.clear();terminal_index_=0;
        if(const auto* w=workspace()){
            const bool all=workspace_matches(*w);
            for(std::size_t i=0;i<w->terminals.size();++i)if(all||terminal_matches(w->terminals[i]))terminals_.push_back(i);
            for(std::size_t i=0;i<terminals_.size();++i)if(w->terminals[terminals_[i]].session==session_)terminal_index_=i;
            if(const auto* t=terminal())session_=t->session;
        }
    }
    std::vector<NavigatorWorkspace> entries_;
    std::vector<std::size_t> workspaces_,terminals_;
    ProjectWorkspaceId project_{};SessionId session_{};
    std::size_t workspace_index_{},terminal_index_{};
    std::string query_;
    bool opened_{},selection_stale_{};
};
// Independent copy of the model and host lifecycle at f8c27fab, captured
// before extraction. No production interaction methods are called here.
class LegacyNavigatorInteraction : public LegacyNavigatorModel {
public:
    void begin(bool background, std::optional<ProjectWorkspaceId> activity) {
        static_cast<LegacyNavigatorModel&>(*this) = LegacyNavigatorModel{};
        editor_ = TerminalFindQuery{};
        focus_ = 0;
        pressed_ = false;
        background_ = background;
        activity_ = activity;
        visible_ = true;
    }

    void close() {
        if (!visible_) return;
        visible_ = false;
        pressed_ = false;
        activity_.reset();
    }

    void replace_catalog(std::vector<NavigatorWorkspace> entries,
                         ProjectWorkspaceId active_project, SessionId active_session) {
        if (!visible_) return;
        const auto selected = target();
        if (activity_) refresh(std::move(entries), active_project, active_session);
        else open(std::move(entries), selected ? selected->project : active_project,
                  selected ? selected->session : active_session);
        search(editor_.text());
    }

    bool is_open() const { return visible_; }
    bool background_only() const { return background_; }
    std::optional<ProjectWorkspaceId> activity_project() const { return activity_; }
    TerminalFindQuery& editor() { return editor_; }
    const TerminalFindQuery& editor() const { return editor_; }
    unsigned focus() const { return focus_; }
    void set_focus(unsigned focus) { focus_ = focus; }
    void apply_query() { search(editor_.text()); }

    void arm_switch() {
        pressed_ = true;
        pressed_target_ = target();
    }

    void cancel_switch() {
        pressed_ = false;
        pressed_target_.reset();
    }

    bool switch_pressed() const { return pressed_; }

    bool take_switch() {
        const bool activate = pressed_ && pressed_target_ == target();
        pressed_ = false;
        pressed_target_.reset();
        return activate;
    }

private:
    bool visible_{};
    bool background_{};
    std::optional<ProjectWorkspaceId> activity_;
    TerminalFindQuery editor_;
    unsigned focus_{};
    bool pressed_{};
    std::optional<NavigatorTarget> pressed_target_;
};

} // namespace stormtty::test
