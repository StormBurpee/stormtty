#pragma once

#include "stormtty/config.hpp"
#include "stormtty/frame.hpp"
#include <algorithm>
#include <cmath>
#include <string_view>

namespace stormtty {
// Opaque semantic chrome colors shared by native shell and portable panels.
// Material opacity/reveal animation belongs to the caller. Never use these to
// recolor terminal glyphs or explicit ANSI cells. High-contrast callers replace
// these tokens at the paint boundary (Settings resolves stable defaults before
// semantic mapping), retaining platform HighlightText and disabled-text rules.
struct UiColors {
    Color canvas, surface, navigation, raised;
    Color ink, secondary, subdued, divider, accent;
    Color control, hover, pressed, selected, selected_hover, selected_border;
    Color on_accent, positive, danger;
    bool operator==(const UiColors&) const = default;
};

inline Color ui_color(std::string_view value, Color fallback) noexcept {
    const auto digit=[](char c) { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1; };
    if(value.size()!=7 || value[0]!='#') return fallback;
    int channels[6]{};
    for(unsigned i=0;i<6;++i) if((channels[i]=digit(value[i+1]))<0) return fallback;
    return {static_cast<std::uint8_t>(channels[0]*16+channels[1]),static_cast<std::uint8_t>(channels[2]*16+channels[3]),static_cast<std::uint8_t>(channels[4]*16+channels[5]),255};
}
inline Color ui_mix(Color a, Color b, double amount) noexcept {
    amount=std::isfinite(amount)?std::clamp(amount,0.,1.):0.;
    const auto channel=[&](int x,int y){return static_cast<std::uint8_t>(std::lround(x+(y-x)*amount));};
    return {channel(a.r,b.r),channel(a.g,b.g),channel(a.b,b.b),255};
}
inline double ui_luminance(Color color) noexcept {
    const auto linear=[](unsigned channel){const double s=channel/255.;return s<=.04045?s/12.92:std::pow((s+.055)/1.055,2.4);};
    return .2126*linear(color.r)+.7152*linear(color.g)+.0722*linear(color.b);
}
inline UiColors resolve_ui_colors(const Theme& theme) noexcept {
    UiColors c;
    c.canvas=ui_color(theme.background,{17,21,27,255});
    c.surface=ui_color(theme.chrome,{24,29,36,255});
    c.ink=ui_color(theme.foreground,{238,237,236,255});
    c.accent=ui_color(theme.accent,{140,158,255,255});
    c.navigation=ui_mix(c.surface,c.canvas,.55);
    c.control=ui_mix(c.surface,c.ink,.035);
    c.hover=ui_mix(c.surface,c.ink,.06);
    c.raised=c.hover;
    c.pressed=ui_mix(c.surface,c.ink,.12);
    c.secondary=ui_mix(c.ink,c.surface,.26);
    c.subdued=ui_mix(c.ink,c.surface,.42);
    c.divider=ui_mix(c.surface,c.ink,.18);
    c.selected=ui_mix(c.surface,c.accent,.16);
    c.selected_hover=ui_mix(c.surface,c.accent,.22);
    c.selected_border=ui_mix(c.surface,c.accent,.55);
    // Choose the stronger contrasting label for custom accent hues too; this
    // does not alter the user's accent or terminal foreground/ANSI palette.
    const double luminance=ui_luminance(c.accent);
    c.on_accent=(luminance+.05)/.05 >= 1.05/(luminance+.05)?Color{0,0,0,255}:Color{255,255,255,255};
    c.positive=ui_mix(c.accent,c.ink,.25);
    // Destructive chrome is not terminal ANSI red. Keep the familiar warm red
    // where readable, otherwise move toward the black/white endpoint with the
    // strongest minimum contrast on both normal and selected surfaces. Some
    // adversarial custom pairs cannot share 4.5:1 ink; use the best endpoint in
    // that case without changing any user-authored foreground/base colors.
    const auto surface_luminance=ui_luminance(c.surface),selected_luminance=ui_luminance(c.selected);
    const auto contrast=[](double a,double b) { return (std::max(a,b)+.05)/(std::min(a,b)+.05); };
    const auto minimum_contrast=[&](Color ink) {
        const auto luminance=ui_luminance(ink);
        return std::min(contrast(luminance,surface_luminance),contrast(luminance,selected_luminance));
    };
    const Color black{0,0,0,255},white{255,255,255,255};
    const auto black_contrast=std::min(contrast(0,surface_luminance),contrast(0,selected_luminance));
    const auto white_contrast=std::min(contrast(1,surface_luminance),contrast(1,selected_luminance));
    const auto endpoint=black_contrast>=white_contrast?black:white;
    const auto target=std::min(4.5,std::max(black_contrast,white_contrast));
    const Color red{244,155,158,255};
    c.danger=endpoint;
    for(unsigned step=0;step<=32;++step) {
        const auto candidate=ui_mix(red,endpoint,step/32.);
        if(minimum_contrast(candidate)>=target) { c.danger=candidate; break; }
    }
    return c;
}
} // namespace stormtty
