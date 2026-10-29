#include "stormtty/parked_attention.hpp"
#include "stormtty/session.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
using namespace stormtty;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string frame_text(const FramePtr& frame) {
    std::string text;
    if (frame) for (const auto& row : frame->lines) {
        for (const auto& cell : row->cells) text += cell.grapheme.empty() ? " " : cell.grapheme;
        text += '\n';
    }
    return text;
}
struct WakeCounter {
    std::mutex mutex; std::condition_variable cv; unsigned wakes{};
    // Optional observed-publication acknowledgment. The callback and test use
    // this same mutex, so observing a new frame alone cannot race its notify.
    std::weak_ptr<Session> observed_session;
    std::uint64_t notified_generation{};
    void signal() {
        {
            std::lock_guard lock(mutex); ++wakes;
            if (const auto session=observed_session.lock())
                if (const auto frame=session->frame()) notified_generation=frame->generation;
        }
        cv.notify_all();
    }
};
template<class Predicate> bool await(Predicate predicate, const std::shared_ptr<WakeCounter>& wake, std::chrono::milliseconds timeout=5s) {
    const auto end=std::chrono::steady_clock::now()+timeout;
    std::unique_lock lock(wake->mutex);
    while (!predicate()) {
        if (wake->cv.wait_until(lock,end)==std::cv_status::timeout) return predicate();
    }
    return true;
}
int child(std::string_view mode, std::string_view exit_signal = {}) {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8); SetConsoleOutputCP(CP_UTF8);
    DWORD output_mode{};
    if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &output_mode))
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
    if(mode=="shell-context") {
        std::cout << "\x1b]7;file:///c/repo\a\x1b]7400;stormtty-context;1;set;file:///c/repo;C:/repo;C:/repo;branch;main\a" << std::flush;
        std::string input; std::getline(std::cin,input);
        std::cout << "\x1b]7;file:///c/other\a" << std::flush;
        std::getline(std::cin,input);
        std::cout << "\x1b]7400;stormtty-context;1;set;file:///c/other;C:/other;;none;\a" << std::flush;
        std::getline(std::cin,input);
        return 0;
    }
    if(mode=="command-complete" || mode=="command-interrupted") {
        std::cout << "\x1b]133;A\a\x1b]133;B\a\x1b]133;D;0\a\x1b]133;C\a";
        if(mode=="command-complete") std::cout << "\x1b]133;D;19\a";
        std::cout << std::flush;
        std::string input; std::getline(std::cin,input);
        return 7;
    }
    if(mode=="echo") {
#ifdef _WIN32
        const auto identity=std::to_string(GetCurrentProcessId());
#else
        const auto identity=std::to_string(getpid());
#endif
        // PTY transport may split the prefix, PID, reset and newline across
        // separate frames. Only the trailing marker establishes the complete
        // baseline; seeing READY alone does not mean output has finished.
        std::cout << "\x1b[31mREADY:" << identity << "\x1b[0m\r\nTHEME-FIXTURE-READY" << std::flush;
        std::string line; std::getline(std::cin,line);
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        std::cout << "GOT:" << line << " PID:" << identity << "\r\n" << std::flush;
        return 7;
    }
    if(mode=="flood") {
        for(std::uint64_t i=0;;++i) std::cout << "STREAM:" << i << " \x1b[32moutput\x1b[0m\r\n" << std::flush;
    }
    if(mode=="retained-transition") {
        std::cout << "TRANSITION-READY\r\n" << std::flush;
        const auto deadline=std::chrono::steady_clock::now()+10s;
        while(!std::filesystem::exists(exit_signal)) {
            if(std::chrono::steady_clock::now()>=deadline)return 3;
            std::this_thread::sleep_for(5ms);
        }
        std::cout << "TRANSITION-FINAL\r\n" << std::flush;
        return 0;
    }
    if(mode=="retained-history" || mode=="retained-bounded") {
#ifdef _WIN32
        const auto identity=std::to_string(GetCurrentProcessId());
#else
        const auto identity=std::to_string(getpid());
#endif
        std::cout << "old retained NEEDLE\r\n";
        const unsigned count=mode=="retained-bounded"?30000:3000;
        for(unsigned i=0;i<count;++i) std::cout << "history " << i << "\r\n";
        std::cout << "READY:" << identity << "\r\n" << std::flush;
        if(mode=="retained-history") { std::string input; std::getline(std::cin,input); }
        // A real child may die during synchronized output or an incomplete VT
        // sequence. Preserve the delivered printable bytes; invent no suffix.
        std::cout << "\x1b[?2026hFINAL:" << identity << "\r\n\x1b[" << std::flush;
        return 23;
    }
    return 3;
}
struct CloseSession {
    std::shared_ptr<Session> session;
    ~CloseSession() { session->request_close(); (void)session->wait_closed(5s); }
};
void parked_attention_policy() {
    check(parked_command_finished_notice("build",0)=="build · Command finished","successful command notice contains filler");
    check(parked_command_finished_notice("build",std::nullopt)=="build · Command finished","unknown command status was invented");
    check(parked_command_finished_notice("build",7)=="build · Command finished · exit 7","nonzero command status missing");
    check(parked_command_finished_notice("build",-1)=="build · Command finished · exit -1","signed explicit command status changed");
    for (const bool reversed : {false,true}) {
        ParkedNoticeBatch batch;
        const auto first=reversed?ParkedNotice::command_finished:ParkedNotice::process_failed;
        const auto second=reversed?ParkedNotice::process_failed:ParkedNotice::command_finished;
        check(batch.consider(first),"first notice not selected");
        (void)batch.consider(second);
        check(batch.selected()==ParkedNotice::process_failed,"cross-session completion hid failure");
        check(!batch.consider(ParkedNotice::process_exited)&&!batch.consider(ParkedNotice::none),"lower-priority notice replaced failure");
    }
    ParkedNoticeBatch exits;
    check(exits.consider(ParkedNotice::command_finished)&&exits.consider(ParkedNotice::process_exited),"exit did not outrank completion");
    check(!exits.consider(ParkedNotice::command_finished)&&exits.selected()==ParkedNotice::process_exited,"completion replaced process exit");
    SessionInfo info; info.state=SessionState::running;
    info.command={CommandPhase::completed,4,4,0};
    ParkedAttention first(info), independent(info);
    check(first.observe(info)==ParkedNotice::none&&!first.unread_completion(),"parking replayed old completion");
    info.command={CommandPhase::running,6,5,19}; // C-D-C in one publication.
    check(first.observe(info)==ParkedNotice::command_finished&&first.unread_completion(),"coalesced command completion lost");
    for(int i=0;i<10;++i)check(first.observe(info)==ParkedNotice::none,"repeat wake replayed completion");
    check(!independent.unread_completion(),"sessions shared acknowledgement state");
    check(independent.observe(info)==ParkedNotice::command_finished,"independent session completion lost");
    // Failed restore and canceled close perform no acknowledgement/removal.
    check(first.unread_completion()&&first.observe(info)==ParkedNotice::none,"failed/canceled placement lost attention");
    info.command.phase=CommandPhase::interrupted;
    check(first.observe(info)==ParkedNotice::none&&first.unread_completion(),"interruption fabricated completion or cleared attention");
    info.state=SessionState::exited; info.exit_code=0;
    check(first.observe(info)==ParkedNotice::process_exited,"new command exit hidden behind previous completion");
    check(first.observe(info)==ParkedNotice::none,"process exit repeated");
    // Successful restore removes the host entry; a later park gets a fresh baseline.
    ParkedAttention reparked(info);
    check(!reparked.unread_completion()&&reparked.observe(info)==ParkedNotice::none,"repark replayed acknowledged completion");

    SessionInfo live; live.state=SessionState::running;
    live.command={CommandPhase::running,1,0,std::nullopt};
    ParkedAttention together(live), separate(live), failed(live), different(live), interrupted(live);
    auto end=live; end.command={CommandPhase::completed,1,1,0};
    check(separate.observe(end)==ParkedNotice::command_finished,"live completion not notified");
    end.state=SessionState::exited; end.exit_code=0;
    check(together.observe(end)==ParkedNotice::command_finished,"same-wake completion/exit not coalesced");
    check(together.observe(end)==ParkedNotice::none&&separate.observe(end)==ParkedNotice::none,"completion/exit duplicated notice");
    end.state=SessionState::failed; end.error="transport failure";
    check(failed.observe(end)==ParkedNotice::process_failed&&failed.unread_completion(),"completion hid process failure");
    check(failed.observe(end)==ParkedNotice::none,"failure notice repeated");
    end.state=SessionState::running; end.exit_code.reset();
    check(different.observe(end)==ParkedNotice::command_finished,"different-result setup lost completion");
    end.state=SessionState::exited; end.exit_code=7;
    check(different.observe(end)==ParkedNotice::process_exited,"different process result hidden");
    live.command.phase=CommandPhase::interrupted;
    check(interrupted.observe(live)==ParkedNotice::none&&!interrupted.unread_completion(),"interruption invented unread completion");
    live.command={CommandPhase::completed,3,3,std::nullopt};
    check(interrupted.observe(live)==ParkedNotice::command_finished,"generation jump/optional status lost");
    check(interrupted.observe(live)==ParkedNotice::none,"generation jump replayed");
}
void command_lifecycle(const std::string& executable) {
    for (const auto mode : {"command-complete", "command-interrupted", "command-failure"}) {
        auto wake=std::make_shared<WakeCounter>();
        const bool complete=std::string_view(mode)=="command-complete";
        const bool failure=std::string_view(mode)=="command-failure";
        ProcessOptions p; p.executable=executable;
        p.arguments={"--child",complete?"command-complete":"command-interrupted"};
        auto session=Session::start(p,{},[wake]{wake->signal();},SessionRetentionPolicy::retain_document);
        CloseSession close{session};
        check(await([&]{return session->info().command.phase==
            (complete?CommandPhase::completed:CommandPhase::running);},wake),"command metadata not published");
        const auto observed=session->info().command;
        check(observed.command_generation==1,"startup markers invented a command");
        check(!session->process_finished()&&!session->info().exit_code,"command end confused with process exit");
        check(observed.completed_generation==(complete?1u:0u),"completion generation incorrect");
        check(observed.last_exit_code==(complete?std::optional<std::int32_t>(19):std::nullopt),"command status incorrect");
        if (failure) {
            // Existing actor callback harness: a non-invalid_argument exception
            // reaches fail(), which must interrupt metadata before reaping.
            check(session->copy_selection([](std::string){throw std::runtime_error("command actor failure fixture");})
                ==SessionSendResult::accepted,"failure fixture callback rejected");
            check(session->wait_closed(5s),"failed command actor did not reap/close");
            check(session->info().state==SessionState::failed,"actor failure not reported");
        } else {
            KeyInput enter; enter.key=Key::enter;
            check(session->key(enter)==SessionSendResult::accepted,"command child exit input rejected");
            check(session->wait_for_process_exit(5s),"command child did not exit");
            check(session->info().exit_code==7,"shell process exit status lost");
        }
        const auto final=session->info().command;
        check(final.phase==(complete?CommandPhase::completed:CommandPhase::interrupted),"EOF/failure left command running");
        check(final.command_generation==observed.command_generation&&final.completed_generation==observed.completed_generation,
            "EOF/failure invented command completion");
        check(final.last_exit_code==observed.last_exit_code,"EOF/failure invented command exit status");
        check(observed.phase==(complete?CommandPhase::completed:CommandPhase::running),"owned command snapshot mutated");
    }
}
void shell_context_publication(const std::string& executable) {
    auto wake=std::make_shared<WakeCounter>();
    ProcessOptions p; p.executable=executable; p.arguments={"--child","shell-context"};
    auto session=Session::start(p,{},[wake]{wake->signal();});
    CloseSession close{session};
    check(await([&]{return session->info().shell_context.has_value();},wake),"shell context not published");
    const auto owned=session->info();
    check(owned.working_directory==owned.shell_context->directory_key && owned.shell_context->head=="main","cwd/context publication reordered");
    KeyInput enter; enter.key=Key::enter;
    check(session->key(enter)==SessionSendResult::accepted,"context child input rejected");
    check(await([&]{return session->info().working_directory=="file:///c/other";},wake),"new cwd not published");
    check(!session->info().shell_context,"previous directory Git leaked");
    check(session->key(enter)==SessionSendResult::accepted,"context child second input rejected");
    check(await([&]{auto i=session->info();return i.shell_context && i.shell_context->git==GitHeadKind::none;},wake),"non-repository context not published");
    const auto last=session->info().shell_context;
    check(session->key(enter)==SessionSendResult::accepted,"context child final input rejected");
    check(session->wait_for_process_exit(5s),"context child did not exit");
    check(session->info().shell_context==last,"final owned context lost");
    check(owned.shell_context->head=="main","published owned context mutated");
}
void retained_document(const std::string& executable) {
    auto wake=std::make_shared<WakeCounter>();
    ProcessOptions p; p.executable=executable; p.arguments={"--child","retained-history"};
    TerminalOptions t; t.size={80,8,8,16}; t.scrollback_lines=10000;
    auto session=Session::start(p,t,[wake]{wake->signal();},SessionRetentionPolicy::retain_document);
    CloseSession close{session};
    { std::lock_guard lock(wake->mutex); wake->observed_session=session; }
    check(await([&]{return frame_text(session->frame()).find("READY:")!=std::string::npos;},wake),"retained child did not reach ready marker");
    check(!session->process_finished() && !session->wait_for_process_exit(1ms),"running child reported a terminal result");
    const auto ready_text=frame_text(session->frame());
    const auto begin=ready_text.find("READY:")+6;
    const auto identity=ready_text.substr(begin,ready_text.find_first_of(" \r\n",begin)-begin);
    SearchOptions busy{"history"}; busy.max_matches=4096; busy.max_work_units_per_step=1;
    const auto pending=session->search(busy); check(pending.result==SessionSendResult::accepted,"pre-exit search rejected");
    check(session->text("EXIT")==SessionSendResult::accepted,"retained child exit input rejected");
    KeyInput enter; enter.key=Key::enter; check(session->key(enter)==SessionSendResult::accepted,"retained child Enter rejected");
    check(session->wait_for_process_exit(5s),"retained process did not finish");
    const auto final_frame=session->frame();
    check(frame_text(final_frame).find("FINAL:"+identity)!=std::string::npos,"exit published before final synchronized-output bytes");
    check(session->info().state==SessionState::exited && session->info().exit_code==23,"retained process result incorrect");
    check(session->process_finished() && !session->closed() && !session->wait_closed(10ms),"retention ambiguously changed actor completion");
    check(!session->diagnostics().provider.empty(),"process diagnostics lost with released transport");
    check(await([&]{auto r=session->search_snapshot();return r&&r->token.request_id==pending.request_id&&r->status==SearchStatus::complete;},wake),"search did not survive process exit");
    check(session->search_snapshot()->matches.size()==3000,"retained search lost history");
    const auto sent=session->info().bytes_sent, received=session->info().bytes_received;
    check(session->key(enter)==SessionSendResult::closed,"exited document accepted key");
    check(session->text("must not restart")==SessionSendResult::closed,"exited document accepted text");
    check(session->paste("x")==SessionSendResult::closed,"exited document accepted paste");
    check(session->mouse({})==SessionSendResult::closed,"exited document accepted mouse protocol input");
    check(session->focus(true)==SessionSendResult::closed,"exited document accepted focus protocol input");
    check(session->scroll(-100000)==SessionSendResult::accepted,"retained scroll rejected");
    check(await([&]{return frame_text(session->frame()).find("old retained NEEDLE")!=std::string::npos;},wake),"retained scrollback unavailable");
    check(session->select(0,0,18,0)==SessionSendResult::accepted,"retained selection rejected");
    struct Copy { std::mutex mutex; std::optional<std::string> text; };
    auto copy=std::make_shared<Copy>();
    check(session->copy_selection([copy,wake](std::string text){
        {std::lock_guard lock(copy->mutex);copy->text=std::move(text);}wake->signal();
    })==SessionSendResult::accepted,"retained copy rejected");
    check(await([&]{std::lock_guard lock(copy->mutex);return copy->text.has_value();},wake),"retained copy did not complete");
    {std::lock_guard lock(copy->mutex);check(copy->text->find("old retained NEEDLE")!=std::string::npos,"retained copy lost selection");}
    SearchOptions limited{"history"}; limited.max_work_units_per_step=1; limited.max_total_work_units=1;
    auto find=session->search(limited);
    check(await([&]{auto r=session->search_snapshot();return r&&r->token.request_id==find.request_id&&r->status==SearchStatus::limited;},wake),"retained search work bound ignored");
    check(session->search_snapshot()->work_units==1,"retained search exceeded work budget");
    check(session->search({std::string(4097,'x')}).result==SessionSendResult::too_large,"retained query size bound ignored");
    for(unsigned i=0;i<150;++i)check(session->search({"absent"+std::to_string(i)}).result==SessionSendResult::accepted,"retained search coalescing failed");
    find=session->search({"NEEDLE"});
    check(await([&]{auto r=session->search_snapshot();return r&&r->token.request_id==find.request_id&&r->status==SearchStatus::complete;},wake),"post-exit search failed");
    const auto found=session->search_snapshot(); check(found->matches.size()==1,"post-exit search not over full retained document");
    check(session->select_search_match(found->token,0)==SessionSendResult::accepted,"retained search selection rejected");
    check(await([&]{auto r=session->search_snapshot();return r&&r->selected_index==0;},wake),"retained search selection did not apply");
    const Color foreground{230,231,232,255}, background{12,13,14,255};
    for(unsigned i=0;i<2000;++i)check(session->set_default_colors({static_cast<std::uint8_t>(180+i%50),231,232,255},background)==SessionSendResult::accepted,"retained theme previews hit input bound");
    check(session->set_default_colors(foreground,background)==SessionSendResult::accepted,"retained final theme rejected");
    check(await([&]{auto f=session->frame();return f&&f->foreground==foreground&&f->background==background;},wake),"retained theme latest value lost");
    check(session->request_resize({60,10,8,16})==SessionSendResult::accepted,"retained local resize rejected");
    check(await([&]{auto f=session->frame();return f&&f->columns==60&&f->rows==10;},wake),"retained local resize did not apply");
    check(session->scroll_to_bottom()==SessionSendResult::accepted,"retained bottom rejected");
    check(await([&]{return frame_text(session->frame()).find("FINAL:"+identity)!=std::string::npos;},wake),"reopening retained output lost child identity/final marker");
    const auto cancelled=session->cancel_search();
    check(await([&]{auto r=session->search_snapshot();return r&&r->token.request_id==cancelled.request_id&&r->status==SearchStatus::cancelled;},wake),"retained cancellation failed");
    // Controlled slow-consumer regression: actor work may take longer than the
    // old 40ms guess. This injects scheduling delay; it is not an idle barrier.
    check(session->copy_selection([](std::string) { std::this_thread::sleep_for(80ms); })==SessionSendResult::accepted,"retained slow callback rejected");
    check(session->clear_selection()==SessionSendResult::accepted,"retained clear selection rejected");
    // A 40ms sleep was not an actor-completion barrier: a delayed clear or its
    // pending publication/notification could legitimately land in the idle
    // observation interval. A distinct final display command fences all prior
    // FIFO operations. Wait for its published frame AND callback acknowledgment,
    // not just a frame which becomes visible before notify() has run.
    check(session->request_resize({61,10,8,16})==SessionSendResult::accepted,"retained idle fence rejected");
    check(await([&] {
        const auto f=session->frame();
        return f&&f->columns==61&&f->rows==10&&wake->notified_generation==f->generation;
    },wake),"retained final display publication was not acknowledged");
    unsigned wakes{}; {std::lock_guard lock(wake->mutex);wakes=wake->wakes;}
    const auto generation=session->frame()->generation;
    std::this_thread::sleep_for(120ms);
    {std::lock_guard lock(wake->mutex);check(wake->wakes==wakes,"retained idle document kept waking UI");}
    check(session->frame()->generation==generation,"retained idle document kept publishing frames");
    check(session->info().bytes_sent==sent&&session->info().bytes_received==received,"retained display work touched/restarted transport");
    check(frame_text(final_frame).find("FINAL:"+identity)!=std::string::npos&&final_frame->columns==80,"retained edits mutated old owned frame");
    const auto start=std::chrono::steady_clock::now();session->request_close();session->request_close();
    check(std::chrono::steady_clock::now()-start<100ms,"retained disposal blocked caller");
    check(session->wait_closed(5s)&&session->wait_for_process_exit(0ms),"retained actor did not dispose independently of process exit");
    check(session->scroll(-1)==SessionSendResult::closed&&session->search({"needle"}).result==SessionSendResult::closed,"disposed document accepted commands");
    check(session->set_default_colors(foreground,background)==SessionSendResult::closed,"disposed document accepted theme");
}
void retained_transition_releases_completed_callbacks(const std::string& executable) {
    // The child exits only after the actor is gated. Then an input rejection
    // proves the exit callback arrived before the owner-capturing copy is queued.
    // This places that copy in the final LIVE batch, not the retained loop.
    struct Signal {
        std::filesystem::path path;
        bool created{};
        ~Signal(){if(created){std::error_code error;std::filesystem::remove(path,error);}}
    } signal{std::filesystem::current_path()/("stormtty-session-exit-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".signal")};
    check(!std::filesystem::exists(signal.path),"transition signal collision");
    struct Gate {
        std::mutex mutex;std::condition_variable cv;bool entered{},release{};
        std::atomic<bool> copied{};
        void open(){ {std::lock_guard lock(mutex);release=true;}cv.notify_all(); }
    };
    auto gate=std::make_shared<Gate>();
    struct ReleaseGate {std::shared_ptr<Gate> gate;~ReleaseGate(){gate->open();}} release{gate};
    ProcessOptions p;p.executable=executable;p.arguments={"--child","retained-transition",signal.path.string()};
    auto wake=std::make_shared<WakeCounter>();
    auto session=Session::start(p,{},[wake]{wake->signal();},SessionRetentionPolicy::retain_document);
    check(await([&]{return frame_text(session->frame()).find("TRANSITION-READY")!=std::string::npos;},wake),"transition child not ready");
    check(session->copy_selection([gate](std::string){
        std::unique_lock lock(gate->mutex);gate->entered=true;gate->cv.notify_all();
        gate->cv.wait(lock,[&]{return gate->release;});
    })==SessionSendResult::accepted,"transition gate rejected");
    {std::unique_lock lock(gate->mutex);check(gate->cv.wait_for(lock,5s,[&]{return gate->entered;}),"transition actor did not enter gate");}
    check(!session->process_finished(),"transition actor already retained before gate");
    signal.created=std::filesystem::create_directory(signal.path);
    check(signal.created,"transition exit signal failed");
    bool exited{};
    // At most seven accepted probes: together with the final copy they fit the
    // eight-command live batch. No probe can reach a child while actor is gated.
    for(unsigned attempt=0;attempt<8;++attempt){
        const auto result=session->focus(false);
        if(result==SessionSendResult::closed){exited=true;break;}
        check(result==SessionSendResult::accepted,"transition exit probe rejected unexpectedly");
        if(attempt<7)std::this_thread::sleep_for(1s);
    }
    check(exited,"transport exit was not observed while actor was gated");
    check(session->copy_selection([owned=session,gate](std::string){(void)owned;gate->copied=true;})==SessionSendResult::accepted,"transition owning callback rejected");
    gate->open();
    check(session->wait_for_process_exit(5s),"transition process result not published");
    check(gate->copied.load(),"owning callback did not execute in final live batch");
    check(frame_text(session->frame()).find("TRANSITION-FINAL")!=std::string::npos,"transition lost final bytes");
    std::weak_ptr<Session> released=session;session.reset();
    const auto deadline=std::chrono::steady_clock::now()+5s;
    while(!released.expired()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(5ms);
    const bool disposed=released.expired();
    if(auto rescue=released.lock()){rescue->request_close();(void)rescue->wait_closed(5s);}
    check(disposed,"completed live-batch callback retained the final Session owner indefinitely");
}
void retained_bounds_failure_and_destructor(const std::string& executable) {
    ProcessOptions p;p.executable=executable;p.arguments={"--child","retained-bounded"};
    TerminalOptions t;t.size={80,4,8,16};t.scrollback_lines=10;
    auto wake=std::make_shared<WakeCounter>();
    auto session=Session::start(p,t,[wake]{wake->signal();},SessionRetentionPolicy::retain_document);
    CloseSession close{session};
    check(session->wait_for_process_exit(10s),"bounded retained child failed to exit");
    auto find=session->search({"old retained NEEDLE"});
    check(await([&]{auto r=session->search_snapshot();return r&&r->token.request_id==find.request_id&&r->status==SearchStatus::complete;},wake),"bounded retained search failed");
    check(session->search_snapshot()->matches.empty(),"retention bypassed scrollback bound");
    find=session->search({"FINAL:"});
    check(await([&]{auto r=session->search_snapshot();return r&&r->token.request_id==find.request_id&&r->status==SearchStatus::complete;},wake),"bounded final-marker search failed");
    check(session->search_snapshot()->matches.size()==1,"bounded retention lost final bytes");
    session->request_close();check(session->wait_closed(5s),"bounded document disposal hung");
    auto legacy=Session::start(p,t);
    check(legacy->wait_closed(10s)&&legacy->process_finished(),"default final-frame-only child failed to dispose");
    check(frame_text(legacy->frame()).find("FINAL:")!=std::string::npos,"default final frame froze at a pre-hold viewport");
    check(legacy->scroll(-1)==SessionSendResult::closed,"default policy unexpectedly retained readable terminal");
    p.executable=executable+".missing";
    auto failed=Session::start(p,t,{},SessionRetentionPolicy::retain_document);
    check(failed->wait_for_process_exit(5s),"retained launch failure did not settle process wait");
    check(failed->info().state==SessionState::failed&&!failed->info().error.empty(),"retained launch failure lacks diagnostic");
    check(!failed->closed()&&failed->text("x")==SessionSendResult::closed,"launch failure retained a live transport or disposed document");
    failed->request_close();check(failed->wait_closed(5s),"launch failure document disposal hung");
    auto lifetime=std::make_shared<int>(1);std::weak_ptr<int> released=lifetime;
    auto abandoned=Session::start(p,t,[lifetime]{},SessionRetentionPolicy::retain_document);
    check(abandoned->wait_for_process_exit(5s),"destructor fixture failed");
    lifetime.reset();abandoned.reset();
    const auto deadline=std::chrono::steady_clock::now()+5s;
    while(!released.expired()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(5ms);
    check(released.expired(),"last Session owner did not dispose retained actor");
    // Discarding a queued callback can release the final Session owner. Its
    // destructor reenters request_close, so closure disposal must be unlocked.
    struct Gate { std::mutex mutex; std::condition_variable cv; bool entered{}, release{}; };
    auto gate=std::make_shared<Gate>();lifetime=std::make_shared<int>(2);released=lifetime;
    auto queued=Session::start(p,t,[lifetime]{},SessionRetentionPolicy::retain_document);
    check(queued->wait_for_process_exit(5s),"queued-disposal fixture failed");
    check(queued->copy_selection([gate](std::string){
        std::unique_lock lock(gate->mutex);gate->entered=true;gate->cv.notify_all();
        gate->cv.wait(lock,[&]{return gate->release;});
    })==SessionSendResult::accepted,"queued-disposal gate rejected");
    {std::unique_lock lock(gate->mutex);check(gate->cv.wait_for(lock,5s,[&]{return gate->entered;}),"queued-disposal gate timed out");}
    check(queued->copy_selection([owned=queued](std::string){(void)owned;})==SessionSendResult::accepted,"queued owner rejected");
    queued->request_close();lifetime.reset();queued.reset();
    {std::lock_guard lock(gate->mutex);gate->release=true;}gate->cv.notify_all();
    const auto queued_deadline=std::chrono::steady_clock::now()+5s;
    while(!released.expired()&&std::chrono::steady_clock::now()<queued_deadline)std::this_thread::sleep_for(5ms);
    check(released.expired(),"discarding queued final owner deadlocked retained disposal");
    p.executable=executable;p.arguments={"--child","flood"};
    for(unsigned i=0;i<8;++i){
        auto race=Session::start(p,t,{},SessionRetentionPolicy::retain_document);
        race->request_close();
        check(race->wait_for_process_exit(5s)&&race->wait_closed(5s),"retained explicit-close/start race hung");
    }
}
}
int main(int argc,char** argv) {
    if((argc==3 || argc==4) && std::string_view(argv[1])=="--child")return child(argv[2],argc==4?argv[3]:"");
    try {
        const auto executable=std::filesystem::absolute(argv[0]).string();
        {
            auto wakes=std::make_shared<WakeCounter>();
            ProcessOptions p; p.executable=executable; p.arguments={"--child","echo"};
            TerminalOptions t; t.size={80,24,8,16};
            auto session=Session::start(p,t,[wakes]{wakes->signal();});
            check(await([&]{return frame_text(session->frame()).find("THEME-FIXTURE-READY")!=std::string::npos;},wakes),"complete theme fixture output did not reach frame");
#ifdef STORMTTY_TEST_DEADLINE_unsupported
            check(session->info().deadline.requested&&!session->info().deadline.available,"unsupported timer did not use production CV fallback");
#endif
            const auto original=session->frame();
            const auto initial_text=frame_text(original);
            const auto identity_start=initial_text.find("READY:")+6;
            const auto identity=initial_text.substr(identity_start,initial_text.find_first_of(" \r\n",identity_start)-identity_start);
            const auto sent_before=session->info().bytes_sent;
            const auto received_before=session->info().bytes_received;
            const Color preview_fg{224,225,226,255}, preview_bg{16,17,18,255};
            check(session->set_default_colors(preview_fg,preview_bg)==SessionSendResult::accepted,"live theme not accepted");
            check(await([&]{auto f=session->frame();return f && f->foreground==preview_fg && f->background==preview_bg;},wakes),"live theme did not repaint existing session");
            const auto preview=session->frame();
            check(preview->lines[0]->cells[0].foreground==original->lines[0]->cells[0].foreground,"explicit ANSI foreground changed");
            // Rapid previews use one separately bounded latest-value slot. Neither
            // obsolete previews nor the final Cancel can displace PTY input.
            for (unsigned i=0;i<2000;++i) {
                const Color fg{static_cast<std::uint8_t>(180+i%50),225,226,255};
                check(session->set_default_colors(fg,preview_bg)==SessionSendResult::accepted,"rapid theme update was backpressured");
            }
            check(session->set_default_colors(original->foreground,original->background)==SessionSendResult::accepted,"theme cancel not accepted");
            check(await([&]{auto f=session->frame();return f && f->generation>preview->generation && f->foreground==original->foreground && f->background==original->background;},wakes),"latest theme cancel was lost or reordered");
            check(session->info().bytes_sent==sent_before,"display-only update wrote PTY bytes");
            const auto after_theme=session->frame();
            const auto after_theme_text=frame_text(after_theme);
            if(after_theme_text!=initial_text) {
                std::cerr << "Theme content mismatch: received=" << received_before << "->" << session->info().bytes_received
                    << " frame=" << original->generation << "->" << after_theme->generation
                    << "\nInitial frame:\n" << initial_text << "After theme/cancel:\n" << after_theme_text;
            }
            check(after_theme_text==initial_text,"theme update changed terminal content");
            check(original->background!=preview->background && preview->background==preview_bg,"old theme snapshot mutated");
            bool rejected{};
            try { session->set_default_colors({1,2,3,1},preview_bg); } catch(const std::invalid_argument&) { rejected=true; }
            check(rejected,"non-opaque session theme accepted");
            check(session->text("hello")==SessionSendResult::accepted,"text not accepted");
            KeyInput enter;enter.key=Key::enter;
            check(session->key(enter)==SessionSendResult::accepted,"enter not accepted");
            check(session->wait_closed(5s),"natural child exit did not close actor");
            check(session->info().state==SessionState::exited,"natural child exit state");
#ifdef STORMTTY_TEST_DEADLINE_failure
            check(session->info().deadline.failures==1,"failed timer did not fall back exactly once");
#endif
            check(session->info().exit_code==7,"exit code lost");
            check(session->process_finished()&&session->wait_for_process_exit(0ms),"default process-exit observation missing");
            check(frame_text(session->frame()).find("GOT:hello PID:"+identity)!=std::string::npos,"input ordering or child identity changed during theme preview");
            const auto exited_frame=session->frame();
            check(session->set_default_colors(preview_fg,preview_bg)==SessionSendResult::closed,"exited session accepted a theme update");
            check(session->frame()==exited_frame,"exited session final frame changed");
            check(frame_text(original).find("READY")!=std::string::npos,"published old frame invalidated");
            check(session->text("late")==SessionSendResult::closed,"closed input accepted");
            session->request_close();session->request_close();
        }
        {
            auto wakes=std::make_shared<WakeCounter>();
            ProcessOptions p;p.executable=executable;p.arguments={"--child","flood"};
            auto session=Session::start(p,{},[wakes]{wakes->signal();});
            check(await([&]{return session->info().bytes_received>10000;},wakes),"flood child failed");
            check(session->paste(std::string(2*1024*1024,'x'))==SessionSendResult::too_large,"input bound not enforced");
            const auto start=std::chrono::steady_clock::now();session->request_close();
            check(std::chrono::steady_clock::now()-start<100ms,"UI close blocked");
            check(session->wait_closed(5s),"close-under-flood did not finish");
        }
        {
            auto wakes=std::make_shared<WakeCounter>();
            ProcessOptions p;p.executable=executable+".missing";
            auto session=Session::start(p,{},[wakes]{wakes->signal();});
            check(session->wait_closed(5s),"launch failure hung");
            check(session->info().state==SessionState::failed,"launch failure not reported");
            check(!session->info().error.empty(),"launch failure lacks diagnostic");
        }
        for(int i=0;i<8;++i) {
            ProcessOptions p;p.executable=executable;p.arguments={"--child","flood"};
            auto session=Session::start(p,{});session->request_close();
            check(session->wait_closed(5s),"immediate close race hung");
        }
        parked_attention_policy();
        command_lifecycle(executable);
        shell_context_publication(executable);
        retained_document(executable);
        retained_bounds_failure_and_destructor(executable);
        retained_transition_releases_completed_callbacks(executable);
        std::cout << "Session: command completion independent of process, EOF/failure interruption; retained document read/search/display, bounded scrollback, final held output, idle/disposal; real child, input ordering, immutable frame, bounded queue, flood close, launch failure, immediate-close races passed\n";
        return 0;
    } catch(const std::exception& e){std::cerr << "FAIL: " << e.what() << '\n';return 1;}
}
