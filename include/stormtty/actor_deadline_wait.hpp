#pragma once
#include <chrono>
#include <memory>
#include <cstdint>
namespace stormtty {
// Single waiting actor, arbitrary notifier threads. Notification remains latched
// across unlock-to-wait; handles outlive every waiter/notifier. No callbacks.
class ActorDeadlineWait {
public:
    using Clock=std::chrono::steady_clock;
    enum class Reason : std::uint32_t { notified, deadline, failed };
    struct Result { Reason reason; std::uint32_t error{}; };
    virtual ~ActorDeadlineWait()=default;
    // Call while holding the queue lock, only after its predicate is false.
    virtual bool prepare_wait() noexcept=0;
    virtual void notify() noexcept=0;
    virtual Result wait_until(Clock::time_point) noexcept=0;
};
// Capability probe. Unsupported platform/resource failure uses the existing CV.
std::unique_ptr<ActorDeadlineWait> make_actor_deadline_wait() noexcept;
}
