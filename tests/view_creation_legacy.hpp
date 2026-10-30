#pragma once

#include "../src/platform/windows/application_actors.hpp"
#include "stormtty/application_workspaces.hpp"
#include <stdexcept>
#include <utility>

namespace stormtty::test {

// Independent characterization of NativeApplication::create_view at3350a761.
// Native candidate construction/destruction and publication are driven by the
// test/host between these explicit boundaries, never by callbacks or destructors.
class LegacyViewCreation {
public:
    LegacyViewCreation(ApplicationWorkspaces& projects, windows::ApplicationActors& actors)
        : projects_(projects), actors_(actors) {
    }

    WindowId begin() {
        if (active_) {
            throw std::runtime_error("A window is already being prepared.");
        }
        const auto staged = projects_.begin_window_creation();
        if (!staged) {
            throw std::runtime_error(staged.error);
        }
        id_ = *staged.value;
        active_ = true;
        dirty_ = false;
        return id_;
    }

    void record_actor(WindowId candidate, SessionId actor) {
        if (candidate != id_) {
            throw std::logic_error("Wrong candidate ledger");
        }
        created_.push_back(actor);
    }

    void commit_catalog() {
        const auto committed = projects_.commit_window_creation(id_);
        if (!committed) {
            throw std::runtime_error(committed.error);
        }
        created_.clear();
    }

    bool finish_publication() {
        active_ = false;
        return std::exchange(dirty_, false);
    }

    bool rollback_after_candidate_destruction() {
        projects_.abort_window_creation(id_);
        for (const auto id : created_) {
            actors_.dispose(id);
        }
        created_.clear();
        active_ = false;
        return std::exchange(dirty_, false);
    }

    bool defer_model_change() {
        if (!active_) {
            return false;
        }
        dirty_ = true;
        return true;
    }

    bool active() const {
        return active_;
    }

private:
    ApplicationWorkspaces& projects_;
    windows::ApplicationActors& actors_;
    WindowId id_;
    std::vector<SessionId> created_;
    bool active_{};
    bool dirty_{};
};

} // namespace stormtty::test
