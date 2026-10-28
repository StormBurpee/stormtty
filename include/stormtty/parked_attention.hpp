#pragma once
#include "stormtty/session.hpp"

namespace stormtty {
enum class ParkedNotice { none, command_finished, process_exited, process_failed };

// A host may have only one notice slot. Pick the strongest event in the entire
// update before publishing it, so session iteration order cannot hide a failure.
class ParkedNoticeBatch {
public:
    bool consider(ParkedNotice candidate) noexcept {
        if (priority(candidate) <= priority(selected_)) return false;
        selected_ = candidate;
        return true;
    }
    [[nodiscard]] ParkedNotice selected() const noexcept { return selected_; }
private:
    static constexpr unsigned priority(ParkedNotice value) noexcept {
        switch (value) {
        case ParkedNotice::process_failed: return 3;
        case ParkedNotice::process_exited: return 2;
        case ParkedNotice::command_finished: return 1;
        default: return 0;
        }
    }
    ParkedNotice selected_{ParkedNotice::none};
};

// One constant-size, display-only value per parked SessionId, owned by the host.
// Not placement persistence, process control, or a lossless command event queue.
class ParkedAttention {
public:
    explicit ParkedAttention(const SessionInfo& baseline) noexcept
        : completed_(baseline.command.completed_generation), process_state_(baseline.state) {}

    [[nodiscard]] bool unread_completion() const noexcept { return unread_; }
    ParkedNotice observe(const SessionInfo& info) noexcept {
        const bool completed = info.command.completed_generation > completed_;
        if (completed) {
            completed_ = info.command.completed_generation;
            unread_ = true;
        }
        const bool process_changed = process_state_ != info.state;
        process_state_ = info.state;
        // Failure wins even when C/D and process exit arrive in the same wake.
        if (process_changed && info.state == SessionState::failed) return ParkedNotice::process_failed;
        if (process_changed && info.state == SessionState::exited) {
            // Suppress only the same explicitly completed command/result already
            // notified. A new running/interrupted command or different process
            // result is a distinct exit, and must not be hidden.
            const bool same_completion = info.command.phase == CommandPhase::completed &&
                info.command.command_generation == info.command.completed_generation &&
                info.exit_code && info.command.last_exit_code &&
                *info.command.last_exit_code >= 0 &&
                *info.exit_code == static_cast<std::uint32_t>(*info.command.last_exit_code);
            if (same_completion && completed) {
                notified_ = completed_;
                return ParkedNotice::command_finished;
            }
            if (same_completion && notified_ == completed_ && notified_ != 0) return ParkedNotice::none;
            return ParkedNotice::process_exited;
        }
        if (completed) {
            notified_ = completed_;
            return ParkedNotice::command_finished;
        }
        return ParkedNotice::none;
    }
private:
    std::uint64_t completed_{}, notified_{};
    SessionState process_state_{};
    bool unread_{};
};
} // namespace stormtty
