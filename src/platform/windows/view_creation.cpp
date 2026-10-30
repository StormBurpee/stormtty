#include "view_creation.hpp"

#include <stdexcept>
#include <utility>

namespace stormtty::windows {

ViewCreation::ViewCreation(ApplicationWorkspaces& projects, ApplicationActors& actors)
    : projects_(projects), actors_(actors) {
}

WindowId ViewCreation::begin() {
    if (active()) {
        throw std::runtime_error("A window is already being prepared.");
    }
    const auto staged = projects_.begin_window_creation();
    if (!staged) {
        throw std::runtime_error(staged.error);
    }
    candidate_ = *staged.value;
    phase_ = Phase::staging;
    dirty_ = false;
    return candidate_;
}

void ViewCreation::record_actor(WindowId candidate, SessionId actor) {
    if (phase_ != Phase::staging || candidate != candidate_) {
        throw std::logic_error("Actor does not belong to the unpublished candidate.");
    }
    // Allocate the exact ledger entry before process launch or route creation.
    // A later launch/registration failure still needs metadata/route disposal.
    created_.push_back(actor);
}

void ViewCreation::commit_catalog() {
    if (phase_ != Phase::staging) {
        throw std::logic_error("No unpublished candidate catalog to commit.");
    }
    const auto committed = projects_.commit_window_creation(candidate_);
    if (!committed) {
        throw std::runtime_error(committed.error);
    }
    created_.clear();
    phase_ = Phase::publishing;
}

bool ViewCreation::finish_publication() {
    if (phase_ != Phase::publishing) {
        throw std::logic_error("Candidate ownership has not reached publication.");
    }
    phase_ = Phase::idle;
    return std::exchange(dirty_, false);
}

bool ViewCreation::rollback_after_candidate_destruction() {
    if (phase_ != Phase::staging) {
        throw std::logic_error("Only an unpublished candidate can be rolled back.");
    }
    projects_.abort_window_creation(candidate_);
    for (const auto id : created_) {
        actors_.dispose(id);
    }
    created_.clear();
    phase_ = Phase::idle;
    return std::exchange(dirty_, false);
}

bool ViewCreation::defer_model_change() {
    if (!active()) {
        return false;
    }
    dirty_ = true;
    return true;
}

bool ViewCreation::active() const {
    return phase_ != Phase::idle;
}

} // namespace stormtty::windows
