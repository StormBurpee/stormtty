#include "stormtty/session_attention.hpp"
#include "stormtty/workspace_ribbon.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using namespace stormtty;

namespace {

constexpr SessionId first{7};
constexpr SessionId second{8};
constexpr WatchIdentity actor{1, first.value, 3};
constexpr WatchIdentity other{1, second.value, 4};

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

SessionInfo running(std::uint64_t generation = 1) {
    SessionInfo info;
    info.state = SessionState::running;
    info.command.phase = CommandPhase::running;
    info.command.command_generation = generation;
    return info;
}

SessionInfo completed(std::uint64_t generation = 1) {
    auto info = running(generation);
    info.command.phase = CommandPhase::completed;
    info.command.completed_generation = generation;
    info.command.last_exit_code = 0;
    return info;
}

void identity_and_queries() {
    SessionAttention attention;
    check(!attention.arm_watch(first, other, running(), true), "session and identity must agree");
    check(!attention.arm_watch(first, actor, running(), false), "background arm rejected");
    check(attention.arm_watch(first, actor, running(), true), "foreground arm accepted");
    check(!attention.arm_watch(first, actor, running(2), true), "one stored watch per session");

    const WatchIdentity replacement{actor.host, actor.session, actor.actor + 1};
    check(attention.stored_watch(first), "raw menu query retains stored watch");
    check(!attention.current_watch(first, replacement), "current presentation rejects stale incarnation");
    check(attention.current_watch(first, actor), "same actor presentation available");
    check(!attention.observe_watch(first, actor, running()), "duplicate running snapshot quiet");
    check(attention.observe_watch(first, replacement, completed()), "identity reset reports changed");
    check(!attention.stored_watch(first), "identity reset removes stored entry");
    check(!attention.observe_watch(first, replacement, completed()), "missing observation quiet");
    check(attention.arm_watch(first, replacement, running(2), true), "replacement can explicitly arm");
    check(attention.observe_watch(first, replacement, completed(2)), "replacement result arrives");
    check(!attention.acknowledge_watch(first, actor, 2), "old incarnation cannot acknowledge");
    check(!attention.cancel_watch(first), "cancel is not unread result acknowledgment");

    static_assert(std::is_same_v<decltype(attention.stored_watch(first)), const CommandWatch*>);
    static_assert(std::is_same_v<decltype(attention.parked(first)), const ParkedAttention*>);
    for (const auto& [session, watch] : attention.watch_entries()) {
        check(session == first && watch.identity() == replacement, "immutable range retains identity");
    }
}

void exact_result_and_isolation() {
    SessionAttention attention;
    check(attention.arm_watch(first, actor, running(), true), "first watch armed");
    check(attention.arm_watch(second, other, running(), true), "other watch armed");
    attention.baseline_after_park(first, running());
    attention.baseline_after_park(second, running());
    check(attention.observe_watch(first, actor, completed()), "watch result changes once");
    check(!attention.observe_watch(first, actor, completed()), "watch result duplicate quiet");
    check(attention.observe_parked(first, completed()) == ParkedNotice::command_finished, "parked completion");
    check(attention.observe_parked(second, completed()) == ParkedNotice::command_finished, "independent completion");
    check(!attention.acknowledge_watch(first, actor, 2), "wrong generation cannot acknowledge");
    check(attention.parked(first)->unread_completion(), "wrong acknowledgment preserves parked bit");
    check(attention.acknowledge_watch(first, actor, 1), "exact result acknowledged");
    check(!attention.stored_watch(first) && !attention.parked(first)->unread_completion(), "matching parked generation acknowledged together");
    check(!attention.acknowledge_watch(first, actor, 1), "repeated acknowledgment quiet");
    check(attention.parked(second)->unread_completion() && attention.stored_watch(second), "other session unchanged");

    check(attention.arm_watch(first, actor, running(2), true), "new command explicitly armed");
    check(attention.observe_watch(first, actor, completed(2)), "new watched result");
    check(attention.observe_parked(first, completed(3)) == ParkedNotice::command_finished, "newer parked completion");
    check(!attention.acknowledge_watch(first, actor, 1), "stale result action cannot clear new watch");
    check(attention.acknowledge_watch(first, actor, 2), "current watch can dismiss");
    check(attention.parked(first)->unread_generation() == 3, "dismiss never clears newer parked completion");
    check(attention.cancel_watch(second), "cancel waiting watch");
    check(attention.parked(second)->unread_completion(), "cancel preserves unrelated parked result");
    check(!attention.cancel_watch(second), "repeat cancel harmless");
    attention.forget(first);
    attention.forget(first);
    check(!attention.parked(first) && !attention.stored_watch(first), "repeat disposal forget harmless");
    check(attention.parked(second)->unread_completion(), "forget isolated to exact session");
}

void placement_and_visibility_contract() {
    SessionAttention attention;
    check(attention.arm_watch(first, actor, running(), true), "arm before park");
    // Host commits successful placement before creating this baseline, then
    // performs its normal update. A failed placement makes neither call.
    attention.baseline_after_park(first, completed());
    check(attention.observe_parked(first, completed()) == ParkedNotice::none, "baseline before first update does not replay old D");
    check(!attention.parked(first)->unread_completion(), "old completion remains read");
    check(attention.observe_watch(first, actor, completed()), "watch independently retains exact D");
    check(attention.observe_parked(first, completed(2)) == ParkedNotice::command_finished, "hidden project completion observed");

    // Display, preview, Escape and unsuccessful navigation perform only queries.
    for (int query = 0; query != 4; ++query) {
        check(attention.current_watch(first, actor)->unread(), "query must not acknowledge watch");
        check(attention.parked(first)->unread_completion(), "query must not acknowledge parked completion");
    }
    attention.restored(first);
    check(!attention.parked(first), "successful restore clears only parked state");
    check(attention.current_watch(first, actor)->unread(), "ordinary restore keeps unread watch");
    attention.restored(first);
    check(attention.acknowledge_watch(first, actor, 1), "successful same-actor Open can acknowledge exact result");

    check(attention.observe_parked(second, completed(5)) == ParkedNotice::none, "missing baseline fallback quiet");
    check(!attention.parked(second)->unread_completion(), "fallback does not invent unread history");
    attention.restored(second);
    attention.baseline_after_park(second, completed(6));
    check(attention.observe_parked(second, completed(6)) == ParkedNotice::none, "repark takes fresh baseline");
}

void notification_priority_and_dedup() {
    SessionAttention attention;
    attention.baseline_after_park(first, running());
    attention.baseline_after_park(second, running());
    check(attention.arm_watch(first, actor, running(), true), "dedup watch arm");
    attention.observe_watch(first, actor, completed());
    check(attention.observe_parked(first, completed()) == ParkedNotice::command_finished, "one parked completion notice");

    WorkspaceActivitySummary summary;
    summary.add(true, attention.parked(first)->unread_generation(), attention.current_watch(first, actor), SessionState::running);
    check(summary.unread() == 1, "same watch and parked completion count once");
    auto exited = completed();
    exited.state = SessionState::exited;
    exited.exit_code = 0;
    check(attention.observe_parked(first, exited) == ParkedNotice::none, "matching process exit does not repeat completion");
    check(attention.observe_parked(first, exited) == ParkedNotice::none, "repeated exit quiet");

    auto failed = completed();
    failed.state = SessionState::failed;
    const auto failure = attention.observe_parked(second, failed);
    check(failure == ParkedNotice::process_failed, "failure outranks concurrent completion");
    for (const auto order : {std::array{ParkedNotice::command_finished, ParkedNotice::process_exited, failure},
                             std::array{failure, ParkedNotice::process_exited, ParkedNotice::command_finished}}) {
        ParkedNoticeBatch batch;
        for (const auto notice : order) batch.consider(notice);
        check(batch.selected() == ParkedNotice::process_failed, "host batch priority independent of session order");
    }
    check(attention.observe_parked(second, failed) == ParkedNotice::none, "failure duplicate quiet");
}

void opaque_qualification_rollback() {
    SessionAttention attention;
    attention.baseline_after_park(first, running());
    attention.observe_parked(first, completed());
    constexpr SessionId initially_unparked{9};
    const std::array fixture{first, initially_unparked};
    auto checkpoint = attention.qualification_parked_checkpoint(fixture);
    attention.restored(first);
    attention.baseline_after_park(initially_unparked, running());
    attention.baseline_after_park(second, running());
    attention.observe_parked(second, completed(2));
    check(attention.arm_watch(first, actor, running(2), true), "watch outside parked checkpoint");
    static_assert(!std::is_copy_constructible_v<SessionAttention::QualificationParkedCheckpoint>);
    static_assert(std::is_move_constructible_v<SessionAttention::QualificationParkedCheckpoint>);
    static_assert(noexcept(attention.restore_qualification_parked_checkpoint(std::move(checkpoint))));
    attention.restore_qualification_parked_checkpoint(std::move(checkpoint));
    check(!attention.parked(initially_unparked), "rollback restores original absence");
    check(attention.parked(first)->unread_generation() == 1, "rollback preserves exact unread generation");
    check(attention.parked(second)->unread_generation() == 2, "rollback cannot replace unrelated project attention");
    check(attention.stored_watch(first)->generation() == 2, "rollback cannot replace watch policy");
    auto exited = completed();
    exited.state = SessionState::exited;
    exited.exit_code = 0;
    check(attention.observe_parked(first, exited) == ParkedNotice::none, "rollback preserves notification history");
    attention.restore_qualification_parked_checkpoint(std::move(checkpoint));
    check(attention.parked(first)->unread_generation() == 1, "consumed rollback is harmless");
}

} // namespace

int main() {
    try {
        identity_and_queries();
        exact_result_and_isolation();
        placement_and_visibility_contract();
        notification_priority_and_dedup();
        opaque_qualification_rollback();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "session_attention_tests passed\n";
}
