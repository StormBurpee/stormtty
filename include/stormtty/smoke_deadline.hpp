#pragma once
#include <cstdint>

namespace stormtty {
// Fixture-only phase bounds. Activity/bytes never extend a deadline. The hard
// bound leaves ten seconds for cleanup under CTest's unchanged 60-second guard.
class SmokeDeadline {
public:
    enum class Phase { startup, interaction, exit };
    explicit SmokeDeadline(std::uint64_t now = 0) : started_(now), phase_started_(now) {}
    bool advance(Phase next, std::uint64_t now) noexcept {
        if (next <= phase_ || expired(now)) return false;
        phase_=next; phase_started_=now; return true;
    }
    bool expired(std::uint64_t now) const noexcept {
        return now < started_ || now < phase_started_ || now-started_ >= 50000 || phase_elapsed(now) >= phase_budget();
    }
    std::uint64_t phase_elapsed(std::uint64_t now) const noexcept { return now >= phase_started_ ? now-phase_started_ : 0; }
    std::uint64_t phase_budget() const noexcept { return phase_ == Phase::exit ? 10000 : 20000; }
    Phase phase() const noexcept { return phase_; }
private:
    std::uint64_t started_{}, phase_started_{};
    Phase phase_{Phase::startup};
};
}
