#pragma once

#include "native_wake.hpp"
#include "stormtty/session.hpp"
#include "stormtty/session_attention.hpp"
#include "stormtty/session_identities.hpp"
#include "stormtty/session_observation_cache.hpp"
#include "stormtty/session_wake_router.hpp"
#include <map>
#include <memory>
#include <ranges>
#include <vector>

namespace stormtty::windows {

// UI-thread owner of registration and wake-route discoverability. Actors and
// retained callbacks may outlive registration. There is no explicit actor join;
// releasing the last shared reference retains Session's destruction behavior.
// Process launch remains the host's creation transaction.
class ActorLifetimeRegistry final {
public:
    struct SnapshotEntry {
        SessionId id;
        std::shared_ptr<Session> actor;
        WatchIdentity identity;

        bool operator==(const SnapshotEntry&) const = default;
    };

    ~ActorLifetimeRegistry();

    [[nodiscard]] SessionId allocate();
    Session::Wake callback(SessionId id, const std::shared_ptr<WakeState>& endpoint);
    void register_actor(SessionId id, std::shared_ptr<Session> actor);

    // Borrowed lookup/range: UI thread only. Do not retain a reference across
    // erasure or reentrant host work. Removal passes advance before disposing.
    // These hot paths allocate nothing and do not copy shared ownership.
    [[nodiscard]] Session* find(SessionId id) const noexcept;
    [[nodiscard]] auto read() const noexcept {
        return std::ranges::subrange(sessions_.cbegin(), sessions_.cend());
    }

    // Explicit ownership for queued actions. Missing registration is an error.
    [[nodiscard]] std::shared_ptr<Session> retain(SessionId id) const;
    [[nodiscard]] bool contains(SessionId id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] WatchIdentity identity(SessionId id) const noexcept;

    // Existing owned qualification snapshot, never a per-frame view.
    [[nodiscard]] std::vector<SnapshotEntry> snapshot() const;

    void dispose(SessionId id, const std::shared_ptr<WakeState>& completion = {});

private:
    // Preserve declaration order, including failure/destructor unwind.
    SessionIdentities identities_;
    std::map<SessionId, std::shared_ptr<Session>> sessions_;
    SessionWakeRouter<WakeState> wakes_;
};

struct ApplicationActors {
    SessionObservationCache observations;
    SessionAttention attention;
    ActorLifetimeRegistry registry; // Closes before presentation metadata dies.

    // The one coordinator for registration, observation and attention cleanup.
    void dispose(SessionId id, const std::shared_ptr<WakeState>& completion = {});
};

} // namespace stormtty::windows
