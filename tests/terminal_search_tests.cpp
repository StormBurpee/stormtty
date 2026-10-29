#include "stormtty/smoke_commands.hpp"
#include "stormtty/session.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
using namespace stormtty;
using namespace std::chrono_literals;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
TerminalOptions make_search_terminal_options(unsigned columns = 40, unsigned rows = 4) {
    TerminalOptions options; options.size = {static_cast<std::uint16_t>(columns), static_cast<std::uint16_t>(rows),8,16};
    return options;
}
SearchSnapshotPtr finish(TerminalCore& core) {
    unsigned steps{};
    while (core.search_needs_work()) {
        const auto before = core.search_snapshot()->work_units;
        const auto value = core.search_step();
        check(value->work_units >= before, "work counter went backwards");
        check(++steps < 10000, "search failed to reach a terminal budget/progress state");
    }
    return core.search_snapshot();
}
std::string frame_text(const FramePtr& frame) {
    std::string text;
    if (frame) for (const auto& row : frame->lines) {
        for (const auto& cell : row->cells) if (cell.span) text += cell.grapheme.empty() ? " " : cell.grapheme;
        text += '\n';
    }
    return text;
}
#ifdef _WIN32
int native_context_child(unsigned index) {
    const auto old_cp=GetConsoleOutputCP();
    struct RestoreCodePage {UINT value;~RestoreCodePage(){SetConsoleOutputCP(value);}} restore_cp{old_cp};
    if(!SetConsoleOutputCP(CP_UTF8))return 29;
    const HANDLE input=GetStdHandle(STD_INPUT_HANDLE),output=GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD old_input{},old_output{};
    if(!GetConsoleMode(input,&old_input)||!GetConsoleMode(output,&old_output))return 30;
    struct Restore {HANDLE handle;DWORD mode;~Restore(){SetConsoleMode(handle,mode);}} ri{input,old_input},ro{output,old_output};
    if(!SetConsoleMode(input,ENABLE_VIRTUAL_TERMINAL_INPUT|ENABLE_EXTENDED_FLAGS)||!SetConsoleMode(output,old_output|ENABLE_VIRTUAL_TERMINAL_PROCESSING))return 31;
    const auto emit=[&](std::string_view value){DWORD written{};return WriteFile(output,value.data(),static_cast<DWORD>(value.size()),&written,nullptr)&&written==value.size();};
    std::wstring cwd(32768,L'\0');const auto cwd_length=GetCurrentDirectoryW(static_cast<DWORD>(cwd.size()),cwd.data());
    if(!cwd_length||cwd_length>=cwd.size())return 35;
    cwd.resize(cwd_length);const auto size=WideCharToMultiByte(CP_UTF8,0,cwd.data(),static_cast<int>(cwd.size()),nullptr,0,nullptr,nullptr);
    if(size<=0)return 36;
    std::string actual_directory(static_cast<std::size_t>(size),'\0');
    WideCharToMultiByte(CP_UTF8,0,cwd.data(),static_cast<int>(cwd.size()),actual_directory.data(),size,nullptr,nullptr);
    const auto suffix=std::to_string(index);
    const auto directory=index==2?"C:/work/\xe6\x97\xa5\xe6\x9c\xac/"+std::string(180,'x')+"/shared": "C:/work/project-"+suffix+"/shared";
    const auto key="file:///C:/context-fixture-"+suffix;
    const auto title=index==0?"cmd.exe":index==1?"Review · café":"Build · 日本語";
    const auto packet="\x1b]2;"+std::string(title)+"\x07\x1b]7;"+key+"\x07\x1b]7400;stormtty-context;1;set;"+key+";"+directory+";C:/work/project-"+suffix+";"+(index==1?"detached;123456789abcdef":"branch;feature/context-"+suffix)+"\x07\r\nSTORMTTY_CONTEXT_READY_"+suffix+"\r\n";
    if(!emit(packet))return 32;
    unsigned bytes{};char value{};DWORD read{};
    while(bytes++<4096&&ReadFile(input,&value,1,&read,nullptr)&&read==1) {
        if(value=='q')return emit("\x1b]9;9;"+actual_directory+"\x07\x1b]7400;stormtty-context;1;clear\x07\r\nSTORMTTY_CONTEXT_DONE\r\n")?0:33;
    }
    return 34;
}
int native_mouse_child() {
    const HANDLE input=GetStdHandle(STD_INPUT_HANDLE),output=GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD old_input{},old_output{};
    if(!GetConsoleMode(input,&old_input)||!GetConsoleMode(output,&old_output))return 20;
    struct Restore {HANDLE handle;DWORD mode;~Restore(){SetConsoleMode(handle,mode);}} restore_input{input,old_input},restore_output{output,old_output};
    if(!SetConsoleMode(input,ENABLE_VIRTUAL_TERMINAL_INPUT|ENABLE_EXTENDED_FLAGS)||
       !SetConsoleMode(output,old_output|ENABLE_VIRTUAL_TERMINAL_PROCESSING))return 21;
    const auto emit=[&](std::string_view value){DWORD written{};return WriteFile(output,value.data(),static_cast<DWORD>(value.size()),&written,nullptr)&&written==value.size();};
    if(!emit("\x1b[?1003h\x1b[?1006h\r\nSTORMTTY_MOUSE_READY\r\n"))return 22;
    // Host owns this child and its deadline. Only the explicit sentinel exits;
    // captured mouse release reports are consumed without echo or shell execution.
    unsigned bytes{};char value{};DWORD read{};
    while(bytes++<4096&&ReadFile(input,&value,1,&read,nullptr)&&read==1) {
        if(value=='q')return emit("\x1b[?1003l\x1b[?1006l\r\nSTORMTTY_MOUSE_DONE\r\n")?0:23;
    }
    return 24;
}
#endif
void safe_native_fixture() {
    TerminalCore core(make_search_terminal_options());
    for(bool managed : {false,true}) {
        const auto command=native_find_fixture_command(managed);
        check(TerminalCore::paste_is_safe(command),"Find fixture must use ordinary safe paste");
        check(core.encode_paste(command)==command,"Find fixture encoding changed printable command");
        check(!native_find_fixture_output(command),"command echo must not satisfy output gate");
    }
    const auto mouse_command=native_mouse_child_command("C:/fixture with spaces/terminal_search_tests.exe");
    check(!mouse_command.empty()&&TerminalCore::paste_is_safe(mouse_command),"native mouse child command must be paste-safe");
    check(core.encode_paste(mouse_command)==mouse_command,"native mouse child command encoding changed");
    check(native_mouse_child_command("bad\npath").empty()&&native_mouse_child_command("bad\"path").empty(),"unsafe child path accepted");
    const std::string unsafe="printf 'alpha\nbeta\n'";
    check(!TerminalCore::paste_is_safe(unsafe),"multiline paste must remain unsafe");
    bool rejected=false;try{core.encode_paste(unsafe);}catch(const std::invalid_argument&){rejected=true;}
    check(rejected,"ordinary paste encoding must reject multiline fixture");
    check(native_find_fixture_output("STORMTTY_FIND_NEEDLE beta   "),"actual output row not recognized");
    check(!native_find_fixture_output("prompt STORMTTY_FIND_NEEDLE beta"),"prompt echo accepted as output");
}
void visible_selection_publication() {
    TerminalCore t(make_search_terminal_options(100,30));
    t.feed("prompt>echo NEEDLE alpha & echo NEEDLE beta\r\nNEEDLE alpha\r\nNEEDLE beta\r\nprompt>");
    (void)t.snapshot();
    t.clear_selection();t.begin_search(1,{"NEEDLE"});auto result=finish(t);
    check(result->matches.size()==4,"visible search fixture count");
    check(t.select_search_match(result->token,0)==SearchApplyResult::applied,"newest visible search selection rejected");
    auto frame=t.snapshot();bool selected=false;
    for(const auto& row:frame->lines)for(const auto& cell:row->cells)selected=selected||cell.selected;
    check(selected,"accepted newest search selection missing from immutable frame");
    t.clear_selection();t.begin_search(2,{"ABSENT"});result=finish(t);frame=t.snapshot();
    for(const auto& row:frame->lines)for(const auto& cell:row->cells)check(!cell.selected,"cleared search selection remained in immutable frame");
}
void real_history_navigation() {
    TerminalCore t(make_search_terminal_options());
    t.feed("old NEEDLE\r\n");
    for (unsigned i=0;i<200;++i) t.feed("padding " + std::to_string(i) + "\r\n");
    t.feed("recent needle\r\n");
    check(frame_text(t.snapshot()).find("old NEEDLE") == std::string::npos, "history fixture remained visible");
    t.begin_search(1, {"needle"}); const auto result = finish(t);
    check(result->status == SearchStatus::complete && result->matches.size() == 2, "scrollback search did not find both hits");
    check(result->matches[0].start.row > result->matches[1].start.row, "matches not newest first");
    check(t.select_search_match(result->token,1) == SearchApplyResult::applied, "old history result not selectable");
    check(t.selection_text() == "NEEDLE", "history selection did not use actual match");
    check(frame_text(t.snapshot()).find("old NEEDLE") != std::string::npos, "find did not reveal history");
    check(t.navigate_search(result->token,SearchDirection::backward,false) == SearchApplyResult::boundary, "nonwrapping boundary ignored");
    check(t.navigate_search(result->token,SearchDirection::backward,true) == SearchApplyResult::applied, "backward wrap failed");
    check(t.selection_text() == "needle", "wrap did not reach newest hit");
    check(t.navigate_search(result->token,SearchDirection::forward,false) == SearchApplyResult::boundary, "forward boundary ignored");
    check(t.navigate_search(result->token,SearchDirection::forward,true) == SearchApplyResult::applied, "forward wrap failed");
    check(t.selection_text() == "NEEDLE", "forward wrap did not reach oldest hit");
}
void utf8_and_literal_semantics() {
    TerminalCore t(make_search_terminal_options(8,6));
    const std::string grapheme = "\xE7\x95\x8C" "e\xCC\x81" "o";
    t.feed("alpha" + grapheme + "mega\r\n\xC3\x89 \xC3\xA9\r\nERROR error .");
    t.begin_search(1,{grapheme}); auto result=finish(t);
    check(result->status==SearchStatus::complete && result->matches.size()==1,"wrapped UTF-8 literal not found");
    check(result->matches[0].end.row > result->matches[0].start.row,"soft-wrapped match did not cross rows");
    check(t.select_search_match(result->token,0)==SearchApplyResult::applied,"Unicode match selection failed");
    check(t.selection_text()==grapheme,"grapheme match formatting corrupted");
    t.begin_search(2,{"\xC3\xA9"}); result=finish(t);
    check(result->matches.size()==1,"unexpected Unicode case folding");
    t.begin_search(3,{"error"}); result=finish(t);
    check(result->matches.size()==2 && SearchSnapshot::ascii_case_insensitive,"ASCII matching contract incorrect");
    t.begin_search(4,{"."}); check(finish(t)->matches.size()==1,"literal dot treated as regex");
}
void alternate_isolation_and_owned_results() {
    SearchSnapshotPtr primary;
    {
        TerminalCore t(make_search_terminal_options()); t.feed("PRIMARY needle"); t.begin_search(1,{"needle"}); primary=finish(t);
        check(primary->matches.size()==1 && !primary->alternate_screen,"primary search mismatch");
        t.feed("\x1b[?1049h\x1b[HALT needle");
        check(t.select_search_match(primary->token,0)==SearchApplyResult::stale,"primary token accepted in alternate screen");
        auto alternate=finish(t);
        check(alternate->matches.size()==1 && alternate->alternate_screen,"alternate isolation failed");
        check(t.select_search_match(alternate->token,0)==SearchApplyResult::applied,"alternate hit not selectable");
        check(t.selection_text()=="needle","alternate match selected wrong bytes");
        t.feed("\x1b[?1049l"); const auto restored=finish(t);
        check(restored->matches.size()==1 && !restored->alternate_screen,"primary search not restored");
        check(t.select_search_match(alternate->token,0)==SearchApplyResult::stale,"alternate token accepted after leaving screen");
    }
    check(primary->matches.size()==1 && primary->matches[0].start.column==8,"owned results invalid after core destruction");
}
void revision_resize_eviction_and_reset() {
    auto options=make_search_terminal_options(80,4); options.scrollback_lines=10;
    TerminalCore t(options); t.feed("before needle\r\n"); t.begin_search(1,{"needle"}); auto original=finish(t);
    const auto revision=original->token.revision;
    t.resize({20,4,8,16});
    check(t.select_search_match(original->token,0)==SearchApplyResult::stale,"resize accepted stale match");
    auto resized=finish(t); check(resized->token.revision>revision && resized->matches.size()==1,"resize did not refresh search");
    std::string output; for(unsigned i=0;i<30000;++i)output+="other " + std::to_string(i) + "\r\n";
    t.feed(output); check(t.select_search_match(resized->token,0)==SearchApplyResult::stale,"eviction accepted stale match");
    const auto evicted=finish(t); check(evicted->status==SearchStatus::complete && evicted->matches.empty(),"evicted result survived");
    t.feed("needle"); check(finish(t)->matches.size()==1,"new live result missing");
    t.feed("\x1b" "c"); check(finish(t)->matches.empty(),"RIS preserved stale search results");
}
void budgets_errors_and_cancellation() {
    TerminalCore t(make_search_terminal_options()); t.feed("word word word word");
    SearchOptions capped{"word"}; capped.max_matches=2;
    t.begin_search(1,capped); auto result=finish(t);
    check(result->status==SearchStatus::limited && result->matches.empty() && result->matches_found>2,"result cap reported false completion");
    SearchOptions work{"word"}; work.max_total_work_units=1; work.max_work_units_per_step=1;
    t.begin_search(2,work); result=finish(t);
    check(result->status==SearchStatus::limited && result->work_units==1,"work limit not enforced");
    t.begin_search(3,{std::string("\xc0\xaf",2)}); check(t.search_snapshot()->status==SearchStatus::error,"invalid UTF-8 not rejected");
    t.begin_search(4,{std::string(4097,'x')}); check(t.search_snapshot()->status==SearchStatus::error && t.search_snapshot()->query.empty(),"oversized query copied into result");
    t.begin_search(5,{"word"}); t.cancel_search(6);
    check(t.search_snapshot()->status==SearchStatus::cancelled && !t.search_needs_work(),"cancel did not release active search");
    t.begin_search(7,{"other"}); t.begin_search(8,{"word"}); result=finish(t);
    check(result->token.request_id==8 && result->matches.size()==4,"replacement query reused old results");
    const auto old_token=result->token;
    t.begin_search(8,{"other"}); result=finish(t);
    check(t.select_search_match(old_token,0)==SearchApplyResult::stale,"reused request ID accepted a previous query token");
    t.begin_search(9,{}); check(t.search_snapshot()->status==SearchStatus::idle && !t.search_needs_work(),"empty query not idle");
    TerminalCore large(make_search_terminal_options(400,400)); large.feed(std::string(160000,'x') + "needle");
    SearchOptions memory{"needle"}; memory.memory_budget_bytes=64*1024;
    large.begin_search(10,memory); result=finish(large);
    check(result->status==SearchStatus::limited && result->message.find("memory")!=std::string::npos,"allocator denial became a fake complete search");
    large.begin_search(11,{"needle"}); result=finish(large);
    check(result->status==SearchStatus::complete && result->matches.size()==1,"search budget exhaustion poisoned the terminal or allocator");
}
void bounded_quanta_and_stale_navigation() {
    TerminalCore t(make_search_terminal_options()); std::string data;
    for(unsigned i=0;i<4000;++i)data+="line " + std::to_string(i) + (i%25==0 ? " needle\r\n":" padding\r\n");
    t.feed(data); SearchOptions options{"needle"}; options.max_work_units_per_step=1;
    t.begin_search(1,options); unsigned steps{};
    while(t.search_needs_work()) {
        const auto before=t.search_snapshot()->work_units; const auto result=t.search_step();
        check(result->work_units-before<=1,"search quantum exceeded configured work budget");
        check(++steps<10000,"bounded search stalled");
    }
    auto result=t.search_snapshot(); check(result->status==SearchStatus::complete && result->matches.size()==160,"bounded history search lost matches");
    check(steps>1,"history scan was not incremental");
    const auto stable=result; t.feed(" output");
    check(t.navigate_search(stable->token,SearchDirection::backward)==SearchApplyResult::stale,"streaming output accepted stale token");
    result=finish(t); check(result->token.revision!=stable->token.revision,"streaming revision did not advance");
    check(stable->matches.size()==160,"old snapshot mutated");
    check(!t.search_needs_work() && t.search_step()==result,"completed search requires idle polling");
}
void mutation_during_materialization() {
    TerminalCore t(make_search_terminal_options(40,200));
    for (unsigned i=0;i<150;++i) t.feed("needle\r\n");
    SearchOptions options{"needle"}; options.max_work_units_per_step=1;
    t.begin_search(1,options);
    unsigned steps{};
    while (t.search_snapshot()->matches_found != 150) {
        t.search_step(); check(++steps < 1000,"active-area search did not find fixture");
    }
    const auto before=t.search_step();
    check(before->status==SearchStatus::running && before->matches.empty(),"partial result escaped during conversion");
    t.feed("new needle");
    check(t.select_search_match(before->token,0)==SearchApplyResult::stale,"in-progress revision survived output");
    const auto after=finish(t);
    check(after->status==SearchStatus::complete && after->matches.size()==151,"mutation during conversion lost or duplicated hits");
    check(before->matches.empty(),"owned progress snapshot mutated");
}
struct WakeCounter {
    std::mutex mutex; std::condition_variable cv;
    void signal(){{std::lock_guard lock(mutex);}cv.notify_all();}
};
template<class Predicate> bool await(Predicate test,const std::shared_ptr<WakeCounter>& wake,std::chrono::milliseconds timeout=8s) {
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    std::unique_lock lock(wake->mutex);
    while(!test()) if(wake->cv.wait_until(lock,deadline)==std::cv_status::timeout)return test();
    return true;
}
int child() {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8); SetConsoleOutputCP(CP_UTF8);
    DWORD mode{};
    if(GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE),&mode))SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE),mode|ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
    std::cout<<"old NEEDLE\r\n";
    for(unsigned i=0;i<3000;++i)std::cout<<"history "<<i<<"\r\n";
    std::cout<<"recent needle\r\nREADY\r\n"<<std::flush;
    std::string input;
    while(std::getline(std::cin,input)) {
        if(!input.empty()&&input.back()=='\r')input.pop_back();
        if(input=="EXIT")return 0;
        std::cout<<"ECHO:"<<input<<"\r\n"<<std::flush;
    }
    return 0;
}
void session_coalescing_and_fairness(const std::string& executable) {
    auto wake=std::make_shared<WakeCounter>(); ProcessOptions process; process.executable=executable; process.arguments={"--search-child"};process.input_capacity_bytes=3; // Force mixed frontend writes through partial transport chunks.
    auto session=Session::start(process,make_search_terminal_options(80,8),[wake]{wake->signal();});
    struct Close { std::shared_ptr<Session> value; ~Close(){value->request_close(); (void)value->wait_closed(5s);} } close{session};
    check(await([&]{return frame_text(session->frame()).find("READY")!=std::string::npos;},wake),"session child fixture did not arrive");
    auto submitted=session->search({"needle"}); check(submitted.result==SessionSendResult::accepted,"search not accepted");
    check(await([&]{const auto r=session->search_snapshot();return r&&r->token.request_id==submitted.request_id&&r->status==SearchStatus::complete;},wake),"actor search did not complete");
    auto result=session->search_snapshot(); check(result->matches.size()==2,"session did visible-only search");
    check(session->select_search_match(result->token,1)==SessionSendResult::accepted,"history selection not queued");
    check(await([&]{return frame_text(session->frame()).find("old NEEDLE")!=std::string::npos;},wake),"session did not reveal historical match");
    for(unsigned i=0;i<150;++i)check(session->search({"absent"+std::to_string(i)}).result==SessionSendResult::accepted,"typing coalescing hit queue cap");
    submitted=session->search({"READY"});
    check(await([&]{const auto r=session->search_snapshot();return r&&r->token.request_id==submitted.request_id&&r->status==SearchStatus::complete;},wake),"latest typing request did not win");
    result=session->search_snapshot(); check(result->query=="READY"&&result->matches.size()==1,"superseded search published as current");
    const auto update=result->update_id; std::this_thread::sleep_for(40ms);
    check(session->search_snapshot()->update_id==update,"completed search kept idle polling");
    SearchOptions busy{"history"}; busy.max_matches=4096; busy.max_work_units_per_step=1;
    submitted=session->search(busy);
    check(session->text("PING")==SessionSendResult::accepted,"input blocked by search"); KeyInput enter;enter.key=Key::enter; (void)session->key(enter);
    check(await([&]{return frame_text(session->frame()).find("ECHO:PING")!=std::string::npos;},wake),"PTY/input fairness lost during search");
    check(await([&]{const auto r=session->search_snapshot();return r&&r->token.request_id==submitted.request_id&&r->status==SearchStatus::complete;},wake),"interleaved large query failed to complete");
    result=session->search_snapshot(); check(result->matches.size()==3000,"large concurrent query lost history");
    check(session->paste("PONG")==SessionSendResult::accepted,"second input blocked"); (void)session->key(enter);
    check(await([&]{return frame_text(session->frame()).find("ECHO:PONG")!=std::string::npos;},wake),"second PTY echo lost");
    const auto accounting=session->info();
    check(accounting.sent.total()==accounting.bytes_sent,"actual partial transport writes lost accounting");
    check(accounting.sent.text_bytes>=4&&accounting.sent.paste_bytes>=4&&accounting.sent.key_bytes>=2,"mixed text/paste/key origins not retained through partial writes");
    check(await([&]{const auto r=session->search_snapshot();return r&&r->status==SearchStatus::complete&&r->token.revision!=result->token.revision;},wake),"live query not refreshed");
    (void)session->navigate_search(result->token,SearchDirection::backward);
    check(await([&]{const auto r=session->search_snapshot();return r&&r->last_apply_result==SearchApplyResult::stale;},wake),"session blindly applied stale navigation");
    submitted=session->cancel_search();
    check(await([&]{const auto r=session->search_snapshot();return r&&r->token.request_id==submitted.request_id&&r->status==SearchStatus::cancelled;},wake),"actor cancellation not observable");
    check(session->search({std::string(4097,'x')}).result==SessionSendResult::too_large,"session query bound missing");
    session->request_close(); check(session->wait_closed(5s),"session close did not finish");
    check(session->search({"needle"}).result==SessionSendResult::closed,"closed session accepted unsupported Find work");
}
}
int main(int argc,char** argv) {
#ifdef _WIN32
    if(argc==3&&std::string_view(argv[1])=="--native-context-child")return native_context_child(static_cast<unsigned>(std::atoi(argv[2])));
    if(argc==2&&std::string_view(argv[1])=="--native-mouse-child")return native_mouse_child();
#endif
    if(argc==2&&std::string_view(argv[1])=="--search-child")return child();
    const std::vector<std::pair<const char*,std::function<void()>>> tests{
        {"safe native Find fixture",safe_native_fixture},
        {"visible selection publication",visible_selection_publication},
        {"real scrollback and bidirectional wrap",real_history_navigation},
        {"UTF-8/graphemes/literal ASCII-only folding",utf8_and_literal_semantics},
        {"alternate isolation and owned lifetime",alternate_isolation_and_owned_results},
        {"revision, resize, eviction and reset",revision_resize_eviction_and_reset},
        {"result/work/memory bounds and cancellation",budgets_errors_and_cancellation},
        {"bounded quanta and stale navigation",bounded_quanta_and_stale_navigation},
        {"output during owned-coordinate materialization",mutation_during_materialization},
        {"session coalescing, cancellation and I/O fairness",[&]{session_coalescing_and_fairness(std::filesystem::absolute(argv[0]).string());}}
    };
    unsigned failures{};
    for(const auto& [name,test]:tests)try{test();std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& e){++failures;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}
    std::cout<<tests.size()-failures<<'/'<<tests.size()<<" search tests passed; native UI Find not exercised\n";
    return failures?1:0;
}
