#pragma once

#include "stormtty/session.hpp"
#include "stormtty/workspace.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace stormtty {

// Application-owned, UI-thread-confined last observations for chrome invalidation.
// These are not authoritative Session state. Windows share this history even when
// their scoped project catalogs, profile configuration or placement differ.
class SessionObservationCache {
public:
    bool observe_state(SessionId session, SessionState state) {
        const auto [entry, inserted] = states_.try_emplace(session, state);
        if (!inserted && entry->second == state) {
            return false;
        }

        entry->second = state;
        return true;
    }

    bool observe_background_label(SessionId session, std::string_view label) {
        // Preserve the original operator[] policy: the first empty label is
        // recorded, but does not invalidate. First state observation does.
        auto& previous = background_labels_[session];
        if (previous == label) {
            return false;
        }

        previous = label;
        return true;
    }

    std::optional<SessionState> state(SessionId session) const {
        const auto found = states_.find(session);
        if (found == states_.end()) {
            return std::nullopt;
        }

        return found->second;
    }

    // Borrowed read-only value; do not retain across observation, forgetting,
    // qualification restoration or reentrant host operations.
    const std::string* background_label(SessionId session) const {
        const auto found = background_labels_.find(session);
        if (found == background_labels_.end()) {
            return nullptr;
        }

        return &found->second;
    }

    void forget(SessionId session) {
        states_.erase(session);
        background_labels_.erase(session);
    }

    class QualificationBackgroundLabelsCheckpoint {
    public:
        QualificationBackgroundLabelsCheckpoint(QualificationBackgroundLabelsCheckpoint&&) noexcept = default;

    private:
        friend class SessionObservationCache;
        QualificationBackgroundLabelsCheckpoint() = default;
        // Capture may allocate; moving the capsule must not depend on whether
        // the platform's std::map move constructor allocates a sentinel node.
        std::unique_ptr<std::map<SessionId, std::string>> labels_;
    };

    // Activity's failure-only rollback historically snapshots ALL application
    // labels, including other windows, but never states. Keep that exact scope;
    // this is not an actor-identity-checked restoration or production operation.
    [[nodiscard]] QualificationBackgroundLabelsCheckpoint qualification_background_labels_checkpoint() const {
        QualificationBackgroundLabelsCheckpoint checkpoint;
        checkpoint.labels_ = std::make_unique<std::map<SessionId, std::string>>(background_labels_);
        return checkpoint;
    }

    void restore_qualification_background_labels_checkpoint(
        QualificationBackgroundLabelsCheckpoint&& checkpoint) noexcept {
        auto labels = std::move(checkpoint.labels_);
        if (!labels) {
            return;
        }

        background_labels_.swap(*labels);
    }

private:
    std::map<SessionId, SessionState> states_;
    std::map<SessionId, std::string> background_labels_;
};

} // namespace stormtty
