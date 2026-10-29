#include "core_test.hpp"
#include "stormtty/agent_panel_controller.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
using namespace stormtty;
int main(){return run_core_tests("agent_panel_controller",[]{
    AgentPanelController unavailable;
    CHECK(unavailable.view("workspace-a","A","/a").state==AgentPanelState::unavailable);
    CHECK(!unavailable.attention());CHECK(!unavailable.any_active());CHECK(unavailable.attention_workspace().empty());
    AgentPanelAction action;action.workspace_id="workspace-a";action.kind=AgentPanelActionKind::start;action.prompt="Do not execute";
    CHECK(!unavailable.dispatch(action,"/a"));
    // Inert configuration must neither execute this deliberately nonexistent path
    // nor create a service/thread. No Connect action is dispatched by this configuration check.
    AgentStdioOptions options;options.executable="/nonexistent/authored-codex-fixture";options.account_reference="fixture-account";
    AgentPanelController configured(options);
    const auto view=configured.view("workspace-b","B","/b");
    CHECK(view.state==AgentPanelState::disconnected);CHECK(view.workspace_id=="workspace-b");CHECK(view.connection_scope.find(options.executable)!=std::string::npos);
    CHECK(!configured.dispatch(action,"/a"));CHECK(!configured.any_active());
    AgentStdioOptions local;local.executable="C:\\Tools\\codex.exe";local.account_reference="chatgpt:old@example.com";
    WorkspaceAgentAssociation association{std::string(32,'a'),"codex",local.account_reference,"saved-thread"};
    AgentPanelController restored(local,{}, {association});
    CHECK(restored.associations().size()==1);CHECK(restored.associations()[0].thread_id=="saved-thread");CHECK(restored.can_reconfigure());
    CHECK(restored.reconfigure({"C:\\Tools\\codex.exe","chatgpt:old@example.com"}));CHECK(restored.associations().size()==1);
    const auto before=restored.view(association.workspace_persistent_id,"A","C:/a").settings_generation;
    CHECK(restored.reconfigure({"C:\\Tools\\codex.exe","chatgpt:new@example.com"}));
    CHECK(restored.view(association.workspace_persistent_id,"A","C:/a").settings_generation>before);
    AgentPanelAction stale;stale.workspace_id=association.workspace_persistent_id;stale.kind=AgentPanelActionKind::interrupt;stale.settings_generation=before;
    CHECK(!restored.dispatch(stale,"C:/a"));
    CHECK(restored.associations().empty());CHECK(!restored.any_active());
    CHECK(restored.view(association.workspace_persistent_id,"A","C:/a").connection_scope.find("chatgpt:new@example.com")!=std::string::npos);
    AgentPanelController wrong_account(local,{},{{std::string(32,'b'),"codex","chatgpt:other@example.com","other-thread"}});
    CHECK(wrong_account.associations().empty());
    // Authored factory exercises the entire UI Save→Connect consent boundary.
    // It never starts a process: only validates the service's initialize request.
    std::mutex mutex;std::condition_variable cv;bool complete=false;
    std::atomic<unsigned> launch_calls{0};std::atomic<bool> accepted{false},confirmed{false};
    std::string runtime_label;
    AgentPanelController setup_controller({},[&]{std::lock_guard lock(mutex);complete=true;cv.notify_one();},{},
        [&](const AgentStdioOptions& launch,std::shared_ptr<AgentService> service,std::string& error)->std::unique_ptr<AgentStdioTransport>{
            ++launch_calls;confirmed=launch.user_confirmed_launch;runtime_label=launch.runtime_version;
            accepted=service->connect(launch.runtime_version,launch.account_reference);
            const auto outbound=service->take_outbound();accepted=accepted&&!outbound.empty()&&outbound.front().find("initialize")!=std::string::npos;
            error="Authored transport fixture: no process was launched";return {};
        });
    auto click=[](AgentPanel& panel,std::string_view id){for(const auto& c:panel.controls())if(c.id==id){Point p{c.bounds.x+1,c.bounds.y+1};panel.pointer_down(p);panel.pointer_up(p);return;}CHECK(false);};
    const std::string workspace_id(32,'c');AgentPanel panel;
    auto setup_view=setup_controller.view(workspace_id,"Fixture","C:/fixture");setup_view.setup_available=true;panel.open(setup_view);
    click(panel,"settings.open");panel.text("C:\\Fixture\\codex.exe");click(panel,"settings.account");panel.text("chatgpt:fixture@example.com");click(panel,"settings.save");
    auto save=panel.take_action();CHECK(save&&save->kind==AgentPanelActionKind::save_settings);CHECK(launch_calls==0);
    CHECK(setup_controller.reconfigure(save->provider_settings));CHECK(launch_calls==0);
    panel.settings_result(true);panel.update(setup_controller.view(workspace_id,"Fixture","C:/fixture"));
    panel.close();panel.open(setup_controller.view(workspace_id,"Fixture","C:/fixture"));CHECK(launch_calls==0);CHECK(!panel.take_action());
    click(panel,"connect");CHECK(!panel.take_action());CHECK(launch_calls==0);
    click(panel,"connect");auto connect=panel.take_action();CHECK(connect&&connect->kind==AgentPanelActionKind::connect);
    CHECK(setup_controller.dispatch(*connect,"C:/fixture"));
    {std::unique_lock lock(mutex);CHECK(cv.wait_for(lock,std::chrono::seconds(5),[&]{return complete;}));}
    CHECK(launch_calls==1);CHECK(accepted);CHECK(confirmed);CHECK(runtime_label=="not-probed");
    CHECK(setup_controller.view(workspace_id,"Fixture","C:/fixture").state==AgentPanelState::disconnected);
});}
