#pragma once
#ifndef STORMTTY_QUALIFICATION_INPUT_EVIDENCE
#error "Deadline candidate is restricted to isolated qualification builds"
#endif
#include <chrono>
#include <memory>
#include <cstdint>
namespace stormtty::qualification {
// Single waiting actor, arbitrary notifier threads. Notification remains latched
// across unlock-to-wait; handles outlive every waiter/notifier. No callbacks.
class DeadlineWait {
public:
    using Clock=std::chrono::steady_clock;
    enum class Reason : std::uint32_t { notified, deadline, failed };
    struct Result { Reason reason; std::uint32_t error{}; };
    virtual ~DeadlineWait()=default;
    virtual void notify() noexcept=0;
    virtual Result wait_until(Clock::time_point) noexcept=0;
};
// Unsupported high-resolution timer creation fails; never label fallback as A/B.
std::unique_ptr<DeadlineWait> make_high_resolution_deadline_wait();
}
