// Stock-only readiness diagnostic. Derived from unchanged PR7 raw probe.
// No runtime patch, production linkage, real user state, or causal claim.
#include "stormtty/session.hpp"
#include "stormtty/qualification_input_evidence.hpp"
#include "evidence-contract.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
using namespace stormtty;
using namespace std::chrono_literals;
namespace fs=std::filesystem;
namespace {
constexpr std::string_view payload="PASTE_QUAL_0123456789_[x]_end";
std::string hex(std::string_view value) { constexpr char digits[]="0123456789abcdef"; std::string result; for(unsigned char c:value){result+=digits[c>>4];result+=digits[c&15];}return result; }
std::string utf8(const fs::path& p) {auto u=p.generic_u8string();return {reinterpret_cast<const char*>(u.data()),u.size()};}
void require(bool ok,const char* label) {if(!ok)throw std::runtime_error(label);}
std::string screen(const std::shared_ptr<Session>& s){std::string text;if(auto f=s->frame())for(const auto& r:f->lines){for(const auto& c:r->cells)if(c.span)text+=c.grapheme.empty()?" ":c.grapheme;text+='\n';}return text;}
template<class F> bool until(F fn,std::chrono::milliseconds timeout=10s){const auto end=std::chrono::steady_clock::now()+timeout;do{if(fn())return true;std::this_thread::sleep_for(5ms);}while(std::chrono::steady_clock::now()<end);return fn();}
struct Fixture {fs::path root;Fixture(){std::array<wchar_t,32768> tmp{};auto n=GetTempPathW(static_cast<DWORD>(tmp.size()),tmp.data());require(n&&n<tmp.size(),"temp");GUID id{};std::array<wchar_t,40> token{};require(SUCCEEDED(CoCreateGuid(&id))&&StringFromGUID2(id,token.data(),static_cast<int>(token.size())),"guid");root=fs::path(tmp.data())/(std::wstring(L"StormTTY.Readiness.")+token.data());require(fs::create_directory(root),"new-root");}
    bool close() noexcept {if(root.empty())return true;std::error_code ec;fs::remove_all(root,ec);const bool ok=!ec;
        std::cout<<"READINESS_SCRATCH removed="<<ok<<"\n";if(ok)root.clear();return ok;}
    ~Fixture(){close();}};
struct Actor {
    std::shared_ptr<Session> s;
    bool close() noexcept {
        if(!s)return true;
        s->request_close();const bool closed=s->wait_closed(5s);
        const auto info=s->info();
        std::cout<<"READINESS_CLEANUP closed="<<closed<<" exit_present="<<info.exit_code.has_value()
                 <<" exit_code="<<info.exit_code.value_or(0)<<" error_present="<<!info.error.empty()
                 <<" requested_close=1\n"<<std::flush;
        // Bound teardown before Session destruction, as in the managed fixture.
        if(!closed)TerminateProcess(GetCurrentProcess(),125);
        s.reset();return closed&&info.error.empty();
    }
    ~Actor(){close();}
};
const char raw_script[]=R"ZSH(emulate -R zsh
unsetopt MULTIBYTE
zmodload zsh/system zsh/datetime || exit 91
stty raw -echo || exit 92
trap 'stty sane' EXIT
printf '\033[?2004h\033[?1004hRAW_READY\r\n'
integer round=0 max_gap=0 esc_gap=0 gap=0
typeset -F 9 previous=0 stamp=0
previous_char=''
bytes=''; statuses=''
integer read_count=-999 read_status=0
while true; do
  read_count=-999
  @SYSREAD@
  read_status=$?
  statuses+=${read_status},${read_count}';'
  if (( read_status != 0 || read_count != 1 )); then
    printf 'INPUT:%d:%s:END\r\nSTATUS:%d:%s:END\r\nREADINESS_READER_ERROR:%d:%d:END\r\n' $((round+1)) "$bytes" $((round+1)) "$statuses" $read_status $read_count
    exit 93
  fi
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
    printf 'STATUS:%d:%s:END\r\n' $round "$statuses"
    statuses=''; bytes=''; previous=0; max_gap=0; esc_gap=0
  fi
done
)ZSH";
using readiness::cursor_reply;
std::string unhex(std::string_view h){std::string out;require(h.size()%2==0,"hex-size");auto digit=[](char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("hex-digit");};for(std::size_t i=0;i<h.size();i+=2)out+=char(digit(h[i])*16+digit(h[i+1]));return out;}
bool run_case(const fs::path& runtime,bool timed,unsigned mode) {
    constexpr unsigned kind=2;
    Fixture fixture;auto evidence=std::make_shared<qualification::InputEvidence>();
    ProcessOptions p;p.grid={180,50};p.working_directory=utf8(fixture.root);p.qualification_input_evidence=evidence;
    p.provider=ProcessProvider::windows_inbox_basic;
    {const auto script=fixture.root/L"raw.zsh";
      std::string reader=raw_script;const auto token=reader.find("@SYSREAD@");require(token!=std::string::npos,"reader-template");
      reader.replace(token,9,timed?"sysread -i 0 -s 1 -t 30 -c read_count ch":"sysread -i 0 -s 1 -c read_count ch");
      std::ofstream file(script,std::ios::binary);file<<reader;file.close();require(file.good(),"fixture-write");
      p.executable=utf8(runtime/L"usr/bin/zsh.exe");p.arguments={"-df",utf8(script)};
      p.environment={{"HOME",utf8(fixture.root)},{"ZDOTDIR",utf8(fixture.root)},{"PATH",utf8(runtime/L"usr/bin")},{"MSYSTEM","MSYS"},{"TERM","xterm-256color"},{"LC_ALL","C"},{"MSYS",std::nullopt},{"ENV",std::nullopt},{"BASH_ENV",std::nullopt},{"STORMTTY_NATIVE_ZSH_STATE",std::nullopt}};}
    TerminalOptions t;t.size.columns=180;t.size.rows=50;Actor actor{Session::start(p,t)};auto s=actor.s;
    require(until([&]{auto f=s->frame();return f&&f->bracketed_paste&&screen(s).find("RAW_READY")!=std::string::npos;}),"raw-ready");
    std::uint64_t after=evidence->snapshot().completed;const std::string expected="\x1b[200~"+std::string(payload)+"\x1b[201~\r";
    bool ok=true; unsigned completed{};
    try {
    for(unsigned round=1;round<=20;++round){
      if(mode&2)for(unsigned i=0;i<4;++i){TerminalSize size=t.size;size.columns=static_cast<std::uint16_t>(i==3?(round%2?170:180):(160+(i%2)*10));require(s->request_resize(size)==SessionSendResult::accepted,"resize-enqueue");}
      require(s->paste(std::string(payload))==SessionSendResult::accepted,"paste-enqueue");KeyInput enter;enter.key=Key::enter;require(s->key(enter)==SessionSendResult::accepted,"enter-enqueue");
      const auto marker="INPUT:"+std::to_string(round)+":";std::string received;
      const bool consumed=until([&]{auto text=screen(s);text.erase(std::remove(text.begin(),text.end(),'\n'),text.end());text.erase(std::remove(text.begin(),text.end(),' '),text.end());const auto pos=text.rfind(marker);if(pos==std::string::npos)return false;const auto end=text.find(":END",pos);if(end==std::string::npos)return false;received=text.substr(pos+marker.size(),end-pos-marker.size());received.erase(std::remove(received.begin(),received.end(),'\n'),received.end());received.erase(std::remove(received.begin(),received.end(),' '),received.end());return true;});
      const auto gap_marker="GAPS:"+std::to_string(round)+":";
      std::string gaps;
      const bool gap_report=consumed&&until([&]{auto text=screen(s);const auto pos=text.rfind(gap_marker);if(pos==std::string::npos)return false;const auto end=text.find(":END",pos);if(end==std::string::npos)return false;gaps=text.substr(pos+gap_marker.size(),end-pos-gap_marker.size());return true;});
      require(gaps.size()<40 && gaps.find_first_not_of("0123456789:")==std::string::npos,"gap-numeric");
      std::string statuses;const auto status_marker="STATUS:"+std::to_string(round)+":";
      const bool status_report=consumed&&until([&]{auto text=screen(s);text.erase(std::remove(text.begin(),text.end(),'\n'),text.end());text.erase(std::remove(text.begin(),text.end(),' '),text.end());const auto pos=text.rfind(status_marker);if(pos==std::string::npos)return false;const auto end=text.find(":END",pos);if(end==std::string::npos)return false;statuses=text.substr(pos+status_marker.size(),end-pos-status_marker.size());return true;});
      require(statuses.size()<=8192&&statuses.find_first_not_of("0123456789;,-")==std::string::npos,"reader-status-numeric");
      qualification::InputEvidence::Snapshot snap;std::string emitted;bool complete{};unsigned cursor_replies{};
      // The child can reply before the writer's post-WriteFile callback publishes.
      // Observe that callback for at most 250ms; never resend input or retry a case.
      const bool published=until([&]{
        snap=evidence->snapshot(after);emitted.clear();cursor_replies=0;complete=!snap.missing_since_after;
        for(const auto& r:snap.records){complete&=r.success&&r.accepted==r.requested&&r.stored>=r.accepted;const std::string_view bytes(reinterpret_cast<const char*>(r.bytes.data()),std::min<std::size_t>(r.stored,r.accepted));
          if(cursor_reply(bytes))++cursor_replies;else emitted.append(bytes);}
        return emitted.size()>=expected.size();
      },250ms);
      after=snap.completed;
      if(cursor_replies) {
        std::cout<<"PASTE_PROTOCOL kind="<<kind<<" mode="<<mode<<" round="<<round<<" standalone_cursor_replies="<<cursor_replies<<'\n';
        for(const auto& r:snap.records){const std::string_view bytes(reinterpret_cast<const char*>(r.bytes.data()),std::min<std::size_t>(r.stored,r.accepted));if(cursor_reply(bytes))std::cout<<"PASTE_CPR_WRITE sequence="<<r.sequence<<" accepted_hex="<<hex(bytes)<<'\n';}
      }
      const bool equal=consumed&&unhex(received)==expected;
      std::string expected_status;
      if(consumed)for(std::size_t i=0;i<unhex(received).size();++i)expected_status+="0,1;";
      const bool statuses_ok=status_report&&statuses==expected_status;
      const bool write_shape_ok=readiness::intact_authored_writes(snap,expected);
      std::cout<<"READINESS_ROUND round="<<round<<" timed="<<timed<<" mode="<<mode
          <<" consumed="<<consumed<<" writer_published="<<published<<" complete="<<complete
          <<" equal="<<equal<<" statuses_ok="<<statuses_ok<<" write_shape_ok="<<write_shape_ok<<" expected_hex="<<hex(expected)
          <<" child_hex="<<received<<" sysread_status_count="<<statuses
          <<" retried_eintr=unobserved sigwinch_count=unobserved\n";
      std::cout<<"PASTE_GAPS kind="<<kind<<" mode="<<mode<<" round="<<round<<" max_and_after_escape_us="<<gaps<<'\n';
      const auto transport=s->diagnostics();
      if (transport.windows_control) std::cout<<"PASTE_RESIZE kind="<<kind<<" mode="<<mode<<" round="<<round<<" requested="<<transport.windows_control->requested_resize_generation<<" applied="<<transport.windows_control->applied_resize_generation<<" calls="<<transport.windows_control->resize_calls<<" failures="<<transport.windows_control->resize_failures<<'\n';
      qualification::InputEvidence::dump(std::cout,snap,"authored-paste-only");
      if(!consumed||!gap_report||!published||!complete||emitted!=expected||!equal||!statuses_ok||!write_shape_ok){
        std::cout<<"PASTE_MISMATCH kind="<<kind<<" mode="<<mode<<" round="<<round<<" consumed="<<consumed<<" complete="<<complete<<" writer_published="<<published<<" expected_hex="<<hex(expected)<<" child_hex="<<received<<'\n';
        std::cout<<"READINESS_PARTIAL_SCREEN_BEGIN\n"<<screen(s)<<"READINESS_PARTIAL_SCREEN_END\n";
        ok=false;break;
      }
      ++completed;
    }
    }catch(...) {
      // Only synthetic fixture output is read; retain partial/error status before teardown.
      std::cout<<"READINESS_PARTIAL_SCREEN_BEGIN\n"<<screen(s)<<"READINESS_PARTIAL_SCREEN_END\n";
      qualification::InputEvidence::dump(std::cout,evidence->snapshot(after),"failure-tail");
      actor.close();throw;
    }
    const bool closed=actor.close();const bool removed=fixture.close();ok=ok&&closed&&removed;
    std::cout<<"PASTE_CASE kind="<<kind<<" mode="<<mode<<" planned=20 completed="<<completed<<" code="<<(ok?0:1)<<'\n';return ok;
}
}
int main(){try{
 int n{};auto raw=CommandLineToArgvW(GetCommandLineW(),&n);require(raw&&n==4,"args");
 struct Args{LPWSTR* p;~Args(){LocalFree(p);}} args{raw};
 const fs::path runtime=raw[1];const std::wstring_view reader=raw[2],control=raw[3];
 require(reader==L"timed"||reader==L"direct","reader-mode");
 require(control==L"plain"||control==L"resize","resize-mode");
 const bool timed=reader==L"timed";const unsigned mode=control==L"resize"?2:0;
 const bool passed=run_case(runtime,timed,mode);
 std::cout<<"READINESS_RESULT code="<<(passed?0:1)<<" reader="<<(timed?"timed":"direct")
          <<" control="<<(mode?"resize":"plain")<<" provider=inbox runtime=official production_cause_established=0\n";
 return passed?0:1;
}catch(const std::exception& e){std::cout<<"READINESS_EXCEPTION stage="<<e.what()<<"\n";return 2;}
 catch(...){std::cout<<"READINESS_EXCEPTION stage=unknown\n";return 2;}}
