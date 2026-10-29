#include "core_test.hpp"
#include "stormtty/agent_panel_controller.hpp"
using namespace stormtty;
int main(){return run_core_tests("agent_panel_controller",[]{
    AgentPanelController unavailable;
    CHECK(unavailable.view("workspace-a","A","/a").state==AgentPanelState::unavailable);
    CHECK(!unavailable.attention());CHECK(!unavailable.any_active());CHECK(unavailable.attention_workspace().empty());
    AgentPanelAction action;action.workspace_id="workspace-a";action.kind=AgentPanelActionKind::start;action.prompt="Do not execute";
    CHECK(!unavailable.dispatch(action,"/a"));
    // Inert configuration must neither execute this deliberately nonexistent path
    // nor create a service/thread. No Connect action is dispatched in this test.
    AgentStdioOptions options;options.executable="/nonexistent/authored-codex-fixture";options.account_reference="fixture-account";
    AgentPanelController configured(options);
    const auto view=configured.view("workspace-b","B","/b");
    CHECK(view.state==AgentPanelState::disconnected);CHECK(view.workspace_id=="workspace-b");CHECK(view.connection_scope.find(options.executable)!=std::string::npos);
    CHECK(!configured.dispatch(action,"/a"));CHECK(!configured.any_active());
});}
