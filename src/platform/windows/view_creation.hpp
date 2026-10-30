#pragma once

#include "application_actors.hpp"
#include "stormtty/application_workspaces.hpp"

namespace stormtty::windows {

// UI-thread unpublished-view transaction. This owner never constructs, stores,
// destroys or publishes a native view. The host explicitly destroys its candidate
// before rollback and finishes publication before notifying persistence.
class ViewCreation final {
public:
    ViewCreation(ApplicationWorkspaces& projects, ApplicationActors& actors);

    ViewCreation(const ViewCreation&) = delete;
    ViewCreation& operator=(const ViewCreation&) = delete;

    WindowId begin();
    void record_actor(WindowId candidate, SessionId actor);
    void commit_catalog();

    // Return whether deferred model changes need a host notification. Neither
    // operation calls back into the host, schedules I/O, or catches its errors.
    bool finish_publication();
    bool rollback_after_candidate_destruction();
    bool defer_model_change();
    bool active() const;

private:
    enum class Phase { idle, staging, publishing };

    ApplicationWorkspaces& projects_;
    ApplicationActors& actors_;
    WindowId candidate_;
    std::vector<SessionId> created_;
    Phase phase_{Phase::idle};
    bool dirty_{};
};

} // namespace stormtty::windows
