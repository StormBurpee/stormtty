// Full shipped managed loader qualification; reuse the already-qualified byte
// transport + real VT loop. Never replace PROMPT or the production Process route.
#define STORMTTY_BRIDGE_NO_MAIN
#include "stage2-host.cpp"
#include "stormtty/windows_shell_state.hpp"
#include "stormtty/windows_managed_shell.hpp"
#include <shlobj.h>
#include <filesystem>
#include <fstream>
namespace fs=std::filesystem;
namespace {
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point start) {return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
void environment(const wchar_t* name,const std::wstring& value) {need(SetEnvironmentVariableW(name,value.c_str())!=0,"fixture environment");}
std::wstring wide(const std::string& utf8) {
    const auto size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8.data(),static_cast<int>(utf8.size()),nullptr,0);
    need(size>0,"UTF8 path");std::wstring result(static_cast<std::size_t>(size),L'\0');
    need(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8.data(),static_cast<int>(utf8.size()),result.data(),size)==size,"UTF8 conversion");return result;
}
struct Scratch {
    fs::path root,home,cwd,previous_cwd{fs::current_path()};
    Scratch() {
        PWSTR value{};need(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&value)),"local appdata");
        struct Free {PWSTR p;~Free(){CoTaskMemFree(p);}} owned{value};
        root=fs::path(value)/(L"StormTTY bridge managed \u00e9 \u8def\u5f84 "+std::to_wstring(GetCurrentProcessId())+L" "+std::to_wstring(GetTickCount64()));
        need(fs::create_directory(root),"fresh scratch");home=root/L"home";cwd=root/L"cwd";
        need(fs::create_directory(home)&&fs::create_directory(cwd),"scratch directories");
    }
    bool cleanup() noexcept {
        std::error_code error;fs::current_path(previous_cwd,error);if(error)return false;
        fs::remove_all(root,error);return !error;
    }
    ~Scratch(){cleanup();}
};
fs::path completion_dump(const fs::path& state) {
    fs::path found;
    for(const auto& item:fs::directory_iterator(state)) {
        auto name=item.path().filename().wstring();
        if(name.starts_with(L"zcompdump-")&&!name.ends_with(L".zwc")&&!name.ends_with(L".old")&&!name.ends_with(L".lock")) {
            need(found.empty()&&item.is_regular_file(),"completion dump identity");found=item.path();
        }
    }
    need(!found.empty(),"completion dump missing");return found;
}
void ready(Bridge& bridge,unsigned seq,int status=0) {bridge.expect("__MANAGED_READY:"+std::to_string(seq)+":"+std::to_string(status)+"__");}
void measure(const fs::path& runtime,const fs::path& config,Scratch& scratch,unsigned sample) {
    unsigned phase=0;Bridge b;
    try {
        auto before=Clock::now();auto state=stormtty::prepare_managed_shell_state_at(scratch.root,false);
        const auto state_ms=elapsed(before);need(static_cast<bool>(state),"native state validation");
        fs::path prior_dump;fs::file_time_type prior_time{};
        if(sample==1) {
            need(!fs::exists(state.directory/L"config.zsh"),"fresh observer config");
            need(fs::file_size(config)<=65536,"observer config bound");
            std::ifstream input(config,std::ios::binary);need(input.good(),"read observer config");
            std::string contents(65537,'\0');input.read(contents.data(),static_cast<std::streamsize>(contents.size()));
            const auto bytes=input.gcount();need(input.eof()&&bytes<=65536,"observer read bound");contents.resize(static_cast<std::size_t>(bytes));
            // Create normally under the validator-protected directory, inheriting
            // its DACL. CopyFile-style security copying must not import repo ACLs.
            std::ofstream output(state.directory/L"config.zsh",std::ios::binary);output.write(contents.data(),static_cast<std::streamsize>(contents.size()));
            output.close();need(output.good(),"write observer config");
        } else {prior_dump=completion_dump(state.directory);prior_time=fs::last_write_time(prior_dump);}
        // Same scoped host-owned bootstrap contract as the production launcher.
        environment(L"HOME",scratch.home.generic_wstring());
        environment(L"ZDOTDIR",(runtime/L"stormtty/zsh").generic_wstring());
        environment(L"STORMTTY_NATIVE_ZSH_STATE",state.directory.generic_wstring());
        environment(L"STORMTTY_SESSION_ID",L"bridge-managed-qualification");
        environment(L"STORMTTY_EMPTY_HOME",L"");
        auto setup=stormtty::managed_shell_console_environment();
        need(setup.value.has_value(),"console helper contract");environment(L"STORMTTY_NATIVE_CONSOLE_SETUP",wide(*setup.value));
        need(SetCurrentDirectoryW(scratch.cwd.c_str())!=0,"scratch cwd");
        before=Clock::now();b.launch((runtime/L"usr/bin/stormtty-bridge-broker.exe").wstring());ready(b,1);
        const auto launch_ms=elapsed(before);const auto startup_responses=b.cursor_reports;phase=1;
        b.command("bridge_contract");b.expect("__MANAGED_CONTRACT_OK__");ready(b,2);phase=2;
        need(startup_responses>0,"no real startup cursor feedback");
        // Serial synthetic keystrokes; observation includes the 2-ms host poll
        // and TerminalCore feed/snapshot, NOT OS key-to-physical-display latency.
        const std::string text="bridge_typing 'newquotedtext0123456789'";
        std::string prefix;std::vector<double> observations;b.settle();
        for(char c:text) {
            prefix+=c;const auto old_cursor=b.terminal.snapshot()->cursor;const auto old_bytes=b.output_bytes;
            need(old_cursor.column+1<80,"typing fixture must not wrap");
            auto sent=Clock::now();b.send(std::string(1,c));bool observed=false;
            do {b.pump();const auto cursor=b.terminal.snapshot()->cursor;
                observed=b.output_bytes>old_bytes && cursor.row==old_cursor.row && cursor.column==old_cursor.column+1 && b.screen().find(prefix)!=std::string::npos;
            } while(!observed && Clock::now()-sent<2s);
            need(observed,"typed glyph/cursor observation");observations.push_back(elapsed(sent));
        }
        b.key(stormtty::Key::enter);b.expect("__MANAGED_TYPING:newquotedtext0123456789__");ready(b,3);phase=3;
        b.send(b.terminal.encode_paste("bridge_unicode '中🙂 pasted'"));b.key(stormtty::Key::enter);
        b.expect("__BRIDGE_UNICODE:中🙂 pasted__");ready(b,4);
        b.send(b.terminal.encode_paste("bridge_length '"+std::string(4096,'x')+"'"));b.key(stormtty::Key::enter);
        b.expect("__BRIDGE_LENGTH:4096__");ready(b,5);phase=4;
        b.send("bridge_unicode 'abXc'");b.resize(100,35);b.key(stormtty::Key::left);b.key(stormtty::Key::left);b.key(stormtty::Key::backspace);b.key(stormtty::Key::enter);
        b.expect("__BRIDGE_UNICODE:abc__");ready(b,6);phase=5;
        for(unsigned i=0;i<10;i++)b.resize(90+i,30+i);
        b.resize(132,43);b.settle();b.command("bridge_size");b.expect("__BRIDGE_SIZE:132x43__");ready(b,7);phase=6;
        const auto cursor_before=b.cursor_reports;
        b.command("bridge_native");b.expect("__BRIDGE_NATIVE_READY:132:43__");
        b.resize(148,47);b.settle();b.send("native-ok\r");b.expect("__BRIDGE_NATIVE_DONE:148:47__");
        b.expect("__BRIDGE_NATIVE_UNICODE:中🙂__");ready(b,8);
        const auto native_responses=b.cursor_reports-cursor_before;need(native_responses>0,"native cursor feedback");phase=7;
        b.command("bridge_size");b.expect("__BRIDGE_SIZE:148x47__");ready(b,9);
        b.command("bridge_wait");b.expect("__BRIDGE_WAIT__");b.key(stormtty::Key::c,stormtty::control);ready(b,10,130);phase=8;
        b.command("bridge_contract");b.expect("__MANAGED_CONTRACT_OK__");ready(b,11);phase=9;
        b.command("exit 37");need(b.await_exit()==37,"managed shell exit");need(b.cleanup(),"owned managed cleanup");phase=10;
        need(static_cast<bool>(stormtty::prepare_managed_shell_state_at(scratch.root,false)),"postlaunch state validation");
        need(fs::is_regular_file(state.directory/L"history"),"private history missing");
        const auto dump=completion_dump(state.directory);
        if(sample==2)need(dump==prior_dump&&fs::last_write_time(dump)==prior_time,"warm completion cache replaced");
        std::sort(observations.begin(),observations.end());const auto count=observations.size();
        std::cout<<"BRIDGE_MANAGED_SAMPLE sample="<<sample<<" fresh="<<(sample==1)<<" phases="<<phase
            <<" state_prepare_ms="<<state_ms<<" launch_to_zle_observation_ms="<<launch_ms
            <<" typed_echo_samples="<<count<<" typed_echo_median_ms="<<observations[count/2]
            <<" typed_echo_p95_ms="<<observations[(count*95-1)/100]<<" typed_echo_max_ms="<<observations.back()
            <<" startup_cursor_reports="<<startup_responses<<" native_cursor_reports="<<native_responses
            <<" warm_dump_unchanged="<<(sample==2)<<" result=PASS\n";
    } catch(const fs::filesystem_error& e) {
        std::cerr<<"BRIDGE_MANAGED_SAMPLE sample="<<sample<<" phase="<<phase<<" result=FAIL filesystem_code="<<e.code().value()<<"\n";
        b.cleanup();throw;
    } catch(const std::exception& e) {
        std::cerr<<"BRIDGE_MANAGED_SAMPLE sample="<<sample<<" phase="<<phase<<" result=FAIL reason="<<e.what()<<"\n";
        // Numeric phase + fixed synthetic output only; never dump full managed
        // screen/history/environment (the prompt contains a temporary path).
        b.cleanup();throw;
    }
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        need(argc==3,"managed arguments");const auto runtime=fs::absolute(argv[1]);const auto config=fs::absolute(argv[2]);
        need(fs::is_regular_file(runtime/L"stormtty/zsh/.zshrc"),"shipped loader missing");
        need(fs::is_regular_file(runtime/L"stormtty/vendor/oh-my-zsh/oh-my-zsh.sh"),"shipped OMZ missing");
        const auto cwd=fs::current_path();Scratch scratch;
        for(unsigned sample=1;sample<=2;sample++)measure(runtime,config,scratch,sample);
        fs::current_path(cwd); // Release scratch CWD before its exact owned tree removal.
        need(scratch.cleanup(),"scratch cleanup");
        std::cout<<"BRIDGE_MANAGED_RESULT fresh=1 warm=1 result=PASS\n";return 0;
    } catch(...) {std::cout<<"BRIDGE_MANAGED_RESULT result=FAIL\n";return 1;}
}
