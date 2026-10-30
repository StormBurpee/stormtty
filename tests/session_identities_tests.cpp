#include "core_test.hpp"
#include "stormtty/session_identities.hpp"

#include <atomic>
#include <limits>
#include <type_traits>

using namespace stormtty;

namespace {
static_assert(!std::is_copy_constructible_v<SessionIdentities>);

void session_exhaustion() {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t next = maximum - 1;
    CHECK(detail::allocate_session_identity(next) == SessionId{maximum - 1});
    CHECK(detail::allocate_session_identity(next) == SessionId{maximum});
    CHECK(next == 0);

    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        bool exhausted = false;
        try {
            (void)detail::allocate_session_identity(next);
        } catch (const std::runtime_error&) {
            exhausted = true;
        }
        CHECK(exhausted);
        CHECK(next == 0);
    }
}

void watch_exhaustion() {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t incarnation = maximum - 1;
    CHECK(detail::allocate_watch_incarnation(incarnation) == maximum - 1);
    CHECK(incarnation == maximum);
    CHECK(detail::allocate_watch_incarnation(incarnation) == 0);
    CHECK(detail::allocate_watch_incarnation(incarnation) == 0);
    CHECK(incarnation == maximum);

    std::atomic<std::uint64_t> host{maximum - 1};
    CHECK(detail::allocate_watch_host(host) == maximum - 1);
    CHECK(host == maximum);
    CHECK(detail::allocate_watch_host(host) == 0);
    CHECK(detail::allocate_watch_host(host) == 0);
    CHECK(host == maximum);
}

void registration_and_missing_identity() {
    SessionIdentities identities;
    const auto first = identities.allocate();
    const auto second = identities.allocate();
    CHECK(first == SessionId{1});
    CHECK(second == SessionId{2});
    CHECK(!identities.find(first));
    CHECK(identities.find(first).session == first.value);
    CHECK(!identities.find({}));

    // Allocating or abandoning an ID never manufactures an actor incarnation.
    const auto abandoned = identities.allocate();
    identities.forget(abandoned);
    identities.forget(abandoned);
    CHECK(!identities.find(abandoned));

    identities.register_actor(first);
    const auto first_identity = identities.find(first);
    identities.register_actor(second);
    const auto second_identity = identities.find(second);
    CHECK(first_identity);
    CHECK(second_identity);
    CHECK(first_identity.host == second_identity.host);
    CHECK(first_identity.actor != second_identity.actor);
    CHECK(identities.find(first) == first_identity);

    identities.forget(first);
    CHECK(!identities.find(first));
    CHECK(identities.find(second) == second_identity);
    identities.forget(first);
    CHECK(identities.find(second) == second_identity);

    // Even a deliberately reused local ID cannot regain a stale incarnation.
    identities.register_actor(first);
    CHECK(identities.find(first));
    CHECK(identities.find(first).host == first_identity.host);
    CHECK(identities.find(first) != first_identity);

    SessionIdentities another_application;
    const auto another = another_application.allocate();
    another_application.register_actor(another);
    CHECK(another_application.find(another).host != first_identity.host);
}
} // namespace

int main() {
    return run_core_tests("session_identities", [] {
        session_exhaustion();
        watch_exhaustion();
        registration_and_missing_identity();
    });
}
