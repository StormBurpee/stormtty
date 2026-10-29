#pragma once
#include "stormtty/session.hpp"
#include <cstdint>
#include <optional>

namespace stormtty {
// Host-owned, memory-only identity. actor is a non-reused incarnation token,
// NOT a raw pointer or process id. SessionInfo does not supply this identity.
struct WatchIdentity {
    std::uint64_t host{}, session{}, actor{};
    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return host != 0 && session != 0 && actor != 0;
    }
    bool operator==(const WatchIdentity&) const = default;
};
enum class WatchStatus { idle, watching, completed, completion_unconfirmed, interrupted, unavailable };
enum class WatchSurface { none, watch, parked };

// One host-owned instance per live session; no output/history, process control,
// timers, notification transport, or persistence. All calls on the host thread.
class CommandWatch {
public:
    [[nodiscard]] static bool eligible(WatchIdentity identity, const SessionInfo& info,
                                       bool foreground) noexcept {
        return identity && foreground && info.state == SessionState::running &&
            info.command.phase == CommandPhase::running &&
            info.command.command_generation != 0 &&
            info.command.completed_generation < info.command.command_generation;
    }
    // Existing watch/results must be explicitly canceled/dismissed first.
    bool arm(WatchIdentity identity, const SessionInfo& info, bool foreground) noexcept {
        if (status_ != WatchStatus::idle || !eligible(identity, info, foreground)) return false;
        identity_ = identity;
        generation_ = info.command.command_generation;
        status_ = WatchStatus::watching;
        return true;
    }
    // True only for the one transition to a result, not duplicate snapshots.
    bool observe(WatchIdentity identity, const SessionInfo& info) noexcept {
        if (status_ == WatchStatus::idle) return false;
        if (identity != identity_) {
            // Reused session IDs must never inherit the old actor's result.
            cancel();
            return false;
        }
        if (status_ != WatchStatus::watching) return false;
        const auto& command = info.command;
        if (command.command_generation < generation_ ||
            command.completed_generation > command.command_generation) {
            status_ = WatchStatus::unavailable;
        } else if (command.completed_generation == generation_) {
            // This D belongs to the watched command, even after C-D-C or EOF.
            status_ = WatchStatus::completed;
            exit_code_ = command.last_exit_code;
        } else if (info.state == SessionState::failed) {
            status_ = WatchStatus::unavailable;
        } else if (command.completed_generation > generation_) {
            // Latest-state publication is not a lossless event log. A reset
            // could have interrupted our command before a later D. Never borrow
            // the later command's exit code or describe our command as success.
            status_ = WatchStatus::completion_unconfirmed;
        } else if (command.command_generation > generation_ ||
                   command.phase == CommandPhase::interrupted ||
                   info.state == SessionState::closing || info.state == SessionState::exited) {
            status_ = WatchStatus::interrupted;
        } else if (command.phase != CommandPhase::running || info.state != SessionState::running) {
            status_ = WatchStatus::unavailable;
        }
        return status_ != WatchStatus::watching;
    }
    // Neither operation sends input, stops a process, or changes placement.
    void cancel() noexcept { *this = CommandWatch{}; }
    // Only explicit Open result (after successful navigation) or Dismiss calls
    // this. Merely displaying/restoring a workspace is not acknowledgment.
    bool acknowledge() noexcept {
        if (!unread()) return false;
        cancel();
        return true;
    }
    [[nodiscard]] WatchStatus status() const noexcept { return status_; }
    [[nodiscard]] WatchIdentity identity() const noexcept { return identity_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::optional<std::int32_t> exit_code() const noexcept { return exit_code_; }
    [[nodiscard]] bool unread() const noexcept {
        return status_ != WatchStatus::idle && status_ != WatchStatus::watching;
    }
    // Transfer representation to the existing parked row rather than adding a
    // second badge/toast. The parked row ORs this with ParkedAttention's unread
    // bit, never sums them. Restoring changes surface, not acknowledgment.
    [[nodiscard]] WatchSurface surface(bool same_actor_parked) const noexcept {
        return !unread() ? WatchSurface::none :
            same_actor_parked ? WatchSurface::parked : WatchSurface::watch;
    }
private:
    WatchIdentity identity_{};
    std::uint64_t generation_{};
    WatchStatus status_{WatchStatus::idle};
    std::optional<std::int32_t> exit_code_;
};
} // namespace stormtty
