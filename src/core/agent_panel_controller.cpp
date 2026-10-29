#include "stormtty/agent_panel_controller.hpp"
#include <mutex>
#include <thread>
namespace stormtty {
struct AgentPanelController::Entry {
    std::shared_ptr<AgentService> service;
    mutable std::mutex mutex;
    std::unique_ptr<AgentStdioTransport> transport;
    bool launching{};
    std::jthread worker; // Joined before any field the worker references is destroyed.
};
AgentPanelController::AgentPanelController(AgentStdioOptions o,std::function<void()> w):options_(std::move(o)),wake_(std::move(w)){}
AgentPanelController::~AgentPanelController()=default;
AgentPanelView AgentPanelController::view(std::string id,std::string name,std::string directory) const {
    const bool configured=!options_.executable.empty()&&!options_.account_reference.empty();
    AgentSnapshot snapshot;snapshot.association.workspace_persistent_id=id;
    if(auto i=entries_.find(id);i!=entries_.end())snapshot=i->second->service->snapshot();
    auto result=agent_panel_view(snapshot,std::move(name),std::move(directory),configured);
    result.connection_scope=options_.executable+"\nAccount reference: "+options_.account_reference+"\nLaunch app-server only; no task or sign-in is authorized.";
    return result;
}
bool AgentPanelController::dispatch(const AgentPanelAction& a,std::string directory) {
    if(a.workspace_id.empty())return false;
    if(a.kind==AgentPanelActionKind::connect) {
        if(options_.executable.empty()||options_.account_reference.empty())return false;
        auto& entry=entries_[a.workspace_id];
        if(!entry) {entry=std::make_unique<Entry>();entry->service=std::make_shared<AgentService>(WorkspaceAgentAssociation{a.workspace_id,"codex",options_.account_reference,{}});}
        auto* e=entry.get();const auto snapshot=e->service->snapshot();
        if(snapshot.generation!=a.provider_generation||snapshot.connection==AgentConnection::ready||snapshot.connection==AgentConnection::connecting)return false;
        {std::lock_guard lock(e->mutex);if(e->launching)return false;e->launching=true;}
        auto options=options_;options.working_directory=std::move(directory);options.user_confirmed_launch=true;options.state_changed=wake_;
        e->worker=std::jthread([e,options=std::move(options),wake=wake_] {
            try {
                e->transport.reset();std::string error;
                auto transport=AgentStdioTransport::launch(options,e->service,error);
                if(!transport)e->service->disconnect(error.empty()?"Provider launch failed.":error);
                {std::lock_guard lock(e->mutex);e->transport=std::move(transport);e->launching=false;}
            } catch(const std::exception& error) {e->service->disconnect(error.what());std::lock_guard lock(e->mutex);e->launching=false;}
            catch(...) {e->service->disconnect("Provider launch failed.");std::lock_guard lock(e->mutex);e->launching=false;}
            if(wake) {try {wake();} catch(...) {}}
        });
        return true;
    }
    auto i=entries_.find(a.workspace_id);if(i==entries_.end())return false;
    auto& service=*i->second->service;const auto snapshot=service.snapshot();
    if(snapshot.generation!=a.provider_generation)return false;
    switch(a.kind) {
    case AgentPanelActionKind::disconnect: {
        if(active(a.workspace_id))return false;
        auto* e=i->second.get();
        {std::lock_guard lock(e->mutex);if(e->launching)return false;e->launching=true;}
        e->worker=std::jthread([e,wake=wake_] {
            e->transport.reset();e->service->disconnect();
            {std::lock_guard lock(e->mutex);e->launching=false;}
            if(wake){try{wake();}catch(...){}}
        });return true;
    }
    case AgentPanelActionKind::start:return service.start({a.prompt,std::move(directory),true});
    case AgentPanelActionKind::interrupt:return service.interrupt();
    case AgentPanelActionKind::allow:return service.resolve(a.approval_key,AgentApprovalDecision::accept_once);
    case AgentPanelActionKind::decline:return service.resolve(a.approval_key,AgentApprovalDecision::decline);
    case AgentPanelActionKind::connect:break;
    }
    return false;
}
std::string AgentPanelController::attention_workspace() const {for(const auto& [id,e]:entries_)if(!e->service->snapshot().approvals.empty())return id;return {};}
bool AgentPanelController::any_active() const {for(const auto& [id,e]:entries_){(void)e;if(active(id))return true;}return false;}
bool AgentPanelController::attention() const {for(const auto& [id,e]:entries_){(void)id;const auto s=e->service->snapshot();if(!s.approvals.empty()||s.connection==AgentConnection::failed)return true;}return false;}
bool AgentPanelController::active(std::string_view id) const {auto i=entries_.find(std::string(id));if(i==entries_.end())return false;{std::lock_guard lock(i->second->mutex);if(i->second->launching)return true;}const auto s=i->second->service->snapshot();return s.connection==AgentConnection::connecting||s.conversation==AgentConversation::starting||s.conversation==AgentConversation::running||s.conversation==AgentConversation::interrupting||!s.approvals.empty();}
}
