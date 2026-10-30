#pragma once

#include "stormtty/command_watch.hpp"
#include "stormtty/parked_attention.hpp"
#include "stormtty/workspace.hpp"

#include <map>
#include <ranges>
#include <span>
#include <vector>

namespace stormtty {

// Application-owned, UI-thread-confined attention policy. The host supplies
// current actor identity and commits placement/navigation before notifying us.
// No Session handles, workspace mutation, notification delivery or persistence.
class SessionAttention {
public:
    bool arm_watch(
        SessionId session,
        WatchIdentity identity,
        const SessionInfo& info,
        bool foreground) {
        if (identity.session != session.value || watches_.contains(session)) {
            return false;
        }

        CommandWatch watch;
        if (!watch.arm(identity, info, foreground)) {
            return false;
        }

        watches_.emplace(session, std::move(watch));
        return true;
    }

    bool cancel_watch(SessionId session) {
        const auto found = watches_.find(session);
        if (found == watches_.end() || found->second.status() != WatchStatus::watching) {
            return false;
        }

        watches_.erase(found);
        return true;
    }

    // Changed includes removal on identity mismatch. CommandWatch::observe
    // itself returns false for that reset, but the host must redraw its chrome.
    bool observe_watch(
        SessionId session,
        WatchIdentity identity,
        const SessionInfo& info) {
        const auto found = watches_.find(session);
        if (found == watches_.end()) {
            return false;
        }

        const auto before = found->second.status();
        found->second.observe(identity, info);
        if (found->second.status() == WatchStatus::idle) {
            watches_.erase(found);
            return true;
        }

        return before != found->second.status();
    }

    void baseline_after_park(SessionId session, const SessionInfo& baseline) {
        parked_.insert_or_assign(session, ParkedAttention(baseline));
    }

    // Only successful ordinary restore clears parked attention; an unread Watch
    // is independent and survives until explicit exact-result acknowledgment.
    void restored(SessionId session) {
        parked_.erase(session);
    }

    ParkedNotice observe_parked(SessionId session, const SessionInfo& info) {
        // Recovery without a baseline is quiet, never a replay of old results.
        const auto found = parked_.try_emplace(session, info).first;
        return found->second.observe(info);
    }

    bool acknowledge_watch(
        SessionId session,
        WatchIdentity identity,
        std::uint64_t generation) {
        const auto found = watches_.find(session);
        if (found == watches_.end() || found->second.identity() != identity ||
            found->second.generation() != generation || !found->second.unread()) {
            return false;
        }

        if (const auto attention = parked_.find(session); attention != parked_.end()) {
            attention->second.acknowledge_completion(generation);
        }
        watches_.erase(found);
        return true;
    }

    void forget(SessionId session) {
        watches_.erase(session);
        parked_.erase(session);
    }

    // Raw presence is intentionally distinct from current-actor presentation.
    // Existing command/menu availability uses stored state until observation
    // removes a stale incarnation; it must not implicitly acknowledge anything.
    [[nodiscard]] const CommandWatch* stored_watch(SessionId session) const {
        const auto found = watches_.find(session);
        return found == watches_.end() ? nullptr : &found->second;
    }

    [[nodiscard]] const CommandWatch* current_watch(
        SessionId session,
        WatchIdentity identity) const {
        const auto* watch = stored_watch(session);
        return watch && watch->identity() == identity ? watch : nullptr;
    }

    [[nodiscard]] const ParkedAttention* parked(SessionId session) const {
        const auto found = parked_.find(session);
        return found == parked_.end() ? nullptr : &found->second;
    }

    // Borrowed read-only range; do not retain across mutation or reentrant host work.
    [[nodiscard]] auto watch_entries() const {
        return std::ranges::subrange(watches_.cbegin(), watches_.cend());
    }

    // Failure-only native qualification rollback. Opaque and scoped to the
    // fixture's sessions; never exposes mutable policy or restores Watches.
    class QualificationParkedCheckpoint {
    public:
        QualificationParkedCheckpoint(QualificationParkedCheckpoint&&) = default;

    private:
        friend class SessionAttention;
        QualificationParkedCheckpoint() = default;
        std::vector<SessionId> sessions_;
        std::map<SessionId, ParkedAttention> entries_;
    };

    [[nodiscard]] QualificationParkedCheckpoint qualification_parked_checkpoint(
        std::span<const SessionId> sessions) const {
        QualificationParkedCheckpoint checkpoint;
        checkpoint.sessions_.assign(sessions.begin(), sessions.end());
        for (const auto session : sessions) {
            if (const auto* attention = parked(session)) {
                checkpoint.entries_.emplace(session, *attention);
            }
        }

        return checkpoint;
    }

    // The fixture validates that its actor incarnations still match first.
    // Transfer preallocated nodes so failure rollback cannot allocate while
    // unwinding. Missing saved entries mean the session was not parked before.
    void restore_qualification_parked_checkpoint(
        QualificationParkedCheckpoint&& checkpoint) noexcept {
        for (const auto session : checkpoint.sessions_) {
            parked_.erase(session);
        }

        while (!checkpoint.entries_.empty()) {
            parked_.insert(checkpoint.entries_.extract(checkpoint.entries_.begin()));
        }

        checkpoint.sessions_.clear();
    }

private:
    std::map<SessionId, CommandWatch> watches_;
    std::map<SessionId, ParkedAttention> parked_;
};

} // namespace stormtty
