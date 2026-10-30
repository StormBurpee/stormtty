#pragma once

#include "stormtty/command_watch.hpp"
#include "stormtty/workspace.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>

namespace stormtty {
namespace detail {
// Session IDs issue the final value once; zero marks permanent exhaustion.
inline SessionId allocate_session_identity(std::uint64_t& next) {
    if (!next) {
        throw std::runtime_error("Application session identity exhausted.");
    }

    const SessionId id{next};
    next = next == std::numeric_limits<std::uint64_t>::max() ? 0 : next + 1;
    return id;
}

// Watch identities deliberately do not issue the final value. Saturation
// disables watches rather than recycling an incarnation or host identity.
inline std::uint64_t allocate_watch_incarnation(std::uint64_t& next) noexcept {
    return next == std::numeric_limits<std::uint64_t>::max() ? 0 : next++;
}

inline std::uint64_t allocate_watch_host(std::atomic<std::uint64_t>& next) noexcept {
    auto value = next.load();
    while (value != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(value, value + 1)) {
            return value;
        }
    }
    return 0;
}

inline std::uint64_t allocate_watch_host() noexcept {
    static std::atomic<std::uint64_t> next{1};
    return allocate_watch_host(next);
}
} // namespace detail

// Application-owned, UI-thread-only identity policy. Placement and process
// lifetime do not change an actor incarnation. No caller receives its maps.
class SessionIdentities final {
public:
    SessionIdentities() = default;
    SessionIdentities(const SessionIdentities&) = delete;
    SessionIdentities& operator=(const SessionIdentities&) = delete;

    [[nodiscard]] SessionId allocate() {
        return detail::allocate_session_identity(next_session_);
    }

    // Called only after the corresponding actor handle has been inserted.
    // Preserve issuance-before-map-insertion, including allocation failure.
    void register_actor(SessionId id) {
        const auto incarnation = detail::allocate_watch_incarnation(next_incarnation_);
        incarnations_.emplace(id, incarnation);
    }

    [[nodiscard]] WatchIdentity find(SessionId id) const noexcept {
        const auto found = incarnations_.find(id);
        return {host_, id.value, found == incarnations_.end() ? 0 : found->second};
    }

    void forget(SessionId id) {
        incarnations_.erase(id);
    }

private:
    const std::uint64_t host_{detail::allocate_watch_host()};
    std::uint64_t next_session_{1};
    std::uint64_t next_incarnation_{1};
    std::map<SessionId, std::uint64_t> incarnations_;
};
} // namespace stormtty
