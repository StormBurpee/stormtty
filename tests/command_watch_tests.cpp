#include "stormtty/command_watch.hpp"
#include "stormtty/parked_attention.hpp"
#include "stormtty/workspace_ribbon.hpp"
#include "stormtty/workspace_navigator.hpp"
#include <iostream>
#include <stdexcept>

using namespace stormtty;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
constexpr WatchIdentity actor{1, 7, 3};
SessionInfo running(std::uint64_t generation = 1) {
    SessionInfo info;
    info.state = SessionState::running;
    info.command.phase = CommandPhase::running;
    info.command.command_generation = generation;
    return info;
}
SessionInfo completed(std::uint64_t generation = 1, std::optional<std::int32_t> code = 0) {
    auto info = running(generation);
    info.command.phase = CommandPhase::completed;
    info.command.completed_generation = generation;
    info.command.last_exit_code = code;
    return info;
}
CommandWatch armed() {
    CommandWatch watch;
    check(watch.arm(actor, running(), true), "arm observed foreground C");
    return watch;
}
void eligibility() {
    CommandWatch watch;
    check(!watch.arm(actor, running(), false), "background cannot arm");
    check(!watch.arm({}, running(), true), "identity required");
    check(!watch.arm(actor, running(0), true), "nonzero generation required");
    check(!watch.arm(actor, completed(), true), "cannot watch old completion");
    for (auto state : {SessionState::starting, SessionState::closing, SessionState::exited, SessionState::failed}) {
        auto info = running(); info.state = state;
        check(!watch.arm(actor, info, true), "live actor required");
    }
    for (auto phase : {CommandPhase::unavailable, CommandPhase::interrupted}) {
        auto info = running(); info.command.phase = phase;
        check(!watch.arm(actor, info, true), "observed running required");
    }
    watch = armed();
    check(!watch.arm(actor, running(2), true), "one watch per session");
    check(!watch.acknowledge(), "watching is not a result");
}
void exact_and_coalesced() {
    auto watch = armed();
    check(!watch.observe(actor, running()), "duplicate C quiet");
    check(watch.observe(actor, completed(1, 9)), "D result");
    check(watch.status() == WatchStatus::completed && watch.exit_code() == 9, "exact D exit code");
    check(!watch.observe(actor, completed(1, 9)), "duplicate D quiet");
    check(!watch.observe(actor, completed(2, 0)) && watch.exit_code() == 9, "later command cannot overwrite");
    check(!watch.arm(actor, running(3), true), "unread result must be dismissed");
    check(watch.acknowledge() && watch.status() == WatchStatus::idle, "explicit ack");
    watch = armed();
    auto info = completed(1, 4); info.command.command_generation = 2; info.command.phase = CommandPhase::running;
    check(watch.observe(actor, info) && watch.exit_code() == 4, "coalesced C-D-C exact match");
    watch = armed();
    check(watch.observe(actor, completed(4, 42)), "coalesced later D");
    check(watch.status() == WatchStatus::completion_unconfirmed && !watch.exit_code(), "never borrow later exit code");
    // C-RIS-C-D can publish exactly the same snapshot as C-D-C-D.
    // The generic result must not assert that the watched command completed.
    check(watch.status() != WatchStatus::completed, "coalesced reset cannot fake watched success");
    watch = armed();
    check(watch.observe(actor, completed(1, std::nullopt)) && !watch.exit_code(), "D without status not success");
}
void interruption_and_identity() {
    for (auto state : {SessionState::closing, SessionState::exited, SessionState::failed}) {
        auto watch = armed(); auto info = running(); info.state = state;
        check(watch.observe(actor, info), "transport end yields result");
        check(watch.status() != WatchStatus::completed && !watch.exit_code(), "no fabricated completion");
    }
    auto watch = armed(); auto info = running(); info.command.phase = CommandPhase::interrupted;
    check(watch.observe(actor, info) && watch.status() == WatchStatus::interrupted, "reset not completion");
    watch = armed(); info = completed(1, 7); info.state = SessionState::exited;
    check(watch.observe(actor, info) && watch.exit_code() == 7, "D then EOF retains matching command result");
    watch = armed(); info = running(2);
    check(watch.observe(actor, info) && watch.status() == WatchStatus::interrupted, "later C without matching D");
    watch = armed(); info = running(0);
    check(watch.observe(actor, info) && watch.status() == WatchStatus::unavailable, "regression unavailable");
    for (auto other : {WatchIdentity{2,7,3}, WatchIdentity{1,8,3}, WatchIdentity{1,7,4}}) {
        watch = armed(); check(!watch.observe(other, completed()), "replacement not completion");
        check(watch.status() == WatchStatus::idle, "replacement drops old watch");
        watch = armed(); watch.observe(actor, completed()); watch.observe(other, completed());
        check(!watch.unread(), "replacement drops old result too");
    }
    watch = armed(); watch.cancel();
    check(!watch.observe(actor, completed()) && !watch.unread(), "cancel prevents result, not process");
}
void parked_generation_acknowledgment() {
    ParkedAttention parked(running());
    check(!parked.acknowledge_completion(0) && !parked.acknowledge_completion(1), "no unread result to acknowledge");
    auto watch = armed();
    check(parked.observe(completed()) == ParkedNotice::command_finished, "watch then park completion observed");
    watch.observe(actor, completed());
    check(!parked.acknowledge_completion(0) && !parked.acknowledge_completion(2) && parked.unread_completion(), "zero/future acknowledgment preserves unread");
    check(parked.acknowledge_completion(watch.generation()), "exact watched generation dismissed");
    check(!parked.unread_completion() && !parked.acknowledge_completion(1), "repeated acknowledgment is no-op");
    check(parked.observe(completed()) == ParkedNotice::none && !parked.unread_completion(), "ack does not replay old notification");
    check(parked.observe(completed(2, 8)) == ParkedNotice::command_finished && parked.unread_completion(), "future completion still notifies");
    check(!parked.acknowledge_completion(watch.generation()) && parked.unread_completion(), "old watch dismissal preserves newer parked result");
    check(parked.acknowledge_completion(2), "newer exact generation can dismiss");

    // Dismissal cannot change same-completion exit deduplication, distinct exit
    // notification, or failure priority, including a simultaneous new D.
    auto end = completed(2, 8); end.state = SessionState::exited; end.exit_code = 8;
    check(parked.observe(end) == ParkedNotice::none, "ack retains matching exit notification history");
    ParkedAttention different(running());
    different.observe(completed()); different.acknowledge_completion(1);
    end = completed(); end.state = SessionState::exited; end.exit_code = 7;
    check(different.observe(end) == ParkedNotice::process_exited, "ack cannot suppress distinct exit");
    ParkedAttention failed(running());
    failed.observe(completed()); failed.acknowledge_completion(1);
    end = completed(2); end.state = SessionState::failed;
    check(failed.observe(end) == ParkedNotice::process_failed && failed.unread_completion(), "new completion cannot hide failure after ack");
    check(failed.acknowledge_completion(2) && failed.observe(end) == ParkedNotice::none, "ack does not replay failure");

    // A completion already present when parking is baseline, not fresh unread.
    ParkedAttention late_park(completed());
    check(!late_park.acknowledge_completion(1), "complete then park baseline stays quiet");
    check(late_park.observe(completed(3)) == ParkedNotice::command_finished, "generation jump remains observable");
    check(!late_park.acknowledge_completion(1) && late_park.unread_completion(), "late park preserves later generation");
}
void placement() {
    auto watch = armed(); ParkedAttention parked(running());
    check(watch.surface(false) == WatchSurface::none, "watching no completion badge");
    watch.observe(actor, completed()); parked.observe(completed());
    check(watch.surface(false) == WatchSurface::watch, "unparked badge");
    check(watch.surface(true) == WatchSurface::parked, "watch transfers to parked surface");
    const unsigned badges = static_cast<unsigned>(parked.unread_completion() || watch.surface(true) == WatchSurface::parked);
    check(badges == 1, "same actor parked and watched deduplicated");
    check(watch.surface(false) == WatchSurface::watch && watch.unread(), "restore/visibility not ack");
    watch.acknowledge();
    check(watch.surface(false) == WatchSurface::none, "explicit open result/dismiss removes watch badge");
}
void activity() {
    WorkspaceActivitySummary summary;
    summary.add(false,std::nullopt,nullptr,SessionState::running);
    check(summary.terminals==0 && summary.labels().empty(),"live shell alone invents no activity");
    auto watch=armed();summary.add(false,std::nullopt,&watch,SessionState::running);
    check(summary.terminals==1 && summary.running==1 && !summary.unread(),"unparked explicit Watch is represented");
    check(summary.labels().front()=="Watch · running","running label comes only from Watch");
    summary={};summary.add(true,std::nullopt,&watch,SessionState::running);
    check(summary.terminals==1 && summary.background==1 && summary.running==1,"watched-and-parked actor is one entry");
    watch.observe(actor,completed(1,7));summary={};summary.add(true,std::uint64_t{1},&watch,SessionState::running);
    check(summary.terminals==1 && summary.unread()==1 && summary.nonzero==1 && !summary.background_finished,"same parked/watch attention is not counted twice");
    check(summary.labels().front()=="Watch · exit 7","exact nonzero code is preserved");
    const auto measure=[](const std::string& text){return double(text.size());};
    for(int width=0;width<150;++width)check(summary.label(width,measure).size()<=std::size_t(width),"summary fits complete measured tokens");
    check(summary.label(8,measure)=="Activity","narrow fallback does not clip an exit code");
    check(summary.label(200,measure).find("Background 1")!=std::string::npos,"wide summary retains background access label");
    auto success=armed();success.observe(actor,completed());summary={};summary.add(false,std::nullopt,&success,SessionState::exited);
    check(summary.completed==1 && !summary.running && summary.labels().front()=="Watch · complete · exit 0","matching completion retained without claiming exited shell is running");
    auto unknown=armed();unknown.observe(actor,completed(1,std::nullopt));summary={};summary.add(true,std::uint64_t{1},&unknown,SessionState::running);
    check(summary.unknown==1 && !summary.completed && summary.labels().front().find("status unknown")!=std::string::npos,"missing exit status never becomes success");
    for(auto info:{completed(3),running(2),running(0)}) {
        auto unavailable=armed();unavailable.observe(actor,info);summary={};summary.add(true,std::nullopt,&unavailable,SessionState::running);
        check(summary.unavailable==1 && summary.unread()==1 && !summary.completed && !summary.background_finished,"unknown Watch attention is not a finished command");
        check(summary.accessible_label().find("finished")==std::string::npos,"unknown result copy never says finished");
    }
    ParkedAttention later(running());
    later.observe(completed(1,7));
    check(later.unread_generation()==1,"parked attention exposes only its unread generation");
    later.observe(completed(2,0));summary={};summary.add(true,later.unread_generation(),&watch,SessionState::running);
    check(summary.unread()==1 && summary.terminals==1 && summary.nonzero==1 && summary.background_finished==1,"old Watch N cannot hide later parked completion N+1");
    check(summary.accessible_label().find("background completion")!=std::string::npos,"later parked completion remains visible beside retained Watch result");
    auto unconfirmed=armed();unconfirmed.observe(actor,completed(3));summary={};summary.add(true,std::uint64_t{3},&unconfirmed,SessionState::running);
    check(summary.unavailable==1 && summary.background_finished==1 && summary.unread()==1,"real later D survives unknown watched result without doubling actor count");
    later.acknowledge_completion(2);check(!later.unread_generation(),"acknowledged parked generation is absent");
    summary={};summary.add(true,std::uint64_t{1},nullptr,SessionState::failed);
    check(summary.failed==1 && summary.background_finished==1 && !summary.has_watches(),"generic parked completion and lifecycle failure stay distinct");
    check(activity_priority(&success,SessionState::failed,false)>activity_priority(&watch,SessionState::running,false),"actor failure outranks command result");
    check(activity_priority(&watch,SessionState::running,false)>activity_priority(&unknown,SessionState::running,false),"nonzero precedes unknown");
    NavigatorTarget target{{4},{7},false,actor};
    const auto allowed=[&](WatchIdentity identity, bool placed=true, bool parked=false, bool capacity=true){return activity_target_availability(target,{4},{4},identity,placed,parked,capacity).enabled;};
    check(allowed(actor),"retained actor is navigable even if result unknown or process exited");
    for(const auto identity:{WatchIdentity{},WatchIdentity{2,7,3},WatchIdentity{1,8,3},WatchIdentity{1,7,4}})check(!allowed(identity),"all stale incarnation dimensions rejected");
    check(!allowed(actor,false) && !allowed(actor,true,true),"missing/moved placement rejected");
    check(!activity_target_availability(target,{4},{5},actor,true,false,true).enabled,"active workspace cannot silently change");
    check(!activity_target_availability(target,{5},{5},actor,true,false,true).enabled,"scope cannot escape opening workspace");
    target.parked=true;check(!allowed(actor,true,true,false) && allowed(actor,true,true,true),"restore checks tab capacity without clearing result");
    check(watch.unread() && unknown.unread() && success.unread(),"presentation and availability never acknowledge Watches");
}

void activity_refresh_selection() {
    const auto rows=[](WatchIdentity identity=actor,bool parked=false) {
        return std::vector<NavigatorWorkspace>{{{4},"Work","/work",{
            {{7},"Watched","/work","Running",parked,true,identity},
            {{8},"Other","/work","Running",false,false,{1,8,9}}
        }}};
    };
    WorkspaceNavigator nav;
    nav.refresh(rows(),{4},{7});
    const auto original=nav.target();
    check(original && original->identity==actor && !nav.selection_stale(),"initial Activity refresh selects owned actor");
    auto reordered=rows();std::reverse(reordered[0].terminals.begin(),reordered[0].terminals.end());
    nav.refresh(reordered,{4},{8});
    check(nav.target()==original && !nav.selection_stale(),"refresh preserves actor through sorting and changed active pane");
    nav.refresh(rows({1,7,4}),{4},{7});
    check(nav.selection_stale() && !nav.target(),"refresh then Enter cannot select replacement incarnation");
    nav.refresh(rows(),{4},{7});nav.search("");
    check(nav.selection_stale() && !nav.target(),"repeat refresh and unchanged query cannot clear stale guard");
    nav.select_terminal(0);
    check(nav.target()==original && !nav.selection_stale(),"explicit row choice recovers current actor");
    nav.refresh(rows(actor,true),{4},{7});
    check(nav.selection_stale() && !nav.target(),"refresh then Enter cannot silently restore newly parked row");
    nav.move(false,1);
    check(nav.target() && nav.target()->session==SessionId{8} && !nav.selection_stale(),"explicit keyboard selection recovers");
    nav.select_terminal(0);auto removed=rows();removed[0].terminals.erase(removed[0].terminals.begin());
    nav.refresh(removed,{4},{7});
    check(nav.selection_stale() && !nav.target(),"removed selection does not authorize fallback row");
    nav.search("Other");
    check(nav.target() && nav.target()->session==SessionId{8},"changed search explicitly chooses filtered row");
    nav.refresh({}, {4},{7});nav.refresh(rows(),{4},{7});
    check(nav.selection_stale() && !nav.target(),"empty catalog and repopulation preserve stale guard");
    nav.open(rows({1,7,4}),{4},{7});
    check(nav.target() && nav.target()->identity==WatchIdentity{1,7,4} && !nav.selection_stale(),"fresh open resets guard for global navigator compatibility");
    WorkspaceNavigator empty;empty.refresh({{{4},"Empty","/empty",{}}},{4},{});
    empty.refresh(rows(),{4},{7});
    check(empty.selection_stale() && !empty.target(),"arriving row in empty Activity needs explicit selection");
}

}
int main() {
    try { eligibility(); exact_and_coalesced(); interruption_and_identity(); parked_generation_acknowledgment(); placement(); activity(); activity_refresh_selection(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "command_watch_tests passed\n";
}
