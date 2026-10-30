#include "core_test.hpp"
#include "stormtty/application_workspaces.hpp"
#include "stormtty/window_workspace_view.hpp"
#include <type_traits>
#include <array>
using namespace stormtty;
static_assert(!std::is_copy_constructible_v<ApplicationWorkspaces>);
static_assert(!std::is_convertible_v<WindowId,ProjectWorkspaceId>);
namespace {
WindowId window(ApplicationWorkspaces& app) {auto result=app.create_window();CHECK(result);return *result.value;}
ProjectWorkspaceId project(ApplicationWorkspaces& app,WindowId w,std::uint64_t session,std::string name="Project") {
    Workspace layout;CHECK(layout.create_tab({session}));
    CHECK(layout.split(layout.active_pane(),Direction::right,{session+1}));
    auto result=app.create_workspace(w,{name,"/work","Local"},std::move(layout));CHECK(result);return *result.value;
}
void ownership_and_moves() {
    ApplicationWorkspaces app;auto a=window(app),b=window(app);
    auto first=project(app,a,1),second=project(app,a,3),other=project(app,b,5);
    CHECK(app.window(a)->active==first);CHECK(app.window(b)->active==other);
    CHECK(app.activate(a,second));CHECK(app.window(b)->active==other);
    CHECK(!app.activate(b,second));CHECK(!app.activate({},ProjectWorkspaceId{}));
    const auto pane=app.workspace(first)->active_pane();
    auto parked=app.park({first,pane});CHECK(parked);
    const auto metadata=app.catalog().find(first)->metadata;
    const auto layout=app.workspace(first)->state();
    const auto parked_state=app.catalog().find(first)->parked_sessions;
    auto before=app.windows();
    CHECK(!app.move(first,b,a));CHECK(app.windows()==before);
    CHECK(!app.move(first,a,{999}));CHECK(app.windows()==before);
    CHECK(app.move(first,a,a));CHECK(app.windows()==before);
    CHECK(app.move(first,a,b));CHECK(app.owner(first)==b);
    CHECK(app.window(a)->active==second);CHECK(app.window(b)->active==first);
    CHECK(app.catalog().find(first)->metadata==metadata);
    CHECK(app.workspace(first)->state()==layout);
    CHECK(app.catalog().find(first)->parked_sessions==parked_state);
    CHECK(app.catalog().contains_session(*parked.value));CHECK(app.validate());
    for(int i=0;i<100;++i) {CHECK(app.move(first,b,a));CHECK(app.move(first,a,b));CHECK(app.validate());}
    CHECK(app.reattach_to_new_tab({first,*parked.value}));
    CHECK(app.catalog().session_ids(first)==std::vector<SessionId>({{1},{2}}));
    CHECK(app.move(second,a,b));CHECK(app.window(a)->projects.empty());CHECK(!app.window(a)->active);
    CHECK(app.close_empty_window(a));CHECK(!app.close_empty_window(b));CHECK(app.validate());
}
void explicit_close_and_stale_authorization() {
    ApplicationWorkspaces app;auto a=window(app),b=window(app);auto p=project(app,a,10);
    auto initial=app.preview_close(p);CHECK(initial);
    CHECK(app.move(p,a,b));CHECK(!app.close_workspace(*initial.value));
    CHECK(app.move(p,b,a));CHECK(!app.close_workspace(*initial.value)); // ABA is stale too.
    auto changed=app.preview_close(p);CHECK(changed);
    CHECK(app.workspace(p)->create_tab({12}));CHECK(!app.close_workspace(*changed.value));
    CHECK(app.park({p,app.workspace(p)->active_pane()}));
    auto preview=app.preview_close(p);CHECK(preview);CHECK(preview.value->sessions.size()==3);
    auto removed=app.close_workspace(*preview.value);CHECK(removed);
    CHECK(removed.value->project.id==p);CHECK(removed.value->sessions==preview.value->sessions);
    CHECK(removed.value->project.parked_sessions.size()==1);
    CHECK(app.window(a)->projects.empty());CHECK(app.window(a));
    CHECK(!app.close_workspace(*preview.value));CHECK(app.close_empty_window(a));CHECK(app.validate());
}
void snapshots_and_policy() {
    ApplicationWorkspaces app;auto a=window(app),b=window(app);
    auto p=project(app,a,20),q=project(app,b,30);(void)q;
    CHECK(app.park({p,app.workspace(p)->active_pane()}));
    auto captured=capture_application_layout(app,b);CHECK(captured);
    CHECK(captured.value->windows.size()==2);CHECK(captured.value->active_window==1);
    CHECK(!capture_application_layout(app,{999}));
    for(const auto& w:captured.value->windows)for(const auto& entry:w.projects)
        for(const auto& [id,pane]:entry.layout.state().panes){(void)id;CHECK(!pane.session);}
    WindowRestoreHistory history;
    CHECK(!history.choose(RestoreEvent::cold_launch));
    CHECK(history.checkpoint(*captured.value));
    CHECK(!history.choose(RestoreEvent::cold_launch));
    CHECK(history.choose(RestoreEvent::crash_recovery)->layout.windows.size()==2);
    CHECK(history.record_closed(captured.value->windows[0]));
    const auto closed_sequence=history.choose(RestoreEvent::cold_launch)->sequence;
    CHECK(history.choose(RestoreEvent::cold_launch)->layout.windows.size()==1);
    CHECK(history.record_quit(*captured.value));
    CHECK(history.choose(RestoreEvent::cold_launch)->sequence>closed_sequence);
    CHECK(history.choose(RestoreEvent::cold_launch)->layout.windows.size()==2);
    CHECK(!history.choose(RestoreEvent::new_window));CHECK(!history.choose(RestoreEvent::launch_while_running));
    CHECK(history.record_closed(captured.value->windows[1]));
    const auto selected=*history.choose(RestoreEvent::cold_launch);
    CHECK(selected.layout.windows[0].projects[0].metadata.persistent_id==app.catalog().find(q)->metadata.persistent_id);
    auto invalid=*captured.value;invalid.active_window=9;CHECK(!history.record_quit(invalid));
    CHECK(history.choose(RestoreEvent::cold_launch)->sequence==selected.sequence);
    invalid=*captured.value;invalid.windows.push_back(invalid.windows[0]);CHECK(!history.checkpoint(invalid));
    CHECK(history.choose(RestoreEvent::crash_recovery)->layout.windows.size()==2);
    CHECK(history.checkpoint({}));CHECK(history.choose(RestoreEvent::crash_recovery)->layout.windows.empty());
}
void selection_and_nonreuse() {
    ApplicationWorkspaces app;auto a=window(app),b=window(app);
    auto p=project(app,a,200),q=project(app,a,210),r=project(app,a,220);
    CHECK(app.activate(a,q));
    auto saved=capture_application_layout(app,a);CHECK(saved);
    CHECK(saved.value->windows[0].active_index==1);
    CHECK(app.catalog().active_id()==p);
    CHECK(app.move(q,a,b));CHECK(app.window(a)->active==r); // next row
    CHECK(app.move(r,a,b));CHECK(app.window(a)->active==p); // previous at end
    CHECK(app.move(p,a,b));CHECK(app.close_empty_window(a));
    auto replacement=window(app);CHECK(replacement!=a);CHECK(replacement.value>b.value);
    WindowRestoreHistory history;CHECK(history.record_quit(*saved.value));
    auto sequence=history.choose(RestoreEvent::cold_launch)->sequence;
    auto invalid=*saved.value;invalid.active_window=99;CHECK(!history.record_quit(invalid));
    CHECK(history.record_closed(saved.value->windows[0]));
    CHECK(history.choose(RestoreEvent::cold_launch)->sequence==sequence+1);
}
void scoped_views_and_bootstrap() {
    ApplicationWorkspaces empty;WindowWorkspaceView missing(empty,{});CHECK(!missing.snapshot());
    auto empty_id=window(empty);WindowWorkspaceView closed(empty,empty_id);
    CHECK(empty.close_empty_window(empty_id));CHECK(!closed.snapshot());
    ApplicationWorkspaces app;auto a=window(app),b=window(app);
    WindowWorkspaceView av(app,a),bv(app,b);
    auto p=project(app,a,300),q=project(app,b,310);
    CHECK(av.entries().size()==1);CHECK(av.entries()[0].id==p);
    CHECK(&av.entries()[0]==app.catalog().find(p)); // no model copy
    CHECK(av.active_id()==p);CHECK(bv.active_id()==q);
    CHECK(!av.find(q));CHECK(!av.workspace(q));CHECK(!av.activate(q));
    CHECK(!av.rename(q,"Forbidden"));CHECK(!av.remove(q));
    CHECK(!av.park({q,app.workspace(q)->active_pane()}));
    CHECK(av.session_ids(q).empty());CHECK(!av.contains_session({310}));
    CHECK(av.contains_session({300}));CHECK(!av.contains_session({}));
    auto saved=av.snapshot();CHECK(saved);CHECK(saved.value->projects.size()==1);
    CHECK(!app.restore_single_window(a,*saved.value));CHECK(app.validate());
    CHECK(app.move(p,a,b));CHECK(av.entries().empty());CHECK(!av.find(p));CHECK(bv.find(p));
    CHECK(bv.contains_session({300}));CHECK(app.catalog().contains_session({300}));
    ApplicationWorkspaces boot;auto only=window(boot);WindowWorkspaceView view(boot,only);
    auto initial=view.create({"Initial","",""});CHECK(initial);
    CHECK(boot.restore_single_window(only,*saved.value));CHECK(boot.validate());
    CHECK(view.active()->metadata.persistent_id==saved.value->projects[0].metadata.persistent_id);
    CHECK(view.active_id()!=p);CHECK(view.active_id()!=*initial.value);
    const auto before=view.active_id();
    auto malformed=*saved.value;malformed.active_index=9;
    CHECK(!boot.restore_single_window(only,malformed));CHECK(view.active_id()==before);
    CHECK(view.workspace(before)->bind_session(view.active_workspace()->active_pane(),{999}));
    CHECK(!boot.restore_single_window(only,*saved.value));CHECK(view.active_id()==before);
    CHECK(view.contains_session({999}));
}
void atomic_window_close() {
    ApplicationWorkspaces app;auto a=window(app),b=window(app);
    auto p=project(app,a,400),q=project(app,a,410),r=project(app,b,420);
    CHECK(app.park({p,app.workspace(p)->active_pane()}));
    const auto survivor=app.workspace(r)->state();
    auto before=app.preview_close_window(a);CHECK(before);CHECK(before.value->sessions.size()==4);
    CHECK(app.move(q,a,b));CHECK(!app.close_window(*before.value));
    CHECK(app.move(q,b,a));CHECK(!app.close_window(*before.value));
    auto changed=app.preview_close_window(a);CHECK(changed);
    CHECK(app.workspace(q)->create_tab({430}));CHECK(!app.close_window(*changed.value));
    auto authorized=app.preview_close_window(a);CHECK(authorized);
    auto removed=app.close_window(*authorized.value);CHECK(removed);
    CHECK(removed.value->projects.size()==2);CHECK(removed.value->projects[0].project.parked_sessions.size()==1);
    CHECK(!app.window(a));CHECK(app.owner(r)==b);CHECK(app.workspace(r)->state()==survivor);
    CHECK(app.catalog().all_session_ids()==std::vector<SessionId>({{420},{421}}));CHECK(app.validate());
    CHECK(!app.close_window(*authorized.value));
    ProjectWorkspaceCatalog catalog;auto x=catalog.create({"X","",""}),y=catalog.create({"Y","",""}),z=catalog.create({"Z","",""});CHECK(x&&y&&z);
    CHECK(catalog.activate(*y.value));
    const std::array<ProjectWorkspaceId,2> duplicates{*x.value,*x.value};CHECK(!catalog.remove_many(duplicates));CHECK(catalog.entries().size()==3);
    const std::array<ProjectWorkspaceId,1> middle{*y.value};CHECK(catalog.remove_many(middle));CHECK(catalog.active_id()==*z.value);
    const std::array<ProjectWorkspaceId,1> last{*z.value};CHECK(catalog.remove_many(last));CHECK(catalog.active_id()==*x.value);
    auto next=catalog.create({"Next","",""}),tail=catalog.create({"Tail","",""});CHECK(next&&tail);CHECK(catalog.activate(*next.value));
    const std::array<ProjectWorkspaceId,2> invalid{*x.value,ProjectWorkspaceId{999999999}};
    CHECK(!catalog.remove_many(invalid));CHECK(catalog.entries().size()==3);CHECK(catalog.active_id()==*next.value);
    const std::array<ProjectWorkspaceId,2> adjacent{*next.value,*tail.value};CHECK(catalog.remove_many(adjacent));CHECK(catalog.active_id()==*x.value);
    const std::array<ProjectWorkspaceId,1> remaining{*x.value};CHECK(catalog.remove_many(remaining));CHECK(catalog.entries().empty());CHECK(!catalog.active_id());CHECK(catalog.validate());
    const auto empty_window=window(app);auto empty_preview=app.preview_close_window(empty_window);CHECK(empty_preview);
    auto empty_closed=app.close_window(*empty_preview.value);CHECK(empty_closed);CHECK(empty_closed.value->projects.empty());CHECK(app.validate());
}
void unpublished_creation_rollback() {
    ApplicationWorkspaces app;const auto original=window(app);const auto retained=project(app,original,701);
    const auto identity=app.catalog().find(retained)->metadata.persistent_id;
    const auto close=app.preview_close(retained);CHECK(close);
    const auto staged=app.begin_window_creation();CHECK(staged);const auto candidate=*staged.value;
    const auto temporary=project(app,candidate,710);CHECK(app.validate());
    CHECK(!app.begin_window_creation());CHECK(!app.create_window());
    CHECK(!app.move(retained,original,candidate));CHECK(!app.move(temporary,candidate,original));
    CHECK(!app.close_workspace(*close.value));CHECK(!app.create_workspace(original,{"blocked","",""}));
    CHECK(!app.commit_window_creation(original));app.abort_window_creation(original);CHECK(app.window(candidate));
    app.abort_window_creation(candidate);CHECK(!app.window(candidate));CHECK(!app.catalog().find(temporary));
    CHECK(app.owner(retained)==original);CHECK(app.catalog().contains_session({701}));CHECK(!app.catalog().contains_session({710}));
    CHECK(app.catalog().find(retained)->metadata.persistent_id==identity);CHECK(app.validate());
    app.abort_window_creation(candidate);CHECK(app.validate());
    const auto next=app.begin_window_creation();CHECK(next);CHECK(next.value->value>candidate.value);
    project(app,*next.value,720);CHECK(app.commit_window_creation(*next.value));
    app.abort_window_creation(*next.value);CHECK(app.window(*next.value));CHECK(app.validate());
    ApplicationWorkspaces empty;const auto first=empty.begin_window_creation();CHECK(first);
    empty.abort_window_creation(*first.value);CHECK(empty.windows().empty());CHECK(empty.catalog().entries().empty());CHECK(empty.validate());
}
void limits_and_invalid_models() {
    ApplicationWorkspaces app;
    for(std::size_t i=0;i<ApplicationWorkspaces::maximum_windows;++i)CHECK(app.create_window());
    CHECK(!app.create_window());auto a=app.windows()[0].id,b=app.windows()[1].id;
    CHECK(!app.create_workspace({999},{"Invalid","",""}));
    auto p=project(app,a,100);const auto before=app.windows();
    CHECK(!app.create_workspace(b,{"","",""}));CHECK(app.windows()==before);
    Workspace duplicate;CHECK(duplicate.create_tab({100}));
    CHECK(!app.create_workspace(b,{"Duplicate","",""},std::move(duplicate)));CHECK(app.validate());
    auto q=project(app,b,110);
    CHECK(app.workspace(q)->create_tab({100})); // Escape hatch misuse detected.
    const auto invalid_before=app.windows();
    CHECK(!app.validate());CHECK(!app.move(p,a,b));CHECK(app.windows()==invalid_before);
}
}
int main(){return run_core_tests("application_workspaces",[]{ownership_and_moves();explicit_close_and_stale_authorization();snapshots_and_policy();selection_and_nonreuse();scoped_views_and_bootstrap();atomic_window_close();unpublished_creation_rollback();limits_and_invalid_models();});}
