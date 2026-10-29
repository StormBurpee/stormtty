#include "core_test.hpp"
#include "stormtty/agent_panel_controller.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <filesystem>
#include <thread>
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
    const std::string workspace_id(32,'c');
    CHECK(setup_controller.reconfigure({"C:\\Fixture\\codex.exe",{}}));CHECK(launch_calls==0);
    CHECK(!setup_controller.opened(workspace_id,"C:/fixture"));CHECK(launch_calls==0); // override is never auto-launched
    const auto v=setup_controller.view(workspace_id,"Fixture","C:/fixture");
    AgentPanelAction connect;connect.kind=AgentPanelActionKind::connect;connect.workspace_id=workspace_id;connect.provider_generation=v.provider_generation;connect.settings_generation=v.settings_generation;
    CHECK(setup_controller.dispatch(connect,"C:/fixture"));
    {std::unique_lock lock(mutex);CHECK(cv.wait_for(lock,std::chrono::seconds(5),[&]{return complete;}));}
    CHECK(launch_calls==1);CHECK(accepted);CHECK(confirmed);CHECK(runtime_label=="not-probed");
    CHECK(setup_controller.view(workspace_id,"Fixture","C:/fixture").state==AgentPanelState::failed);
    // Panel opening may discover/status-check known binaries, never a model task.
    complete=false;launch_calls=0;unsigned discoveries=0;
    AgentPanelController automatic({},[&]{std::lock_guard lock(mutex);complete=true;cv.notify_one();},{},
        [&](const AgentStdioOptions& launch,std::shared_ptr<AgentService> service,std::string& error)->std::unique_ptr<AgentStdioTransport>{
            ++launch_calls;CHECK(launch.executable=="/fixture/installed/codex");CHECK(launch.working_directory=="/fixture/installed");CHECK(launch.account_reference.empty());
            CHECK(service->connect(launch.runtime_version,launch.account_reference));const auto frames=service->take_outbound();CHECK(frames.size()==1);CHECK(frames[0].find("initialize")!=std::string::npos);CHECK(frames[0].find("turn/start")==std::string::npos);
            error="Fixture status-only launch";return {};
        },[&]{++discoveries;return AgentDiscoveryResult{"/fixture/installed/codex","fixture","/fixture/installed"};});
    CHECK(automatic.opened(workspace_id,"/untrusted/repository"));
    {std::unique_lock lock(mutex);CHECK(cv.wait_for(lock,std::chrono::seconds(5),[&]{return complete;}));}
    CHECK(discoveries==1);CHECK(launch_calls==1);CHECK(!automatic.opened(workspace_id,"/untrusted/repository"));
    CHECK(automatic.view(workspace_id,"Fixture","/fixture").state==AgentPanelState::failed);
    complete=false;launch_calls=0;bool installation_appeared=false;
    AgentPanelController missing({},[&]{std::lock_guard lock(mutex);complete=true;cv.notify_one();},{},
        [&](const AgentStdioOptions&,std::shared_ptr<AgentService>,std::string&)->std::unique_ptr<AgentStdioTransport>{++launch_calls;return {};},
        [&]{return installation_appeared?AgentDiscoveryResult{"/fixture/installed/codex","Fixture appeared","/fixture/installed"}:AgentDiscoveryResult{{},"Fixture missing result.",{}};});
    CHECK(missing.opened(workspace_id,"/untrusted/repository"));
    {std::unique_lock lock(mutex);CHECK(cv.wait_for(lock,std::chrono::seconds(5),[&]{return complete;}));}
    CHECK(launch_calls==0);
    const auto missing_view=missing.view(workspace_id,"Fixture","/fixture");
    CHECK(missing_view.state==AgentPanelState::unavailable);CHECK(missing_view.error=="Fixture missing result.");
    // Real controller output, not an authored panel state: missing CLI must offer
    // Install and one-click Check again, never pretend a discovered entry exists.
    auto control=[](const AgentPanel& panel,std::string_view id){for(const auto& c:panel.controls())if(c.id==id)return true;return false;};
    auto click=[](AgentPanel& panel,std::string_view id){for(const auto& c:panel.controls())if(c.id==id){const Point p{c.bounds.x+1,c.bounds.y+1};panel.pointer_down(p);panel.pointer_up(p);return;}CHECK(false);};
    AgentPanel panel;panel.open(missing_view);CHECK(control(panel,"install_codex"));CHECK(control(panel,"connect"));
    click(panel,"install_codex");const auto install=panel.take_action();CHECK(install&&install->kind==AgentPanelActionKind::install_codex);CHECK(launch_calls==0);
    panel.update(missing_view);click(panel,"connect");const auto retry=panel.take_action();CHECK(retry&&retry->kind==AgentPanelActionKind::connect);
    complete=false;CHECK(missing.dispatch(*retry,"/fixture"));
    {std::unique_lock lock(mutex);CHECK(cv.wait_for(lock,std::chrono::seconds(5),[&]{return complete;}));}
    CHECK(missing.view(workspace_id,"Fixture","/fixture").state==AgentPanelState::unavailable);CHECK(launch_calls==0);
    installation_appeared=true;complete=false;
    panel.update(missing.view(workspace_id,"Fixture","/fixture"));click(panel,"connect");const auto installed_retry=panel.take_action();CHECK(installed_retry&&installed_retry->kind==AgentPanelActionKind::connect);
    CHECK(missing.dispatch(*installed_retry,"/fixture"));
    {std::unique_lock lock(mutex);CHECK(cv.wait_for(lock,std::chrono::seconds(5),[&]{return complete;}));}
    CHECK(launch_calls==1);CHECK(missing.view(workspace_id,"Fixture","/fixture").state==AgentPanelState::failed);
    CHECK(missing.view(workspace_id,"Fixture","/fixture").error=="Provider launch failed."); // old missing diagnostic retired
    panel.update(automatic.view(workspace_id,"Fixture","/fixture"));CHECK(panel.view().state==AgentPanelState::failed);CHECK(!control(panel,"install_codex"));CHECK(control(panel,"connect"));
    CHECK(!control(panel,"composer"));CHECK(!panel.view().task_available);
    WindowScene error_scene;panel.append_scene(error_scene,Theme{});bool honest_error=false;
    for(const auto& text:error_scene.texts)if(text.text=="Codex could not connect")honest_error=true;
    CHECK(honest_error);
    // A successfully launched authored stdio fixture, then explicit Disconnect,
    // is different from missing installation and launch failure. No turn is sent.
    const auto fixture=std::filesystem::path(STORMTTY_FAKE_APP_SERVER);
    const auto fixture_directory=fixture.parent_path().string();
    AgentPanelController found({}, {}, {}, {}, [&]{return AgentDiscoveryResult{fixture.string(),"Fixture found",fixture_directory};});
    CHECK(found.opened(workspace_id,"/untrusted/repository"));
    auto until=[](auto predicate){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!predicate()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(5));CHECK(predicate());};
    until([&]{return found.view(workspace_id,"Fixture",fixture_directory).state==AgentPanelState::ready;});
    auto found_view=found.view(workspace_id,"Fixture",fixture_directory);CHECK(found_view.auth==AgentAuth::signed_in);CHECK(found.associations()[0].thread_id.empty());
    // Real controller -> authored stdio -> panel: a failed turn is not a dead
    // connection. Retrying requires fresh task review, never reconnects/replays.
    CHECK(found_view.task_available);panel.open(found_view);panel.text("fixture-turn-failure");
    click(panel,"start");CHECK(!panel.take_action());click(panel,"start");auto first_turn=panel.take_action();
    CHECK(first_turn&&first_turn->kind==AgentPanelActionKind::start);CHECK(found.dispatch(*first_turn,fixture_directory));
    until([&]{return found.view(workspace_id,"Fixture",fixture_directory).state==AgentPanelState::failed;});
    auto failed_turn=found.view(workspace_id,"Fixture",fixture_directory);
    CHECK(failed_turn.task_available);CHECK(failed_turn.auth==AgentAuth::signed_in);CHECK(failed_turn.provider_generation==found_view.provider_generation);
    panel.update(failed_turn);CHECK(control(panel,"composer"));CHECK(control(panel,"start"));CHECK(!control(panel,"connect"));CHECK(!panel.take_action());
    click(panel,"start");CHECK(!panel.take_action());click(panel,"start");auto next_turn=panel.take_action();
    CHECK(next_turn&&next_turn->kind==AgentPanelActionKind::start);CHECK(found.dispatch(*next_turn,fixture_directory));
    until([&]{return found.view(workspace_id,"Fixture",fixture_directory).state==AgentPanelState::failed;});
    CHECK(found.view(workspace_id,"Fixture",fixture_directory).provider_generation==found_view.provider_generation);
    AgentPanelAction disconnect;disconnect.kind=AgentPanelActionKind::disconnect;disconnect.workspace_id=workspace_id;disconnect.settings_generation=found_view.settings_generation;disconnect.provider_generation=found_view.provider_generation;
    CHECK(found.dispatch(disconnect,fixture_directory));
    until([&]{return found.view(workspace_id,"Fixture",fixture_directory).state==AgentPanelState::disconnected;});
    panel.update(found.view(workspace_id,"Fixture",fixture_directory));CHECK(!control(panel,"install_codex"));CHECK(control(panel,"connect"));


});}
