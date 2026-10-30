#include "core_test.hpp"
#include "stormtty/session_observation_cache.hpp"

#include <array>
#include <type_traits>
#include <utility>

using namespace stormtty;

namespace {

constexpr SessionId first{7};
constexpr SessionId second{8};
constexpr SessionId later{9};

using Checkpoint = SessionObservationCache::QualificationBackgroundLabelsCheckpoint;
static_assert(std::is_nothrow_move_constructible_v<Checkpoint>);
static_assert(!std::is_copy_constructible_v<Checkpoint>);
static_assert(std::is_nothrow_destructible_v<Checkpoint>);
static_assert(noexcept(std::declval<std::map<SessionId, std::string>&>().swap(
    std::declval<std::map<SessionId, std::string>&>())));
static_assert(!std::is_default_constructible_v<Checkpoint>);
static_assert(noexcept(std::declval<SessionObservationCache&>()
    .restore_qualification_background_labels_checkpoint(std::declval<Checkpoint&&>())));
static_assert(std::is_same_v<decltype(std::declval<const SessionObservationCache&>()
    .background_label(first)), const std::string*>);

void state_observations() {
    constexpr std::array states{
        SessionState::starting,
        SessionState::running,
        SessionState::closing,
        SessionState::exited,
        SessionState::failed,
    };

    for (const auto initial : states) {
        SessionObservationCache cache;
        CHECK(!cache.state(first));
        CHECK(!cache.background_label(first));
        CHECK(cache.observe_state(first, initial));
        CHECK(cache.state(first) == initial);
        CHECK(!cache.observe_state(first, initial));
        CHECK(!cache.background_label(first));

        for (const auto next : states) {
            const auto previous = cache.state(first);
            CHECK(cache.observe_state(first, next) == (previous != next));
            CHECK(cache.state(first) == next);
            CHECK(!cache.observe_state(first, next));
        }
    }
}

void label_observations() {
    SessionObservationCache cache;
    CHECK(!cache.background_label(first));
    CHECK(!cache.observe_background_label(first, ""));
    CHECK(cache.background_label(first));
    CHECK(cache.background_label(first)->empty());
    CHECK(!cache.observe_background_label(first, ""));
    CHECK(!cache.state(first));

    std::string label = "Build · 日本語 · running";
    CHECK(cache.observe_background_label(first, label));
    label.clear();
    CHECK(*cache.background_label(first) == "Build · 日本語 · running");
    CHECK(!cache.observe_background_label(first, "Build · 日本語 · running"));
    CHECK(cache.observe_background_label(first, "Build · 日本語 · command finished"));
    CHECK(cache.observe_background_label(first, ""));
    CHECK(!cache.observe_background_label(first, ""));
    CHECK(cache.observe_background_label(second, "first nonempty label"));
    CHECK(!cache.state(second));
}

void shared_history_and_forgetting() {
    SessionObservationCache application;
    auto& first_view = application;
    auto& second_view = application;

    CHECK(first_view.observe_state(first, SessionState::running));
    CHECK(first_view.observe_background_label(first, "Profile A · running"));
    // A second view/transfer inherits observations, not a new first-observe edge.
    CHECK(!second_view.observe_state(first, SessionState::running));
    CHECK(!second_view.observe_background_label(first, "Profile A · running"));
    CHECK(second_view.observe_background_label(first, "Profile B · running"));
    CHECK(first_view.observe_background_label(first, "Profile A · running"));
    CHECK(!first_view.observe_state(first, SessionState::running));

    CHECK(second_view.observe_state(second, SessionState::failed));
    CHECK(second_view.observe_background_label(second, "survivor"));
    application.forget(first);
    application.forget(first);
    application.forget(later);
    CHECK(!application.state(first));
    CHECK(!application.background_label(first));
    CHECK(application.state(second) == SessionState::failed);
    CHECK(*application.background_label(second) == "survivor");
    CHECK(second_view.observe_state(first, SessionState::running));
    CHECK(!second_view.observe_background_label(first, ""));
    CHECK(second_view.background_label(first));
}

void inherited_failure_rollback() {
    SessionObservationCache cache;
    CHECK(cache.observe_state(first, SessionState::running));
    CHECK(cache.observe_background_label(first, "fixture"));
    CHECK(cache.observe_state(second, SessionState::starting));
    CHECK(!cache.observe_background_label(second, ""));
    auto checkpoint = cache.qualification_background_labels_checkpoint();
    auto moved_checkpoint = std::move(checkpoint);
    cache.restore_qualification_background_labels_checkpoint(std::move(checkpoint));
    CHECK(*cache.background_label(first) == "fixture");
    CHECK(cache.background_label(second));

    CHECK(cache.observe_state(first, SessionState::exited));
    CHECK(cache.observe_background_label(first, "changed"));
    cache.forget(second);
    CHECK(cache.observe_state(later, SessionState::failed));
    CHECK(cache.observe_background_label(later, "new label"));
    cache.restore_qualification_background_labels_checkpoint(std::move(moved_checkpoint));

    // Exact old whole-map swap, including unrelated/forgotten labels. This is
    // deliberately not the identity-gated, scoped attention rollback policy.
    CHECK(*cache.background_label(first) == "fixture");
    CHECK(cache.background_label(second));
    CHECK(cache.background_label(second)->empty());
    CHECK(!cache.background_label(later));
    CHECK(cache.state(first) == SessionState::exited);
    CHECK(!cache.state(second));
    CHECK(cache.state(later) == SessionState::failed);
    CHECK(!cache.observe_background_label(first, "fixture"));
    CHECK(!cache.observe_background_label(second, ""));
    CHECK(cache.observe_background_label(later, "new label"));
    cache.restore_qualification_background_labels_checkpoint(std::move(moved_checkpoint));
    CHECK(*cache.background_label(first) == "fixture");
    CHECK(cache.background_label(second));
    CHECK(*cache.background_label(later) == "new label");

    SessionObservationCache empty;
    auto empty_checkpoint = empty.qualification_background_labels_checkpoint();
    CHECK(!empty.observe_background_label(first, ""));
    CHECK(empty.observe_state(first, SessionState::starting));
    empty.restore_qualification_background_labels_checkpoint(std::move(empty_checkpoint));
    CHECK(!empty.background_label(first));
    CHECK(empty.state(first) == SessionState::starting);
}

} // namespace

int main() {
    return run_core_tests("session_observation_cache", [] {
        state_observations();
        label_observations();
        shared_history_and_forgetting();
        inherited_failure_rollback();
    });
}
