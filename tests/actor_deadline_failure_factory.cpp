#include "stormtty/actor_deadline_wait.hpp"
namespace stormtty {
namespace {
class Failure final:public ActorDeadlineWait {
public:
    bool prepare_wait() noexcept override {return true;}
    void notify() noexcept override {}
    Result wait_until(Clock::time_point) noexcept override {return {Reason::failed,1};}
};
}
// Linked only into the dedicated real-Session fallback test, never the app.
std::unique_ptr<ActorDeadlineWait> make_actor_deadline_wait() noexcept {
#ifdef STORMTTY_TEST_DEADLINE_UNSUPPORTED
    return {};
#else
    return std::make_unique<Failure>();
#endif
}
}
