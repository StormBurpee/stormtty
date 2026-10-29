// Qualification-only: authored bytes, isolated children, no user shell/config/history.
#include "stormtty/session.hpp"
#include "stormtty/qualification_input_evidence.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace stormtty;
using namespace std::chrono_literals;
namespace fs=std::filesystem;
namespace {
constexpr std::string_view payload="PASTE_QUAL_0123456789_[x]_end";
std::string hex(std::string_view value) { constexpr char digits[]="0123456789abcdef"; std::string result; for(unsigned char c:value){result+=digits[c>>4];result+=digits[c&15];}return result; }
std::string utf8(const fs::path& p) {auto u=p.generic_u8string();return {reinterpret_cast<const char*>(u.data()),u.size()};}
void require(bool ok,const char* label) {if(!ok)throw std::runtime_error(label);}
void output(std::string_view value) {DWORD n{};require(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),value.data(),static_cast<DWORD>(value.size()),&n,nullptr)&&n==value.size(),"child-output");}
int child(bool vt) {
    DWORD mode{};const auto in=GetStdHandle(STD_INPUT_HANDLE),out=GetStdHandle(STD_OUTPUT_HANDLE);
    require(GetConsoleMode(in,&mode),"child-input-handle");
    require(SetConsoleMode(in,ENABLE_EXTENDED_FLAGS|(vt?ENABLE_VIRTUAL_TERMINAL_INPUT:0)),"child-input-mode");
    require(GetConsoleMode(out,&mode)&&SetConsoleMode(out,mode|ENABLE_VIRTUAL_TERMINAL_PROCESSING),"child-output-mode");
    output("\x1b[?2004h\x1b[?1004hRAW_READY\r\n");
    std::string bytes;unsigned round{}; ULONGLONG previous{},max_gap{},esc_gap{}; char previous_char{};
    for(;;) {char c{};DWORD n{};require(ReadFile(in,&c,1,&n,nullptr)&&n==1,"child-read");const auto now=static_cast<ULONGLONG>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());if(previous){max_gap=std::max(max_gap,now-previous);if(previous_char=='\x1b')esc_gap=std::max(esc_gap,now-previous);}previous=now;previous_char=c;bytes+=c;require(bytes.size()<=1024,"child-bound");
        if(c=='\r'||c=='\n'){++round;output("INPUT:"+std::to_string(round)+":"+hex(bytes)+":END\r\nGAPS:"+std::to_string(round)+":"+std::to_string(max_gap)+":"+std::to_string(esc_gap)+":END\r\n");bytes.clear();previous=0;max_gap=esc_gap=0;}
    }
}
std::string screen(const std::shared_ptr<Session>& s){std::string text;if(auto f=s->frame())for(const auto& r:f->lines){for(const auto& c:r->cells)if(c.span)text+=c.grapheme.empty()?" ":c.grapheme;text+='\n';}return text;}
template<class F> bool until(F fn,std::chrono::milliseconds timeout=10s){const auto end=std::chrono::steady_clock::now()+timeout;do{if(fn())return true;std::this_thread::sleep_for(5ms);}while(std::chrono::steady_clock::now()<end);return fn();}
struct Fixture {fs::path root;Fixture(){std::array<wchar_t,32768> tmp{};auto n=GetTempPathW(static_cast<DWORD>(tmp.size()),tmp.data());require(n&&n<tmp.size(),"temp");GUID id{};std::array<wchar_t,40> token{};require(SUCCEEDED(CoCreateGuid(&id))&&StringFromGUID2(id,token.data(),static_cast<int>(token.size())),"guid");root=fs::path(tmp.data())/(std::wstring(L"StormTTY.PasteInput.")+token.data());require(fs::create_directory(root),"new-root");}~Fixture(){std::error_code ec;fs::remove_all(root,ec);}};
struct Actor {std::shared_ptr<Session> s;~Actor(){if(s){s->request_close();(void)s->wait_closed(5s);}}};
const char raw_script[]=R"ZSH(emulate -R zsh
unsetopt MULTIBYTE
zmodload zsh/system zsh/datetime || exit 91
stty raw -echo || exit 92
trap 'stty sane' EXIT
printf '\033[?2004h\033[?1004hRAW_READY\r\n'
integer round=0 max_gap=0 esc_gap=0 gap=0
typeset -F 9 previous=0 stamp=0
previous_char=''
bytes=''
while true; do
  sysread -i 0 -s 1 -t 30 ch || exit 93
  stamp=$EPOCHREALTIME
  if (( previous > 0 )); then
    gap=$(( (stamp-previous)*1000000 ))
    (( gap > max_gap )) && max_gap=$gap
    [[ $previous_char == $'\e' ]] && (( gap > esc_gap )) && esc_gap=$gap
  fi
  previous=$stamp; previous_char=$ch
  printf -v one '%02x' "'$ch"
  bytes+=$one
  (( ${#bytes} <= 2048 )) || exit 94
  if [[ $ch == $'\r' || $ch == $'\n' ]]; then
    (( ++round ))
    printf 'INPUT:%d:%s:END\r\n' $round "$bytes"
    printf 'GAPS:%d:%d:%d:END\r\n' $round $max_gap $esc_gap
    bytes=''; previous=0; max_gap=0; esc_gap=0
  fi
done
)ZSH";
std::string without_focus(std::string value){for(const auto* sequence:{"\x1b[I","\x1b[O"}){std::size_t p{};while((p=value.find(sequence,p))!=std::string::npos)value.erase(p,3);}return value;}
std::string unhex(std::string_view h){std::string out;require(h.size()%2==0,"hex-size");auto digit=[](char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("hex-digit");};for(std::size_t i=0;i<h.size();i+=2)out+=char(digit(h[i])*16+digit(h[i+1]));return out;}
bool run_case(const fs::path& self,const fs::path& runtime,const fs::path& provider,unsigned kind,unsigned mode) {
    Fixture fixture;auto evidence=std::make_shared<qualification::InputEvidence>();
    ProcessOptions p;p.grid={180,50};p.working_directory=utf8(fixture.root);p.qualification_input_evidence=evidence;
    p.provider=provider.empty()?ProcessProvider::windows_inbox_basic:ProcessProvider::windows_packaged_experimental;p.provider_directory=utf8(provider);
    if(kind<2){p.executable=utf8(self);p.arguments={"--child",kind?"vt":"plain"};}
    else {const auto script=fixture.root/L"raw.zsh";std::ofstream file(script,std::ios::binary);file<<raw_script;file.close();require(file.good(),"fixture-write");
      p.executable=utf8(runtime/L"usr/bin/zsh.exe");p.arguments={"-df",utf8(script)};
      p.environment={{"HOME",utf8(fixture.root)},{"ZDOTDIR",utf8(fixture.root)},{"PATH",utf8(runtime/L"usr/bin")},{"MSYSTEM","MSYS"},{"TERM","xterm-256color"},{"LC_ALL","C"},{"MSYS",std::nullopt},{"ENV",std::nullopt},{"BASH_ENV",std::nullopt},{"STORMTTY_NATIVE_ZSH_STATE",std::nullopt}};}
    TerminalOptions t;t.size.columns=180;t.size.rows=50;Actor actor{Session::start(p,t)};auto s=actor.s;
    require(until([&]{auto f=s->frame();return f&&f->bracketed_paste&&screen(s).find("RAW_READY")!=std::string::npos;}),"raw-ready");
    std::uint64_t after=evidence->snapshot().completed;const std::string expected="\x1b[200~"+std::string(payload)+"\x1b[201~\r";
    bool ok=true; unsigned completed{};
    for(unsigned round=1;round<=20;++round){
      if(mode&1)for(unsigned i=0;i<35;++i)require(s->focus(i%2==0)==SessionSendResult::accepted,"focus-enqueue");
      if(mode&2)for(unsigned i=0;i<4;++i){TerminalSize size=t.size;size.columns=static_cast<std::uint16_t>(i==3?(round%2?170:180):(160+(i%2)*10));require(s->request_resize(size)==SessionSendResult::accepted,"resize-enqueue");}
      require(s->paste(std::string(payload))==SessionSendResult::accepted,"paste-enqueue");KeyInput enter;enter.key=Key::enter;require(s->key(enter)==SessionSendResult::accepted,"enter-enqueue");
      const auto marker="INPUT:"+std::to_string(round)+":";std::string received;
      const bool consumed=until([&]{auto text=screen(s);text.erase(std::remove(text.begin(),text.end(),'\n'),text.end());text.erase(std::remove(text.begin(),text.end(),' '),text.end());const auto pos=text.rfind(marker);if(pos==std::string::npos)return false;const auto end=text.find(":END",pos);if(end==std::string::npos)return false;received=text.substr(pos+marker.size(),end-pos-marker.size());received.erase(std::remove(received.begin(),received.end(),'\n'),received.end());received.erase(std::remove(received.begin(),received.end(),' '),received.end());return true;});
      const auto gap_marker="GAPS:"+std::to_string(round)+":";
      std::string gaps;
      const bool gap_report=consumed&&until([&]{auto text=screen(s);const auto pos=text.rfind(gap_marker);if(pos==std::string::npos)return false;const auto end=text.find(":END",pos);if(end==std::string::npos)return false;gaps=text.substr(pos+gap_marker.size(),end-pos-gap_marker.size());return true;});
      require(gaps.size()<40 && gaps.find_first_not_of("0123456789:")==std::string::npos,"gap-numeric");
      std::cout<<"PASTE_GAPS kind="<<kind<<" mode="<<mode<<" round="<<round<<" max_and_after_escape_us="<<gaps<<'\n';
      const auto transport=s->diagnostics();
      if (transport.windows_control) std::cout<<"PASTE_RESIZE kind="<<kind<<" mode="<<mode<<" round="<<round<<" requested="<<transport.windows_control->requested_resize_generation<<" applied="<<transport.windows_control->applied_resize_generation<<" calls="<<transport.windows_control->resize_calls<<'\n';
      qualification::InputEvidence::Snapshot snap;std::string emitted;bool complete{};
      // The child can reply before the writer's post-WriteFile callback publishes.
      // Observe that callback for at most 250ms; never resend input or retry a case.
      const bool published=until([&]{
        snap=evidence->snapshot(after);emitted.clear();complete=!snap.missing_since_after;
        for(const auto& r:snap.records){complete&=r.success&&r.accepted==r.requested&&r.stored>=r.accepted;emitted.append(reinterpret_cast<const char*>(r.bytes.data()),std::min<std::size_t>(r.stored,r.accepted));}
        return without_focus(emitted).size()>=expected.size();
      },250ms);
      after=snap.completed;
      const bool equal=consumed&&without_focus(unhex(received))==expected;
      if(!consumed||!gap_report||!published||!complete||without_focus(emitted)!=expected||!equal){
        std::cout<<"PASTE_MISMATCH kind="<<kind<<" mode="<<mode<<" round="<<round<<" consumed="<<consumed<<" complete="<<complete<<" writer_published="<<published<<" expected_hex="<<hex(expected)<<" child_hex="<<received<<'\n';
        qualification::InputEvidence::dump(std::cout,snap,"authored-paste-only");ok=false;break;
      }
      ++completed;
    }
    std::cout<<"PASTE_CASE kind="<<kind<<" mode="<<mode<<" planned=20 completed="<<completed<<" code="<<(ok?0:1)<<'\n';return ok;
}
}
int main(){try{int n{};auto raw=CommandLineToArgvW(GetCommandLineW(),&n);require(raw&&n>=2,"args");struct Args{LPWSTR* p;~Args(){LocalFree(p);}} args{raw};if(std::wstring_view(raw[1])==L"--child")return child(n>2&&std::wstring_view(raw[2])==L"vt");
 require(n==2||n==3,"runtime-provider-args");const fs::path runtime=raw[1],provider=n==3?fs::path(raw[2]):fs::path{};std::array<wchar_t,32768> exe{};require(GetModuleFileNameW(nullptr,exe.data(),static_cast<DWORD>(exe.size()))!=0,"self");bool ok=true;
 unsigned control_failures{},vt_failures{};
 for(unsigned kind=0;kind<3;++kind)for(unsigned mode=0;mode<4;++mode){
   bool passed=false;
   std::cout<<"PASTE_CASE_BEGIN kind="<<kind<<" mode="<<mode<<" role="<<(kind==0?"vt-off-control":"raw-vt-reader")<<'\n';
   try {passed=run_case(exe.data(),runtime,provider,kind,mode);}
   catch(const std::exception& e){std::cout<<"PASTE_CASE_EXCEPTION kind="<<kind<<" mode="<<mode<<" code=2 stage="<<e.what()<<'\n';}
   catch(...){std::cout<<"PASTE_CASE_EXCEPTION kind="<<kind<<" mode="<<mode<<" code=2 stage=unknown\n";}
   if(!passed){ok=false;if(kind==0)++control_failures;else ++vt_failures;}
 }
 std::cout<<"PASTE_RESULT code="<<(ok?0:1)<<" provider="<<(provider.empty()?"inbox":"packaged")<<" vt_off_control_failures="<<control_failures<<" raw_vt_reader_failures="<<vt_failures<<" production_cause_established=0\n";return ok?0:1;
}catch(const std::exception& e){std::cout<<"PASTE_RESULT code=2 stage="<<e.what()<<'\n';return 2;}}
