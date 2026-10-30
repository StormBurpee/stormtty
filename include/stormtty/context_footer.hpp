#pragma once
#include "stormtty/workspace.hpp"
#include "stormtty/pane_context.hpp"
#include <algorithm>
#include <cmath>
#include <string>

namespace stormtty {
// Geometry depends only on the window, never on changing shell metadata.
struct ContextFooterLayout { Rect bounds, text, inspect, terminal; };
inline ContextFooterLayout layout_context_footer(double width,double height,double header) {
    width=std::isfinite(width)?std::max(0.,width):0.;
    height=std::isfinite(height)?std::max(0.,height):0.;
    header=std::isfinite(header)?std::clamp(header,0.,height):0.;
    const double footer_height=std::min(26.,height-header);
    const double y=height-footer_height,padding=std::min(16.,width/2);
    const double inner=std::max(0.,width-2*padding),button=std::min(64.,inner);
    const double gap=inner>button?std::min(12.,inner-button):0.;
    const double top=std::min(y,header+14.);
    return {{0,y,width,footer_height},{padding,y,std::max(0.,inner-button-gap),footer_height},
        {width-padding-button,y,button,footer_height},
        {padding,top,inner,std::max(0.,y-top-8.)}};
}
struct ContextFooterPresentation {
    PaneContextPresentation context;
    std::string status,notice;
    bool failed{};
    bool operator==(const ContextFooterPresentation&) const = default;
    std::string detail()const {
        std::string value=context.detail+"\nStatus: "+status;
        if(!notice.empty())value+="\nNotice: "+notice;
        return value;
    }
    template<class Measure> std::string label(double width,Measure measure)const {
        // A notice or lifecycle failure has priority; full context remains in
        // the native Inspect action. Never trim an exit code into another value.
        if(!notice.empty())return notice;
        const auto directory=pane_context_secondary(context,PaneContextDensity::directory);
        const auto full=pane_context_secondary(context);
        for(const auto& candidate:{full.empty()?status:full+" · "+status,
                                   directory.empty()?status:directory+" · "+status})
            if(measure(candidate)<=width)return candidate;
        if(failed || context.retained_exited)return measure(status)<=width?status:failed?"Terminal failed":"Terminal ended";
        return directory.empty()?"Directory unavailable":directory;
    }
};
}
