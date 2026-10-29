#pragma once

#include "stormtty/renderer.hpp"
#include "stormtty/ui_colors.hpp"
#include <array>
#include <cmath>

namespace stormtty {

struct TabSelectionColors { Color surface, border; };
// Base paint only. The host retains its existing focus/hover transition, alpha,
// geometry and theme/project accent markers. OS contrast bypasses theme roles.
inline TabSelectionColors tab_selection_colors(const UiColors& ui, bool high_contrast=false,
        Color contrast_background={}, Color contrast_foreground={}) noexcept {
    if(high_contrast) return {contrast_background,contrast_foreground};
    auto border=ui.selected_border;border.a=180; // Existing tab-border material.
    return {ui.selected,border};
}

// The Settings panel's owned cog, shared with its sidebar entry. Lines render
// with the caller's semantic ink, never font fallback or color-emoji glyphs.
// radius=11, stroke=1.5 preserves the original panel geometry exactly.
inline void append_settings_icon(WindowScene& scene, Point center, double radius,
        Color ink, float dpi_scale=1, double stroke_width=1.5, ChromeZOrder z={}) {
    if(!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(radius) || radius<=0 ||
       !std::isfinite(dpi_scale) || dpi_scale<=0 || dpi_scale>8 || !std::isfinite(stroke_width) || stroke_width<=0) return;
    struct Segment {Point from,to;};
    // Sidebar chrome is evaluated during terminal updates too. Reuse the fixed
    // path rather than adding per-frame trigonometry for this shared icon.
    static const auto path=[] {
        std::array<Segment,56> segments{};
        constexpr double tau=6.283185307179586;
        const auto point=[](double angle,double r){return Point{std::cos(angle)*r,std::sin(angle)*r};};
        const auto tooth_radius=[](int vertex){return vertex%4==1 || vertex%4==2?11.0:8.0;};
        for(int i=0;i<32;++i)segments[i]={point(tau*i/32,tooth_radius(i)),point(tau*(i+1)/32,tooth_radius((i+1)%32))};
        for(int i=0;i<24;++i)segments[32+i]={point(tau*i/24,3.5),point(tau*(i+1)/24,3.5)};
        return segments;
    }();
    const double size=radius/11;
    const auto placed=[&](Point p){return Point{center.x+p.x*size,center.y+p.y*size};};
    for(const auto& segment:path) {
        const auto a=placed(segment.from),b=placed(segment.to);
        scene.primitives.push_back({{float(a.x*dpi_scale),float(a.y*dpi_scale),float((b.x-a.x)*dpi_scale),float((b.y-a.y)*dpi_scale)},
            {0,0,0,0},ink,0,float(stroke_width*dpi_scale),ChromePrimitiveKind::line,false,z});
    }
}

} // namespace stormtty
