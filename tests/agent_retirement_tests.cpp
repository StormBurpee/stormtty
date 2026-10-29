#include "core_test.hpp"
#include "stormtty/agent_panel_controller.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
using namespace stormtty;
template<class F> void until(F predicate){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!predicate()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(5));CHECK(predicate());}
int main(){return run_core_tests("agent_retirement",[]{
 const auto fixture=std::filesystem::path(STORMTTY_FAKE_APP_SERVER);const auto directory=fixture.parent_path().string();const std::string id(32,'a');
 auto discover=[&]{return AgentDiscoveryResult{fixture.string(),"Authored fixture",directory};};
 std::mutex gate;std::condition_variable cv;bool entered=false,release=false;std::atomic<bool> block=false;
 std::vector<WorkspaceAgentAssociation> restored;for(unsigned n=0;n<AgentPanelController::maximum_retiring;++n)restored.push_back({std::string(31,'0')+"0123456789abcdef"[n],"codex","chatgpt:fixture@example.invalid",{}});
 AgentPanelController bounded({}, {}, restored,
  [&](const AgentStdioOptions& options,std::shared_ptr<AgentService> service,std::string& error){
   auto o=options;o.state_changed=[&,service]{if(block&&service->snapshot().connection==AgentConnection::disconnected){std::unique_lock lock(gate);entered=true;cv.notify_all();cv.wait(lock,[&]{return release;});}};
   return AgentStdioTransport::launch(o,std::move(service),error);
  },discover);
 CHECK(bounded.opened(id,directory));until([&]{return bounded.view(id,"Fixture",directory).state==AgentPanelState::ready;});
 const auto before=bounded.view(id,"Fixture",directory);block=true;
 const auto start=std::chrono::steady_clock::now();CHECK(bounded.retire(id));CHECK(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(500));
 CHECK(bounded.view(id,"Fixture",directory).state==AgentPanelState::unavailable);
 AgentPanelAction stale;stale.kind=AgentPanelActionKind::start;stale.workspace_id=id;stale.provider_generation=before.provider_generation;stale.settings_generation=before.settings_generation;stale.prompt="Never execute";CHECK(!bounded.dispatch(stale,directory));stale.kind=AgentPanelActionKind::connect;CHECK(!bounded.dispatch(stale,directory));
 {std::unique_lock lock(gate);CHECK(cv.wait_for(lock,std::chrono::seconds(5),[&]{return entered;}));}
 CHECK(bounded.retiring_count()==1);CHECK(!bounded.can_reconfigure());
 for(unsigned n=0;n<AgentPanelController::maximum_retiring-1;++n)CHECK(bounded.retire(restored[n].workspace_persistent_id));
 CHECK(bounded.retiring_count()==AgentPanelController::maximum_retiring);
 CHECK(!bounded.retire(restored.back().workspace_persistent_id));CHECK(bounded.associations().size()==1); // backpressure leaves catalog intact
 {std::lock_guard lock(gate);release=true;}cv.notify_all();until([&]{return bounded.retiring_count()==0;});
 CHECK(bounded.retire(restored.back().workspace_persistent_id));until([&]{return bounded.retiring_count()==0;});CHECK(bounded.associations().empty());CHECK(bounded.can_reconfigure());
 // Active turns and unresolved approvals are never silently retired.
 AgentPanelController active({}, {}, {}, {},discover);CHECK(active.opened(id,directory));until([&]{return active.view(id,"Fixture",directory).state==AgentPanelState::ready;});
 auto v=active.view(id,"Fixture",directory);AgentPanelAction action;action.kind=AgentPanelActionKind::start;action.workspace_id=id;action.provider_generation=v.provider_generation;action.settings_generation=v.settings_generation;action.prompt="Authored fixture only";
 CHECK(active.dispatch(action,directory));until([&]{return !active.view(id,"Fixture",directory).approvals.empty();});CHECK(!active.retire(id));CHECK(active.retiring_count()==0);
 action.kind=AgentPanelActionKind::interrupt;CHECK(active.dispatch(action,directory));until([&]{return !active.active(id);});CHECK(active.retire(id));until([&]{return active.retiring_count()==0;});
 // A real authored stdio fixture waits in browser sign-in without opening any
 // browser/network. Closing its workspace must reap that daemon as well.
 const auto signed_out_dir=fixture.parent_path()/("agent-signed-out-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));CHECK(std::filesystem::create_directory(signed_out_dir));
 std::weak_ptr<AgentService> sign_in_service;
 {AgentPanelController signing({}, {}, {},[&](const AgentStdioOptions& options,std::shared_ptr<AgentService> service,std::string& error){sign_in_service=service;return AgentStdioTransport::launch(options,std::move(service),error);},[&]{return AgentDiscoveryResult{fixture.string(),"Authored signed-out fixture",signed_out_dir.string()};});
  CHECK(signing.opened(id,directory));until([&]{return signing.view(id,"Fixture",directory).auth==AgentAuth::signed_out;});
  const auto signed_out=signing.view(id,"Fixture",directory);AgentPanelAction login;login.kind=AgentPanelActionKind::sign_in;login.workspace_id=id;login.provider_generation=signed_out.provider_generation;login.settings_generation=signed_out.settings_generation;CHECK(signing.dispatch(login,directory));
  until([&]{return !signing.view(id,"Fixture",directory).login_id.empty();});CHECK(signing.view(id,"Fixture",directory).auth==AgentAuth::signing_in);CHECK(!signing.active(id));CHECK(signing.retire(id));until([&]{return signing.retiring_count()==0;});CHECK(sign_in_service.expired());}
 CHECK(std::filesystem::remove(signed_out_dir));
 // Destruction drains an owned live fixture even when no explicit retire occurs.
 std::weak_ptr<AgentService> shutdown_service;
 {AgentPanelController shutdown({}, {}, {},[&](const AgentStdioOptions& options,std::shared_ptr<AgentService> service,std::string& error){shutdown_service=service;return AgentStdioTransport::launch(options,std::move(service),error);},discover);CHECK(shutdown.opened(id,directory));until([&]{return shutdown.view(id,"Fixture",directory).state==AgentPanelState::ready;});}
 CHECK(shutdown_service.expired());
 // Missing installations have provider generation zero; retired catalog
 // generations still prevent stale Check again actions from recreating entries.
 AgentPanelController missing({}, {}, {}, {},[]{return AgentDiscoveryResult{{},"Missing fixture",{}};});CHECK(missing.opened(id,directory));until([&]{return missing.view(id,"Fixture",directory).state==AgentPanelState::unavailable&&!missing.active(id);});const auto old_missing=missing.view(id,"Fixture",directory);CHECK(old_missing.provider_generation==0);CHECK(missing.retire(id));stale.provider_generation=old_missing.provider_generation;stale.settings_generation=old_missing.settings_generation;CHECK(!missing.dispatch(stale,directory));

});}
