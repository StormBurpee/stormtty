#pragma once
#include "stormtty/workspace.hpp"
#include <map>
#include <memory>
#include <mutex>

namespace stormtty {
// UI-thread registry + actor-thread route. Endpoint supplies wake() noexcept.
// Actor callbacks retain the route, not the registry: removing catalog/registry
// ownership must not suppress the actor's final disposal notification.
template<class Endpoint> class SessionWakeRouter {
public:
    class Route {
    public:
        void wake() noexcept {
            std::shared_ptr<Endpoint> endpoint;
            {std::lock_guard lock(mutex_);endpoint=endpoint_.lock();}
            if(endpoint)endpoint->wake();
        }
    private:
        friend class SessionWakeRouter;
        void bind(const std::shared_ptr<Endpoint>& endpoint) {std::lock_guard lock(mutex_);endpoint_=endpoint;}
        std::mutex mutex_;
        std::weak_ptr<Endpoint> endpoint_;
    };
    std::shared_ptr<Route> bind(SessionId id,const std::shared_ptr<Endpoint>& endpoint) {
        std::lock_guard lock(mutex_);
        // Failed launches may release their callback before registry insertion.
        // Reap expired lookup slots on each bind; they cannot grow unbounded.
        std::erase_if(routes_,[](const auto& entry){return entry.second.expired();});
        auto& stored=routes_[id];auto route=stored.lock();
        if(!route){route=std::make_shared<Route>();stored=route;}
        route->bind(endpoint);return route;
    }
    // Forget discoverability only. Existing callbacks keep their last route and
    // weak endpoint until the actor releases them; no endpoint lifetime extension.
    void erase(SessionId id) {std::lock_guard lock(mutex_);routes_.erase(id);}
private:
    std::mutex mutex_;
    std::map<SessionId,std::weak_ptr<Route>> routes_;
};
} // namespace stormtty
