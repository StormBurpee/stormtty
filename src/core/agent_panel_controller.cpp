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
AgentPanelController::AgentPanelController(AgentStdioOptions o,std::function<void()> w,std::vector<WorkspaceAgentAssociation> restored,LaunchFactory launch):options_(std::move(o)),launch_(std::move(launch)),wake_(std::move(w)){
    if(options_.runtime_version.empty())options_.runtime_version="not-probed";
    if(!launch_)launch_=AgentStdioTransport::launch;
    // Metadata restore is inert and never silently changes account identity.
    for(auto& association:restored) if(association.account_reference==options_.account_reference) {
        auto entry=std::make_unique<Entry>();entry->service=std::make_shared<AgentService>(association);
        entries_.try_emplace(association.workspace_persistent_id,std::move(entry));
    }
}
std::vector<WorkspaceAgentAssociation> AgentPanelController::associations() const {
    std::vector<WorkspaceAgentAssociation> result;for(const auto& [id,e]:entries_){(void)id;result.push_back(e->service->snapshot().association);}return result;
}
AgentPanelController::~AgentPanelController()=default;
bool AgentPanelController::can_reconfigure() const {
    for(const auto& [id,e]:entries_){(void)id;std::lock_guard lock(e->mutex);if(e->launching||e->transport)return false;}
    return true;
}
bool AgentPanelController::reconfigure(const AgentProviderSettings& settings) {
    if(!can_reconfigure()||!validate_agent_provider_settings(settings).empty())return false;
    if(options_.executable==settings.executable&&options_.account_reference==settings.account_reference)return true;
    if(settings_generation_==UINT64_MAX)return false;
    ++settings_generation_;
    // A different account/executable must never inherit thread IDs or approvals.
    entries_.clear();options_.executable=settings.executable;options_.account_reference=settings.account_reference;options_.runtime_version="not-probed";return true;
}
AgentPanelView AgentPanelController::view(std::string id,std::string name,std::string directory) const {
    const bool configured=!options_.executable.empty()&&!options_.account_reference.empty();
    AgentSnapshot snapshot;snapshot.association.workspace_persistent_id=id;
    if(auto i=entries_.find(id);i!=entries_.end())snapshot=i->second->service->snapshot();
    auto result=agent_panel_view(snapshot,std::move(name),std::move(directory),configured);
    if(auto i=entries_.find(id);i!=entries_.end()){std::lock_guard lock(i->second->mutex);result.disconnect_available=!i->second->launching&&static_cast<bool>(i->second->transport);}
    result.settings_generation=settings_generation_;
    result.connection_scope=options_.executable+"\nAccount reference: "+options_.account_reference+"\nLaunch app-server only; no task or sign-in is authorized.";
    return result;
}
bool AgentPanelController::dispatch(const AgentPanelAction& a,std::string directory) {
    if(a.workspace_id.empty()||a.settings_generation!=settings_generation_)return false;
    if(a.kind==AgentPanelActionKind::connect) {
        if(options_.executable.empty()||options_.account_reference.empty())return false;
        auto& entry=entries_[a.workspace_id];
        if(!entry) {entry=std::make_unique<Entry>();entry->service=std::make_shared<AgentService>(WorkspaceAgentAssociation{a.workspace_id,"codex",options_.account_reference,{}});}
        auto* e=entry.get();const auto snapshot=e->service->snapshot();
        if(snapshot.generation!=a.provider_generation||snapshot.connection==AgentConnection::ready||snapshot.connection==AgentConnection::connecting)return false;
        {std::lock_guard lock(e->mutex);if(e->launching)return false;e->launching=true;}
        auto options=options_;options.working_directory=std::move(directory);options.user_confirmed_launch=true;options.state_changed=wake_;
        e->worker=std::jthread([e,options=std::move(options),wake=wake_,launch=launch_] {
            try {
                e->transport.reset();std::string error;
                auto transport=launch(options,e->service,error);
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
    case AgentPanelActionKind::reload_settings:case AgentPanelActionKind::save_settings:case AgentPanelActionKind::connect:break;
    }
    return false;
}
std::string AgentPanelController::attention_workspace() const {for(const auto& [id,e]:entries_)if(!e->service->snapshot().approvals.empty())return id;return {};}
bool AgentPanelController::any_active() const {for(const auto& [id,e]:entries_){(void)e;if(active(id))return true;}return false;}
bool AgentPanelController::attention() const {for(const auto& [id,e]:entries_){(void)id;const auto s=e->service->snapshot();if(!s.approvals.empty()||s.connection==AgentConnection::failed)return true;}return false;}
bool AgentPanelController::active(std::string_view id) const {auto i=entries_.find(std::string(id));if(i==entries_.end())return false;{std::lock_guard lock(i->second->mutex);if(i->second->launching)return true;}const auto s=i->second->service->snapshot();return s.connection==AgentConnection::connecting||s.conversation==AgentConversation::starting||s.conversation==AgentConversation::running||s.conversation==AgentConversation::interrupting||!s.approvals.empty();}
}
