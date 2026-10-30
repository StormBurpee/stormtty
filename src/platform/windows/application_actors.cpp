#include "application_actors.hpp"

#include <utility>

namespace stormtty::windows {

ActorLifetimeRegistry::~ActorLifetimeRegistry() {
    for (const auto& [id, actor] : sessions_) {
        (void)id;
        actor->request_close();
    }
}

SessionId ActorLifetimeRegistry::allocate() {
    return identities_.allocate();
}

Session::Wake ActorLifetimeRegistry::callback(
    SessionId id,
    const std::shared_ptr<WakeState>& endpoint) {
    auto route = wakes_.bind(id, endpoint);
    return [route = std::move(route)] {
        route->wake();
    };
}

void ActorLifetimeRegistry::register_actor(SessionId id, std::shared_ptr<Session> actor) {
    // Handle insertion precedes incarnation issuance/insertion. Do not silently
    // turn this into an atomic registration or roll back its existing failure path.
    sessions_.emplace(id, std::move(actor));
    identities_.register_actor(id);
}

Session* ActorLifetimeRegistry::find(SessionId id) const noexcept {
    const auto found = sessions_.find(id);
    return found == sessions_.end() ? nullptr : found->second.get();
}

std::shared_ptr<Session> ActorLifetimeRegistry::retain(SessionId id) const {
    return sessions_.at(id);
}

bool ActorLifetimeRegistry::contains(SessionId id) const noexcept {
    return sessions_.contains(id);
}

std::size_t ActorLifetimeRegistry::size() const noexcept {
    return sessions_.size();
}

bool ActorLifetimeRegistry::empty() const noexcept {
    return sessions_.empty();
}

WatchIdentity ActorLifetimeRegistry::identity(SessionId id) const noexcept {
    return identities_.find(id);
}

std::vector<ActorLifetimeRegistry::SnapshotEntry> ActorLifetimeRegistry::snapshot() const {
    std::vector<SnapshotEntry> result;
    result.reserve(sessions_.size());
    for (const auto& [id, actor] : sessions_) {
        result.push_back({id, actor, identities_.find(id)});
    }
    return result;
}

void ActorLifetimeRegistry::dispose(
    SessionId id,
    const std::shared_ptr<WakeState>& completion) {
    if (completion) {
        // Rebind before close, including synchronous/final actor wake delivery.
        wakes_.bind(id, completion);
    }
    if (const auto found = sessions_.find(id); found != sessions_.end()) {
        found->second->request_close();
        sessions_.erase(found);
    }
    identities_.forget(id);
    // A failed start may have a route without an actor. Erase discoverability
    // only: retained callbacks keep their route and last weak endpoint.
    wakes_.erase(id);
}

void ApplicationActors::dispose(
    SessionId id,
    const std::shared_ptr<WakeState>& completion) {
    registry.dispose(id, completion);
    observations.forget(id);
    attention.forget(id);
}

} // namespace stormtty::windows
