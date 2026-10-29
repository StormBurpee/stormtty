// Optional Windows lifecycle checks. No runtime/profile default is activated.
#include "stormtty/windows_shell_state.hpp"
#include "stormtty/process.hpp"
#include <windows.h>
#include <shlobj.h>
#include <aclapi.h>
#include <sddl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <shellapi.h>
#include <stdexcept>
namespace fs=std::filesystem;
using namespace stormtty;
using namespace std::chrono_literals;
void check(bool ok){if(!ok)throw std::runtime_error("initialization check failed");}
std::string value(const std::vector<ProcessEnvironmentEntry>& entries,std::string_view name){
    for(auto it=entries.rbegin();it!=entries.rend();++it)if(it->name==name)return it->value.value_or("");return "";
}
struct Fixture {
    fs::path root;
    Fixture(){PWSTR raw{};check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&raw)));
        const fs::path local(raw);CoTaskMemFree(raw);
        GUID id{};check(SUCCEEDED(CoCreateGuid(&id)));
        wchar_t text[40]{};check(StringFromGUID2(id,text,40)>0);
        root=local/(L"StormTTY initialization fixture "+std::wstring(text));
        check(fs::create_directory(root));}
    ~Fixture(){std::error_code ignored;fs::remove_all(root,ignored);}
};
int main(){unsigned stage{};try{
    int argc{};auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);check(argv);
    struct Arguments {LPWSTR* data;~Arguments(){LocalFree(data);}} arguments{argv};
    if(argc==4 && std::wstring_view(argv[1])==L"--hold-initialization") {
        auto held=managed_shell_initialization_at(fs::path(argv[2]),{});
        std::vector<ProcessEnvironmentEntry> e;held->prepare(e);check(value(e,"STORMTTY_NATIVE_ZSH_NO_DUMP")=="0");
        HANDLE release=OpenEventW(SYNCHRONIZE,FALSE,argv[3]);check(release);
        std::cout<<"INITIALIZATION_HELD\n"<<std::flush;
        const auto result=WaitForSingleObject(release,20000);CloseHandle(release);held->finished();
        check(result==WAIT_OBJECT_0);return 0;
    }
    check(argc==1);
    Fixture fixture;unsigned callbacks{};
    auto owner=managed_shell_initialization_at(fixture.root,[&]{++callbacks;return false;});
    std::vector<ProcessEnvironmentEntry> env;owner->prepare(env);stage=1;
    check(value(env,"STORMTTY_NATIVE_ZSH_NO_DUMP")=="0" && owner->ready_event()!=0 && callbacks==1);
    bool contender_ok{};
    std::thread contender([&]{
        auto next=managed_shell_initialization_at(fixture.root,[]{throw std::runtime_error("must not adopt");return false;});
        std::vector<ProcessEnvironmentEntry> other;next->prepare(other);
        contender_ok=value(other,"STORMTTY_NATIVE_ZSH_NO_DUMP")=="1" && next->ready_event()==0 && !value(other,"STORMTTY_NATIVE_ZSH_STATE").empty();
        next->finished();
    });contender.join();check(contender_ok);stage=2;
    auto event=OpenEventA(EVENT_MODIFY_STATE,FALSE,value(env,"STORMTTY_NATIVE_ZSH_INIT_EVENT").c_str());check(event);
    check(SetEvent(event));CloseHandle(event);check(WaitForSingleObject(reinterpret_cast<HANDLE>(owner->ready_event()),0)==WAIT_OBJECT_0);
    owner->ready();check(owner->ready_event()==0);
    const auto state=prepare_managed_shell_state_at(fixture.root,false);check(static_cast<bool>(state));
    std::ofstream(state.directory/L"config.zsh")<<"# existing user config\n";
    auto custom=managed_shell_initialization_at(fixture.root,[&]{++callbacks;return true;});
    env.clear();custom->prepare(env);check(value(env,"STORMTTY_NATIVE_ZSH_NO_DUMP")=="0" && callbacks==1);custom->finished();stage=3;
    // Actual child exit without acknowledgement must release on its controller,
    // not the UI/probe thread that created the initialization object.
    wchar_t system[MAX_PATH]{};check(GetSystemDirectoryW(system,MAX_PATH)>0);
    const auto command=fs::path(system)/L"cmd.exe";auto u8=command.generic_u8string();
    ProcessOptions options;options.executable={reinterpret_cast<const char*>(u8.data()),u8.size()};options.arguments={"/d","/c","exit 0"};
    options.initialization=managed_shell_initialization_at(fixture.root,{});
    auto process=Process::start(std::move(options),{});check(process->wait_for_exit(20s));
    auto after=managed_shell_initialization_at(fixture.root,{});env.clear();after->prepare(env);
    check(value(env,"STORMTTY_NATIVE_ZSH_NO_DUMP")=="0");after->finished();
    // A controller disappearing without ready/finished must not make a possibly
    // interrupted executable cache eligible again on the next launch.
    stage=31;Fixture abandoned_fixture;
    auto abandoned=managed_shell_initialization_at(abandoned_fixture.root,{});
    bool held{};
    std::thread departed([&]{std::vector<ProcessEnvironmentEntry> e;abandoned->prepare(e);held=abandoned->ready_event()!=0;});
    departed.join();check(held);stage=4;
    auto recover=managed_shell_initialization_at(abandoned_fixture.root,{});env.clear();recover->prepare(env);
    const auto uncertain_state=prepare_managed_shell_state_at(abandoned_fixture.root,false);
    std::error_code marker_error;
    const bool marker_present=uncertain_state && fs::exists(uncertain_state.directory/L"completion-init-uncertain",marker_error);
    std::cout<<"INITIALIZATION_ABANDONED recovery_no_dump="<<(value(env,"STORMTTY_NATIVE_ZSH_NO_DUMP")=="1")
             <<" recovery_event="<<(recover->ready_event()!=0)<<" state_valid="<<static_cast<bool>(uncertain_state)
             <<" marker_present="<<marker_present<<" marker_error="<<marker_error.value()<<'\n';
    stage=41;check(value(env,"STORMTTY_NATIVE_ZSH_NO_DUMP")=="1");recover->finished();
    auto uncertain=managed_shell_initialization_at(abandoned_fixture.root,{});env.clear();uncertain->prepare(env);
    stage=42;check(value(env,"STORMTTY_NATIVE_ZSH_NO_DUMP")=="1");uncertain->finished();abandoned.reset();
    // Same-user second process, not a same-thread recursive mutex acquisition.
    stage=5;Fixture process_fixture;
    std::wstring release_name=L"Local\\StormTTY-InitProbe-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    HANDLE release=CreateEventW(nullptr,TRUE,FALSE,release_name.c_str());check(release && GetLastError()!=ERROR_ALREADY_EXISTS);
    struct Event {HANDLE h;~Event(){CloseHandle(h);}} release_guard{release};
    std::wstring module(32768,L'\0');const DWORD length=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));check(length && length<module.size());module.resize(length);
    const auto as_utf8=[](const fs::path& p){auto text=p.generic_u8string();return std::string(reinterpret_cast<const char*>(text.data()),text.size());};
    ProcessOptions child_options;child_options.executable=as_utf8(fs::path(module));
    child_options.arguments={"--hold-initialization",as_utf8(process_fixture.root),std::string(release_name.begin(),release_name.end())};
    struct Observation {std::atomic<bool> held{};std::string output;std::optional<std::uint32_t> code;};
    auto observed=std::make_shared<Observation>();ProcessCallbacks child_callbacks;
    child_callbacks.output=[observed](std::string_view bytes,std::stop_token){
        if(observed->output.size()+bytes.size()>1024)return;
        observed->output+=bytes;if(observed->output.find("INITIALIZATION_HELD")!=observed->output.npos)observed->held.store(true);
    };
    child_callbacks.exited=[observed](ProcessExit exit){observed->code=exit.exit_code;};
    auto holder=Process::start(std::move(child_options),std::move(child_callbacks));
    const auto deadline=std::chrono::steady_clock::now()+10s;
    while(!observed->held.load() && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(5ms);
    check(observed->held.load());auto cross=managed_shell_initialization_at(process_fixture.root,{});env.clear();cross->prepare(env);
    check(value(env,"STORMTTY_NATIVE_ZSH_NO_DUMP")=="1" && !value(env,"STORMTTY_NATIVE_ZSH_STATE").empty());cross->finished();
    check(SetEvent(release));check(holder->wait_for_exit(20s));check(observed->code && *observed->code==0);
    stage=6;Fixture acl_fixture;const auto acl_state=prepare_managed_shell_state_at(acl_fixture.root,false);check(static_cast<bool>(acl_state));
    const auto public_file=acl_state.directory/L"public-runtime-fixture";std::ofstream(public_file)<<"authored fixture";
    auto checker=managed_runtime_access_check();stage=6;
    HANDLE file=CreateFileW(public_file.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    check(file!=INVALID_HANDLE_VALUE);check(checker(reinterpret_cast<std::uintptr_t>(file),false));
    PSID everyone{};check(ConvertStringSidToSidW(L"S-1-1-0",&everyone));
    for(const auto permission:{FILE_GENERIC_READ,FILE_GENERIC_WRITE}) {
        PACL old_acl{},new_acl{};PSECURITY_DESCRIPTOR sd{};
        check(GetNamedSecurityInfoW(public_file.c_str(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&old_acl,nullptr,&sd)==ERROR_SUCCESS);
        EXPLICIT_ACCESSW access{};access.grfAccessPermissions=permission;access.grfAccessMode=GRANT_ACCESS;
        access.Trustee.TrusteeForm=TRUSTEE_IS_SID;access.Trustee.TrusteeType=TRUSTEE_IS_WELL_KNOWN_GROUP;access.Trustee.ptstrName=static_cast<LPWSTR>(everyone);
        check(SetEntriesInAclW(1,&access,old_acl,&new_acl)==ERROR_SUCCESS);
        check(SetNamedSecurityInfoW(const_cast<LPWSTR>(public_file.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,new_acl,nullptr)==ERROR_SUCCESS);
        LocalFree(new_acl);LocalFree(sd);
        check(checker(reinterpret_cast<std::uintptr_t>(file),false)==(permission==FILE_GENERIC_READ));
    }
    LocalFree(everyone);CloseHandle(file);
    std::cout<<"INITIALIZATION_CHECKS code=0 contention=1 config_preserved=1 native_ack=1 child_exit_release=1 abandoned_owner_no_dump=1 public_runtime_acl=1 cross_process_contention=1\n";return 0;
}catch(...){std::cout<<"INITIALIZATION_CHECKS code=1 stage="<<stage<<'\n';return 1;}}
