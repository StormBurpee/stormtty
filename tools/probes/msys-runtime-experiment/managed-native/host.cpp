// Qualification only: existing Session -> ConPTY Process -> real managed Zsh.
// No runtime/default switch, bridge transport, fabricated VT replies or CPR claim.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "stormtty/session.hpp"
#include "stormtty/windows_shell_state.hpp"
#include "stormtty/windows_managed_shell.hpp"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
namespace fs=std::filesystem;
using namespace stormtty;
using namespace std::chrono_literals;
using Clock=std::chrono::steady_clock;
namespace {
struct Failure { unsigned reason; };
void require(bool value,unsigned reason) {if(!value)throw Failure{reason};}
std::string utf8(const fs::path& path) {
    const auto text=path.generic_u8string();return {reinterpret_cast<const char*>(text.data()),text.size()};
}
struct Budget {
    const Clock::time_point end=Clock::now()+50s;
    void check()const {require(Clock::now()<end,1);}
};
struct Scratch {
    fs::path root,home,cwd;
    Scratch() {
        PWSTR raw{};require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&raw)),10);
        struct Free {PWSTR p;~Free(){CoTaskMemFree(p);}} owned{raw};
        root=fs::path(raw)/(L"StormTTY refined managed \u00e9 \u8def\u5f84 "+std::to_wstring(GetCurrentProcessId())+L" "+std::to_wstring(GetTickCount64()));
        require(fs::create_directory(root),11);home=root/L"home";cwd=root/L"cwd";
        require(fs::create_directory(home)&&fs::create_directory(cwd),12);
    }
    bool remove()noexcept {std::error_code ec;fs::remove_all(root,ec);return !ec;}
    ~Scratch(){remove();}
};
struct OwnedSession {
    std::shared_ptr<Session> value;
    bool close() {
        if(!value)return true;
        value->request_close();const bool closed=value->wait_closed(8s);
        // A stuck cleanup cannot unwind into a second unbounded wait. Own process
        // death closes this process's non-inherited terminal Job handles.
        if(!closed)TerminateProcess(GetCurrentProcess(),125);
        value.reset();return closed;
    }
    ~OwnedSession(){close();}
};
std::string screen(const std::shared_ptr<Session>& session) {
    std::string out;
    if(const auto frame=session->frame())for(const auto& row:frame->lines) {
        for(const auto& cell:row->cells)if(cell.span)out+=cell.grapheme.empty()?" ":cell.grapheme;
        out+='\n';
    }
    return out;
}
void expect(const std::shared_ptr<Session>& session,const std::string& token,const Budget& budget,
            std::chrono::milliseconds timeout=4s) {
    const auto end=Clock::now()+timeout;
    do {
        budget.check();if(screen(session).find(token)!=std::string::npos)return;
        require(!session->process_finished(),3);std::this_thread::sleep_for(5ms);
    }while(Clock::now()<end);
    throw Failure{2};
}
void ready(const std::shared_ptr<Session>& s,unsigned sequence,int status,const Budget& budget,
           std::chrono::milliseconds timeout=4s) {
    expect(s,"__MANAGED_READY:"+std::to_string(sequence)+":"+std::to_string(status)+"__",budget,timeout);
}
void key(const std::shared_ptr<Session>& s,Key value,std::uint16_t modifiers=0) {
    KeyInput input;input.key=value;input.modifiers=modifiers;
    require(s->key(input)==SessionSendResult::accepted,4);
}
void command(const std::shared_ptr<Session>& s,std::string text) {
    require(s->text(std::move(text))==SessionSendResult::accepted,4);key(s,Key::enter);
}
void resize(const std::shared_ptr<Session>& s,unsigned columns,unsigned rows) {
    require(s->request_resize({static_cast<std::uint16_t>(columns),static_cast<std::uint16_t>(rows),8,16})==SessionSendResult::accepted,5);
}
void settle(const Budget& budget) {
    const auto end=Clock::now()+200ms;
    do {budget.check();std::this_thread::sleep_for(5ms);}while(Clock::now()<end);
}
fs::path dump_file(const fs::path& state) {
    fs::path found;
    for(const auto& entry:fs::directory_iterator(state)) {
        const auto name=entry.path().filename().wstring();
        if(name.starts_with(L"zcompdump-")&&!name.ends_with(L".zwc")&&!name.ends_with(L".old")&&!name.ends_with(L".lock")) {
            require(found.empty()&&entry.is_regular_file(),14);found=entry.path();
        }
    }
    require(!found.empty(),14);return found;
}
void write_config(const fs::path& source,const fs::path& destination) {
    require(!fs::exists(destination)&&fs::file_size(source)<=65536,15);
    std::ifstream input(source,std::ios::binary);require(input.good(),15);
    std::string data(65537,'\0');input.read(data.data(),static_cast<std::streamsize>(data.size()));
    require(input.eof()&&input.gcount()<=65536,15);data.resize(static_cast<std::size_t>(input.gcount()));
    // New file inherits the protected native state DACL; don't copy source ACLs.
    std::ofstream output(destination,std::ios::binary);output.write(data.data(),static_cast<std::streamsize>(data.size()));
    output.close();require(output.good(),15);
}
void numeric_failure(const std::shared_ptr<Session>& session,unsigned sample,unsigned phase,unsigned reason) {
    unsigned marker=0,sequence=0,status=0;
    if(session) {
        const auto text=screen(session);const auto start=text.rfind("__MANAGED_READY:");
        if(start!=std::string::npos) {
            const auto* begin=text.data()+start+16;const auto* end=text.data()+text.size();
            const auto first=std::from_chars(begin,end,sequence);
            if(first.ec==std::errc{}&&first.ptr!=end&&*first.ptr==':') {
                const auto last=std::from_chars(first.ptr+1,end,status);
                marker=last.ec==std::errc{}&&end-last.ptr>=2&&last.ptr[0]=='_'&&last.ptr[1]=='_';
            }
        }
    }
    SessionInfo info;if(session)info=session->info();
    std::cout<<"REFINED_MANAGED_SAMPLE sample="<<sample<<" phase="<<phase<<" reason="<<reason
        <<" ready_known="<<marker<<" ready_sequence="<<sequence<<" ready_status="<<status
        <<" received="<<info.bytes_received<<" sent="<<info.bytes_sent
        <<" exit_known="<<info.exit_code.has_value()<<" exit_code="<<info.exit_code.value_or(0)
        <<" session_error="<<!info.error.empty()<<" result=FAIL\n";
}
void sample(const fs::path& runtime,const fs::path& config,const fs::path& native,
            const fs::path& provider,Scratch& scratch,unsigned number,const Budget& budget) {
    unsigned phase=0;OwnedSession owned;
    try {
        budget.check();const auto state=prepare_managed_shell_state_at(scratch.root,false);require(static_cast<bool>(state),13);
        fs::path prior;fs::file_time_type prior_time{};
        if(number==1)write_config(config,state.directory/L"config.zsh");
        else {prior=dump_file(state.directory);prior_time=fs::last_write_time(prior);}
        ProcessOptions process;process.executable=utf8(runtime/L"usr/bin/zsh.exe");
        process.arguments={"-d","-i"};process.working_directory=utf8(scratch.cwd);process.grid={160,40};
        process.provider=provider.empty()?ProcessProvider::windows_inbox_basic:ProcessProvider::windows_packaged_experimental;
        process.provider_directory=utf8(provider);
        process.environment={{"HOME",utf8(scratch.home)},{"ZDOTDIR",utf8(runtime/L"stormtty/zsh")},
            {"STORMTTY_SESSION_ID","refined-managed-synthetic"},{"STORMTTY_EMPTY_HOME",""},
            {"STORMTTY_NATIVE_ZSH_STATE",utf8(state.directory)},{"STORMTTY_REFINED_NATIVE",utf8(native)},
            {"PATH",utf8(runtime/L"usr/bin")},{"TERM","xterm-256color"},{"COLORTERM","truecolor"},
            {"LANG","C.UTF-8"},{"LC_ALL","C.UTF-8"},{"MSYSTEM","MSYS"}};
        for(const char* name:{"MSYS","ENV","BASH_ENV","STORMTTY_MSYS_RESIZE_TRACE_DIR","STORMTTY_SHELL_TOOLS",
            "STORMTTY_FZF","STORMTTY_FZF_COMPLETION","STORMTTY_FZF_KEYS","STORMTTY_ZOXIDE","_ZO_DATA_DIR",
            "STORMTTY_AUTOSUGGESTIONS","STORMTTY_HIGHLIGHTING","STORMTTY_PROMPT","ZSH_AUTOSUGGEST_USE_ASYNC",
            "ZSH_AUTOSUGGEST_STRATEGY","SHELL"})process.environment.push_back({name,std::nullopt});
        process.environment.push_back(managed_shell_console_environment());
        TerminalOptions terminal;terminal.size={160,40,8,16};
        const auto started=Clock::now();owned.value=Session::start(std::move(process),terminal);
        const auto& s=owned.value;ready(s,1,0,budget,20s);phase=1;
        const auto startup_ms=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
        command(s,"bridge_contract");expect(s,"__MANAGED_CONTRACT_OK__",budget);ready(s,2,0,budget);phase=2;
        require(s->paste("bridge_unicode '中🙂 pasted'")==SessionSendResult::accepted,4);key(s,Key::enter);
        expect(s,"__BRIDGE_UNICODE:中🙂 pasted__",budget);ready(s,3,0,budget);phase=3;
        require(s->paste("bridge_length '"+std::string(4096,'x')+"'")==SessionSendResult::accepted,4);key(s,Key::enter);
        expect(s,"__BRIDGE_LENGTH:4096__",budget);ready(s,4,0,budget);phase=4;
        require(s->text("bridge_unicode 'abXc'")==SessionSendResult::accepted,4);resize(s,100,35);
        key(s,Key::left);key(s,Key::left);key(s,Key::backspace);key(s,Key::enter);
        expect(s,"__BRIDGE_UNICODE:abc__",budget);ready(s,5,0,budget);phase=5;
        for(unsigned i=0;i<10;i++)resize(s,90+i,30+i);
        resize(s,132,43);settle(budget);command(s,"bridge_size");expect(s,"__BRIDGE_SIZE:132x43__",budget);ready(s,6,0,budget);phase=6;
        command(s,"bridge_native");expect(s,"__BRIDGE_NATIVE_READY:132:43__",budget);
        resize(s,148,47);settle(budget);command(s,"native-ok");
        expect(s,"__BRIDGE_NATIVE_DONE:148:47__",budget);expect(s,"__BRIDGE_NATIVE_UNICODE:中🙂__",budget);ready(s,7,0,budget);phase=7;
        command(s,"bridge_size");expect(s,"__BRIDGE_SIZE:148x47__",budget);ready(s,8,0,budget);
        command(s,"bridge_wait");expect(s,"__BRIDGE_WAIT__",budget);key(s,Key::c,control);ready(s,9,130,budget);phase=8;
        command(s,"bridge_contract");expect(s,"__MANAGED_CONTRACT_OK__",budget);ready(s,10,0,budget);phase=9;
        command(s,"exit 37");
        const auto exit_deadline=Clock::now()+4s;
        while(!s->process_finished()) {budget.check();require(Clock::now()<exit_deadline,6);std::this_thread::sleep_for(5ms);}
        const auto info=s->info();require(info.exit_code&&*info.exit_code==37&&info.error.empty(),7);
        require(owned.close(),8);phase=10;budget.check();
        require(static_cast<bool>(prepare_managed_shell_state_at(scratch.root,false)),13);
        require(fs::is_regular_file(state.directory/L"history"),16);
        const auto dump=dump_file(state.directory);
        if(number==2)require(dump==prior&&fs::last_write_time(dump)==prior_time,17);
        std::cout<<"REFINED_MANAGED_SAMPLE sample="<<number<<" fresh="<<(number==1)<<" phases="<<phase
            <<" startup_ms="<<startup_ms<<" warm_dump_unchanged="<<(number==2)
            <<" received="<<info.bytes_received<<" sent="<<info.bytes_sent<<" exit_code="<<*info.exit_code<<" result=PASS\n";
    }catch(const Failure& fail){numeric_failure(owned.value,number,phase,fail.reason);owned.close();throw;}
    catch(...){numeric_failure(owned.value,number,phase,99);owned.close();throw;}
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        Budget budget;require(argc==4||argc==5,20);
        // runtime CONFIG NATIVE [provider]; all supplied identities are checked
        // by the outer paired-variant runner, and never copied into a bundle.
        const fs::path runtime=fs::absolute(argv[1]),config=fs::absolute(argv[2]),native=fs::absolute(argv[3]);
        const fs::path provider=argc==5?fs::absolute(argv[4]):fs::path{};
        require(fs::is_regular_file(runtime/L"usr/bin/zsh.exe")&&fs::is_regular_file(runtime/L"stormtty/zsh/.zshrc")&&
                fs::is_regular_file(runtime/L"stormtty/vendor/oh-my-zsh/oh-my-zsh.sh")&&fs::is_regular_file(config)&&fs::is_regular_file(native),21);
        Scratch scratch;for(unsigned i=1;i<=2;i++)sample(runtime,config,native,provider,scratch,i,budget);
        require(scratch.remove(),22);budget.check();
        std::cout<<"REFINED_MANAGED_RESULT fresh=1 warm=1 result=PASS\n";return 0;
    }catch(...){std::cout<<"REFINED_MANAGED_RESULT result=FAIL\n";return 1;}
}
