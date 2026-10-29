#pragma once
#include "stormtty/commands.hpp"
#include "stormtty/project_workspaces.hpp"
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace stormtty {
struct NavigatorTerminal {
    SessionId session;
    std::string title, directory, state;
    bool parked{}, active{};
};
struct NavigatorWorkspace {
    ProjectWorkspaceId project;
    std::string name, directory;
    std::vector<NavigatorTerminal> terminals;
};
struct NavigatorTarget { ProjectWorkspaceId project; SessionId session; bool parked{}; bool operator==(const NavigatorTarget&) const = default; };
// Owned presentation snapshot only. Host revalidates both identities and their
// association at commit; typing/navigation never switches or launches anything.
class WorkspaceNavigator {
public:
    void open(std::vector<NavigatorWorkspace> entries, ProjectWorkspaceId project, SessionId session) {
        entries_=std::move(entries);project_=project;session_=session;query_.clear();filter();
    }
    // Filtering preserves catalog order and the selected stable identity when
    // still present; it does not reorder workspaces on every keystroke.
    void search(std::string query) {query_=std::move(query);filter();}
    const std::string& query() const{return query_;}
    const std::vector<NavigatorWorkspace>& entries() const{return entries_;}
    const std::vector<std::size_t>& workspaces() const{return workspaces_;}
    const std::vector<std::size_t>& terminals() const{return terminals_;}
    std::size_t workspace_index() const{return workspace_index_;}
    std::size_t terminal_index() const{return terminal_index_;}
    const NavigatorWorkspace* workspace() const{return workspaces_.empty()?nullptr:&entries_[workspaces_[workspace_index_]];}
    const NavigatorTerminal* terminal() const{const auto* w=workspace();return w&&!terminals_.empty()?&w->terminals[terminals_[terminal_index_]]:nullptr;}
    std::optional<NavigatorTarget> target() const {const auto* w=workspace();if(!w)return {};const auto* t=terminal();return NavigatorTarget{w->project,t?t->session:SessionId{},t&&t->parked};}
    void select_workspace(std::size_t index){if(index>=workspaces_.size())return;workspace_index_=index;project_=workspace()->project;session_={};filter_terminals();}
    void select_terminal(std::size_t index){if(index>=terminals_.size())return;terminal_index_=index;session_=terminal()->session;}
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
};
struct NavigatorLayout {
    Rect panel, query, close, heading, workspace_body, terminal_body, footer;
    std::vector<Rect> workspace_rows,terminal_rows;
    std::size_t workspace_first{},terminal_first{};
    bool compact{};
};
inline NavigatorLayout layout_workspace_navigator(double width,double height,const WorkspaceNavigator& model,unsigned focus) {
    NavigatorLayout out;if(width<240||height<260)return out;out.compact=width<680;
    const double w=std::max(0.0,std::min(780.0,width-24)),h=std::max(0.0,std::min(448.0,height-64));
    out.panel={(width-w)/2,std::min(72.0,std::max(12.0,(height-h)/3)),w,h};
    const auto p=out.panel;
    out.query={p.x+16,p.y+12,std::max(0.0,w-66),32};out.close={p.x+w-42,p.y+14,28,28};
    out.heading={p.x+16,p.y+54,std::max(0.0,w-32),24};out.footer={p.x,p.y+std::max(88.0,h-56),w,std::min(56.0,h)};
    const double body_y=p.y+86,body_h=std::max(0.0,out.footer.y-body_y-8),left=out.compact?0:std::min(222.0,w*.34);
    if(!out.compact||focus==1)out.workspace_body={p.x+8,body_y,out.compact?w-16:left-8,body_h};
    if(!out.compact||focus!=1)out.terminal_body={p.x+(out.compact?8:left+8),body_y,std::max(0.0,w-(out.compact?16:left+16)),body_h};
    auto rows=[](Rect b,double row_h,std::size_t count,std::size_t selected,std::size_t& first,std::vector<Rect>& result){
        const auto shown=std::min(count,static_cast<std::size_t>(b.height/row_h));if(!shown||b.width<=0)return;
        first=selected>=shown?selected-shown+1:0;first=std::min(first,count-shown);
        for(std::size_t i=0;i<shown;++i)result.push_back({b.x,b.y+i*row_h,b.width,row_h});
    };
    rows(out.workspace_body,42,model.workspaces().size(),model.workspace_index(),out.workspace_first,out.workspace_rows);
    rows(out.terminal_body,54,model.terminals().size(),model.terminal_index(),out.terminal_first,out.terminal_rows);
    return out;
}
} // namespace stormtty
