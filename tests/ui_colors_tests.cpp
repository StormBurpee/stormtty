#include "core_test.hpp"
#include "stormtty/ui_colors.hpp"
#include "stormtty/settings_panel.hpp"
#include <limits>
#include <cstdio>
using namespace stormtty;
namespace {
double contrast(Color a,Color b) {const auto x=ui_luminance(a),y=ui_luminance(b);return (std::max(x,y)+.05)/(std::min(x,y)+.05);}
void check_palette(const Theme& theme) {
    const auto original=theme;
    const auto c=resolve_ui_colors(theme);
    CHECK(theme==original);
    CHECK(c.canvas==ui_color(theme.background,{}));
    CHECK(c.surface==ui_color(theme.chrome,{}));
    CHECK(c.ink==ui_color(theme.foreground,{}));
    CHECK(c.accent==ui_color(theme.accent,{}));
    CHECK(c.navigation==ui_mix(c.surface,c.canvas,.55));
    CHECK(c.selected==ui_mix(c.surface,c.accent,.16));
    CHECK(c.hover==ui_mix(c.surface,c.ink,.06));
    CHECK(c.raised==c.hover);
    CHECK(c.divider==ui_mix(c.surface,c.ink,.18));
    for(auto color:{c.canvas,c.surface,c.navigation,c.raised,c.ink,c.secondary,c.subdued,c.divider,c.accent,c.control,c.hover,c.pressed,c.selected,c.selected_hover,c.selected_border,c.on_accent,c.positive}) CHECK(color.a==255);
    CHECK(contrast(c.on_accent,c.accent)>=4.5);
}
}
int main() {return run_core_tests("semantic UI colors",[]{
    for(const auto& theme:builtin_themes()) {
        check_palette(theme);
        const auto c=resolve_ui_colors(theme);
        CHECK(contrast(c.ink,c.surface)>=4.5);
        CHECK(contrast(c.secondary,c.surface)>=4.5);
        CHECK(contrast(c.ink,c.selected)>=4.5);
        Config config;config.theme=theme;SettingsPanel panel;panel.open(config);panel.resize({1040,720});
        WindowScene scene;panel.append_scene(scene,1);
        CHECK(std::any_of(scene.primitives.begin(),scene.primitives.end(),[&](const auto& p){return p.fill==c.surface && p.background_replace;}));
        CHECK(std::any_of(scene.primitives.begin(),scene.primitives.end(),[&](const auto& p){return p.fill==c.navigation;}));
        CHECK(std::any_of(scene.texts.begin(),scene.texts.end(),[&](const auto& t){return t.color==c.ink;}));
        const Color background{255,255,255,255},foreground{0,0,0,255},highlight{0,80,20,255},highlight_text{255,255,0,255},disabled{80,30,10,255};
        panel.set_high_contrast(true,background,foreground,highlight,highlight_text,disabled);
        WindowScene contrast_scene;panel.append_scene(contrast_scene,1);
        const auto allowed=[&](Color paint){return paint.a==0 || paint==background || paint==foreground || paint==highlight || paint==highlight_text || paint==disabled;};
        for(const auto& p:contrast_scene.primitives) {CHECK(allowed(p.fill));CHECK(allowed(p.stroke));}
        for(const auto& t:contrast_scene.texts) CHECK(allowed(t.color));
        CHECK(panel.draft().theme==theme);

    }
    const auto stable=resolve_ui_colors(Theme{});
    // Exact-color high-contrast remapping must not confuse thumb/primary ink
    // with either highlight fills or foreground/detail roles.
    for(auto role:{stable.accent,stable.selected,stable.selected_hover,stable.ink,stable.divider,stable.secondary,stable.subdued,stable.positive}) CHECK(stable.on_accent!=role);
    Theme custom;custom.name="not-a-built-in";custom.background="#301423";custom.chrome="#412033";custom.foreground="#eeddcc";custom.accent="#fF8000";
    check_palette(custom);
    auto renamed=custom;renamed.name="daylight";
    CHECK(resolve_ui_colors(renamed).navigation==resolve_ui_colors(custom).navigation);
    CHECK(resolve_ui_colors(custom).raised!=resolve_ui_colors(Theme{}).raised);
    const Color fallback{12,34,56,255};
    for(auto value:{"","#12345","#1234567","1234567","#GG1234","#00zz00"}) CHECK(ui_color(value,fallback)==fallback);
    CHECK(ui_color("#aB09Ff",fallback)==Color{171,9,255,255});
    CHECK(ui_mix({0,0,0,1},{255,255,255,2},-1)==Color{0,0,0,255});
    CHECK(ui_mix({0,0,0,1},{255,255,255,2},2)==Color{255,255,255,255});
    CHECK(ui_mix({0,0,0,1},{255,255,255,2},std::numeric_limits<double>::quiet_NaN())==Color{0,0,0,255});
    for(unsigned r=0;r<256;r+=17) for(unsigned g=0;g<256;g+=17) for(unsigned b=0;b<256;b+=17) {
        const Color accent{static_cast<std::uint8_t>(r),static_cast<std::uint8_t>(g),static_cast<std::uint8_t>(b),255};
        char hex[8]{};std::snprintf(hex,sizeof hex,"#%02X%02X%02X",r,g,b);custom.accent=hex;
        CHECK(contrast(resolve_ui_colors(custom).on_accent,accent)>=4.5);
    }
});}
