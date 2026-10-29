#include "stormtty/agent_setup_service.hpp"
#include <windows.h>
#include <shlobj.h>
namespace stormtty {
std::filesystem::path local_agent_settings_path(){
    PWSTR raw=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&raw)))return {};
    struct Release {PWSTR value;~Release(){CoTaskMemFree(value);}} release{raw};
    return std::filesystem::path(raw)/L"StormTTY"/L"agent.local.stormtty";
}
AgentSetupService::AgentSetupService(std::filesystem::path path,std::function<void()> wake):path_(std::move(path)),wake_(std::move(wake)){}
AgentSetupService::~AgentSetupService(){if(worker_.joinable())worker_.join();}
void AgentSetupService::load(){
    if(busy_)return;
    if(worker_.joinable())worker_.join();
    busy_=true;
    try {worker_=std::thread([this]{
        AgentSetupCompletion result;
        try {auto loaded=load_agent_provider_settings(path_);result.snapshot=std::move(loaded.snapshot);result.error=std::move(loaded.error);if(loaded.settings)result.settings=std::move(*loaded.settings);}
        catch(const std::exception& e){result.error=e.what();}catch(...){result.error="Could not load local provider settings.";}
        {std::lock_guard lock(mutex_);completion_=std::move(result);}
        if(wake_){try{wake_();}catch(...){}}
    });}catch(...){busy_=false;throw;}
}
bool AgentSetupService::save(const AgentProviderSettings& settings){
    if(busy_||!baseline_)return false;
    if(worker_.joinable())worker_.join();
    busy_=true;
    try {worker_=std::thread([this,settings,baseline=*baseline_]{
        AgentSetupCompletion result;result.saved=true;result.settings=settings;
        try {auto saved=save_agent_provider_settings(path_,settings,baseline);result.snapshot=std::move(saved.snapshot);result.error=saved.conflict?"Local provider settings changed. Use Reload saved; unsaved edits require confirmation.":std::move(saved.error);}
        catch(const std::exception& e){result.error=e.what();}catch(...){result.error="Could not save local provider settings.";}
        {std::lock_guard lock(mutex_);completion_=std::move(result);}
        if(wake_){try{wake_();}catch(...){}}
    });}catch(...){busy_=false;throw;}return true;
}
std::optional<AgentSetupCompletion> AgentSetupService::take_completion(){
    std::optional<AgentSetupCompletion> result;{std::lock_guard lock(mutex_);result.swap(completion_);}
    if(result){if(worker_.joinable())worker_.join();busy_=false;if(result->snapshot)baseline_=result->snapshot;else if(!result->saved)baseline_.reset();}
    return result;
}
}
