#include "core_test.hpp"
#include "stormtty/session_wake_router.hpp"
#include <atomic>
#include <thread>
using namespace stormtty;
namespace {
struct Endpoint {std::atomic<unsigned> count{};void wake() noexcept{++count;}};
void retired_completion() {
    auto endpoint=std::make_shared<Endpoint>();SessionWakeRouter<Endpoint> router;
    auto actor_route=router.bind({1},endpoint);actor_route->wake();CHECK(endpoint->count==1);
    router.erase({1}); // Same ordering as native retire_unused_sessions.
    actor_route->wake();CHECK(endpoint->count==2); // Actor disposal still wakes modal stage2.
    std::weak_ptr<Endpoint> weak=endpoint;endpoint.reset();CHECK(weak.expired());actor_route->wake();
}
void move_then_retire() {
    auto source=std::make_shared<Endpoint>(),destination=std::make_shared<Endpoint>();
    SessionWakeRouter<Endpoint> router;auto callback=router.bind({2},source);
    CHECK(router.bind({2},destination)==callback);source.reset();callback->wake();CHECK(destination->count==1);
    router.erase({2});callback->wake();CHECK(destination->count==2);
    // A forgotten old registration cannot accidentally redirect a replacement.
    auto other=std::make_shared<Endpoint>();auto replacement=router.bind({2},other);CHECK(replacement!=callback);
    callback->wake();CHECK(destination->count==3);CHECK(other->count==0);
    replacement->wake();CHECK(other->count==1);
}
void concurrent_routing_and_registry_lifetime() {
    auto a=std::make_shared<Endpoint>(),b=std::make_shared<Endpoint>();
    std::shared_ptr<SessionWakeRouter<Endpoint>::Route> callback;
    {
        SessionWakeRouter<Endpoint> router;callback=router.bind({3},a);
        std::thread actor([callback]{for(unsigned i=0;i<10000;++i)callback->wake();});
        for(unsigned i=0;i<1000;++i)CHECK(router.bind({3},i%2?a:b)==callback);
        actor.join();CHECK(a->count+b->count==10000);
        router.erase({3});
    }
    const auto count=a->count+b->count;callback->wake();CHECK(a->count+b->count==count+1);
}
}
int main(){return run_core_tests("session_wake_router",[]{retired_completion();move_then_retire();concurrent_routing_and_registry_lifetime();});}
