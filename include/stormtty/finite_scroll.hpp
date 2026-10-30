#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stormtty {
// Shared Settings/palette presentation only. Owners invalidate semantic changes
// with reset/cancel; neither selection nor terminal input belongs in this model.
struct FiniteScroll {
    double display{}, target{}, start{};
    std::uint64_t started{}, sampled{};
    bool active{};
    void cancel() noexcept { active=false; target=start=display; started=sampled=0; }
    void reset(double value=0) noexcept { display=value; cancel(); }
    void finish() noexcept { if(active) display=target; cancel(); }
    bool tick(std::uint64_t now) noexcept {
        if(!active || now<=sampled) return false;
        sampled=now;
        const double t=std::min(1.0,double(now>=started?now-started:0)/90.0), r=1-t;
        const double old=display;
        display=t>=1?target:start+(target-start)*(1-r*r*r);
        if(t>=1)cancel();
        return old!=display;
    }
    void wheel(double delta,double maximum,std::uint64_t now,bool animate) noexcept {
        if(!std::isfinite(delta)||delta==0||!std::isfinite(maximum))return;
        maximum=std::max(0.0,maximum);
        // Accumulate bursts, but discard residual travel when reversing.
        const bool same=animate && active && ((delta>0)==(target>display));
        if(active)now=std::max(now,sampled);
        if(same)tick(now);
        const double next=std::clamp((same?target:display)+std::clamp(delta,-32768.0,32768.0),0.0,maximum);
        cancel();
        if(!animate){reset(next);return;}
        const double distance=next-display;
        if(distance==0)return;
        // Immediate small response; remaining travel ends within 90 ms.
        display+=std::copysign(std::min(8.0,std::abs(distance)*.15),distance);
        start=display;target=next;started=sampled=now;active=display!=target;
    }
};
}
