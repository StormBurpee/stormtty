#include "stormtty/agent_panel_controller.hpp"
#include "stormtty/agent_discovery.hpp"
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
namespace stormtty {
struct AgentPanelController::Entry {
    std::shared_ptr<AgentService> service;
    mutable std::mutex mutex;
    std::unique_ptr<AgentStdioTransport> transport;
    enum class Discovery { unresolved, missing, available, launch_failed };
    bool launching{};
    Discovery discovery{Discovery::unresolved};
    std::string executable, discovery_error;
    std::jthread worker; // Joined before any field the worker references is destroyed.
};
// Exactly one cleanup worker, with queued + in-flight ownership bounded. Entry
// destruction joins its launch worker and transport on this thread, never UI.
struct AgentPanelController::Reaper {
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::unique_ptr<Entry>> queue;
    std::size_t count{};
    bool stopping{};
    std::function<void()> wake;
    std::thread worker;
    explicit Reaper(std::function<void()> notify):wake(std::move(notify)),worker([this]{run();}) {}
    void run() {
        for(;;) {
            std::unique_ptr<Entry> entry;
            {std::unique_lock lock(mutex);changed.wait(lock,[&]{return stopping||!queue.empty();});
             if(queue.empty()&&stopping)return;
             entry=std::move(queue.front());queue.pop_front();}
            entry.reset();
            {std::lock_guard lock(mutex);--count;}changed.notify_all();
            if(wake){try{wake();}catch(...){}}
        }
    }
    bool enqueue(std::unique_ptr<Entry>& entry,bool wait=false) {
        std::unique_lock lock(mutex);
        if(wait)changed.wait(lock,[&]{return count<maximum_retiring;});
        else if(count>=maximum_retiring)return false;
        queue.push_back(std::move(entry));++count;lock.unlock();changed.notify_one();return true;
    }
    std::size_t pending() const {std::lock_guard lock(mutex);return count;}
    ~Reaper() {{std::lock_guard lock(mutex);stopping=true;}changed.notify_all();if(worker.joinable())worker.join();}
};
AgentPanelController::AgentPanelController(AgentStdioOptions o,std::function<void()> w,std::vector<WorkspaceAgentAssociation> restored,LaunchFactory launch,DiscoveryFactory discover):options_(std::move(o)),launch_(std::move(launch)),discover_(std::move(discover)),wake_(std::move(w)){
    reaper_=std::make_unique<Reaper>(wake_);
    if(options_.runtime_version.empty())options_.runtime_version="not-probed";
    if(!launch_)launch_=AgentStdioTransport::launch;
    if(!discover_)discover_=[] {return discover_codex();};
    // Metadata restore is inert and never silently changes account identity.
    for(auto& association:restored) if(options_.account_reference.empty() || association.account_reference==options_.account_reference) {
        auto entry=std::make_unique<Entry>();entry->service=std::make_shared<AgentService>(association);
        entries_.try_emplace(association.workspace_persistent_id,std::move(entry));
    }
}
std::vector<WorkspaceAgentAssociation> AgentPanelController::associations() const {
    std::vector<WorkspaceAgentAssociation> result;for(const auto& [id,e]:entries_){(void)id;auto a=e->service->snapshot().association;if(!a.account_reference.empty())result.push_back(std::move(a));}return result;
}
AgentPanelController::~AgentPanelController() {
    // Application shutdown drains ownership (and may wait); ordinary workspace
    // close uses nonblocking retire() instead. The reaper owns every process join.
    for(auto& [id,entry]:entries_){(void)id;reaper_->enqueue(entry,true);}
    entries_.clear();reaper_.reset();
}
std::size_t AgentPanelController::retiring_count() const {return reaper_->pending();}
bool AgentPanelController::retire(std::string_view id) {
    auto i=entries_.find(std::string(id));if(i==entries_.end())return true;
    if(active(id)||settings_generation_==UINT64_MAX)return false;
    if(!reaper_->enqueue(i->second))return false;
    // This is also the UI catalog generation: reject old Connect actions even
    // when the retired service never connected and its provider generation is 0.
    ++settings_generation_;
    entries_.erase(i);return true;
}
bool AgentPanelController::can_reconfigure() const {
    if(retiring_count()!=0)return false;
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
    bool configured=!options_.executable.empty();
    auto discovery=Entry::Discovery::unresolved;std::string discovery_error;
    if(auto i=entries_.find(id);i!=entries_.end()) {
        std::lock_guard lock(i->second->mutex);discovery=i->second->discovery;discovery_error=i->second->discovery_error;
        if(discovery==Entry::Discovery::missing)configured=false;
        else if(discovery==Entry::Discovery::available||discovery==Entry::Discovery::launch_failed)configured=true;
    }
    AgentSnapshot snapshot;snapshot.association.workspace_persistent_id=id;
    if(auto i=entries_.find(id);i!=entries_.end())snapshot=i->second->service->snapshot();
    auto result=agent_panel_view(snapshot,std::move(name),std::move(directory),configured);
    if(auto i=entries_.find(id);i!=entries_.end()){std::lock_guard lock(i->second->mutex);result.disconnect_available=!i->second->launching&&static_cast<bool>(i->second->transport);}
    if(discovery==Entry::Discovery::launch_failed)result.state=AgentPanelState::failed;
    if(!discovery_error.empty())result.error=discovery_error;
    result.auth=snapshot.auth; result.account_label=snapshot.account_label; result.login_id=snapshot.login_id; result.auth_url=snapshot.auth_url;
    if(auto i=entries_.find(id);i!=entries_.end()){std::lock_guard lock(i->second->mutex);if(i->second->launching)result.state=AgentPanelState::connecting;}
    result.settings_generation=settings_generation_;
    result.connection_scope=options_.executable+"\nAccount reference: "+options_.account_reference+"\nLaunch app-server only; no task or sign-in is authorized.";
    return result;
}
bool AgentPanelController::opened(std::string id,std::string directory) {
    if(!options_.executable.empty()) return false; // Unknown override needs explicit consent.
    auto i=entries_.find(id);
    if(i!=entries_.end() && i->second->service->snapshot().generation!=0) return false;
    AgentPanelAction action; action.kind=AgentPanelActionKind::connect; action.workspace_id=std::move(id); action.settings_generation=settings_generation_;
    return dispatch(action,std::move(directory));
}
bool AgentPanelController::dispatch(const AgentPanelAction& a,std::string directory) {
    if(a.workspace_id.empty()||a.settings_generation!=settings_generation_)return false;
    if(a.kind==AgentPanelActionKind::connect) {
        auto& entry=entries_[a.workspace_id];
        if(!entry) {entry=std::make_unique<Entry>();entry->service=std::make_shared<AgentService>(WorkspaceAgentAssociation{a.workspace_id,"codex",options_.account_reference,{}});}
        auto* e=entry.get();const auto snapshot=e->service->snapshot();
        if(snapshot.generation!=a.provider_generation||snapshot.connection==AgentConnection::ready||snapshot.connection==AgentConnection::connecting)return false;
        {std::lock_guard lock(e->mutex);if(e->launching)return false;e->launching=true;e->discovery=Entry::Discovery::unresolved;e->discovery_error.clear();}
        auto options=options_;options.account_reference=snapshot.association.account_reference;options.working_directory=std::move(directory);options.user_confirmed_launch=true;options.state_changed=wake_;
        e->worker=std::jthread([e,options=std::move(options),wake=wake_,launch=launch_,discover=discover_] {
            try {
                std::unique_ptr<AgentStdioTransport> old_transport;
                {std::lock_guard lock(e->mutex);old_transport=std::move(e->transport);}
                old_transport.reset();std::string error;
                auto resolved=options;
                if(resolved.executable.empty()) {
                    const auto discovered=discover();
                    if(discovered.executable.empty()) { e->service->disconnect(discovered.detail); {std::lock_guard lock(e->mutex);e->discovery=Entry::Discovery::missing;e->discovery_error=discovered.detail;e->executable.clear();e->launching=false;}if(wake){try{wake();}catch(...){}}return; }
                    resolved.executable=discovered.executable;
                    resolved.working_directory=discovered.working_directory;
                }
                {std::lock_guard lock(e->mutex);e->executable=resolved.executable;e->discovery=Entry::Discovery::available;}
                auto transport=launch(resolved,e->service,error);
                if(!transport)e->service->disconnect(error.empty()?"Provider launch failed.":error);
                {std::lock_guard lock(e->mutex);if(!transport){e->discovery=Entry::Discovery::launch_failed;e->discovery_error=error.empty()?"Provider launch failed.":error;}e->transport=std::move(transport);e->launching=false;}
            } catch(const std::exception& error) {e->service->disconnect(error.what());std::lock_guard lock(e->mutex);e->discovery=Entry::Discovery::launch_failed;e->discovery_error=error.what();e->launching=false;}
            catch(...) {e->service->disconnect("Provider launch failed.");std::lock_guard lock(e->mutex);e->discovery=Entry::Discovery::launch_failed;e->discovery_error="Provider launch failed.";e->launching=false;}
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
            std::unique_ptr<AgentStdioTransport> old_transport;
            {std::lock_guard lock(e->mutex);old_transport=std::move(e->transport);}
            old_transport.reset();e->service->disconnect();
            {std::lock_guard lock(e->mutex);e->discovery_error.clear();e->launching=false;}
            if(wake){try{wake();}catch(...){}}
        });return true;
    }
    case AgentPanelActionKind::sign_in:return service.login();
    case AgentPanelActionKind::cancel_sign_in:if(a.login_id!=snapshot.login_id)return false;return service.cancel_login();
    case AgentPanelActionKind::refresh_account:return service.refresh_account();
    case AgentPanelActionKind::open_sign_in:case AgentPanelActionKind::install_codex:return false; // Host-only explicit links.
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
